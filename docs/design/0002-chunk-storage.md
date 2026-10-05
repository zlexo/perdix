# RFC 0002: Chunk storage

| | |
|---|---|
| **Status** | Accepted (2026-10-02) |
| **Milestone** | M2 First Chunk |
| **Issue** | #14 |

## Summary

How blocks are stored: block state ids, world and chunk coordinates, the order of blocks inside a chunk, and a palette-compressed format that keeps memory per chunk small enough for a long view distance on an 8 GB machine. Meshing (RFC 0003), generation, lighting and saving all read and write through this.

## Motivation

View distance is a memory problem before it is a rendering problem. A rough count, for a view distance of 1,024 blocks with 32-block chunks, if a plain cylinder 512 blocks tall were loaded around the camera:

| | Estimate |
|---|---|
| Loaded chunks | about 3,200 columns of 16 chunks: **about 51,000 chunks** |
| Stored as plain 16-bit arrays (64 KiB each) | **about 3.1 GiB** |
| Palette-compressed, best case: 60% of chunks are a single block (air above the ground, stone deep below) and the rest average 4 bits per block | **about 320 MiB** |

The first number does not fit next to everything else on the reference machine; the second does. The cylinder is only a yardstick, though: which chunks are loaded is RFC 0006's decision. Its region holds the chunks where terrain meets the sky out to the view distance, plus everything within 256 blocks of the camera: about 17,900 chunks at 1,024 blocks, against 65,987 for a cylinder 17 chunks tall counted the same way. If the surface spans 2 chunks per column (M4 measures it), their block data is about 110 MiB without caves and about 190 MiB with RFC 0008's caves and ores. As plain arrays those chunks would take about 1.1 GiB, more than twice RFC 0006's 512 MiB budget for chunk data.

