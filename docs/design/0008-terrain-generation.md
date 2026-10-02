# RFC 0008: Terrain generation

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M4 Terrain |
| **Issue** | #23 |

## Summary

How a world seed and a chunk position become blocks: climate fields, a 3D density with overhangs and caves, biomes, surface blocks, and ores and trees that cross chunk borders, in cubic 32 by 32 by 32 chunks with no fixed height (RFC 0002). The same seed and parameter file give byte-identical chunks on every platform and thread count. Every parameter sits on a live slider in the debug overlay, so the look of the terrain is chosen by eye, not guessed in code. Generation speed is what lets the world keep up with a fast-flying camera, so it has a budget per chunk.

## Motivation

- **Speed.** At RFC 0002's example view (1,024 blocks around the camera, 512 tall), flying at 64 blocks per second brings about 2,000 new chunks into view every second. Each one is generated before it can be meshed.
- **No fixed height.** Minecraft generates whole 16 by 16 columns from the bottom of the world to the top. Here a column has no bottom or top, so the surface can be in any chunk, in several, or twice in one column under an overhang.
- **Determinism.** Chunks are generated on many threads in whatever order the camera asks for them. Randomness that depends on order, thread count, standard library or compiler makes worlds differ between runs and machines, and makes tests flaky. It is cheapest to rule out before the first line of generator code.
- **Taste.** What good terrain looks like is the owner's call. Numbers that live in code cost a rebuild per idea; numbers on sliders let hundreds of ideas be tried in an evening.

## Goals and non-goals

Goals:

- The same seed and parameter file give byte-identical chunks on every supported compiler, CPU architecture and thread count, checked by golden hashes in CI.
- A chunk is generated from its position alone, in two passes: a plan that needs nothing else, and a build that needs only the plans of its 26 neighbors, never their blocks.
- 3D terrain: overhangs, cliffs, large caverns, long tunnels and thin tunnels, in a world without a top or bottom.
- Terrain 2^30 blocks from the origin is as smooth as at the origin.
- Biomes chosen from climate values, surface blocks per biome, and ores and trees that cross chunk borders without seams.
- Chunks no terrain can reach cost about a microsecond and need no neighbors; other uniform chunks skip the per-block work.
- Every tunable value is in one parameter file, on a slider in the overlay, and a change shows around the camera within 100 ms (target).

Non-goals:

- Which chunks to generate, when, and in what order: RFC 0006. Threads, priorities and cancellation: RFC 0005.
- Water and other fluids, aquifers, flooded caves: M7 (open question 4). Light: M6.
- Structures (villages, dungeons) and any feature reaching more than 32 blocks from its origin: a later structures RFC.
- Plants and other non-cube blocks: the block models RFC. Features take them unchanged.
- Saving: M8. Determinism lets M8 store only edited chunks; how a saved world pins its generator and parameters is M8's call.
- Level of detail for distant terrain: M9.

## Prior art

