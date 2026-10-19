# RFC 0005: Job system and threading model

| | |
|---|---|
| **Status** | Accepted (2026-10-02) |
| **Milestone** | M3 Infinite |
| **Issue** | #19 |

## Summary

The engine's threads, what each one owns, and the job system that spreads chunk generation and meshing across the CPU. The main thread reads input and draws. A world thread owns every chunk, is the only thread that writes one, and ticks the simulation 20 times a second. Workers run jobs on pinned, frozen versions of chunks and send results back through queues: nearest chunk first, cancelable when a chunk unloads, with no allocation in their inner loops. View distance is bounded by how fast chunks are generated and meshed; this design puts every spare cycle on the nearest work without a frame ever waiting for it.

## Motivation

| | Estimate |
|---|---|
| Chunks loaded at a 1,024-block view distance (RFC 0006) | about 17,900, of which about 9,400 are drawn and meshed |
| New chunks per 32 blocks flown (RFC 0006's region) | about 550 loaded, 385 drawn |
| Meshing one typical chunk (RFC 0003) | under 150 µs; 49 µs in a prototype |
| Loading the whole view on one core (RFC 0008's 400 µs per mixed chunk, RFC 0003's 150 µs per mesh) | about 4 to 6 s |

Neither the main thread (16.7 ms per frame at 60 Hz) nor the world thread (50 ms per tick) can absorb this, so it runs on the other cores, nearest first.

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

Two places create threads: `core`'s job system creates the workers, and `app` creates the world thread and wires the queues. `world` and `mesh` are functions and single-owner types. A dedicated server (ADR 0003) is `core`, `world` and a small `app` of its own. Later RFCs that add I/O, audio or networking threads name their owners.

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

RFC 0002 provides what pins need: `clone()` for copy on write, `const` methods that write no hidden state, compaction as a world-thread pass on unpinned versions, a lock per block-data size class (workers allocate what the world thread frees), and `from_dense` scratch from the caller. Its pull request plan builds them.

### Jobs

A job is a callable stored in a preallocated slot (1,024 slots of 128 bytes by default), with trivially copyable captures of at most 64 bytes: positions, ids, pointers. A job owns nothing, so no ownership passes through the scheduler: inputs stay pinned by the world thread, scratch belongs to the worker, and results leave through inboxes.

```cpp
namespace vx {
enum class JobLevel : std::uint8_t { urgent, normal, background };

struct JobPriority {
    JobLevel level = JobLevel::normal;
    std::uint32_t key = 0;   // lower runs first within a level; streaming uses RFC 0006's squared score
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
    // Never waits for a job and never allocates: it takes the queue lock briefly.
    // Returns an empty handle when every slot is taken.
    template <Job F> [[nodiscard]] JobHandle submit(JobPriority priority, const F& job);
    // True: the job had not started and never will. False: it is running or done.
    bool cancel(JobHandle job) noexcept;
    void run_until_idle();   // deterministic mode only (asserts zero workers): runs queued jobs here
};
} // namespace vx
```

Workers take the most urgent level first, then the lowest key, then the oldest job, so the order is total. `urgent` is remeshing after an edit, the edited chunk first; `normal` is streaming, by distance; `background` is work nobody waits for, like saving (M8). A background job that has waited a second moves up to normal, so background work cannot starve during a long flight; nothing ages into urgent, so edits always run first; the age is read from the job system's clock, which tests control. A worker runs an `urgent` job at user-initiated QoS, whatever its mode (below). A condition variable passes no QoS to the thread it wakes, so when an urgent job wakes a sleeping utility worker, the world thread raises that worker with `pthread_override_qos_class_start_np` (in the macOS SDK's `pthread/qos.h`) until the job starts; then an edit never waits behind other apps' work.