Both compressed figures are best cases: they count the rock below the surface as single-block chunks, but caves and ores (RFC 0008) make most chunks there mixed, and how many depends on how deep they go (RFC 0008's open question 2). Even if no chunk were a single block, compression would still cut the plain arrays to a quarter (about 800 MiB for the cylinder), so the order of magnitude decides the design either way. M4 replaces all of these numbers with measurements from generated terrain.

A single-block chunk costs no block data, but a solid one is not free to mesh: it still has faces wherever it touches air in a neighbor.

## Goals and non-goals

Goals:

- One block type everywhere: a 16-bit block state id.
- Coordinates that are correct for negative positions and cover the whole 32-bit range.
- A chunk of a single block costs no block data at all, only its small header.
- Mixed chunks cost as few bits per block as their number of distinct states allows.
- Reading a block is a shift, a mask and a palette lookup, with no branches on the hot path beyond the storage mode.
- Bulk conversion to and from a plain array, fast enough that generators and the mesher work on plain arrays and never pay per-block compression costs.
- Memory reported per chunk and in total, so the overlay can show it.

Non-goals:

- The world's map of loaded chunks and streaming: RFC 0006. Threading: RFC 0005. Both are M3.
- What a block *is* (names, properties, textures): RFC 0004, the block registry.
- The on-disk format: M8. The in-memory layout is chosen so it can be written out as is, then compressed.
- Light storage: M6.

## Prior art

- **Minecraft's paletted containers** (Java Edition): each 16 by 16 by 16 section stores a palette of block states plus bit-packed indices, with the bit width chosen from the palette size and a direct mode for very mixed sections. Since 1.16 an index never spans two 64-bit words. The design here is the same idea with power-of-two widths.
- **OpenCubicChunks** (a Minecraft mod): cubic chunks instead of fixed-height columns, so the world loads in all three directions and is effectively unlimited up and down: 2^30 blocks each way by default, "Infinite*" in its own tagline.
- **Luanti** (formerly Minetest): 16 by 16 by 16 map blocks, the classic size for cubic storage.
- **Cubyz**: cubic chunks with level of detail, a reference for how far cubic storage can scale.

## Design

### Block states

```cpp
// A block state: one block type plus its properties (for example
// "oak log, facing up"). 0 is always air, so zeroed memory is air.
enum class BlockState : std::uint16_t { air = 0 };
```

16 bits give 65,536 states. Minecraft Java Edition has about 30,000 (29,873 in 26.1, by the state ids in PrismarineJS/minecraft-data), so there is headroom. RFC 0004 (the block registry) assigns the ids.

### Coordinates

```cpp
inline constexpr int chunk_bits = 5;
inline constexpr int chunk_size = 1 << chunk_bits;                          // 32
inline constexpr int chunk_volume = chunk_size * chunk_size * chunk_size;   // 32,768

struct BlockPos { std::int32_t x, y, z; };   // world block coordinates
struct ChunkPos { std::int32_t x, y, z; };   // world chunk coordinates (block >> 5)

constexpr ChunkPos chunk_of(BlockPos p) noexcept {
    return {p.x >> chunk_bits, p.y >> chunk_bits, p.z >> chunk_bits};
}

// The six faces of a block or a chunk, in RFC 0003's order.
enum class Face : std::uint8_t { pos_x, neg_x, pos_y, neg_y, pos_z, neg_z };
```

- Chunks are **cubic**, 32 by 32 by 32. There is no fixed world height: chunks exist wherever generation puts them, up and down (open question 2).
- Block coordinates are 32-bit, so the world spans about plus or minus 2.1 billion blocks on every axis. The camera's 64-bit position (RFC 0001) converts to them exactly.
- Since C++20, `>>` on a negative signed integer is an arithmetic shift, so `-1 >> 5` is `-1`: block -1 lives in chunk -1, at local coordinate 31. The tests pin this.

### Order of blocks in a chunk

```cpp
// x varies fastest, then z, then y: one horizontal layer is 1,024
// consecutive entries.
constexpr std::uint16_t local_index(int x, int y, int z) noexcept {
    return static_cast<std::uint16_t>(x | (z << 5) | (y << 10));
}
```

Terrain is layered horizontally (grass, dirt, stone, water surfaces), so keeping each horizontal layer contiguous keeps related blocks in the same cache lines and the same 64-bit words. It is Minecraft's order too, which makes its data easy to compare against.

### Storage modes

A chunk's blocks are stored in one of three modes:

| Mode | When | Block data |
|---|---|---|
| Uniform | one state fills the whole chunk | none; the state lives in the header |
| Paletted | 2 to 256 distinct states | a palette of states, plus an index per block at 1, 2, 4 or 8 bits |
| Direct | more than 256 distinct states | the 16-bit states themselves, no palette |

Widths are powers of two, so 64 divided by the width is a whole number of indices per 64-bit word and no index ever straddles two words. Reading block `i` at width `w`:

```cpp
// w is 1, 2, 4, 8 or 16; log2_w is its base-2 logarithm.
const std::uint32_t bit = std::uint32_t{i} << log2_w;
const std::uint64_t word = words[bit >> 6];
const std::uint32_t entry = static_cast<std::uint32_t>(word >> (bit & 63)) & mask;
return palette_mode ? palette[entry] : static_cast<BlockState>(entry);
```

Block data per chunk at each width:

| Bits per block | Palette size | Block data |
|---|---|---|
| 0 (uniform) | 1 | 0 |
| 1 | 2 | 4 KiB |
| 2 | 4 | 8 KiB |
| 4 | 16 | 16 KiB |
| 8 | 256 | 32 KiB |
| 16 (direct) | none | 64 KiB |

Five power-of-two sizes means the block data comes from five size classes. Each class keeps a free list, so loading and unloading chunks for hours does not fragment the heap. Generation jobs allocate block data on workers and the world thread frees it (RFC 0005), so the lists are shared between threads and each size class takes a lock. Allocation is rare (a generated chunk, a width change, a clone), so the lock costs little; per-thread caches can come later if profiling shows contention.

### Changing blocks

`set` first finds the state's palette index with a linear scan of the palette, then writes in place. The scan is cheap while the palette is small (4.5 ns per `set` measured in a 4-bit chunk) and grows with it (37.8 ns in an 8-bit chunk holding 200 states). Generation never pays it, because generators write plain arrays and use `from_dense`; edits and simulation do. A reverse lookup table for 8-bit palettes is added only if profiling shows `set` in very mixed chunks matters.

A new state that does not fit the palette first looks for a dead entry: one pass counts how often each palette entry is used (a few microseconds, and only on this rare path), and an entry with no blocks left is reused. Only when every entry is live does the width double and the chunk repack once (1 to 2 to 4 to 8 bits, then direct). Without that check, overwritten states that still hold palette slots would force growth that compaction could have avoided.

Palettes do not shrink on every `set`. When a state's last block is overwritten, its palette entry stays until the chunk is next compacted. Compaction is a write, so it cannot ride along with meshing or saving, which read pinned versions on workers (RFC 0005): the world thread runs it as a pass of its own, on versions no job has pinned. It is not free, since it counts how often each palette entry is used (a few microseconds) and repacks when the width can shrink, so the pass visits only chunks edited since their last compaction.

### Bulk conversion

Generators write a plain `BlockState[32768]` and compress it once with `from_dense`, which builds the palette with a reverse lookup table from state to palette index, then picks the smallest width that fits. The table and the unpacked indices (160 KiB in the prototype) live in a `PaletteScratch` that the caller passes in, like RFC 0003's `MeshScratch`, so `from_dense` keeps no hidden per-thread state; RFC 0005 keeps one in each worker's scratch. A generator that already knows a chunk holds one state, such as all air above the terrain or all stone below the caves, skips the array and calls `uniform` (RFC 0008).

The mesher calls `to_dense` to get a plain array, so its inner loops never touch the packed format. A second form of `to_dense` takes a layout (an offset plus row and layer strides) and writes straight into the inside of the mesher's padded 34 by 34 by 34 array, saving a full extra copy per mesh. The one-block border around it comes from all 26 neighbors (RFC 0003): the 6 face neighbors through `copy_face`, and the 12 edge and 8 corner neighbors through `get`, 392 single-block reads in all.

### API sketch

```cpp
namespace vx {

// Where to_dense puts block (x, y, z): out[offset + x + z * row_stride + y * layer_stride].
struct DenseLayout {
    std::size_t offset;
    std::size_t row_stride;
    std::size_t layer_stride;
};

// from_dense's working memory: a reverse lookup table from state to palette index,
// and the unpacked indices. Owned by the caller and never shared.
struct PaletteScratch;

class ChunkStorage {
public:
    ChunkStorage() noexcept;   // uniform air, no allocation
    [[nodiscard]] static ChunkStorage uniform(BlockState state) noexcept;   // any one state, no allocation
    [[nodiscard]] ChunkStorage clone() const;   // move-only, so copies are explicit (copy on write, RFC 0005)

    [[nodiscard]] static ChunkStorage from_dense(std::span<const BlockState, chunk_volume> blocks,
                                                 PaletteScratch& scratch);
    void to_dense(std::span<BlockState, chunk_volume> out) const noexcept;

    // Writes block (x, y, z) to out[offset + x + z * row_stride + y * layer_stride],
    // so the mesher fills the inside of its padded 34^3 array in one pass.
    void to_dense(std::span<BlockState> out, DenseLayout layout) const noexcept;

    // The 32 by 32 layer of blocks on one face, for a neighbor's mesher.
    void copy_face(Face face, std::span<BlockState, chunk_size * chunk_size> out) const noexcept;

    [[nodiscard]] BlockState get(std::uint16_t index) const noexcept;
    void set(std::uint16_t index, BlockState state);

    void compact();   // drop unused palette entries, shrink the width if possible

    [[nodiscard]] bool is_uniform() const noexcept;
    [[nodiscard]] int bits_per_block() const noexcept;   // 0, 1, 2, 4, 8 or 16
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
};

} // namespace vx
```

`ChunkStorage` is a move-only value type with no locks inside. Which thread may touch a chunk is RFC 0005's design: once a chunk is in the world, only the world thread writes it, and jobs on workers read pinned versions, several jobs at once, while an edit to a pinned chunk writes to a `clone`. So `const` methods never write hidden state (no caches, counters or lazy compaction), and every write to a chunk in the world, `compact` included, runs on the world thread.

## Alternatives considered

- **Plain 16-bit arrays:** the simplest and fastest to read, and four to ten times the memory, depending on how many chunks are a single block. Kept as the working format inside generators and the mesher, where speed matters and only a few chunks are live at once.
- **Run-length encoding:** smaller for layered terrain, but reading one block means walking runs. Kept in mind for the disk format.
- **Sparse voxel octrees:** compact for large uniform regions and good for ray tracing, but slower to read and edit, and this is a rasterizer (ADR 0002).
- **Odd bit widths (3, 5, 6 or 7 bits):** up to 37.5% smaller, at the cost of indices that straddle words or wasted bits per word. Palettes of 17 to 32 states save the most: they need 5 bits but get 8, so 20,480 bytes of block data instead of 32,768, or 21,848 (33% smaller) when no index straddles a word, as in Minecraft. Power-of-two widths keep the read path branch-free and simple.
- **16 by 16 by 16 chunks:** finer edits and streaming, but eight times as many chunks, headers, map entries and draw ranges for the same view distance. Open question 1.
- **Fixed-height columns like Minecraft:** simpler to generate for, but the world gets a ceiling and a floor, and streaming and level of detail only work in two dimensions. Open question 2.

## Performance budget

On the reference machine, release build, measured with nanobench in a 4-bit chunk. `get` is measured as throughput over independent random positions; the latency of one read whose index depends on the previous read is reported next to it, not budgeted.

| Operation | Target | Prototype |
|---|---|---|
| `get`, throughput | under 2 ns | 0.78 ns |
| `get`, dependent latency | not budgeted | 4.3 ns |
| `set`, no growth | under 10 ns | 4.5 ns |
| `to_dense` | under 30 µs per chunk | 5.4 µs |
| `from_dense` | under 100 µs per chunk | 14.3 µs |
| Header of a uniform chunk | 32 bytes or less | 32 bytes |

The prototype column comes from a throwaway implementation of this design measured on the reference machine on 2026-10-02 (Apple Clang 17, `-O2 -mcpu=apple-m3`, median of seven runs, a chunk with ten terrain-like states). It shows the budget is reachable with room to spare; the implementation's own benchmarks replace it.

## Test plan

- **Coordinates:** block -1 is chunk -1, local 31; block -32 is chunk -1, local 0; block -33 is chunk -2, local 31; round trips at the edges of the 32-bit range.
- **Index order:** neighbors along x are 1 apart, along z 32 apart, along y 1,024 apart.
- **Storage:** a default chunk is uniform air with zero block data, and `uniform` gives the same for any state; `clone` gives an equal copy that later writes to either leave the other unchanged; one `set` makes it 1-bit; growth through every width up to direct mode; `compact` after overwrites shrinks the width again.
- **Round trips:** random chunks (seeded, so failures reproduce) survive `from_dense` then `to_dense` unchanged at every width, all through one reused `PaletteScratch`, and `get` agrees with a plain array after thousands of random `set` calls.
- **Memory:** `memory_bytes` matches the table above at every width.
- **Pools:** several threads allocate and free block data of every size class at once, under ThreadSanitizer (RFC 0005's `linux-tsan` job).
- **Concurrent reads:** several threads read one chunk through its `const` methods at once, under ThreadSanitizer.
- **Layering control:** `engine/world` is the second layer, so its pull request proves that including a header from a layer it does not link fails to compile (RFC 0000's test plan).

## Pull request plan

1. This RFC.
2. `engine/world` layer: `BlockState`, coordinates, `local_index`, with tests and the layering control.
3. The bit-packed array (power-of-two widths), with tests.
4. Benchmark harness: nanobench in `bench/`, run in release, numbers printed for the pull request. First benchmarks: the packed array.
5. `ChunkStorage`: modes, `uniform`, `clone`, `get`, `set`, growth, direct mode, `compact`, with tests.
6. Bulk conversion (`from_dense`, `to_dense`, `copy_face`) with tests and benchmarks.
7. Size-class pools for block data, with a lock per class, and memory accounting.

## Open questions

All settled on 2026-10-02: every recommendation below was accepted.

1. **Chunk size: 32 or 16 blocks on a side.** Recommendation: **32**. Eight times fewer chunks to track, mesh and draw for the same view distance, and a 32-block row still fits a 64-bit mask with its border for fast meshing (RFC 0003). The cost: editing one block remeshes a bigger chunk.
2. **World height: unlimited, or fixed like Minecraft.** Recommendation: **unlimited**, with cubic chunks. Mountains and caves can go as high and deep as generation wants, and streaming and level of detail work in all three directions. This is a game-design call as much as a technical one.
3. **Block state ids: 16 or 32 bits.** Recommendation: **16**. 65,536 states is plenty, and every plain array, palette and direct-mode chunk is half the size.

## As built

Pull request plan item 2, the world layer:

- **`LocalPos` and `block_at` join the sketch**, which only went from blocks to chunks: the test plan's round trips need the way back. The coordinate types compare with a defaulted `==`, for the tests and later for maps keyed by position.
- **The layer is headers only, an interface library, until item 3** brings its first source and makes it the static library RFC 0000 describes.
- **`engine/world` is the third layer, not the second:** the renderer landed first, in milestone 1, without the layering control. It is proven here instead: a world header that includes `<vx/render/renderer.hpp>` fails to compile with `'vx/render/renderer.hpp' file not found`, since only targets that link `vx::render` see its headers.
- **A CI job, `Layering` (#28),** fails when a file under `engine/world` or `engine/mesh` includes a Vulkan, volk, VMA, SDL or GLFW header: include paths fence off the engine's own headers, not system ones.

Pull request plan item 3, the bit-packed array:

- **`PackedArray`** has `get`, `set`, `repacked` for a copy at another width, and `clone`, the only way to copy one, since copies of a chunk's blocks are copy on write (RFC 0005). Item 5's paletted mode keeps a chunk's palette indices in it, and direct mode its block states.
- **It adds 16 bytes to a chunk's header:** the words behind one pointer, sized by the width, and the width's mask and base-2 logarithm, which every read uses. A `std::vector` would take 24 bytes for the words alone, of the 32 that the performance budget allows a uniform chunk's whole header.
- **Repacking to a narrower width needs every entry to fit**, which debug builds assert; compaction (item 5) checks before it narrows.

Pull request plan item 4, the benchmark harness:

- **nanobench 4.6.0**, its latest release, fetched as sources only and compiled once; `vx_bench` holds every benchmark. On macOS it asks for a performance core: run on the efficiency cores (`taskpolicy -b`), the same benchmarks took 1.8 to 5.1 times as long, and varied more.
- **Every build runs `vx_bench --quick` as the test `the benchmarks run`:** each benchmark once, timing nothing, so CI checks that the benchmarks still run without asserting a time.
- **The packed array's first numbers**, the median of five release runs on the reference machine (Apple Clang 17, `-O3`): a read takes 0.33 ns as throughput, a write 0.62 ns, and a repack from 4 to 8 bits 52 µs. They are the array alone, under a chunk's palette; item 5 measures chunks against the budget.
- **A dependent read takes 2.72 ns**, the median of seven runs. The first latency loop gave 3.86 ns, but it also timed its index hash: Apple Clang fuses the hash's multiply into one multiply-add with the value just read, so the multiply waited for the read. Run for run against the new loop the same day, the old one gave 3.46 ns.

Pull request plan item 5, chunk storage, landed as two pull requests: the modes with reads and writes, then compaction.

- **A uniform chunk's packed array is empty:** `PackedArray` gained a state with no words, which is also what a move now leaves behind. The header is 32 bytes, the budget: the array, the palette's pointer, its size and the uniform state.
- **A palette holds as many entries as its width allows from the start**, 2, 4, 16 or 256, so adding a state allocates nothing until the width doubles. `memory_bytes` counts it with the block data: the table's figure plus 4 to 512 bytes.
- **Compaction narrows a chunk, or changes nothing:** when the width cannot shrink, unused entries stay for `set` to reuse, since they cost no memory there.
- **The counting pass takes about 16 µs in a 4-bit chunk, not a few microseconds.** Counted into one tally, it took 62 µs: in a run of one state, each count waits on the one before. Each word's entries go into eight tallies in turn, summed at the end.
- **The numbers**, medians of five release runs on the reference machine, in a 4-bit chunk of layered terrain with ten states: `get` takes 0.38 ns as throughput, against a budget of 2 ns, and 3.82 ns as dependent latency (4.58 ns from the first loop, which also timed its index hash's multiply, as item 4's did; the median of seven runs, run for run with the old loop); `set` without growth 3.55 ns, against 10 ns; and `set` in an 8-bit chunk of 200 states 32.2 ns. Measured back to back, the same `set` took 3.05 ns before compaction joined it in its source file: where the compiler places code moves a nanosecond-scale loop that much.
- **Concurrent reads under ThreadSanitizer wait for RFC 0005's `linux-tsan` job**, which does not exist yet, as do item 7's pools.

Pull request plan item 6, bulk conversion:

- **`PaletteScratch` keeps its two tables on the heap**, 160 KiB, so the object itself is 16 bytes wherever the caller keeps it. `from_dense` resets only the table entries it set, so the table never needs a full clear between chunks.
- **`copy_face`'s order**, which RFC 0003 left open: the other two axes in `local_index`'s order, `x + 32 * z` for a y face, `z + 32 * y` for an x face and `x + 32 * y` for a z face. An x face reads block by block, since its blocks lie 32 apart; the other faces decode runs of 32.
- **`chunk_area`**, 1,024, names a face's blocks beside `chunk_volume`.
- **`DenseLayout`'s members default to a plain chunk-sized array**, so `to_dense(out, {})` writes what `to_dense(out)` does; a test holds the two together.
- **Conversion packs and decodes a word at a time**, through one helper each way.
- **The numbers**, medians of five release runs on the reference machine, on a 4-bit terrain chunk: `to_dense` takes 7.4 µs, against a budget of 30 and the prototype's 5.4; into the padded array 8.6 µs; `from_dense` 16.1 µs, against 100 and the prototype's 14.3; `copy_face` 0.27 µs for a y face and 0.36 µs for an x face.

Pull request plan item 7, the pools and memory accounting:

- **One pool for the process, never destroyed**, so a chunk freed during static destruction still finds it. Freed blocks stay cached for the life of the process.
- **The free list lives in the freed blocks**, each one's first word holding the next one's address, so freeing never allocates and cannot fail. A block is zeroed when it is handed out.
- **Under AddressSanitizer a cached block is poisoned** until it is handed out again, all but its first word, so a use after free of block data still reports in the `Linux (ASan + UBSan)` job, though the memory never goes back to the heap. The first word is the free-list link, which LeakSanitizer must read to reach the blocks behind it: it does not follow pointers kept in poisoned memory, and reported every cached block past the first as leaked when the link was poisoned too. Freeing a block twice stops at an assert, which finds it poisoned already. Guard Malloc, the local check on macOS, cannot see inside the pool.
- **`PackedArray` owns its words through a plain pointer:** a `unique_ptr` with the pool's deleter would need the width beside it, and the array must stay 16 bytes.
- **Palettes stay on the heap**, 4 to 512 bytes each, too small for classes of their own.
- **Memory accounting is `block_data::stats()`**, each class's blocks in use and cached, beside each chunk's `memory_bytes`; the overlay shows it once streaming (RFC 0006) fills the world.
- **The pool saves no time on the reference machine:** run interleaved with the heap's version, five release runs each, making and dropping a 4-bit array took 245 ns against 262, with nanobench's error near 15%, and `from_dense` 16.1 µs either way. What it buys is a heap that does not fragment over hours of streaming, which a benchmark of seconds cannot show.
- **Several threads at once** are tested with four threads marking each block they hold; with the lock taken out of `allocate`, the test fails every run. ThreadSanitizer waits for RFC 0005's job.