- **Minecraft 1.18** (Java Edition, November 2021) rebuilt terrain around a 3D density, positive meaning solid. Three 2D noises, continentalness, erosion and weirdness (folded into "peaks and valleys", PV = 1 - |3|W| - 2|), feed splines for a height offset and a vertical "factor" that decides how much 3D noise shows. Terrain no longer comes from biomes; biomes are picked afterwards, each one a set of intervals in a six-dimensional space (temperature, humidity, continentalness, erosion, weirdness, depth), and a position takes the closest. Density is evaluated at the corners of 4 by 8 by 4 block cells and interpolated. Cheese caves (large pockets), spaghetti caves (tunnels along the zero crossings of noise), noodle caves (1 to 5 blocks wide) and stone pillars are part of the density; older carver caves remain, and aquifers decide where fluids go. We take the density, the climate model and the cave family; what relies on a fixed height (whole-column passes, a gradient pinned between the world's floor and ceiling) does not carry over.
- **Minecraft's features step:** a feature may write into the 3 by 3 chunks around the one being decorated, so which of two overlapping features wins depends on generation order, and one seed can differ by travel route (bug MC-55596). Up to 1.13, population was offset by 8 blocks to stay inside a 2 by 2 grid of loaded chunks, and Forge logs "cascading worldgen lag" when a mod's feature reaches further and forces more chunks to generate. The lesson: a feature never writes into a chunk that may already exist.
- **Henrik Kniberg, "Reinventing Minecraft world generation"** (talk, 2022): the 1.18 design explained by one of its designers. A squashing factor and a height offset over 3D noise, splines of continentalness, erosion and PV, and biomes from tables of discrete climate levels. The mental model for the shape and biome stages here.
- **Luanti** (formerly Minetest) generates 80^3 "mapchunks" of 5^3 mapblocks in one operation, inside a buffer one mapblock wider on every side, so caves and dungeons can spill over the edge. Mapgen v7 shapes mountains with 3D noise and defines tunnels with two 3D noises. The world ends about 31,000 blocks from the origin in every direction.
- **Cubyz** (Zig, GPL-3.0; read for ideas, no code taken) places trees from position-seeded randomness in cached 128^3 "structure map" fragments and registers each one with every chunk its bounding box touches; a chunk applies its list in priority order. The closest prior art to the gather design below.
- **Randomness from position.** Squirrel Eiserloh, "Noise-Based RNG" (GDC 2017); Jarzynski and Olano, "Hash Functions for GPU Rendering" (JCGT, 2020), who note that random numbers derived from position do not depend on thread count or evaluation order. SplitMix64 (Steele, Lea and Flood; Vigna's public-domain `splitmix64.c`), PCG (MIT or Apache-2.0) and xxHash (BSD 2-clause) are the usual building blocks. Minecraft 1.18 moved generation to Xoroshiro128++ with generators derived from positions and names.

## Design

### The pipeline

| Stage | Pass | Resolution | Needs from other chunks |
|---|---|---|---|
| 1. Climate and shape fields | plan | every 4 blocks, per column of chunks | nothing; cached per worker for the column |
| 2. Density, caves included | plan | lattice of 9 by 6 by 9 points | nothing |
| 3. Biomes | plan | cells of 4 by 4 by 4 blocks | nothing |
| 4. Placements of features | plan | candidate columns | nothing |
| 5. Fill and surface | build | every block | nothing |
| 6. Features | build | blocks a placement touches | the plans of the 26 neighbors |
| 7. Handoff | build | | nothing: `ChunkStorage::from_dense`, once |

A plan is about 3 KiB and is kept until the chunks that need it are built, so streaming (RFC 0006) plans one ring beyond the chunks it builds. Plans and builds are pure functions of the seed, the parameters and the position: which worker made them, and when, cannot matter.

### Determinism: random streams

Nothing draws from a shared generator. Every random decision comes from a stream keyed by what it decides and where:

```cpp
// SplitMix64's finalizer, constants from Vigna's splitmix64.c.
constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

Rng rng{stream_seed(world_seed, salt("feature.oak"), chunk)};   // one stream per decision and place
```

- `salt` is the 64-bit FNV-1a hash of a name, so streams depend on names, not on declaration order. Adding a noise field or a feature leaves every other one unchanged, which the taste loop needs: tuning one thing must not reshuffle the rest.
- `Rng` is SplitMix64, a counter run through `mix64`. Floats come from the top 24 bits (exact); integers below `n` from a multiply and a shift. Both are written out, because the algorithms of `std::uniform_int_distribution` and the other standard distributions are implementation-defined, so libstdc++, libc++ and MSVC may disagree. `std::random_device`, clocks, addresses, `std::hash` and the iteration order of unordered containers never reach the output either.
- Hashing is unsigned 64-bit arithmetic, which wraps by definition; nothing relies on signed overflow.

### Determinism: floating point

IEEE 754 rounds addition, subtraction, multiplication, division and square root correctly, so the same operations in the same order give the same bits everywhere. Differences come from compilers and libraries, and each has a rule:

| Hazard | Rule |
|---|---|
| Fused multiply-add. Clang's default `-ffp-contract=on` fuses `a * b + c` on arm64, which always has FMA, but not on baseline x86-64; GCC's default outside ISO C modes is `fast`; MSVC 2022 does not fuse under `/fp:precise`. | `-ffp-contract=off` on every engine target, in RFC 0000's CMake helper next to the warnings. |
| `-ffast-math`, `/fp:fast` | Never, anywhere in the build. |
| `sin`, `exp`, `pow` and friends are not correctly rounded (glibc does not aim for it), so math libraries can disagree. | Not used in generation. Curves and noise need only arithmetic, `floor`, `abs` and `sqrt`. |
| Approximate SIMD instructions: x86 `rcp` and `rsqrt` even differ between Intel and AMD. | Not used. |
| SIMD widths | Element-wise only, no reductions. The scalar code is the reference, and SIMD code is tested bit for bit against it. |
| Precision far from the origin | Positions are `int32`. Scaling and rotation into noise space happen in `double`; only the offset inside a noise cell becomes `float`. |
| The floating-point environment | Generation never changes rounding or flush-to-zero modes. |
| Excess precision (the 32-bit x87 unit) | Not on our targets: x86-64 and arm64 do `float` math in `float`. |

Measured on the reference machine while writing this RFC: 100,000 interpolations built with Apple Clang 17 give different bits on arm64 than the same code built for x86-64 and run under Rosetta 2, and identical bits once both use `-ffp-contract=off`. Turning contraction off cost about 10% in scalar noise. The rules make agreement the expected result, not a proof; the golden hashes on every compiler and architecture are what holds the line, and a mismatch is a bug in these rules.

### Noise

Our own noise: OpenSimplex2 (KdotJPG's CC0 reference) ported to C++, in its `ImproveXZ` 3D form, which the reference recommends for terrain with Y up, and its 2D form for climate, summed into octaves (fBm). One call evaluates a whole lattice. The noise's own lattice indices are 64-bit, so no position overflows, and a point's value depends only on its world position, never on which chunk asked, so neighbors agree bit for bit on shared faces. A scalar version is the reference; a 4-wide version (NEON, SSE2) must match it bit for bit.

The alternatives, measured on the reference machine (4-octave 3D fBm over 64 chunk-sized grids, Apple Clang 17, release, x86-64 paths under Rosetta 2):

| | FastNoise2 1.1.1 | FastNoiseLite 1.1.1 | Our OpenSimplex2 |
|---|---|---|---|
| License | MIT | MIT | CC0 reference |
| Time per sample | 17.8 ns (NEON, strict mode) | 92 ns (scalar, contraction off) | target: 25 ns |
| Same bits on arm64 and x86-64 | Only with `FASTNOISE2_STRICT_FP`, off by default. Default builds use `-ffast-math` and gave three different results on NEON, SSE and AVX2; strict builds matched across NEON, SSE2, SSE4.1 and AVX2. | With contraction off | By construction, and tested |
| Far from the origin | Positions are `float`: 2.4 times rougher at a million blocks, 22 times at ten million | `double` positions: unchanged | `double` positions |
| Also | Picks a SIMD path at run time; fetches FastSIMD through CPM | Hashing relies on signed overflow, which UBSan reports | Two pull requests of our own code |

"Rougher" is the RMS of the second difference along a row of samples: 2.0e-4 at the origin and 4.3e-3 at ten million blocks for FastNoise2, 1.9e-4 at both for FastNoiseLite. Which to use is open question 1.

### Climate and shape

Per lattice column (every 4 blocks), five fBm fields after one shared domain warp: continentalness `c`, erosion `e`, weirdness `w`, temperature and humidity, each roughly -1 to 1. Temperature and humidity only choose biomes; the other three also shape the terrain, as in Minecraft 1.18. Shape is a fixed composition of one-dimensional curves (cubic Hermite splines from the parameter file), simpler than Minecraft's nested splines and easier to tune by hand:

```cpp
const float pv = 1.0f - std::abs(3.0f * std::abs(w) - 2.0f);   // peaks and valleys: -1 where w = 0
const float land = inland(c);                                    // 0 at sea, 1 inland
const float rough = relief(e);                                   // 1 where erosion is low
const float height = continent(c) + land * (rough * peaks(pv) + valleys(pv));    // above sea level
const float squash = flat_squash + rough * (rough_squash - flat_squash);          // density per block
const float jag = land * rough * jaggedness(pv) * std::max(jagged_noise, 0.0f);   // extra on peaks
```

Valleys follow the zero crossings of weirdness, which is how Minecraft's rivers wind. Sea level is y = 0 by default, so y reads as height above the sea. One column of chunks needs 81 lattice columns of climate, about 2.6 KiB; each worker keeps recent columns in its own scratch memory (RFC 0005), so stacked chunks reuse them and nothing is shared or locked.

### Density, caves and the lattice

Density is evaluated at the corners of 4 by 8 by 4 cells, Minecraft's size, and interpolated between them. A chunk has 9 by 5 by 9 corners plus one layer of cells above it (the apron, 8 blocks, for the surface stage): 486 points, 67 times fewer than blocks.

```cpp
float terrain = squash * (sea_level + height + jag - y) + base3d(x, y, z);   // > 0 is solid
float cave = std::min({std::max(cheese, pillars), spaghetti, noodle});       // < 0 is open
float density = std::min(terrain, cave);
```

- `base3d` is fBm of amplitude 1, so terrain can rise above or hang over its curve height by up to `1 / squash` blocks: plains (large squash) follow the curves, mountains (small squash) get cliffs and overhangs.
- **Cheese:** a 3D field minus a threshold, biased toward solid near the surface, so caverns open underground and not in fields. **Pillars:** a vertically stretched field that leaves stone columns standing in caverns.
- **Spaghetti:** `max(|a|, |b|) - r` for two independent fields `a` and `b`. The zero crossings of two noises meet along curves, which become tunnels; a slow third field varies `r`. **Noodles:** the same with higher frequency and a smaller radius, switched on and off by a toggle field.
- Caves live between a depth under the surface and a floor depth, both parameters, tapered at both ends.

Interpolation is `a * (1 - t) + b * t`, with `t` a multiple of a quarter or an eighth, so `t` and `1 - t` are exact. That form keeps signs: if every corner of a cell is positive, every block in it is solid. The uniform tests below rely on it.

### Biomes and surface

A biome is a box in (temperature, humidity, continentalness, erosion, weirdness, depth) plus its blocks and features, listed in the parameter file. Each 4 by 4 by 4 cell takes the nearest box (distance zero inside; ties go to the earlier biome), as in Minecraft's multi-noise biomes, looked up with a hashed offset of up to 2 blocks per column so borders do not follow the cell grid. Depth below the surface is in the lookup though no M4 biome uses it, so cave biomes later are data, not code.

The fill stage interpolates density to blocks: solid becomes stone (deep stone below a dithered band), open becomes air. The surface stage then walks each column down from the apron: the first solid block under air becomes the biome's top block (sand or gravel below sea level), and the blocks below it, down to the biome's filler depth plus 2D noise, become filler. Filler depth is at most 8 blocks, the apron's height. An overhang's top is ground like any other.

### Features across chunk borders

Two steps, and only the second looks at other chunks:

1. **Plan.** Each chunk lists the placements whose origin lies inside it: position, feature, and a 32-bit seed for the feature's own stream. Ores are drawn per chunk inside their depth bands. Trees get one candidate per 4 by 4 cell of columns; a candidate becomes a tree where the interpolated column holds the biome's top block with air above (at the chunk's top layer, the apron answers). Decisions read only this chunk's lattice and biomes, never other features.
2. **Build.** A chunk gathers the placements from its own plan and the 26 around it whose bounds reach it, sorts them by (feature order in the file, origin, index), and writes the blocks of each that fall inside the chunk, under per-feature replace rules: ores replace stone, logs replace air and leaves, leaves replace air.

Every chunk applies the same placements in the same order, so a tree split across four chunks is the same tree in each, whatever order they were built in. A feature reaches at most 32 blocks from its origin, so 26 neighbors always suffice. Minecraft's model, writing into neighbors, depends on order and needs the neighbors' blocks in memory; recomputing the neighbors' placements inside every build costs 26 extra lattices per chunk. A plan is that work done once and kept: about 3 KiB instead of a 64 KiB block array.

### Uniform chunks

| Case | Test | Skips |
|---|---|---|
| Out of reach | Above the highest possible terrain plus the tallest feature, or below the deepest cave, ore and terrain variation. Bounds come from the parameters once; each curve segment is bounded by its Bezier control points, so they are conservative by construction. | Everything: no plan, no neighbors |
| Out of the column's reach | The same test with the column's own highest surface and smallest squash, from stage 1 | The lattice; the plan is empty |
| Uniform lattice | All 486 corners have one sign, and no gathered placement reaches the chunk | The block array and `from_dense` |
| Mixed | | Nothing |

A column's surface range spans `sea_level + height + jag` plus or minus `1 / squash` over its 81 lattice columns, raised by the tallest feature that can reach it from the column or its 8 neighbors, and rounded out to whole chunks. Caves start at a parameter depth below the surface and taper there, so with the default preset none opens to the sky; a preset that lets them open widens the range by their depth.

All three need a uniform `ChunkStorage` of any state, not only air. RFC 0002's API has only the default constructor and `from_dense`, so this RFC adds `ChunkStorage::uniform(BlockState)`.

### The taste loop

- **One list.** `visit(TerrainParams&, Visitor&)` names every tunable value with its range and the earliest stage it affects. The file reader, the file writer and the overlay are visitors, so a new parameter is one line.
- **Sliders.** The overlay (RFC 0001's Dear ImGui) has a section per group: a slider per value, a small plot per curve with draggable points, the seed with a "new seed" button, and save, save as, load and revert.
- **Instant regeneration.** A change publishes a new immutable snapshot with a new version, and streaming (RFC 0006) regenerates from the earliest affected stage, nearest the camera first: a surface value rebuilds without replanning; a climate value also clears the column caches. While a slider moves, the 9 by 9 columns around the camera regenerate continuously, newest value wins; on release the rest of the view follows. Results from older versions are dropped.
- **Map view.** Continents are thousands of blocks across, wider than any view, so the overlay also draws a top-down map (height, climate or biome, 16 blocks per pixel by default) from stage 1 alone, redrawn on every change.
- **The price.** The overlay shows the shares of out-of-reach, uniform and mixed chunks, plan and build times, and memory per chunk, so taller mountains or deeper caves show their cost while being tuned.
- **The file.** `assets/core/terrain/default.toml`, in the repository. It is read with toml++, which RFC 0004 brings for block files, and written by a small writer of our own that puts each value's range in a comment and prints floats to 9 significant digits, which round-trips every `float`. It also keeps camera bookmarks (seed, position, direction) for comparing looks. Blocks are named by key (`core:stone`) and resolved through the registry on load; problems are reported in RFC 0004's file, line and column format. The golden hashes are computed from this preset, so a new look is a reviewed diff.

### API sketch

```cpp
namespace vx {

inline constexpr int cell_xz = 4, cell_y = 8;                                  // blocks per lattice cell
inline constexpr int lattice_volume = 9 * 6 * 9;                               // corners, apron included

enum class Fill : std::uint8_t { air, solid, mixed };                          // before features

struct Placement {
    BlockPos origin;
    std::uint16_t feature;   // index into TerrainParams::features, also the order features apply in
    std::uint16_t variant;
    std::uint32_t seed;      // the feature's own stream: height, shape, gaps in the leaves
};

struct ChunkPlan {
    ChunkPos pos;
    std::uint32_t version;                        // of the seed and parameters it was made with
    Fill fill;
    std::array<float, lattice_volume> density;
    std::array<std::uint8_t, 512> biome;          // per 4 by 4 by 4 cell
    std::vector<Placement> placements;            // origins inside this chunk, sorted
};

// Immutable and shared by all workers; mutable state lives in each worker's GenScratch.
class TerrainGenerator final : public Generator {   // RFC 0006's interface
public:
    TerrainGenerator(std::uint64_t seed, std::shared_ptr<const TerrainParams> params);

    // RFC 0006's terrain disc: the chunks of a column where anything solid can meet the sky,
    // including features that reach it from its 8 neighbors (a crown leaning over from a
    // higher column). From stage 1 alone; never narrower than the truth. A job.
    [[nodiscard]] ChunkRange surface_range(ColumnPos column, GenScratch& scratch) const override;

    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override;

    // Pass 1: needs nothing from other chunks.
    [[nodiscard]] ChunkPlan plan(ChunkPos pos, GenScratch& scratch) const override;

    // Pass 2: the 27 plans around and including the chunk, ordered like local_index over
    // 3 by 3 by 3; null for a neighbor with no plan: out of reach, or above its column's
    // surface range (RFC 0006). Reads no other chunk's blocks.
    [[nodiscard]] ChunkStorage build(std::span<const ChunkPlan* const, 27> plans, GenScratch& scratch) const override;
};

} // namespace vx
```

## Alternatives considered

- **Writing into neighbors** (Minecraft's features step, and its older 2 by 2 population): the result depends on generation order, and neighbors must be in memory, writable, at the right stage.
- **Large generation units** (Luanti's mapchunks, 125 mapblocks at once): borders become rare, but every request generates far more than a fast camera needs next, and the unit still needs a rule at its own border.
- **Carver caves** (random walks, in Minecraft and Luanti): a tunnel starting in one chunk runs through many others, so every chunk would gather starts from a wide 3D neighborhood. Noise caves are a function of position. Carvers with a bounded reach can come later.
- **Terrain from biomes**, blended at their borders (Minecraft before 1.18): seams and blending cost; 1.18 dropped it.
- **Minecraft's nested splines:** more expressive than six curves, far harder to tune by hand.
- **Fixed-point noise:** deterministic without float rules, but slower and awkward with float parameters. The float rules suffice, and the golden tests prove it.
- **FastNoise2 or FastNoiseLite:** see Noise and open question 1.
- **Parameters as C++ constants:** no file to parse, but every idea costs a rebuild, which is what the taste loop exists to avoid. **JSON:** no comments, and RFC 0004 already chose TOML for block files.

## Performance budget

Targets on the reference machine, release build, one thread, measured with nanobench over a fixed set of 256 chunk positions from the default preset and three seeds:

| Measure | Target |
|---|---|
| 3D noise, 4 octaves, per sample | 25 ns |
| Climate for one column of chunks (81 lattice columns) | 40 µs |
| Out-of-reach test | 1 µs |
| Plan, chunk out of its column's reach | 5 µs |
| Plan, any other chunk | 100 µs |
| Build, uniform chunk that no placement reaches | 5 µs |
| Build, mixed chunk, `from_dense` included | 300 µs |
| Plan in memory | 4 KiB |
| Scratch per worker | 1 MiB |
| Taste loop: 9 by 9 columns around the camera regenerated, all workers | 100 ms |

What that buys: at RFC 0002's example view, flying at 64 blocks per second brings about 2,000 chunks into view each second. If 40% are mixed, as RFC 0002 assumes, generating them costs about a third of one performance core, which leaves room for meshing (RFC 0003) and for flying several times faster. Caves and ores make most chunks in the underground band mixed, so the real share depends on how deep they go (open question 2); the benchmark flythrough (RFC 0006) measures it and the real throughput.

## Test plan

- **Golden hashes.** For three seeds and 64 chunk positions (surface, caves, sky, deep stone, negative coordinates, and positions at 2^20 and 2^30), a golden file holds two hashes per chunk: the blocks, hashed by state key (`core:stone`) rather than id, since RFC 0004's ids shift when a block is added, and the fields, the raw bits of lattice and climate floats, which catch float drift before it changes a block. CI checks them on Linux with GCC and Clang; the reference machine checks Apple Clang on arm64 and an x86-64 build under Rosetta 2 until a macOS job exists. Updating them takes an explicit flag and a reason in the pull request.
- **Surface range.** Over the golden positions and random columns, including trees at column borders, no solid block under open sky lies outside `surface_range`.
- **Controls.** An arm64 build with `-ffp-contract=fast` must fail the field hashes, and a build that applies features in reverse order must fail the region test. A check that cannot fail is not a check.
- **Thread count.** A 9 by 9 by 9 region generated in RFC 0005's deterministic single-threaded mode, and with every worker in shuffled order, gives identical hashes per chunk, also under ThreadSanitizer.
- **Purity.** Plans and builds repeated in another order, with cold and warm column caches, are identical.
- **Seams.** Lattice points shared by neighbors are bit-identical. A 3 by 3 by 3 region built chunk by chunk equals the same region built as one 96^3 array with every placement applied in order.
- **Uniform paths.** At every benchmark position, the out-of-reach and uniform answers equal a full build. Curve bounds never fall below a dense sampling of the curve.
- **Noise.** SIMD equals scalar bit for bit; values stay within -1 to 1 with no NaN; smoothness at 2^30 blocks is within 10% of the origin's.
- **Parameter file.** Save, load and save again gives identical bytes; unknown keys warn; missing keys take defaults; out-of-range values are clamped with a warning.
- **By eye.** A pull request that changes the look includes before and after screenshots from the preset's bookmarks.

## Pull request plan

1. This RFC.
2. Random streams (`mix64`, `salt`, `stream_seed`, `Rng`) with known-answer tests; `-ffp-contract=off` and a no-fast-math check in the CMake helper, with the interpolation control.
3. Noise: scalar OpenSimplex2 in 2D and 3D, fBm and the lattice call, with range, smoothness and far-coordinate tests and the first benchmarks.
4. Noise: the 4-wide path for NEON and SSE2, bit-equal to scalar, with before and after numbers.
5. Curves with Bezier bounds, `TerrainParams`, the visitor, reading the preset with toml++ and writing it back, and the default preset.
6. Climate and shape with the per-worker column cache, `surface_range`, and the map view: the first look at continents.
7. Density, interpolation and fill; the out-of-reach and uniform paths; `ChunkStorage::uniform`; the golden hashes start here.
8. Caves: cheese, pillars, spaghetti and noodles.
9. Biomes and surface rules.
10. Plans, placements and the gather build, with ores and trees; the region and thread-count tests.
11. Replacing RFC 0006's placeholder generator, with flythrough numbers: the M4 milestone image.
12. Overlay sliders, curve editors, save and load, live regeneration around the camera, and the price panel.

## Open questions

1. **Noise: our own OpenSimplex2, or FastNoise2.** Recommendation: **our own**. FastNoise2 in strict mode is deterministic and five times faster than FastNoiseLite, but its `float` positions make noise measurably rougher millions of blocks out, while RFC 0001 tests at ten million blocks and RFC 0002 promises the whole 32-bit range. FastNoiseLite keeps precision at a fifth of the speed. Our own costs two pull requests and must reach 25 ns per sample; if the 4-wide port cannot, FastNoise2 in strict mode is the fallback, and terrain quality falls off past about a million blocks.
2. **How tall and how deep (taste).** Minecraft 1.18 builds from y -64 up to 319 with the sea at 63: about 256 blocks above the sea and 128 below. Recommendation: a default preset with peaks up to about 256 blocks above the sea and caves and ores down to 256 below, then solid stone forever, so the interesting band fits RFC 0002's 512-block loaded height. Deeper caves mean more mixed chunks underground, each costing memory and generation time; taller or deeper is a slider away, and the price panel shows the cost.
3. **The first biomes (taste).** Recommendation: eight, as data: ocean floor, beach, plains, forest, taiga, desert, snowy plains and stony peaks. They need 15 blocks from RFC 0004's `core` pack: stone, deep stone, dirt, grass, sand, sandstone, gravel, snow, coal, iron and gold ore, and logs and leaves for oak and spruce.
4. **Oceans before M7 (taste).** Recommendation: **dry basins** until M7 brings water and its rendering. Sea level and underwater surfaces (sand, gravel) exist from M4, so coasts read correctly the day water arrives. Filling the sea now with a placeholder block shows coastlines earlier, at the price of a block RFC 0003 cannot draw well yet, and of water walls where caves meet the sea, since aquifers are M7's.

## As built

Not built yet.
