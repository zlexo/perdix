# RFC 0005: Job system and threading model

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M3 Infinite |
| **Issue** | #19 |

## Summary

The engine's threads, what each one owns, and the job system that spreads chunk generation and meshing across the CPU. The main thread reads input and draws. A world thread owns every chunk, is the only thread that writes one, and ticks the simulation 20 times a second. Workers run jobs on pinned, frozen versions of chunks and send results back through queues: nearest chunk first, cancelable when a chunk unloads, with no allocation in their inner loops. View distance is bounded by how fast chunks are generated and meshed; this design puts every spare cycle on the nearest work without a frame ever waiting for it.

## Motivation

| | Estimate |
|---|---|
| Chunks loaded at a 1,024-block view distance (RFC 0002) | about 51,000 |
| New chunks in range per 32 blocks flown (a strip 64 chunks wide, 16 tall) | about 1,000 |
| Meshing one typical chunk (RFC 0003 target) | under 150 µs |
| Loading the whole view on one core, at 1 ms per chunk for generation and meshing | about 51 s |

The 1 ms is a placeholder until RFC 0008 budgets generation. Neither the main thread (16.7 ms per frame at 60 Hz) nor the world thread (50 ms per tick) can absorb this, so it runs on the other cores, nearest first.

Parallel work needs rules. Generation, meshing, and later lighting and saving read chunks; generation results, edits and simulation write them. RFC 0002 made `ChunkStorage` a value type without locks and left "who may touch a chunk from which thread" to this RFC. A race there is a crash or a corrupted chunk an hour into a session, so the rules must make races impossible by construction, with a sanitizer to check.

## Goals and non-goals

Goals:

- Every piece of shared state has one owning thread or one lock. Chunks belong to the world thread, and jobs read pinned snapshots that cannot change under them.
- An edit's remesh runs before queued streaming work, and streaming runs nearest chunk first.
- Canceling a queued job tells the caller for certain whether it will still run.
- The main thread never waits for another thread and no job waits for another job, so nothing can deadlock.
- No allocation to submit a job or in its inner loops; at most one per job, for its result.
- A deterministic mode for tests; Tracy shows threads, jobs and queues; ThreadSanitizer checks the threaded code on Linux CI.
- Worker count and QoS are settings, with defaults from a sweep on the reference machine.

Non-goals:

- Which chunks to load, in what order, and when to unload them: RFC 0006, using this RFC's priorities and cancellation.
- GPU memory, uploads, transfer queues and async compute: RFC 0007. Terrain generation: RFC 0008. The block registry: RFC 0004.
- Networking (after M9), audio, and disk I/O for saving (M8): later RFCs.
- Fibers, jobs that wait for jobs, and parallelism inside one chunk's job.

## Prior art

- **Naughty Dog** (Christian Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015): 6 workers locked to cores, 160 fibers, three priority queues, no stealing, jobs waiting on atomic counters. We take the fixed priority levels, not the fibers.
- **enkiTS** (Doug Binks, written for the voxel game Avoyd): 3 priority levels by default, tasks pinned to a thread, dependencies, threads stealing from each other; no cancellation, no order within a level.
- **oneTBB, Taskflow, and "Job System 2.0"** (Stefan Reinalter, Molecular Matters): work stealing on Chase-Lev deques. oneTBB 2021 keeps priorities only per arena; Taskflow 3.8 removed task priorities as inconsistent with work stealing.
- **Bevy**: task pools split by latency (compute, async compute, I/O). **Unreal Engine**: a game thread, a render thread one or two frames behind it, and an RHI thread.
- **Minecraft Java Edition**: a render thread, a server thread that owns the world and ticks it 20 times a second, and a background pool, one thread per CPU thread minus one, that runs world generation: the shape of this design. Its "Chunk Builder" option can rebuild an edited section at once on the render thread, because threaded rebuilds can briefly show holes.
- **Sodium**: chunk builds on 2 threads on 8 cores by default, from a heap ordered mostly by distance; a rebuild the player caused within 16 blocks jumps the queue, and by default the render thread does not wait for it. Our `urgent` level.
- **Cubyz**: one pool, one priority heap under a mutex, and every 100 ms each queued task is asked for its current priority and whether it is still needed. Our kind of queue, re-sorted in place where ours stays short.
- **Luanti**: separate mesh and map-generation threads; map generation defaults to one thread, because its built-in generators have known bugs with more.
- **Apple**: XNU's scheduler documentation on how QoS maps to cores, and "Tuning your code's performance for Apple silicon": hand out work dynamically, with more tasks than cores, and block instead of spinning or yielding.

