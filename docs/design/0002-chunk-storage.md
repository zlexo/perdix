# RFC 0002: Chunk storage

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M2 First Chunk |
| **Issue** | #14 |

## Summary

How blocks are stored: block state ids, world and chunk coordinates, the order of blocks inside a chunk, and a palette-compressed format that keeps memory per chunk small enough for a long view distance on an 8 GB machine. Meshing (RFC 0003), generation, lighting and saving all read and write through this.

## Motivation

View distance is a memory problem before it is a rendering problem. A rough count, for a view distance of 1,024 blocks with 32-block chunks and 512 blocks of height loaded:

| | Estimate |
|---|---|
| Loaded chunks | about 3,200 columns of 16 chunks: **about 51,000 chunks** |
| Stored as plain 16-bit arrays (64 KiB each) | **about 3.1 GiB** |
| Palette-compressed, if 60% of chunks are a single block (air above the ground, stone deep below) and the rest average 4 bits per block | **about 320 MiB** |

The first number does not fit next to everything else on the reference machine; the second does. The assumptions are rough and get replaced by measurements from generated terrain in M4, but the order of magnitude decides the design.

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

- The world's map of loaded chunks, streaming and threading: M3.
- What a block *is* (names, properties, textures): the block registry RFC.
- The on-disk format: M8. The in-memory layout is chosen so it can be written out as is, then compressed.
- Light storage: M6.

## Prior art

- **Minecraft's paletted containers** (Java Edition): each 16 by 16 by 16 section stores a palette of block states plus bit-packed indices, with the bit width chosen from the palette size and a direct mode for very mixed sections. Since 1.16 an index never spans two 64-bit words. The design here is the same idea with power-of-two widths.
- **OpenCubicChunks** (a Minecraft mod): cubic chunks instead of fixed-height columns, so the world extends without limit up and down and loads in all three directions.
- **Luanti** (formerly Minetest): 16 by 16 by 16 map blocks, the classic size for cubic storage.
- **Cubyz**: cubic chunks with level of detail, a reference for how far cubic storage can scale.

## Design

### Block states

```cpp
// A block state: one block type plus its properties (for example
// "oak log, facing up"). 0 is always air, so zeroed memory is air.
enum class BlockState : std::uint16_t { air = 0 };
```

16 bits give 65,536 states. Minecraft Java Edition has roughly 25,000, so there is headroom. The block registry RFC assigns the ids.

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

Five power-of-two sizes means the block data comes from five size classes. Each class keeps a free list, so loading and unloading chunks for hours does not fragment the heap.

### Changing blocks

`set` first finds the state's palette index with a linear scan of the palette, then writes in place. The scan is cheap while the palette is small (4.5 ns per `set` measured in a 4-bit chunk) and grows with it (37.8 ns in an 8-bit chunk holding 200 states). Generation never pays it, because generators write plain arrays and use `from_dense`; edits and simulation do. A reverse lookup table for 8-bit palettes is added only if profiling shows `set` in very mixed chunks matters.

A new state that does not fit the palette first looks for a dead entry: one pass counts how often each palette entry is used (a few microseconds, and only on this rare path), and an entry with no blocks left is reused. Only when every entry is live does the width double and the chunk repack once (1 to 2 to 4 to 8 bits, then direct). Without that check, overwritten states that still hold palette slots would force growth that compaction could have avoided.

Palettes do not shrink on every `set`. When a state's last block is overwritten, its palette entry stays until the chunk is next compacted, which happens when it is meshed or saved: both already read every block, so counting usage costs nothing extra there.

### Bulk conversion

Generators write a plain `BlockState[32768]` and compress it once with `from_dense`, which builds the palette with a per-thread reverse lookup table, then picks the smallest width that fits. The mesher calls `to_dense` to get a plain array (and the one-block border it needs from the six neighbors through `copy_face`), so its inner loops never touch the packed format. A second form of `to_dense` takes a layout (an offset plus row and layer strides) and writes straight into the inside of the mesher's padded 34 by 34 by 34 array, saving a full extra copy per mesh.