The scheduler has no dependencies: a job is submitted only once it can run. The world thread sees chunks arrive, so it decides when a chunk is ready to mesh (RFC 0006's rule: it and all 26 neighbors are ready) and remeshes it when it or a neighbor changes. Chunk pipelines are state machines on the world thread, not job graphs.

A running job is never interrupted; it lasts microseconds to milliseconds, and the world thread drops results for chunks that are gone. When a chunk unloads, its queued jobs are canceled and their inputs unpinned at once; a running job's inputs are unpinned when it reports back.

Only shutdown and tests wait for jobs, and no engine lock is held while a job runs or while another engine lock is taken: with no cycle of waits, there is no deadlock to find. Idle workers sleep on a condition variable, never spinning or yielding, and nothing uses a standalone fence, which ThreadSanitizer does not support. Shutdown runs in a fixed order: the world thread stops feeding, cancels everything queued, drains until every running job has reported back, frees its chunks and exits; then the job system, which asserts it is idle, joins its workers; the mesh result pool goes last.

### Feeding: a short queue

The scheduler holds only work that can start soon. The backlog of wanted chunks stays on the world thread, ordered by RFC 0006, and each pass of the world loop tops every level up to a limit, 256 jobs by default; three full levels plus the running jobs fit in the 1,024 slots. When the player moves, at most a few tens of milliseconds of stale work sit ahead of the new nearest chunks; an unloading chunk is usually still in the backlog, so canceling is rare; and pins and pending results scale with the queue, not the backlog. If `submit` ever returns an empty handle, the world thread releases the pins it took and leaves the chunk in its backlog. The queue must outlast the world thread's longest busy stretch, or workers run dry: 256 mesh jobs keep six workers busy for about 2.4 ms at the prototype's 49 µs per mesh plus copies (6 ms at RFC 0003's 150 µs budget), so ticks stay short (below), and the sweep tunes the limit.

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
| main | world | actions (break, place) with the view ray, the player's position, meshes uploaded so far, RFC 0007's arena-full flag, a stop request |
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
    results.drain(integrate);         // stores chunks, unpins, forwards fresh meshes; RFC 0006 integrates in steps of at most 0.5 ms
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
| Main thread time spent waiting for other threads | inbox lock hand-offs only, microseconds per frame |
| Meshing throughput, all workers against one, user-initiated | at least 4 times; also measured at utility, where the probe saw about half of that |
| Allocations | none to submit, at most one per job |
| Memory | 128 KiB of job slots, about 2.7 MiB of scratch per worker |

## Test plan

- **Order:** in deterministic mode, levels first, then keys, then submission order; on a manual clock, a background job that waits a second moves up to normal, and a normal job never becomes urgent.
- **Cancellation:** canceling a queued job returns true and it never runs; a running or finished job returns false; an empty handle, or a stale one on a reused slot, returns false and touches nothing.
- **Limits:** a full slot table returns an empty handle; a capture over 64 bytes or not trivially copyable fails to compile (checked like RFC 0000's layering control).
- **Pins:** writing a pinned chunk copies it and leaves the pinned version unchanged; a completion unpins the versions its record names even after an edit replaced them; an unloaded chunk is freed only after its last job reports back; a successful cancel unpins at once; shutting down with jobs in flight frees everything, and the Linux AddressSanitizer job finds no leaks.
- **Inboxes and batches:** per-producer order holds under several producers; a push between a drain and the owner's next wait is not lost; breaking a block on a chunk border delivers both meshes in one message, also when a newer remesh of one of them lands first or the other chunk unloads.
- **Stress:** 100,000 jobs with random priorities and cancels from several threads each run once or are canceled once.
- **Determinism property:** seeded scenarios (load, edit, unload, reload), stepped to quiescence between steps with a test-only wait for idle, end with identical chunk and mesh hashes with 0, 1 and 6 workers, which checks that, in those scenarios, the final state does not depend on completion order.
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
7. `world`: chunk versions and pins (chunks, plans, parameter snapshots) on RFC 0006's chunk map, with tests, after RFC 0002's plan items 5 to 7.
8. `app`: the world thread and its loop with an injectable clock (actions and the targeted block, results, forwarding and edit batches, feeding, backpressure, the catch-up mode); the determinism property test. RFC 0006's plan states how its items interleave with items 7 and 8.
9. The sweep and the 10-minute run on the reference machine; defaults set from them, with the numbers in the pull request.

## Open questions

All settled on 2026-10-02: every recommendation below was accepted.

1. **Render on the main thread or its own.** Recommendation: **the main thread**. With the world on its own thread, a frame is a queue drain and a few draws; a render thread adds a frame of latency and double-buffered state. Revisit if RFC 0007's frame budget is missed.
2. **Pins or copies for job inputs.** Recommendation: **pins**: 27 increments per mesh job instead of tens of KiB of copying, with a copy only when an edit lands mid-job. The price is `clone()` and world-side compaction in RFC 0002.
3. **Worker QoS: utility, switching to user-initiated to catch up.** Recommendation: **yes**. In the probe, utility kept the frame thread at full speed and on time but halved throughput on memory-heavy work; user-initiated doubled it but slowed the frame thread by a third and woke it late. Switching only while chunks near the player are missing spends frame time when the player is waiting anyway. A fixed class is simpler, and the sweep measures all three with real jobs. This is a taste call between smooth frames and fill-in speed.
4. **Edits on arrival or at the next tick.** Recommendation: **on arrival**: a placed block appears a frame or two later instead of up to 50 ms later, while ticks stay fixed for simulation. A game-feel call as much as a technical one.

## As built

Pull request plan item 2, the ThreadSanitizer job (#236), landed as one pull request: the `linux-tsan` preset and its CI job, with a planted race.

- **The planted race stays in the build:** the TSan build alone also builds a program in which two threads add to one counter, and its test passes only when ThreadSanitizer reports the race, so the job cannot stay green by no longer finding races. The preset is Clang 18 with `-fsanitize=thread` and `-O1`; the job runs every test but the Vulkan ones.
- **ThreadSanitizer cannot run on the reference machine:** Apple Clang 17's and Homebrew Clang 20's builds crash before `main`. The Linux job is the only race check, and `scripts/ci-local.sh` says so. It has not run yet: GitHub Actions was blocked by billing while M3 was built. Locally, the threaded code relies on stress tests and on plain mutex and condition variable designs.

Item 3, the thread helpers and the deterministic job system (#237), landed as three pull requests: thread names, QoS and `cache_line`; `JobSystem` in its deterministic mode; its tests and benchmark.

- **The helpers are as designed:** `cache_line` is 128 bytes, `set_thread_name` is cut to 15 characters on Linux, and `set_thread_qos` does nothing outside macOS.
- **Each level is a heap ordered by key, then by arrival, and every slot keeps its place in its heap,** so a cancel takes a job out where it is. Background jobs that have waited a second are found through a list kept oldest first, on a clock tests set. With no workers, `run_until_idle` runs jobs on the calling thread, one lock a job.
- **Submitting and running an empty job takes 56 to 60 ns** (1,000 jobs with keys in random order; release build, reference machine), against the budget of 100 ns. With other builds running, an interleaved comparison measured about 100 ns both with item 4's workers built in and without them (medians of 99 and 104 ns).
- **Every planted mutant fails a test** but a generation wrapping past zero, which takes four billion jobs on one slot. The tests cancel queued, running, finished, empty and stale handles, fill the slot table, age jobs, run 600 random keys in order with every third canceled from the middle of a heap, and check that the `Job` concept refuses callables that are large, owning, throwing, over-aligned or not copy-constructible.

Item 4, the workers (#238), landed as three pull requests: workers that sleep, wake, change class and shut down; the stress test, with the review's tests; the two benchmarks.

- **Each worker sleeps on a condition variable of its own,** and each queued job wakes one sleeper. Workers run at utility, an urgent job at user-initiated, and everything at user-initiated while streaming catches up, changing class between jobs.
- **Only a sleeping utility worker is raised for an urgent job,** through `pthread_override_qos_class_start_np` until it starts a job, and the worker ends the override itself. The review found that starting an override allocates inside libpthread: it is the one allocation a submit can make, where the RFC allows none. It also found the raise leaking when another worker took the job or a cancel removed it, and no timeout for tests that fail by hanging; such a worker now ends its raise before it sleeps again, and the core tests time out after five minutes.
- **The stress test:** four threads submit 25,000 jobs each at random levels and keys and cancel random earlier ones, while six workers run them; every job runs once or is canceled once. It passed 40 runs in a row.
- **Six workers run 2.0 to 2.25 million empty jobs a second,** submitting included (budget 1 million), and a sleeping worker starts an urgent job in 30 to 35 µs at the 99th percentile (budget 50 µs); release build, reference machine, other builds running.
- **The app sets no `LSSupportsGameMode` key:** it is a bare executable, without the bundle and `Info.plist` that would carry it.

Item 5, `WakeEvent`, `Inbox` and the `MeshResultPool` (#239), landed as three pull requests: wake events and inboxes; the pool; a test of frees during allocations.

- **The results inboxes reserve two messages per job slot, not one,** since a slot is free again before its result is drained.
- **The pool's size classes are powers of two from 4 KiB to 2 MiB,** RFC 0003's worst mesh rounded up, each with its own lock and free list. Freeing never allocates: a cached buffer's first bytes link it to the next. Under AddressSanitizer a cached buffer is poisoned until it is handed out again.
- **The review's blocker was a counter in a test,** `size_t += c ? 1 : 0`, which GCC's `-Wsign-conversion` rejects and Clang does not; the shape had come back nine times, and the local checks now search for it. The review also found no test freeing buffers while others are allocated, which four mutants survived, and no poisoning of cached buffers; both were added.

Item 6, Tracy (#240), landed after item 8, so that the world thread's ticks and plots could land whole, as three pull requests: job zones, thread names and the queue lock; the streamer's jobs; ticks, plots and mesh zones.

- **A job's zone is named by its type's `profile_name`** (Surface, Plan, Generate, Mesh), or Job without one; a name that is not a C string fails to compile. A job's chunk or column is formatted on the stack, only inside a zone. Threads are named Main, World and Worker 0 upward.
- **The scheduler's lock is a `TracyLockable`,** its condition variables then `condition_variable_any`. The review found it costs about 29 MB a second of capture at 120,000 jobs a second; whether it stays is left to item 9.
- **Ticks are a discontinuous frame set of their own,** "Tick", each with a Tick zone. Each plot is taken where a pass already reads its number: the three inboxes' depths before each drain, pins, copies on write as a running total, meshes not uploaded, and the urgent and normal levels' queued jobs, with the background level once a tick. The review found the first version plotting everything at the tick, after a pass had emptied the inboxes.
- **Checked headless** with Tracy 0.13.1's `tracy-capture` and `tracy-csvexport` on the threaded world loop test, in the profiling build with two workers: 185 Surface, 228 Plan, 169 Generate and 56 Mesh zones, each with its position, 10 Tick zones and 250 samples of each plot.

Item 7, chunk versions and pins (#243), landed as three pull requests: `Versions<T>`; job records; the property test.

- **`Versions<T>` holds blocks and plans alike:** it makes a version current, pins and unpins it, copies it on a write while it is pinned (`clone()` for chunks), and frees it at once when it is neither current nor pinned, so block data returns to RFC 0002's pool when the last job reading it reports. Versions live at stable addresses, in blocks of 256 reused through a free list.
- **Job records come from a pool sized to the job slots** and name what each job pinned: a mesh job's 27 chunk versions, or a uniform neighbor's state, and a build job's 27 plans, with the chunk's load id and the job's sequence. A completion, a successful cancel or a failed submit releases exactly those. RFC 0008's parameter snapshot joins the record when it exists.
- **The property test runs 4,000 random steps over six chunks** (loads, edits, unloads, mesh jobs, runs, cancels and completions) against a model: after each step every version lives exactly while it is current or pinned, and every job read the blocks it was handed. All 15 planted mutants fail it or another test.

Item 8, the world thread and its loop (#245), landed as six pull requests: collision against streamed chunks; what the loop needs of the streamer; the loop; its tests; edits and their batches; backpressure, ticks, catching up and `WorldThread`.

- **The loop is built headless in `app`'s `vx_app_world` library** (decided 2026-10-09): `WorldLoop`, and `WorldThread` to run it at user-interactive QoS. The app first ran it with RFC 0006's item 7, when the player moved onto its ticks; to RFC 0010's moves a chunk not ready is one solid cube. The meshing scratch, RFC 0003's padded chunk and the mesher's, is about 1.7 MiB a worker and belongs to the loop; the streamer keeps the generation scratch.
- **Backpressure counts the meshes sent alone that the main thread is done with,** uploaded or dropped, clamped at those sent; streaming mesh jobs wait while more than 512 are outstanding, and an edit's never do. The review found that counting a batch's meshes could wrap the count to 1.8e19, and that never counting dropped meshes could hold streaming back for good.
- **An edit's batch takes in any pending batch one of its chunks is in,** and goes once every member has a mesh started after the edit; a member that unloads leaves it. So a merged batch can exceed RFC 0007's 8 meshes, and an edit on every pass would hold a batch back: fine at a player's rate, to revisit once ticks edit.
- **Ticks come 20 a second on an injectable clock;** ticks that fall due run back to back, those more than a second behind are skipped, and none runs after a stop. Each tick checks the near cylinder, and while a chunk of it has no mesh sent, the workers run at user-initiated.
- **With 0, 1 and 6 workers, loading, leaving, coming back and editing end with the same chunks and meshes,** each mesh equal to what RFC 0003's mesher makes from the loop's blocks: the review had found eight wrong-mesh mutants passing tests that compared no mesh contents. It also found `settle()` hanging after a stop, and shutdown running 185 queued generator calls; shutdown now cancels what is queued first.
- **The region at view distance 32 settled from nothing in 0.26 s;** an idle pass took about 0 µs, and a pass walking 8,900 held meshes 7 µs (release build, reference machine, six workers).
- **The messages follow the table above where their work has landed:** an edit carries a block and its state until M5 brings the view ray and the targeted block. RFC 0007's arena signal reached the world thread with RFC 0006's item 8 (#256): the frames counted full since the last report and whether the arena is full now. The player's keys and landings, flythrough samples, the merge switch and the view distance joined with later items.

Item 9, the worker sweep and the 10-minute run (#258): the tools landed as four pull requests: workers at one QoS class (#356); `--workers`, `--worker-qos` and the ten-minute flythrough (#357); `scripts/worker-sweep.sh` (#358), its runs interleaved (#382). The sweep itself was stopped on 2026-10-19 after its first three runs: the owner chose features over tuning defaults that already work.

- **The defaults stay the design's:** the cores less two (6 on the reference machine) at `switching`. The three runs that finished, one worker at each mode (release build, view distance 32, on battery with the owner using the machine), drew frames at 3.5 to 3.8 ms at the median in every phase and loaded in 1.1 to 1.3 s, so nothing waits on the tuning. Their 99th percentiles, 4 to 23 ms, moved with what else the machine was doing.
- **Not measured with it:** meshing throughput with all workers against one (the budget asks at least 4 times), and the 10-minute run. `scripts/worker-sweep.sh` and `--flythrough=long` stay for when a change needs the curve or the heat.
- **The scheduler's lock stays a `TracyLockable`, in the profiling build only** (item 6's open point): release and debug builds take a plain `std::mutex` and `std::condition_variable`, so the capture cost the review found, about 29 MB a second at 120,000 jobs a second, is paid only where the lock's waits are wanted in the timeline.

GitHub Actions was blocked by billing while M3 was built, so these pull requests merged on local runs of CI's jobs; GCC, AddressSanitizer, ThreadSanitizer and lavapipe have not built them yet.