## Design

### Threads

| Thread | Count | Owns | Runs | macOS QoS |
|---|---|---|---|---|
| Main | 1 | window, input, camera, renderer | events and input; per frame: send actions and the player's position, drain the world's messages into RFC 0007's uploader, render (RFC 0001) | user-interactive |
| World | 1 | every chunk, the chunk map, simulation | 20 ticks per second; between ticks: edits, results, feeding jobs | user-interactive |
| Worker | cores minus two: 6 here | its scratch memory | jobs | utility; user-initiated while catching up |

Rendering stays on the main thread. SDL3 and GLFW, the candidates in RFC 0001, both require their event and window calls there, and with the world on its own thread a frame is little CPU work (this RFC's open question 1).

Below `app`, nothing creates a thread: `world` and `mesh` are functions and single-owner types, `core`'s job system creates the workers, and `app` creates the world thread and wires the queues. A dedicated server (ADR 0003) is `core`, `world` and a small `app` of its own. Later RFCs that add I/O, audio or networking threads name their owners.

| State | Written by | Read by |
|---|---|---|
| Current chunk versions, the chunk map, pins | world thread | world thread |
| A pinned chunk version, a mesh job's record | nobody while in use | the jobs holding it, the world thread |
| Block visuals (RFC 0004) | before jobs run | everyone |
| Plans and parameter snapshots (RFC 0008) | the world thread, then frozen | the jobs pinning them |
| Worker scratch | its worker | its worker |
| Job queue, inboxes | any thread, under their lock | workers, inbox owners |

### Chunk versions and pins

1. Once a `ChunkStorage` is in the world, only the world thread writes or frees it. Generation jobs build new ones and hand them over.
2. To hand chunk data to a job, the world thread pins the chunk's current version. A pinned version is a frozen snapshot.
3. Writing to a pinned chunk first copies it (copy on write); the map points at the copy, and the frozen version stays for its jobs.
4. Pins are plain integers changed only by the world thread: up when it submits a job, down when the job's completion (its last act) arrives or a cancel succeeds, on exactly the versions the job's record names, never on whatever is current by then.
5. A version that is neither current nor pinned is freed at once, so an unloaded chunk lives until its last job reports back.
6. The same rules cover RFC 0008's plans and parameter snapshots: a build job pins the 27 plans it reads and the snapshot they were made with, so a neighbor leaving the region, a dropped plan or new parameters published mid-build (RFC 0006) cannot free what a job reads.

A mesh job pins its chunk and the 26 neighbors: 27 increments, and no copy unless an edit lands while the job runs. The world thread keeps one record per job in flight, in a pool sized to the job slots: the versions it pinned, the chunk's load id (never reused) and a sequence number. The job captures a pointer to its record. A uniform neighbor has no blocks to pin; its state goes into the job record instead. The worker builds RFC 0003's padded chunk in its scratch from the pinned versions with `to_dense`, `copy_face` and `get` (for the edge and corner neighbors). Jobs that compute changes (decoration, later light) return them as data for the world thread to apply.

Pins need no atomics. Ordering comes from the queues: the world thread's writes precede the submit (a lock release), the worker's reads follow the take (the matching acquire), and the unpin follows draining the completion (another pair).

RFC 0002 changes, all in `world`: `ChunkStorage` gains `clone()`, being move-only; its `const` methods must not write hidden state, since several jobs read one pinned version at once; `compact()` is a write, so the world thread runs it on unpinned versions as a pass of its own, not for free during meshing; each block-data size class takes a lock, because generation jobs allocate on workers what the world thread frees; and `from_dense` takes its reverse lookup table from the caller's scratch.

### Jobs

A job is a callable stored in a preallocated slot (1,024 slots of 128 bytes by default), with trivially copyable captures of at most 64 bytes: positions, ids, pointers. A job owns nothing, so no ownership passes through the scheduler: inputs stay pinned by the world thread, scratch belongs to the worker, and results leave through inboxes.

```cpp
namespace vx {
enum class JobLevel : std::uint8_t { urgent, normal, background };

struct JobPriority {
    JobLevel level = JobLevel::normal;
    std::uint32_t key = 0;   // lower runs first within a level; streaming uses squared chunk distance
};

template <class F>   // JobContext::worker() indexes per-worker scratch
concept Job = std::is_trivially_copyable_v<F> && sizeof(F) <= 64 &&
              std::is_nothrow_invocable_v<const F&, JobContext&>;

struct JobHandle {   // generations start at 1, so a default handle is empty; stale handles are harmless
    std::uint32_t slot = 0, generation = 0;
    explicit operator bool() const noexcept { return generation != 0; }
};

class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc);   // workers (0: deterministic), slots, QoS
    ~JobSystem();   // asserts nothing is queued or running, then joins the workers
    // Never blocks or allocates. Returns an empty handle when every slot is taken.
    template <Job F> [[nodiscard]] JobHandle submit(JobPriority priority, const F& job);
    // True: the job had not started and never will. False: it is running or done.
    bool cancel(JobHandle job) noexcept;
    void run_until_idle();   // deterministic mode only (asserts zero workers): runs queued jobs here
};
} // namespace vx
```

Workers take the most urgent level first, then the lowest key, then the oldest job, so the order is total. `urgent` is remeshing after an edit, the edited chunk first; `normal` is streaming, by distance; `background` is work nobody waits for, like saving (M8). A background job that has waited a second moves up to normal, so background work cannot starve during a long flight; nothing ages into urgent, so edits always run first; the age is read from the job system's clock, which tests control. A worker runs an `urgent` job at user-initiated QoS, whatever its mode (below). A condition variable passes no QoS to the thread it wakes, so when an urgent job wakes a sleeping utility worker, the world thread raises that worker with `pthread_override_qos_class_start_np` (in the macOS SDK's `pthread/qos.h`) until the job starts; then an edit never waits behind other apps' work.

