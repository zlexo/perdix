# RFC 0006: World streaming

| | |
|---|---|
| **Status** | Accepted (2026-10-02) |
| **Milestone** | M3 Infinite |
| **Issue** | #22 |

## Summary

Which chunks are in memory around the camera, how they get there and how they leave. With cubic 32 by 32 by 32 chunks and no fixed world height (RFC 0002), the loaded region is a disc of terrain out to the view distance plus a full-height cylinder near the camera. This RFC defines that shape, the world's chunk map, each chunk's lifecycle, the order of work, the budgets that keep frame time flat, hysteresis, memory limits, a placeholder generator until RFC 0008, and the benchmark flythrough whose numbers every performance pull request quotes.

## Motivation

The projection has no far plane (RFC 0001), so the view distance is whatever streaming keeps loaded. At 1,024 blocks (32 chunks), 3,209 chunk columns lie within the view distance. A cylinder 512 blocks tall over them is about 51,000 chunks (RFC 0002's estimate), a sphere is 137,065, and every chunk border the camera crosses brings 65 new columns.

Every chunk that enters must be generated, then meshed once its 26 neighbors exist (RFC 0003), on worker threads and in an order that fills the screen first. On one thread, one border crossing would cost about 8 ms just to mesh and compress its 130 surface chunks (49 µs and 14 µs each, as a spike on the reference machine measured), a whole frame at 120 frames per second. Cubic chunks add two problems: there is no column to load from top to bottom, and a region centered on the camera loses the ground as soon as the camera flies higher than the region reaches. Performance work also needs one repeatable measurement. The vision sets the headline target once there is a baseline; the benchmark flythrough is that baseline.

## Goals and non-goals

Goals:

- Terrain to the full view distance at any camera height, and everything near the camera: caves, sky, builds.
- An explicit lifecycle per chunk, every transition unit tested without a GPU. Meshing waits for all 26 neighbors, so a chunk is meshed once on arrival.
- The chunks around the player first, then what is in view, nearest first. Chunks the generator knows in advance to be uniform (open sky, deep rock) cost no job.
- Flat frame time: streaming runs on RFC 0005's world thread, and the main thread only hands finished meshes to RFC 0007 within its upload budget.
- Memory under a budget, with under 128 bytes of bookkeeping per loaded chunk. Under pressure the view distance shrinks instead of holes appearing.
- A deterministic benchmark flythrough, pasted into performance pull requests and shared as benchmark results.

Non-goals:

- Threads, jobs, pinned chunk versions and the queues between threads: RFC 0005. Streaming decides what to submit, in what order, and what to cancel.
- GPU memory, uploads (including the transfer queue RFC 0001 deferred to M3), multi-draw and culling: RFC 0007.
- Terrain shapes and the generation passes: RFC 0008. Until then, a placeholder generator.
- Saving to disk (M8) and level of detail (M9). The lifecycle keeps a place for saving.
- How fast an edit must appear: RFC 0005 (an edit's remeshes back on the main thread within 1 ms).

## Prior art

- **Minecraft** (Java Edition): chunk tickets set load levels. A player's ticket is level 31, the level rises by one per chunk of distance up to 44, and chunks at 34 and above are not accessible, only generated, so the usable area sits inside rings of partly generated chunks. Generation runs in steps (structure starts through features to full), and features may place blocks in the 3 by 3 chunks around them. Since 1.18, render distance loads a cylinder instead of a square (21w37a), and simulation distance is a separate setting (21w38a). We take the ring of generated but unusable chunks, the circle, and a near region distinct from the visible one.
- **OpenCubicChunks** (CubicChunks for Minecraft 1.12, 16-block cubes): the server loads a box of cubes, the horizontal view distance by a separate vertical distance (`verticalCubeLoadDistance`, 8 cubes by default), and diffs the old and new box on every move. We take a vertical policy separate from the horizontal one.
- **Cubyz**: 32-block cubic chunks and a spherical render distance per level of detail, each level doubling the voxel size; its renderer stores meshes in fixed 64 by 64 by 64 ring arrays indexed by coordinates modulo 64. The sphere fits once there is level of detail. That is M9's, where Distant Horizons, a level of detail system that lets Minecraft render far past its normal distance, is the reference point.
- **Luanti** (formerly Minetest, 16-block map blocks): the server sends blocks in growing shells, skips blocks outside the player's view cone, and beyond 4 map blocks skips blocks that are only air. The client caps blocks in memory (`client_mapblock_limit`, 9,300 by default) but keeps a minimum that depends on the view range, and drops blocks unused for 600 s. We take in-view first, near-free uniform chunks, and never evicting what is in range.
- **Sodium**: scores each pending chunk rebuild by distance, a bonus inside the view frustum and how long it has waited; budgets each frame's builds by estimated worker time and upload size; and marks rebuilds the player causes nearby as important, which by default go to the front of the queue instead of making the frame wait. Before a build, the main thread clones the palette-compressed blocks of the section and its neighbors for the worker. We take the scoring, the per-frame budget and edits first; RFC 0005's pinned versions replace the clone.

## Design

Streaming lives on RFC 0005's world thread, which owns every chunk and the chunk map. `world` holds the region, the map and each chunk's data lifecycle as single-owner types, so a dedicated server gets all of it. The mesh half of the lifecycle is a small table beside it in `app`'s world loop. The main thread receives finished meshes and unloads in order and hands them to RFC 0007.

### The loaded region

Distances are whole chunks from the camera's chunk, so the region changes only when the camera crosses a chunk border.

- **Terrain disc.** Every column within the view distance R (a circle) holds its surface band: the chunks where anything solid can meet the sky, trees included, from the generator's `surface_range`. The band reaches down to the lowest surface of the column's four side neighbors, so cliffs along column borders are drawn, and stretches to include any chunk a player edited (M5), so builds stay visible from afar.
- **Near cylinder.** Within 8 chunks (256 blocks) of the camera horizontally, every chunk from 8 below the camera's chunk to 8 above: caves, overhangs, builds, and the sky around a flying camera.
- **Margin.** Meshing reads all 26 neighbors (RFC 0003), so the loaded region is the drawn region grown by one chunk in every direction; RFC 0008's generator also plans one ring beyond that. Margin chunks are generated and never drawn.

At 1,024 blocks, assuming the surface spans 2 chunks per column (M4 replaces this with a measurement):

| Shape | Drawn | Loaded |
|---|---|---|
| Sphere | 137,065 | 157,107 |
| Box, 65 by 65 columns, 17 chunks tall | 71,825 | 85,291 |
| Cylinder, 17 chunks tall | 54,553 | 65,987 |
| **Terrain disc and near cylinder** | **about 9,400** | **about 17,900** |
| The same at 2,048 blocks | about 28,700 | about 57,500 |

The disc reaches 2,048 blocks with fewer chunks than a cylinder loads at 1,024. Above the band, the margins and the near cylinder are sky. Below it they are rock, uniform only where no cave or ore reaches. At RFC 0002's 4 bits per block (16 KiB) for mixed chunks, block data is about 110 MiB at 1,024 blocks and 420 MiB at 2,048 on the placeholder terrain, which has no caves. RFC 0008 puts caves and ores 256 blocks deep, which makes the margins below the band mixed: about 190 MiB at 1,024 blocks and 650 MiB at 2,048. Plans add 3 to 4 KiB each, and the plans below the band stay as long as their columns are loaded, because those chunks are never built: with them and the bookkeeping, chunk data is about 240 MiB at 1,024 blocks and 800 MiB at 2,048. Because the disc follows the terrain and not the camera, the ground stays loaded at any height. A generator that cannot bound a column tightly returns a wider range, at the cost of more chunks. Ground under an overhang wider than a column never meets the sky, so beyond the near cylinder it is not loaded: seen from the side at a distance, the space under such an overhang can show sky. RFC 0008's `surface_range` can widen the band under overhangs if that shows.

**Hysteresis.** A chunk loads when it enters the region and unloads only when it leaves the region recomputed with every distance 2 chunks larger. The camera's chunk changes by one at each border, so moving back and forth across a chunk border loads and unloads nothing after the first crossing. Moving two chunks in any direction, as little as 47 blocks diagonally, does unload the far edge. The cost is about 12% more loaded columns at 1,024 blocks (432 on top of 3,473). Meshes follow the same rule one chunk further in, so a drawn chunk never loses a neighbor.

### The chunk map

The map is an index from `ChunkPos` to a record. Slots are 16 bytes: the 12-byte position and a 32-bit record index. Records live in a pool with a free list, at stable indices:

```cpp
struct ChunkRecord {                  // 48 bytes, one per loaded chunk
    ChunkPos pos;
    ChunkState state;                 // see the lifecycle
    std::uint8_t planned_neighbors;   // 0 to 26
    std::uint8_t ready_neighbors;     // 0 to 26
    std::uint8_t flags;               // edited, in the drawn region
    BlockState uniform;               // the whole chunk, when uniform
    std::uint32_t version;            // bumped by every edit, from one world-wide counter
    std::uint32_t queue_index;        // position in its backlog, or none
    std::uint32_t plan;               // index of its RFC 0008 plan, or none
    std::uint32_t load_id;            // never reused: tells a reloaded chunk's results from the old one's (RFC 0005)
    ChunkStorage* blocks;             // current version, pinned for jobs by RFC 0005; null when uniform
};
```

- Open addressing with linear probing over a power-of-two array, doubled when more than half full. At half full a hit averages 1.5 probes and a miss 2.5 (Knuth); eight slots share a 128-byte cache line on the reference machine. Erasing shifts the rest of the probe run back instead of leaving tombstones, so hours of streaming do not slow lookups.
- At 17,900 chunks the map is 65,536 slots, 1 MiB. Bookkeeping per chunk is the record, 32 to 64 bytes of map and 16 bytes of mesh state: about 2 MiB in all.

Chunk coordinates use 27 bits each (block >> 5), 81 bits in all, so they do not pack losslessly into 64 bits the way Minecraft's protocol packs a block position (26, 26 and 12 bits, in a bounded world). The hash packs the low 21 bits of each and mixes them with MurmurHash3's 64-bit finalizer (Austin Appleby, public domain). Both multipliers are odd and each xor-shift can be undone, so the finalizer is a bijection: chunks less than 2^21 chunks (67 million blocks) apart on every axis never share a 64-bit hash, and equality still compares all 12 bytes.

```cpp
constexpr std::uint64_t low21(std::int32_t v) noexcept {
    return static_cast<std::uint32_t>(v) & 0x1F'FFFFu;
}

constexpr std::uint64_t hash(ChunkPos p) noexcept {
    std::uint64_t k = low21(p.x) | low21(p.y) << 21 | low21(p.z) << 42;
    k = (k ^ (k >> 33)) * 0xFF51'AFD7'ED55'8CCDull;
    k = (k ^ (k >> 33)) * 0xC4CE'B9FE'1A85'EC53ull;
    return k ^ (k >> 33);   // slot = hash & (capacity - 1)
}
```

### Chunk lifecycle

The world half, one state per record:

| State | Meaning | Leaves when |
|---|---|---|
| `queued` | In the region, waiting for a plan job | dispatched: `planning` |
| `planning` | A job makes its plan (RFC 0008), which needs nothing else | done: `planned` |
| `planned` | Waiting for the plans of its 26 neighbors | all 26 planned: `build_queued` |
| `build_queued` | All 27 plans exist; waiting in the build backlog | dispatched: `building` |
| `building` | A job builds its blocks from the 27 plans | done: `ready` |
| `ready` | Blocks in memory, waiting for its neighbors' blocks | its 26th neighbor is ready: `complete` |
| `complete` | It and all 26 neighbors are ready: it can be meshed | a neighbor unloads: `ready` |
| `unloading` | Left the region; waits for its job (from M8, its save) | done: freed; back in range: previous state |

- A chunk that leaves the region is freed at once and its queued jobs are canceled (RFC 0005), except while a job runs and, from M8, when edited: those pass through `unloading`. Chunks in the outer plan ring stay `planned`.
- A column's chunks are requested once the surface ranges of the column and its eight neighbors are known (small column jobs, first in line). A chunk out of the generator's reach (RFC 0008's `out_of_reach`) or above its column's surface range is uniform: it goes straight to `ready`, with no job and no block data. It counts as planned and ready for its neighbors, builds read a null plan for it (RFC 0008), and mesh jobs take its state from their job record instead of pinning blocks it does not have (RFC 0005).
- Each record counts its planned and its ready neighbors; a chunk changing state updates its 26 neighbors (26 lookups). A plan is dropped once every chunk that reads it is built, and made again if a new neighbor needs it. `planned_neighbors` counts the plans that exist right now, so dropping a plan lowers its neighbors' counts and remaking it raises them; a chunk is build-queued only while all 27 plans exist.
- A plan that a running build reads is pinned like a chunk version (RFC 0005), and so is the generator's parameter snapshot (RFC 0008). Freeing a chunk that leaves the region, dropping a plan and publishing new parameters all wait for the last job that reads them, so a neighbor leaving mid-build cannot free a plan in use.
- When RFC 0008's parameters change, the generator's version bumps: unedited chunks regenerate from the earliest affected pass, nearest first, keeping their old blocks and meshes on screen until replaced, and results of older versions are dropped. Light (M6) will add a stage under the same neighbor rule.

The world reports events to the mesh half: `shown` (complete and in the drawn region), `changed`, `hidden` (left the drawn region) and `freed`. The mesh half keeps a 16-byte record per chunk in the drawn region:

| State | Meaning | Leaves when |
|---|---|---|
| `waiting` | In the drawn region; the chunk is not complete yet | shown: `queued` |
| `queued` | Needs a mesh: the first one, or after a change here or in a neighbor | dispatched: `meshing` |
| `meshing` | A job meshes pinned versions of the chunk and its 26 neighbors (RFC 0005) | result: `sent`, or `drawn` if empty |
| `sent` | Forwarded to the main thread, where RFC 0007 uploads it within its frame budget | uploaded: `drawn` |
| `drawn` | On screen; RFC 0007 owns it from here | changed: `queued`, still drawn; hidden: removed |

A `changed` event while `meshing` or `sent` marks the record to queue again once the current mesh is drawn. `hidden` or `freed` removes the record in any state, and a mesh result that arrives for a removed record is dropped; RFC 0005's load id tells it from a newer chunk at the same position.

A complete chunk that is uniform air, or uniform opaque with six uniform opaque face neighbors, has no faces and skips meshing. A drawn mesh stays on screen until its replacement is drawable, so changes never flicker or leave holes.

Edits (M5) arrive on the world thread (RFC 0005) and apply to `ready` or `complete` chunks only. `set_block` bumps the version, marks the chunk edited and reports as changed the chunk plus every neighbor whose padded border holds the block: up to 7 neighbors for a corner block, 8 chunks in all. Their remeshes run at RFC 0005's `urgent` level. A mesh older than the chunk's version is still forwarded when it is newer than the last one forwarded, and the chunk stays queued, so steady edits cannot starve a chunk. Edited chunks are never unloaded until saving exists (M8); from then on, unloading saves them first.

### Priorities

Columns, plans, builds and meshes each keep a backlog on the world thread, ordered by a score: the distance from the camera in chunks, doubled when the chunk is outside the view frustum. RFC 0005 keeps its job queue short and is topped up from the backlogs, lowest score first, with the squared score as the job key.

- The 27 chunks around the camera score 0; uniform chunks never queue. Generation tests the chunk's box grown by one chunk against the frustum, so the neighbors a visible chunk needs rank as visible.
- Doubling means what is just behind the camera arrives with what is in view at twice the distance: turning around shows nearby terrain, while distant terrain behind waits.
- Streaming reports `catching_up` while any chunk of the drawn region within the near cylinder is neither drawn nor known empty: the holes the flythrough counts within 256 blocks. RFC 0005 runs its workers at user-initiated priority while it is set.
- Scores are recomputed when the camera crosses a chunk border or turns more than 15 degrees, and the backlogs are rebuilt as heaps in linear time: up to the 17,900 chunks of a region after a teleport, a few hundred while flying.

### Per-frame budgets

Streaming's bookkeeping runs on the world thread, so it never lands in a frame. The main thread's share is the ordered stream of meshes and unloads: per frame it hands RFC 0007 as many meshes as fit its 1 ms upload budget, the rest wait, in order, on the main thread for the next frame, and while too many wait, the world thread holds back new mesh jobs (RFC 0005). On the world thread, each integration is a few lookups and pointer moves, hundreds per millisecond. Streaming integrates for at most 0.5 ms at a time before feeding the workers again, so a burst such as a teleport neither starves the workers nor delays a tick by more than that.

### Memory limits and eviction

Chunk data (block data, plans and bookkeeping) has a budget, 512 MiB by default; RFC 0007 budgets GPU memory. Over budget, the streamer first unloads chunks kept only by hysteresis, farthest first. If that is not enough, it lowers the effective view distance one chunk at a time and unloads what falls outside, and the overlay says so ("view distance 29 of 32, memory"). When RFC 0007 reports that its mesh arena cannot grow, streaming answers the same way. It raises the distance again one chunk at a time once usage stays under 90% of both budgets for 5 seconds. Chunks inside the region are never evicted, because they would be requested again at once, and edited chunks are never evicted (M8). By the estimate above, 512 MiB holds the default 1,024-block view on any terrain, and the 2,048-block maximum only without caves; with RFC 0008's caves and ores it holds about 1,800 blocks, and the effective view distance says so.

### The placeholder generator

Until RFC 0008, a generator with the same interface: rolling hills from two octaves of value noise (heights 16 to 112), `core:grass` over `core:dirt` over `core:stone` (RFC 0004), and a 16 by 16 stone spire up to y = 640 every 512 blocks, so tall columns and the climb below are exercised. Its plans are empty, its builds ignore their neighbors, and its surface ranges and out-of-reach answers are exact.

### The benchmark flythrough

`app --flythrough` runs the real engine over a fixed world and camera path, then prints one Markdown block for pull requests and benchmark reports.

- **Fixed inputs:** seed `0x5EED`, the placeholder generator (RFC 0008's terrain once it lands), view distance 32, 2560 by 1440, immediate present when offered and FIFO otherwise (printed). A flythrough version bumps whenever an input or the path changes; results compare only within one version. Within each phase the camera's position is a function of time, so every machine faces the same streaming demand per second.

| Phase | Camera | Shows |
|---|---|---|
| `load` | Still at (0, 160, 0) until the region is complete (30 s cap) | cold throughput, time to a full view |
| `hover` | Still, 10 s | the baseline frame time |
| `cruise` | 30 s east at 64 blocks/s | steady streaming |
| `turns` | 15 s of S-curves at 64 blocks/s, the view swinging 90 degrees each way | in-view priority |
| `sprint` | 15 s east at 512 blocks/s | throughput under pressure |
| `climb` | 15 s up to y = 2,000, looking 45 degrees down | the ground stays loaded |
| `teleport` | 1,000,000 blocks east, until complete | unloading; memory back to its `load` level |

- **Per phase:** frame time p50, p99 and max; the world thread's longest streaming step; chunks generated, classified uniform and meshed per second; holes at p99, within 256 blocks and in total (chunks of the drawn region inside the frustum that are neither drawn nor known empty, counted every 30th frame, and those frames are left out of the frame time statistics); chunk data, GPU mesh memory and the process footprint (`phys_footprint` on macOS, resident set on Linux), at the end and at peak. The block opens with the flythrough and engine versions, the commit, the build type, the machine, the driver and the settings, then has one table row per phase.

Performance pull requests paste the block from before and after their change: three runs on the reference machine, plugged in, medians quoted. CI runs a 5-second version headless on lavapipe to keep it working and never asserts a time. A "Benchmark result" issue template collects the same block from other people's machines. The flythrough also gives RFC 0001 the numbers it asked for to revisit two frames in flight.

### API sketch

```cpp
namespace vx {

struct ColumnPos { std::int32_t x, z; };
struct ChunkRange { std::int32_t min_y, max_y; };   // inclusive

// What streaming needs from a generator: RFC 0008's two passes and out-of-reach test,
// plus surface_range. Pure functions of the seed, the parameters and a position.
class Generator {
public:
    virtual ~Generator() = default;
    // Chunks of a column where anything solid can meet the sky, trees included.
    // May be wider than the truth, never narrower. A job.
    [[nodiscard]] virtual ChunkRange surface_range(ColumnPos column, GenScratch& scratch) const = 0;
    // The state filling a chunk no terrain can reach. World thread, about 1 µs.
    [[nodiscard]] virtual std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept = 0;
    [[nodiscard]] virtual ChunkPlan plan(ChunkPos pos, GenScratch& scratch) const = 0;   // a job
    // A job: the 27 plans around and including the chunk, null where out of reach.
    [[nodiscard]] virtual ChunkStorage build(std::span<const ChunkPlan* const, 27> plans,
                                             GenScratch& scratch) const = 0;
};

struct Viewer { double x, y, z; Frustum frustum; };   // 64-bit position (RFC 0001)

struct ChunkEvent {
    enum class Kind : std::uint8_t { shown, changed, hidden, freed } kind;
    ChunkPos pos;
    std::uint32_t record;   // the mesh half keeps its records at the same index
};

class WorldStreamer {   // world thread only (RFC 0005)
public:
    // Settings: view distance 32, near distance and height 8, hysteresis 2 (chunks),
    // memory budget 512 MiB.
    WorldStreamer(const Generator& generator, JobSystem& jobs, StreamingSettings settings);
    void set_viewers(std::span<const Viewer> viewers);   // the camera; a server: one per player
    void integrate(StreamResult&& result);               // a finished job, from RFC 0005's inbox
    void feed();                                         // top up RFC 0005's queue from the backlogs
    [[nodiscard]] std::span<const ChunkEvent> events() const noexcept;   // since the last call
    bool set_block(BlockPos pos, BlockState state);      // M5; false unless the chunk is ready
    [[nodiscard]] StreamingStats stats() const noexcept; // per state, memory, effective view distance, catching_up
};

} // namespace vx
```

`ChunkPlan` and `GenScratch` are RFC 0008's, `JobSystem` is RFC 0005's. The mesh half reads the events, submits mesh jobs and forwards their results to the main thread.

## Alternatives considered

- **A sphere or a box** (Cubyz, OpenCubicChunks): 137,065 and 71,825 drawn chunks at 1,024 blocks, against 54,553 for a cylinder. The sphere reaches 1,024 blocks into sky and rock right next to the camera, which pays off only with level of detail (M9); the box adds corners that round fog hides.
- **A ring array instead of a hash map** (Cubyz): no hashing, but it is a window centered on the camera, so it cannot hold a disc at another height, a second viewer or an edited chunk far away, and its size is the window's volume rather than the chunks loaded. **`std::unordered_map`** costs a heap node per entry and a pointer chase per lookup.
- **The classic spatial hash** (Teschner et al., 2003: x times 73856093 xor y times 19349663 xor z times 83492791, meant for prime table sizes): with a power-of-two table the slot depends only on the low bits of each coordinate. The finalizer costs about the same and mixes every bit.
- **Meshing with missing neighbors as air** (RFC 0003 allows it): terrain appears a little sooner, but a chunk can be meshed up to 27 times as its neighbors arrive, and border faces flash before the neighbor hides them.
- **Strictly in view first** (Luanti skips blocks outside the view cone): the fastest fill ahead, and empty terrain whenever the player turns around.
- **Time-based unloading** (Luanti): smooth, but memory depends on where the player has been; distance hysteresis is bounded and deterministic.
- **Dropping mesh results that are already stale:** never forwards an outdated mesh, but steady edits can keep a chunk from ever finishing.

## Performance budget

On the reference machine, release build, placeholder terrain, measured by the flythrough unless noted:

| What | Target |
|---|---|
| Chunk map lookup, hit (nanobench) | under 20 ns |
| Bookkeeping per loaded chunk | under 128 bytes |
| Frame time p99, `cruise` against `hover` | within 1 ms |
| Longest streaming step on the world thread | under 1 ms |
| A full 1,024-block region from nothing (`load`), placeholder terrain | under 1 s |
| Generation and meshing throughput in `load`, placeholder terrain | 20,000 chunks per second each; re-based on RFC 0008's per-chunk budgets when its terrain replaces the placeholder (at its targets, six workers build at most about 15,000 mixed chunks per second) |
| Holes within 256 blocks during `sprint` | none at p99 |
| Chunk data at 1,024 blocks | under 128 MiB on the placeholder terrain; under 256 MiB on RFC 0008's (about 240 MiB expected) |

## Test plan

- **Map and hash:** seeded random inserts, erases and lookups agree with `std::unordered_map`, with negative coordinates and keys at the edges of the range; probe runs stay short after heavy churn; the table doubles at half full. The 2,097,152 positions of a 128 by 128 by 128 block of chunks share no 64-bit hash, at the origin and at both edges of the range.
- **Region:** counts match the formulas (3,209 drawn and 3,473 loaded columns at 32 chunks); for random cameras and surface ranges, every drawn chunk has its 26 neighbors loaded and every loaded chunk its 26 neighbors planned; crossing a border back and forth loads and unloads nothing after the first time; climbing keeps the disc; nothing outside the 32-bit world is requested.
- **Lifecycle,** in RFC 0005's deterministic mode: each transition in both tables; a chunk becomes build-queued exactly when its 26th neighbor's plan lands and is complete exactly at its 26th ready neighbor, and back to ready when one unloads; leaving while a job runs frees the record when the job ends, and coming back keeps its result; uniform chunks never create jobs; a corner edit changes 8 chunks and an inner one 1; a stale mesh newer than the last forwarded one is forwarded and the chunk stays queued; edited chunks survive leaving the region; a generator version change regenerates unedited chunks nearest first and drops older results.
- **Budgets and memory,** with a fake clock: a burst of 10,000 results is integrated in steps of at most 0.5 ms plus one result. With a small memory budget, eviction takes hysteresis chunks farthest first, then lowers the view distance, never touches the region or edited chunks, and raises it again later.
- **Placeholder:** chunks generated on different threads in different orders are byte-identical; `out_of_reach` and the chunks above a column's surface range agree with a full build; no surface lies outside `surface_range`.
- **CI:** the 5-second flythrough on lavapipe checks invariants every frame: no chunk drawn without its 26 neighbors, state counts that add up, and chunk data after `teleport` within 5% of its level after `load`.
- **By eye:** the flythrough with the overlay (counts per state, backlog lengths, memory, effective view distance): no holes near the camera, and the ground stays during the climb.

## Pull request plan

1. This RFC.
2. `ChunkMap`: the hash, open addressing with backward-shift deletion, tests and nanobench benchmarks.
3. Region shapes as pure functions: disc, near cylinder, margin, plan ring, hysteresis, with property tests.
4. The `Generator` interface and the placeholder, with determinism, `out_of_reach` and `surface_range` tests.
5. `WorldStreamer`'s data half: records, columns, plans, builds, neighbor counts, events and `set_block`, on RFC 0005's world thread and pins.
6. Backlogs and priorities feeding RFC 0005's queue; integration in steps; regeneration when the generator's version changes.
7. The mesh half: mesh records, events and the empty fast path, on RFC 0005's forwarding to the main thread and RFC 0007. The world streams in as you fly.
8. The memory budget, eviction and the effective view distance, with RFC 0007's arena signal; overlay counters.
9. The benchmark flythrough with its CI smoke run, the "Benchmark result" issue template and a page on running it.

This plan interleaves with RFC 0005's: item 2 here, RFC 0005's item 7 (versions and pins), items 3 to 5 here, RFC 0005's item 8 (the world thread and its loop: forwarding, batches, backpressure and catch-up), then items 6 to 9 here.

## Open questions

All settled on 2026-10-02: every recommendation below was accepted.

1. **Default view distance** (the owner's taste call). Recommendation: **32 chunks (1,024 blocks)**, with a slider from 4 to 64. About 17,900 loaded chunks and 110 to 240 MiB of chunk data by the estimate above (without and with caves); confirmed or lowered with the first flythrough numbers before M3 ends.
2. **Vertical policy: a terrain disc plus a near cylinder, or a plain cylinder around the camera.** Recommendation: **the disc**. It keeps the ground at any height and loads about a quarter of the cylinder's chunks at the same distance (17,900 against 65,987). The costs: RFC 0008 must add `surface_range` (its per-column first pass already finds each column's highest surface), and caves more than 256 blocks away are not loaded. It also assumes RFC 0002's open question 2 is settled as unlimited height; with a fixed height, columns load whole and this question goes away.
3. **Memory budget: a fixed 512 MiB, or a share of physical memory.** Recommendation: **fixed**, with a setting. Benchmark results compare across machines only if the budget is the same. By the estimate, 512 MiB holds the default view on any terrain; the 2,048-block maximum fits only without caves.
4. **Our own chunk map, or `ankerl::unordered_dense`.** Recommendation: **our own**. It is small, keeps slots at 16 bytes and is the part worth understanding. unordered_dense (by nanobench's author) is excellent and would save that code; its erase takes a second lookup to move the last entry into the hole, and it adds a dependency for one narrow use.

## As built

Pull request plan item 2, the chunk map (#235), landed as two pull requests: the map; its hash tests and benchmark.

- **The map is as designed:** 16-byte slots in a power-of-two array, doubled before it is more than half full, linear probing, erasing by shifting the probe run back, the RFC's hash, and equality over all 12 bytes. The review found that an insert refused for a mapped position could still double the table, and that `reserve` could loop forever; a refused insert now changes nothing, and `reserve` stops doubling before it overflows.
- **A hit takes about 9.6 ns when each find waits for the one before** (budget 20 ns), and 1.7 ns when finds overlap; a miss 1.3 ns, overlapping; an erase and an insert together 5.8 ns. A region of 17,045 chunks in 65,536 slots; release build, reference machine.
- **Tested** against `std::unordered_map` over 200,000 seeded operations; the 2,097,152 positions of a 128-chunk cube share no hash, at the origin and at both edges of the range; and a region flown 2,000 chunks east, 670,000 erases and as many inserts, keeps its probe runs as short as a map built fresh.

Item 3, the region's shapes (#241), landed as two pull requests: the region as pure functions; its property tests.

- **A column's chunks come as sorted, disjoint runs, at most 26:** one for each of the 25 bands a planned column reads, and one for the near cylinder. The review found the first bound, 18, overflowing for bands unlike terrain. Distances are 64-bit circle tests, and every range is clamped to the 32-bit world.
- **The counts match the RFC:** 3,209 drawn and 3,473 loaded columns at 32 chunks, 432 more kept by the hysteresis, 3,745 planned.
- **`runs()` is deleted on a temporary.** A range-for over a temporary region's runs read a span into a destroyed object: every debug run passed, and two region tests failed in release, which the review found. Each stack's tests have also run in a release build since. Ten mutants the review found surviving, such as the world's edge column refused or a plan ring without diagonals, now fail a test.

Item 4, the `Generator` interface and the placeholder (#242), landed as two pull requests: the interface and the hills; their tests.

- **`ChunkPlan` and `GenScratch` are as small as the placeholder needs** until RFC 0008; `Generator::version()` joined with item 6. The placeholder's noise is integer arithmetic, so a seed makes the same world on every compiler and machine. Its hills are 16 to 111 blocks high, grass over three blocks of dirt over stone.
- **Where the noise's cells meet, the ground levels off:** it moves at 25 of 876 cell edges and 383 cell middles, and no slope is steeper than three blocks. Chunks built on four threads in different orders are byte for byte the same, and `out_of_reach` and the surface ranges agree with full builds, spires included.
- **A mixed chunk builds in 44 to 48 µs** (release build, reference machine, other builds running). Six mutants the review found surviving, a transposed build and noise that ignores z among them, now fail a test.

Item 5, the streamer's data half (#244), landed as eight pull requests: chunk records; the columns and their surface jobs; records for the region; plans, uniform chunks and chunks that leave mid-job; builds; tests of builds that leave or outlive the streamer; events and `set_block`; a random camera walk with edits.

- **Only the drawn flag comes from the region,** kept while the chunk stays inside the region grown by the hysteresis. A chunk is loaded while a drawn chunk lies within one of it, and has a record while a loaded chunk does, counted on each record (`drawn_near`, `loaded_near`). The first design marked each column's loaded and planned chunks from its own runs; the random walk broke it, since surfaces arrive in any order.
- **The column ring is max(view, near) plus the hysteresis,** plus the two columns that plans and bands read. The review's blocker: the first ring used the view distance alone, so at a view distance of 4 under a near distance of 8, which the RFC allows, 36 drawn chunks never came and 352 records waited forever. Every test had used a near distance no wider than the view.
- **Neighbors past the world's edge count as planned and ready;** before the review's fix, drawn chunks at the edge never completed. 14 of the review's 29 mutants survived the first tests, and 12 more tests followed.
- **Plans stay as long as their record lives** (a placeholder plan is 12 bytes); dropping them once every reader is built comes with RFC 0008. Builds pin the 27 plans and read them in RFC 0008's order, x fastest, then z, then y, with null for a uniform neighbor.
- **`ChunkRecord` is 64 bytes,** against the sketch's 48: about 214 bytes per loaded chunk with the map, against the target of 128, carried to item 8. At view distance 32 over the placeholder hills the region settled at 35,350 records and 9,123 chunks shown, where the RFC estimated about 9,400 drawn.

Item 6, backlogs, priorities, steps and regeneration (#246), landed as seven pull requests: the score and the backlog heap; the streamer fed by score; steps of half a millisecond, with the loop's meshes ranked too; regeneration; its tests and two fixes; two more fixes; tests for the review's surviving mutants.

- **The score is the RFC's,** squared for the job's key: the distance in chunks, doubled when the chunk's box grown by a chunk lies outside the view, 0 for the 27 chunks around the camera. Surfaces go first, then plans and builds by score; a plan whose column has no surface yet is parked until one arrives. `set_viewer` takes one viewer, the camera's chunk, eye and view, where the sketch took a span. The world loop's streaming meshes wait in a backlog ranked the same way; an edit's remeshes stay first in, first out.
- **`integrate()` drains into a pending list and stops 0.5 ms after a result,** a surface's records included, on RFC 0005's `JobClock`; the loop then passes again at once. Before the steps, one drain took 7.6 ms.
- **An edited chunk never regenerates, plan included** (decided 2026-10-09): its neighbors' borders must match the blocks it keeps, and the random walk caught edited chunks outside the region waiting forever to replan. Unedited chunks regenerate as designed, nearest first, keeping their blocks and meshes until plans of the new version, and then new blocks, replace them.
- **The review found four bugs,** all fixed: a ready chunk unloading through a version change kept the old blocks for good if it came back before its job reported; a build running through a change counted as current, so neighbors could read its old plan; a surface's records were made after the step's clock stopped; a version change on every pass queued every column and chunk again. 17 of its 33 mutants survived the first tests; all now fail one.
- **Not done here: an incremental region refresh.** `set_camera` refreshes every column at a chunk-border crossing, 0.85 to 1.2 ms at view distance 32 when measured (9 ms for a teleport); item 9 measures what the refresh costs a pass.
- **Found by item 9's review and left open:** a complete chunk with a replan job running can leave the region and come back drawn; it is set back to complete before `advance()` records whether it was shown, so no `shown` event goes out and it stays a hole. Only a generator version change reaches it, and the placeholder's version never changes.

Item 7, the mesh half (#255), landed as eight pull requests: known-empty meshes without a job, with the merge switch for streamed chunks; the walking frame; a tick's reach; the player on the world thread's ticks; `StreamedMeshes` on the main thread; its order tests and a test of the ground under the player's eye; the app streaming the world; the demo chunk's removal.

- **A known-empty chunk sends its empty mesh with no job,** even while jobs are held: uniform air, or a uniform opaque state among six uniform opaque face neighbors. About two in five meshes shown at view distance 32 are empty (3,800 of 9,123), and each had been a job that pinned 27 chunks and filled a 1.7 MiB padded chunk. The mesh half has no `waiting` state, since a chunk is shown only once complete.
- **On the main thread, unloads go first, then batches whole, then streaming meshes in order,** until the budget says later, one does not fit, or one belongs to a chunk a waiting batch holds; a chunk's meshes from before its unload are dropped. Two review rounds found a mesh that did not fit blocking the unloads behind it, so streaming could stop for good; batches waiting behind streaming meshes; and a batch that did not fit overtaken by a newer mesh of its own chunk. All were fixed, with 13 planted-bug controls red.
- **The player waits while its chunk loads** (decided 2026-10-09). In the app's first run the ground near the player was missing: the player spawned inside its own chunk before it loaded, and a chunk not ready is a solid cube to moves, which keeps a body out but cannot hold one up inside, so it fell through and stood inside the hill. No tick runs while the player's reach meets a chunk not loaded (RFC 0010's As built, open question 7); the first hold covered the body alone, and the review moved it to the reach. A test runs two seconds of ticks before any job, then checks that the ground fills the lower half of the frame from the player's eye.
- **The app runs the world thread with the cores less two as workers** (six on the reference machine), sizes the arena from the view distance, and starts the player on the grass by the spire at the origin. RFC 0003's merge switch remeshes every streamed chunk, each job carrying its own options.
- **Found in verification:** with empty meshes going while jobs were held, one fast pass could start a job for every chunk, and the backpressure test failed 19 times in 200 under load; at four jobs a pass it held in all 200.

Item 8, the memory budget (#256), landed after item 9 as four pull requests: the budget; the streamer's limits; the world loop's checks; the arena's state and the overlay's streaming lines.

- **The budget is a policy of its own in `world`** (`MemoryBudget`), which the world loop checks every 250 ms on its clock against the chunk data `chunk_bytes()` counts. Plans are not counted: the placeholder's are 12 bytes. RFC 0008's, 3 to 4 KiB each, will be.
- **Each check over budget lowers one step:** the hysteresis first, 2 to 1 to 0, so the farthest ring of chunks kept only by it goes first, then the view distance, a chunk at a time, down to 4. Chunks in the region and edited chunks never go: the streamer applies the limits as it applies a move, and keeps edited chunks outside every region, as before.
- **The mesh arena's state reaches the world thread a few times a second, and at once when it fills:** full, and its share of its limit. A full arena lowers the limits as chunk data over its budget does.
- **Under 90% of both budgets for 5 seconds the limits rise one step,** and again after each further 5 seconds: the view distance first, back to the player's, then the hysteresis.
- **The Render distance slider sets the player's limits:** followed at once while the budget holds nothing back; a nearer choice caps the budget's at once, and a farther one is come back to a step at a time.
- **The overlay's streaming lines,** from a report the main thread asks for a few times a second: chunks by how far along they are, what waits for a job, the chunk data against its budget, the view distance and hysteresis against the player's, and why they are lower (chunk or GPU memory).
- **The review found three things to fix and five gaps in the tests,** all fixed. Its look at the fix round found each fill of the arena counted twice: frames after a lowering still ended full before the unloads applied, so the next report's count rose again and lowered the limits a second time, which defeated the backoff. The arena's report now carries whether it is full with its count, and a fill counts only when the latest report says full and the count rose.

Item 9, the benchmark flythrough (#257), moved ahead of item 8 to give optimizations a baseline, and landed as nine pull requests: the streamer's totals and record checks; the world loop's samples; the main thread's holes; the path and plans; the phases; each phase's numbers; the report and its problems; the footprint, window size and option; the app flying it, with CI's step, the issue template and `docs/BENCHMARK.md`.

- **The report adds the world loop's longest pass beside the longest step:** the step is the budgeted integration, the pass everything one pass does, less answering samples. The pass shows what the step hides, the region refresh at a chunk-border crossing.
- **Samples come in three kinds,** completion, full and checked, one request out at a time. A phase waiting for the region asks for completion on each frame it can once no upload waits, and a complete region is answered in full, so `load` ends a frame or two after completion instead of at the next 30th frame. Holes are counted on both threads, each once: the world counts chunks with no mesh sent, the main thread meshes received but not drawn. The checks recount rather than trust counters: neighbor counts and state rules, the mesh half against the world half, and records outside the region grown by the hysteresis.
- **`--flythrough=quick` is CI's run:** view distance 16 at 640 by 360, checked on every sample, waits capped at 300 s as a hang guard, and chunk data after the teleport within 5% of its level after `load`; it exits 4 on a problem and 3 on a validation error. It runs on lavapipe under Xvfb and has not run on GitHub yet.
- **Three review rounds,** with over 40 planted-bug controls red, found among other things the load time quantized to the 30-frame sampling, holes counted as not sent rather than not drawn, and a closing answer landing on the newest phase instead of the one it was asked for. Two edge cases stay open: a phase that passes entirely while a closing request is out reports zeros for step, pass and memory, and a late boundary request counts a frame of the next phase in the closed phase's numbers.
- **The first report** (release build, reference machine, commit `af66a94`, three runs with 6 GB of 7 GB of swap in use; medians, or the third run where marked): `load` in 0.69 s (target under 1 s); 17,500 chunks generated and 11,200 meshed a second in `load` (target 20,000 each); 9 holes within 256 blocks at p99 in `sprint` (target none); a longest step of 0.70 ms (third run; target under 1 ms); frame p99 of 54.6 ms in `cruise` against 52.9 ms in `hover` (third run; target within 1 ms); 53 to 58 MiB of chunk data (third run; target under 128 MiB).
- **The longest pass is 10 to 12 ms at chunk-border crossings and 55 ms at the teleport** (medians), against the half millisecond the RFC lets a teleport delay a tick: a budgeted, incremental region refresh is the first optimization the report argues for. Frames are GPU-bound, 22 ms at the median in `hover` to 29 ms in `climb`. The footprint grew from about 550 to 640 MiB over the run while chunk data and meshes came back to their `load` levels. Runs 1 and 2 each had one frame of about a second, not investigated.

After item 9, the Render distance slider (#332) landed as one pull request of three commits: the streamer's view distance; the world loop's message; the overlay's slider. It settles open question 1 in the app.

- **"Render distance" sits in the overlay beside View bobbing,** 4 to 64 chunks with the blocks shown, 32 at every start (nothing is saved yet), disabled during a flythrough. It applies when let go, not while dragged: each change makes the region again on the world thread, and a drag would queue dozens.
- **`WorldStreamer::set_view_distance` makes the region again around the camera,** and chunks join and leave as on a move, the hysteresis included. The review, over 550 randomized seeds, found a nearer distance stranding the plans parked for the farther ring's columns while their surfaces were still queued, so the region never read complete until the next border crossing; parked plans are now tried again.
- **The arena keeps its startup size,** made for 32 chunks: 21.7 million quads (165 MiB) against about 25 MiB of meshes on the placeholder hills, so 64 fits. Rough terrain at 64 would overflow it, its far meshes waiting until something unloads, until RFC 0007's repack, which has to land before RFC 0008's terrain. A shrink from 64 to 4 is a one-time pass of about 52 ms on the world thread. In the app, 32, then 8, then 16 chunks took the arena from 3,172 meshes to 475 and 1,373.

GitHub Actions was blocked by billing while M3 was built, so these pull requests merged on local runs of CI's jobs; GCC, AddressSanitizer, ThreadSanitizer and lavapipe have not built them yet.