### API sketch

```cpp
namespace vx {

class ChunkStorage {
public:
    ChunkStorage() noexcept;   // uniform air, no allocation

    [[nodiscard]] static ChunkStorage from_dense(std::span<const BlockState, chunk_volume> blocks);
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

`ChunkStorage` is a move-only value type with no locks inside. Who may touch a chunk from which thread is the job system's design (M3); until then, everything runs on one thread.

## Alternatives considered

- **Plain 16-bit arrays:** the simplest and fastest to read, and about ten times the memory. Kept as the working format inside generators and the mesher, where speed matters and only a few chunks are live at once.
- **Run-length encoding:** smaller for layered terrain, but reading one block means walking runs. Kept in mind for the disk format.
- **Sparse voxel octrees:** compact for large uniform regions and good for ray tracing, but slower to read and edit, and this is a rasterizer (ADR 0002).
- **Odd bit widths (3, 5, 6 bits):** up to 25% smaller for some palettes, at the cost of indices that straddle words or wasted bits per word. Power-of-two widths keep the read path branch-free and simple.
- **16 by 16 by 16 chunks:** finer edits and streaming, but eight times as many chunks, headers, map entries and draw ranges for the same view distance. Open question 1.
- **Fixed-height columns like Minecraft:** simpler to generate for, but the world gets a ceiling and a floor, and streaming and level of detail only work in two dimensions. Open question 2.

## Performance budget

On the reference machine, release build, measured with nanobench over random positions in a 4-bit chunk:

| Operation | Target |
|---|---|
| `get` | under 2 ns |
| `set`, no growth | under 10 ns |
| `to_dense` | under 30 µs per chunk |
| `from_dense` | under 100 µs per chunk |
| Header of a uniform chunk | 32 bytes or less |

## Test plan

- **Coordinates:** block -1 is chunk -1, local 31; block -32 is chunk -1, local 0; block -33 is chunk -2, local 31; round trips at the edges of the 32-bit range.
- **Index order:** neighbors along x are 1 apart, along z 32 apart, along y 1,024 apart.
- **Storage:** a default chunk is uniform air with zero block data; one `set` makes it 1-bit; growth through every width up to direct mode; `compact` after overwrites shrinks the width again.
- **Round trips:** random chunks (seeded, so failures reproduce) survive `from_dense` then `to_dense` unchanged at every width, and `get` agrees with a plain array after thousands of random `set` calls.
- **Memory:** `memory_bytes` matches the table above at every width.
- **Layering control:** `engine/world` is the second layer, so its pull request proves that including a header from a layer it does not link fails to compile (RFC 0000's test plan).

## Pull request plan

1. This RFC.
2. `engine/world` layer: `BlockState`, coordinates, `local_index`, with tests and the layering control.
3. The bit-packed array (power-of-two widths), with tests.
4. Benchmark harness: nanobench in `bench/`, run in release, numbers printed for the pull request. First benchmarks: the packed array.
5. `ChunkStorage`: modes, `get`, `set`, growth, direct mode, `compact`, with tests.
6. Bulk conversion (`from_dense`, `to_dense`, `copy_face`) with tests and benchmarks.
7. Size-class pools for block data and memory accounting.

## Open questions

1. **Chunk size: 32 or 16 blocks on a side.** Recommendation: **32**. Eight times fewer chunks to track, mesh and draw for the same view distance, and a 32-block row still fits a 64-bit mask with its border for fast meshing (RFC 0003). The cost: editing one block remeshes a bigger chunk.
2. **World height: unlimited, or fixed like Minecraft.** Recommendation: **unlimited**, with cubic chunks. Mountains and caves can go as high and deep as generation wants, and streaming and level of detail work in all three directions. This is a game-design call as much as a technical one.
3. **Block state ids: 16 or 32 bits.** Recommendation: **16**. 65,536 states is plenty, and every plain array, palette and direct-mode chunk is half the size.

## As built

Not built yet.