The scheduler has no dependencies: a job is submitted only once it can run. The world thread sees chunks arrive, so it decides when a chunk is ready to mesh (RFC 0006 sets the rule; RFC 0003 reads a missing neighbor as air) and remeshes when a neighbor lands. Chunk pipelines are state machines on the world thread, not job graphs.

A running job is never interrupted; it lasts microseconds to milliseconds, and the world thread drops results for chunks that are gone. When a chunk unloads, its queued jobs are canceled and their inputs unpinned at once; a running job's inputs are unpinned when it reports back.

Only shutdown and tests wait for jobs, and no engine lock is held while a job runs or while another engine lock is taken: with no cycle of waits, there is no deadlock to find. Idle workers sleep on a condition variable, never spinning or yielding, and nothing uses a standalone fence, which ThreadSanitizer does not support. Shutdown runs in a fixed order: the world thread stops feeding, cancels everything queued, drains until every running job has reported back, frees its chunks and exits; then the job system, which asserts it is idle, joins its workers; the mesh result pool goes last.

### Feeding: a short queue

The scheduler holds only work that can start soon. The backlog of wanted chunks stays on the world thread, ordered by RFC 0006, and each pass of the world loop tops every level up to a limit, 256 jobs by default; three full levels plus the running jobs fit in the 1,024 slots. When the player moves, at most a few tens of milliseconds of stale work sit ahead of the new nearest chunks; an unloading chunk is usually still in the backlog, so canceling is rare; and pins and pending results scale with the queue, not the backlog. If `submit` ever returns an empty handle, the world thread releases the pins it took and leaves the chunk in its backlog. The queue must outlast the world thread's longest busy stretch, or workers run dry: 256 mesh jobs keep six workers busy for about 6 ms, so ticks stay short (below), and the sweep tunes the limit.

### Handing results back

```cpp
template <class T>
class Inbox {   // vx::Inbox: many producers, one consumer
public:
    Inbox(std::size_t capacity, WakeEvent& owner);   // the owner sleeps on a WakeEvent, with a deadline
    void push(T&& message);                           // any thread; never waits for the owner
    template <class F> void drain(F&& f);             // owner only: f(T&&) for everything queued
};
```

An inbox is a lock and two arrays swapped on drain. Messages from one producer arrive in order, with no order across producers, and whatever a producer wrote before `push` is visible after `drain`. A push wakes the owner only when the inbox was empty, so a busy owner drains many messages per wake-up; a `WakeEvent` is a latched flag, so a signal sent just before the owner sleeps is not lost. The results inbox reserves one message per job slot, so it never grows; the others grow once if a burst outruns their reservation, rather than dropping or blocking.

| From | To | Carries |
|---|---|---|
| main | world | actions (break, place) with the view ray, the player's position, meshes uploaded so far, a stop request |
| workers | world | generated chunks, finished meshes |
| world | main | meshes, unloads, the targeted block |

The main thread never reads a chunk. It sends the action and the view ray; the world thread finds the targeted block (M5's raycast), applies the edit, and sends back the target for the selection outline. Player physics is simulation and lives on the world thread too.

Meshes travel through the world thread, so the main thread sees one ordered stream and a mesh can never arrive after its chunk's unload. The world thread forwards a mesh only if its chunk is loaded with the same load id and the mesh is newer than the last one forwarded; otherwise it frees it. The remeshes of one edit form a batch that goes out as a single message once all have finished, and RFC 0007's `upload_batch` uploads it whole in the frame it arrives, outside the upload budget, so breaking a block never leaves a hole. While a batch is pending, newer results for its chunks join it instead of going out alone; a member whose chunk unloads leaves it; load ids are checked when it is sent. The world thread holds back new streaming mesh jobs while meshes forwarded minus meshes uploaded (a running count the main thread sends with the player's position) is over a limit; edits are exempt.

A generated chunk is a `ChunkStorage` allocated on the worker. A mesh job copies its quads from scratch into one result buffer from a `MeshResultPool` in `core`: CPU memory in size classes with a lock each, like RFC 0002's block data, allocated on workers and freed by the main thread once RFC 0007 has copied the quads into its arena. A mesh update owns its buffer, so RFC 0003's spans into scratch never leave the worker.

### The world loop

```cpp
while (!stopping) {                   // set by the main thread's stop request
    wake.wait_until(next_tick);       // returns early when an inbox has messages
    from_main.drain(handle);          // actions become edits (marking chunks dirty); position
    results.drain(integrate);         // stores chunks; unpins, forwards fresh meshes
    if (clock.now() >= next_tick) { world.tick(); next_tick += tick_period; }
    feed();                           // remesh dirty chunks (urgent), then stream nearest first
}
```

Edits apply on arrival, so a placed block is remeshed within a millisecond, not at the next tick (open question 4). Simulation advances only in `tick()`, 20 times a second; over a second behind, it skips ticks and logs it. Ticks must stay short, because a long one delays feeding and forwarding: simulation work that grows heavy (light, in M6) becomes jobs too.

### Worker count and QoS

The reference machine has 4 performance (P) and 4 efficiency (E) cores, one thread each (`sysctl hw.perflevel0`, `hw.perflevel1`). P cores run at up to 4.06 GHz with 128 KiB of L1 data cache, E cores at up to 2.75 GHz with 64 KiB (clocks measured by Howard Oakley), so a job takes longer on an E core, and RFC 0003's 77 KiB padded chunk fits only a P core's L1.

macOS cannot pin threads: `thread_policy_set` with `THREAD_AFFINITY_POLICY` returns `KERN_NOT_SUPPORTED` here. The lever is the QoS class each thread sets on itself with `pthread_set_qos_class_self_np` (a new thread starts at default QoS). Oakley's measurements also show threads above background QoS preferring P cores and overflowing onto E cores, and background threads kept on E cores at their lowest clock, 744 MHz on M3. XNU's scheduler documentation gives each class's bucket a worst-case scheduling latency (0 for user-interactive, 37.5 ms user-initiated, 75 ms default, 150 ms utility, 250 ms background), runs the earliest deadline first, and rotates long-running threads between P and E cores so they progress evenly. No worker is a P worker or an E worker.

A probe on the reference machine measured the trade-off: a user-interactive frame thread, standing in for the main thread, waking 60 times a second to do 1.65 ms of arithmetic beside six busy workers that ran either pure arithmetic or a meshing-like kernel (copy a 77 KiB padded chunk, build row masks, count faces). 14 and 16 runs per row, the wake-up delay recorded in 11 of each, with other programs running.

| Six workers at | Frame work, median | Frame wake-up delay, median | Throughput, arithmetic | Throughput, meshing-like |
|---|---|---|---|---|
| user-initiated | 2.2 to 2.4 ms | 0.6 to 2.0 ms | baseline | baseline |
| utility | 1.65 ms | 0.12 to 0.28 ms | about 14% lower | about half |

User-initiated workers slowed the frame thread by a third and made it wake late, at the 99th percentile by about 10 ms in some runs: most of a 60 Hz frame. Utility workers left it at full speed and on time and cost little on arithmetic, but on the meshing-like kernel they delivered half the throughput, and four of them did as much as six while eight did less, which fits workers kept mostly on E cores for memory-heavy work, though Tracy cannot show which core ran them. Neither class wins, so workers switch. They run at utility while streaming fills in the distance, which keeps frames smooth, and at user-initiated while RFC 0006 reports `catching_up` (chunks missing near the player) (first load, a teleport, flying faster than streaming), when fill-in speed is what the player is waiting for (open question 3). A worker changes its own class between jobs when the mode changes; QoS affects neither order nor correctness.

Workers default to the core count minus two, 6 here. The main and world threads are user-interactive; the world thread feeds the workers and must not queue behind them. Background QoS, held at 744 MHz, is far too slow for streaming. Utility work also yields to other apps; macOS Game Mode minimizes background activity while a game runs, and `app` opts in with the `LSSupportsGameMode` key. On Linux and Windows the QoS setting has no effect.

### Per-worker scratch

```cpp
// app: one per worker, indexed by JobContext::worker(), never shared.
struct alignas(vx::cache_line) WorkerScratch {
    MeshScratch mesh;   // RFC 0003: padded chunk (77 KiB), masks, quads (1.5 MiB worst case)
    GenScratch gen;     // RFC 0008: dense blocks (64 KiB), noise; RFC 0002's from_dense table
};
```

Scratch is allocated at startup for the worst case (RFC 0003's checkerboard of two different cutout blocks, 196,608 quads, 1.5 MiB), so it never grows: about 2.7 MiB per worker with RFC 0008's 1 MiB of generation scratch, about 16 MiB for six workers. `vx::cache_line` is 128, the line size the reference machine reports; `std::hardware_destructive_interference_size` says 64 under Apple Clang 17, changes with compiler and tuning, and draws a GCC warning in headers. Job slots are 128 bytes, aligned to it. Big buffers live in scratch, not on a job's stack: a `std::thread` gets 512 KiB of stack on macOS.

### Profiling and tests

Each thread names itself for Tracy (`tracy::SetThreadName`) and the OS: `Main`, `World`, `Worker 0` to `Worker 5`. Each job opens a zone named for its kind (`Generate`, `Mesh`) with the chunk position as text. The main thread marks frames with `FrameMark`; the world thread brackets ticks as a discontinuous frame set (`FrameMarkStart("Tick")` and `FrameMarkEnd("Tick")`). Plots show queued jobs per level, pins, copy-on-write copies and inbox depths, and the scheduler lock is a `TracyLockable` in profiling builds. On macOS, Tracy sees neither context switches nor cores, so P and E placement is judged from timings, as in the probe.

With zero workers, jobs wait until the owner calls `run_until_idle()`, which runs them on its own thread as worker 0, in priority order. The world loop takes its clock as a parameter, so tests step ticks without sleeping. The final state never depends on which job finishes first, because results are keyed by chunk, load id and sequence number.

## Alternatives considered

- **Work stealing** (oneTBB, Taskflow, Job System 2.0): a deque per worker, worked from one end by its owner and robbed from the other. It scales to many cores and tiny tasks with almost no contention, but loses here on order, cancellation and risk. A deque runs its newest work first and gives away its oldest, so "nearest first" holds per worker only, which is why oneTBB and Taskflow dropped per-task priorities; canceled jobs can only be marked and skipped; and Chase-Lev's memory orderings on weakly ordered CPUs like the reference machine's needed a paper of their own (Lê and others, PPoPP 2013). Apple's tuning guide names work stealing as the cure for static splits, where E-core threads straggle; a shared queue is just as dynamic. And our jobs are coarse: six workers each taking a 50 µs job pop 120,000 times a second, while on the reference machine an uncontended lock, push and unlock takes about 5 ns, and six threads doing nothing else completed 55 million a second. The API hides the strategy, so stealing can replace the lock if Tracy ever shows contention.
- **Fibers** (Naughty Dog): jobs wait mid-function without blocking a worker, at the price of per-platform context switching, fiber-aware tools, and thread-local storage and OS locks that break when a fiber changes threads. No job here waits.
- **A separate render thread** (Unreal): overlaps rendering with game logic on the game thread; ours already runs on the world thread. Open question 1.
- **Pools or threads per kind of work** (Bevy; a generation pool and a meshing pool): fixed splits of 8 cores sit idle when one kind runs out. Levels in one pool keep the order without the idling.
- **Locks inside chunks:** every read pays, writers stall readers, a mesh job takes 27 locks in some order, and RFC 0002 chose a type without them.
- **Read phases** (workers read live chunks while the world thread holds off writing): no pins, but workers follow the tick schedule, edits wait out phases, and a read outside its phase is a silent race.
- **Plain copies instead of pins:** the job owns its input, which is easy to reason about, but one world thread copies tens of KiB per job for six workers. Open question 2.
- **enkiTS:** close, but without cancellation or order within a level, which this design relies on. Ours is a few hundred lines with exactly these guarantees.

## Performance budget

Targets on the reference machine, release build. Scheduler numbers from nanobench; end-to-end numbers from the M3 benchmark flythrough and Tracy.

| Measure | Target |
|---|---|
| Submit and run an empty job, deterministic mode | under 100 ns |
| Empty jobs through all workers | over 1 million per second |
| Idle worker starting an urgent job | under 50 µs at the 99th percentile |
| World thread: pin a neighborhood and submit a mesh job | under 1 µs |
| An edit reaching the world thread to its remeshes reaching the main thread, idle workers | under 1 ms |
| Main thread time spent waiting for other threads | zero |
| Meshing throughput, all workers against one | at least 4 times |
| Allocations | none to submit, at most one per job |
| Memory | 128 KiB of job slots, about 2.7 MiB of scratch per worker |

## Test plan

- **Order:** in deterministic mode, levels first, then keys, then submission order; on a manual clock, a background job that waits a second moves up to normal, and a normal job never becomes urgent.
- **Cancellation:** canceling a queued job returns true and it never runs; a running or finished job returns false; an empty handle, or a stale one on a reused slot, returns false and touches nothing.
- **Limits:** a full slot table returns an empty handle; a capture over 64 bytes or not trivially copyable fails to compile (checked like RFC 0000's layering control).
- **Pins:** writing a pinned chunk copies it and leaves the pinned version unchanged; a completion unpins the versions its record names even after an edit replaced them; an unloaded chunk is freed only after its last job reports back; a successful cancel unpins at once; shutting down with jobs in flight frees everything, and the Linux AddressSanitizer job finds no leaks.
- **Inboxes and batches:** per-producer order holds under several producers; a push between a drain and the owner's next wait is not lost; breaking a block on a chunk border delivers both meshes in one message, also when a newer remesh of one of them lands first or the other chunk unloads.
- **Stress:** 100,000 jobs with random priorities and cancels from several threads each run once or are canceled once.
- **Determinism property:** seeded scenarios (load, edit, unload, reload), stepped to quiescence between steps with a test-only wait for idle, end with identical chunk and mesh hashes with 0, 1 and 6 workers, which shows that the final state never depends on completion order.
- **ThreadSanitizer:** a `linux-tsan` preset and CI job (Clang, `-fsanitize=thread`, `-O1`) runs every test, in a job of its own because ThreadSanitizer and AddressSanitizer cannot share a build. Control: a planted unsynchronized counter in a test-only target must fail it first. Clang supports ThreadSanitizer on arm64 macOS, but on the reference machine (Apple Clang 17.0.0, macOS 26.6.2) an empty program built with it crashes in `dyld` before `main`, as one built with AddressSanitizer hangs; both are rechecked with each toolchain update.
- **Sweep:** 1 to 8 workers at utility, at user-initiated, and switching between them, with real jobs: chunks per second, frame time at the median and 99th percentile, tick time. Plus a 10-minute flythrough: the reference machine is a fanless 13-inch MacBook Air, and in a sustained multi-core test Tom's Hardware saw this model fall from the low 10,000s to the 7,000s in Cinebench R23.
- **By eye:** Tracy shows named threads, job zones and plots; flying, the nearest chunks fill in first; breaking and placing blocks looks instant, with no holes.

## Pull request plan

1. This RFC.
2. `linux-tsan` preset and CI job, with its planted-race control.
3. `core`: thread names, QoS and `cache_line`; `JobSystem` in deterministic mode with order, cancellation and limit tests, and its benchmarks.
4. `core`: worker threads, wake-ups and shutdown; the stress test.
5. `core`: `WakeEvent`, `Inbox` and the `MeshResultPool`, with tests.
6. Tracy: thread names, job zones, tick frames, plots, the lockable scheduler lock.
7. `world`: RFC 0002's changes (`clone`, `const` methods without hidden writes, locked size classes, the `from_dense` table from scratch, compaction on the world thread).
8. `world`: chunk versions and pins on RFC 0006's chunk map, with tests.
9. `app`: the world thread and its loop with an injectable clock (actions and the targeted block, results, forwarding and edit batches, feeding, backpressure, the catch-up mode); generation and mesh jobs wired; the determinism property test.
10. The sweep and the 10-minute run on the reference machine; defaults set from them, with the numbers in the pull request.

## Open questions

1. **Render on the main thread or its own.** Recommendation: **the main thread**. With the world on its own thread, a frame is a queue drain and a few draws; a render thread adds a frame of latency and double-buffered state. Revisit if RFC 0007's frame budget is missed.
2. **Pins or copies for job inputs.** Recommendation: **pins**: 27 increments per mesh job instead of tens of KiB of copying, with a copy only when an edit lands mid-job. The price is `clone()` and world-side compaction in RFC 0002.
3. **Worker QoS: utility, switching to user-initiated to catch up.** Recommendation: **yes**. In the probe, utility kept the frame thread at full speed and on time but halved throughput on memory-heavy work; user-initiated doubled it but slowed the frame thread by a third and woke it late. Switching only while chunks near the player are missing spends frame time when the player is waiting anyway. A fixed class is simpler, and the sweep measures all three with real jobs. This is a taste call between smooth frames and fill-in speed.
4. **Edits on arrival or at the next tick.** Recommendation: **on arrival**: a placed block appears a frame or two later instead of up to 50 ms later, while ticks stay fixed for simulation. A game-feel call as much as a technical one.

## As built

Not built yet.
