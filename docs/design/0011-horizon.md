# RFC 0011: Horizon

| | |
|---|---|
| **Status** | Accepted (2026-10-19) |
| **Milestone** | M9 Horizon, designed after M3 and before M4 Terrain |
| **Issue** | #333 |

## Summary

How Perdix shows terrain 16 kilometers away. Past the near terrain the world is drawn in rings of coarser chunks: the same 32 by 32 by 32 chunk at every level, its cells 2, 4, 8, 16 and 32 blocks wide, so each ring costs about what the full-detail terrain does and the view's cost grows with the number of rings, not with its area. The generator builds coarse chunks directly; a chunk is swapped for its finer or coarser version only when the replacement is ready, so nothing opens up; a far fog ends the world; and occlusion culling skips what hills hide, on the CPU, because Apple's Vulkan drivers cannot build draws on the GPU cheaply. The headline target, set on 2026-10-09: **16,384 blocks of view distance at 60 frames a second on the reference machine.**

## Motivation

Full detail costs the square of the distance. At today's 1,024 blocks, RFC 0006 draws about 3,200 chunk columns and RFC 0007 about 854,000 quads a frame. The first benchmark flythrough (2026-10-09) measured that frame at 22 to 24 ms on the reference machine, held by the GPU, with most of the machine's swap in use; a run on 2026-10-11 measured 4.6 ms at the median in `hover`. The second pull request settles which holds. Full detail to 16,384 blocks would be 256 times the chunks: no machine draws that.

Most of that detail cannot be seen. At 2560 by 1440 with RFC 0001's 70-degree vertical field of view, a block at distance d covers about 1,028 / d pixels: 4 pixels at 256 blocks, 1 pixel at 1,024, a sixteenth of a pixel at 16,384. Cells that cover a few pixels look the same as blocks there and cost a sixteenth, a sixty-fourth, less.

The terrain generator (RFC 0008) is accepted and not built. Far terrain cannot come from full-detail generation: a 16,384-block radius is about 2.5 million Perdix chunks, hours of work. Every engine that reaches such distances samples its generator at coarse resolution (Cubyz, Distant Horizons since 3.3, FarPlaneTwo). Designing that path now, before M4, means RFC 0008's generator is built for it rather than retrofitted, which is why this RFC comes before M4 (decided 2026-10-09).

## Goals and non-goals

Goals:

- A view distance of 16,384 blocks at 60 frames a second, as the flythrough measures it (budget below).
- One chunk format and one draw path at every level: RFC 0002's storage, RFC 0003's mesher, RFC 0007's arena and multi-draw runs.
- No hole and no crack where levels meet, and no swap that shows less than what it replaces.
- Coarse terrain anchored to the world: a place has the same coarse chunk wherever the camera is, so nothing shimmers as the camera moves.
- The near cylinder (RFC 0006: 8 chunks around the camera) always at full detail.
- Far terrain generated directly at its level, fast enough that the whole view fills from nothing in a few seconds.
- Builds visible from afar once edits exist (M5): an edited chunk updates its coarser versions.
- Occlusion culling that never removes a visible chunk, with a test that proves it.
- A quality setting, the size of a cell on screen, and a fog that ends the view at any distance.

Non-goals:

- Mesh shaders and ray tracing: neither runs on the reference machine's drivers (ADR 0002).
- Smooth far terrain, impostors and baked panoramas: the far terrain stays blocks.
- Light at coarse levels beyond what M6 gives full-detail chunks: M6 carries its light up the levels.
- Water and translucency at coarse levels: M7.
- Saving coarse chunks: M8 stores the edited ones; everything else regenerates.

## Prior art

Read for this RFC from the projects' own sources; the research memo is in the vault (2026-10-09).

- **Cubyz** (Zig, GPL-3.0, ideas only), the closest: 32 by 32 by 32 chunks at every level with cells 1 to 32 blocks; one mesher for all levels; the generator run at the chunk's cell size, with caves and ores gated by level and caves dilated by a cell so they widen rather than vanish; a parent replaced only when all 8 of its children are meshed; each chunk's border faces built twice, against the neighbor at its own level and against the coarser one, the right set chosen each frame; edits passed up the levels (solid beats air, exposed beats buried). Its maintainer measured a coarse chunk at about 5 times an average full-detail one to generate. We take the structure, the swap rule, the border sets and the upward edits.
- **Distant Horizons** 3.3.4 (Minecraft mod): columns of vertical runs at detail levels that double; generation at full detail merged upward, plus a new rough surface generator that samples the density at coarse spacing; one draw per section with CPU frustum culling and no occlusion culling; textures only to its third level, flat colors beyond; its own fog. Rings by distance: 2-block cells from 384 blocks at its default. We take the flat far colors and the coarse surface sampling.
- **Voxy** (Minecraft mod, all rights reserved, reference only): 32-cubed sections to 16-block voxels, a GPU traversal with a depth pyramid, visibility by rasterizing section boxes, draws built on the GPU; OpenGL 4.6, refuses macOS. We take the box test and its "children live only when every child is uploaded".
- **Geometry clipmaps** (Losasso and Hoppe, 2004): nested grids whose spacing doubles per ring, so every ring holds the same number of cells and costs the same; grids snapped to the coarser grid. We take the ring arithmetic.
- **Minecraft's visibility graph** (Checchi, 2014, Java 1.8 on) and Sodium's: per chunk, which pairs of faces an air path connects; a search from the camera's chunk crossing only connected faces facing away. Culls 50 to 99% in caves. We take it for full-detail chunks.
- **Horizon culling** (Downs, Akenine-Moller and Sequin, 2001; Lloyd and Egbert, 2002): terrain walked front to back keeping the highest occluder per screen column. Found in terrain renderers, not in block games; simulated for this RFC on synthetic hills, it hid 18 to 87% of the columns in view for a camera on the ground.
- **zeux/niagara** and Haar and Aaltonen (2015): two-phase GPU occlusion against a depth pyramid. The model for a later step, through instanced clusters (Occlusion culling below).
- **Minecraft 26.2's fog**, read from the jar: environmental fog from 0 to 1,024 blocks, a band of render-distance fog at the view's end, and a 0.75-second fade from the fog color for each new section.

## Design

### Levels and coarse chunks

A **level-L chunk** is RFC 0002's 32 by 32 by 32 chunk whose cells are 2^L blocks wide: it spans 32 x 2^L blocks a side. Level 0 is today's chunk. Its position is its chunk position at its level, `floor(block / (32 x 2^L))` on each axis, so a level-L chunk holds exactly the 8 level-(L-1) chunks inside it, and the grid of every level is fixed in the world. Six levels, 0 to 5 (cells up to 32 blocks), reach 16,384 blocks at the default quality.

`ChunkStorage` stores coarse chunks unchanged: a cell is a block to it. A coarse cell takes one block state, the one the generator or the downsampling (below) chooses.

### Rings: which level where

A coarser level takes over where its cells shrink to **k pixels** on screen, k being the quality setting. With h the framebuffer's height in pixels and the vertical field of view θ, a cell of 2^L blocks at distance d covers 2^L x h / (2 d tan(θ / 2)) pixels, so level L starts at

r_L = 2^L x h / (2 k tan(θ / 2))

which at 1440 rows and 70 degrees is 1,028 x 2^L / k blocks. Within its ring a level's cells shrink from k pixels at the inner edge to k / 2 at the outer edge.

| k | Full detail to | Levels to 16,384 | Chunk columns, all levels | Drawn chunks | Quads drawn | Draw commands |
|---|---|---|---|---|---|---|
| 2 | 1,028 blocks | 5 | about 13,800 | about 5,900 | about 2.4 million | about 10,300 |
| 4 | 514 | 6 | about 4,300 | about 1,900 | about 0.74 million | about 3,250 |
| 8 | 257 | 7 | about 1,500 | about 620 | about 0.25 million | about 1,100 |
| today, full detail to 1,024 | 1,024 | 1 | about 3,300 | about 1,600 | 854,000 | about 2,800 |

The estimates assume 1.5 coarse chunks per column in the terrain band, today's 400 front-facing quads per drawn chunk, the flythrough's 102-degree horizontal view and RFC 0007's 1.75 commands per chunk. At k = 4 the whole 16,384-block view costs about what 1,024 blocks of full detail cost today, and each further doubling of the distance adds one ring, about 16% more.

- **Distance is three-dimensional,** to the nearest point of the chunk's box: cubic chunks let a flying camera see coarser ground below it. The near cylinder stays at level 0 whatever the distance says.
- **Neighbors differ by at most one level** (2:1 balance). The ring arithmetic gives it everywhere but at the corners of rings, where the finer level is kept a little longer.
- **Hysteresis of one chunk of each level,** as RFC 0006's for loading: moving back and forth across a ring's edge swaps nothing after the first time.
- **The rings follow the framebuffer's height and the field of view,** so a smaller window draws coarser chunks at the same distance, and costs less.

### Building coarse chunks

The generator interface (RFC 0006 and RFC 0008) gains a level: `surface_range`, `plan` and `build` take it, and a level-0 call is today's. At level L the generator samples its fields at cell centers, 2^L blocks apart, and:

- **Band-limits its noise:** octaves whose wavelength is under two cells are left out. They would alias into speckle, and leaving them out is what makes a coarse chunk cheap.
- **Skips caves from level 2 on:** RFC 0008's default caves do not open to the sky, so from that far none would show. At level 1 it dilates them by a cell, as Cubyz does, so they widen rather than vanish.
- **Draws trees as canopy cells** on a coarse grid from level 2, each grid cell's chance raised so a forest keeps its density, as Cubyz does, and **makes leaves opaque** past level 0: the cutout pass draws full detail only.
- **Answers `surface_range` per coarse column,** never narrower than the truth, as RFC 0006 asks of level 0. The horizon test reads the same ranges.

A coarse chunk is a pure function of the seed, the parameters and its position and level, so it is the same on every thread and machine. It is not the downsampled full-detail terrain, and the difference shows as a pop when levels swap; the test plan measures it on the placeholder hills now and on RFC 0008's terrain later. Until RFC 0008 lands, the placeholder hills (RFC 0006) gain a coarse path: each cell's height at its column's center, the finer octave dropped once its wavelength is under two cells.

**Edits travel up** (with M5): an edited chunk rebuilds its parent from the parent's 8 children, generated or edited, and so on up to level 5. A coarse cell takes the block of its 8 children that is solid rather than air, and among solid ones the one with the most faces open to air, ties broken by a hash of the position so patterns survive. Coarse chunks that hold an edit are kept, and saved with M8; the rest regenerate whenever they are needed.

### Meshing coarse chunks

RFC 0003's mesher meshes a coarse chunk unchanged: its quads are in cells. The scale reaches the vertex shader in the fourth component of the chunk's origin (RFC 0007's per-chunk float4, today 0), so the 8-byte quad and the arena stay as they are.

- **Textures repeat per block, not per cell,** so a cell of grass looks like a patch of grass blocks and the mipmaps average it. **Past level 2 (cells of 8 blocks and up, under 2 pixels at their ring's inner edge), flat colors:** the quad's texture field indexes a palette of each texture's average color, biome tint included, and the shader samples nothing. Merging then keys on the color alone and makes fewer quads. This also ends the far moire the owner saw on 2026-10-09.
- **Ambient occlusion only at level 0:** a cell's corners are too far apart for it to read as contact shadow.
- **Border faces are built twice for each side,** as Cubyz does: once against the neighbor at the chunk's own level, once against the neighbor one level coarser, sampled at half the resolution. Each frame draws, for each side, the set that matches the level actually drawn next to it. With 2:1 balance those two cases are all there are, so a chunk never waits for its neighbor's level to be meshed, and no face is hidden by a neighbor that is not drawn. The two sets are extra groups in the chunk's arena record.

### Swaps without holes

A chunk is replaced only once its replacement can be drawn. Going finer, a coarse chunk stays until all of its 8 children that have faces are meshed and uploaded, then all of them appear together and it goes in the same frame. Going coarser, the 8 children stay until their parent is uploaded. A chunk and its children are never drawn together, so nothing overlaps or fights over depth.

A swap can still pop, where coarse and fine terrain differ. Newly shown coarse chunks fade in from the fog color over 0.75 s, as 26.2's chunks do: an opaque blend, with no `discard`, which would cost Apple's hidden-surface removal. Swaps far away are mostly fog-colored already.

### Streaming the levels

RFC 0006's streamer keeps each level's chunks as it keeps level 0's: records in the chunk map keyed by position and level, the terrain band per coarse column, plans before builds, a chunk complete when its neighbors at its level and the coarser neighbors its border sets read are ready. Each level's region is its ring, a hollow disc between r_(L-1) and r_L grown by one chunk of the level for the margin. Backlogs rank by the chunk's size on screen, largest first, so the nearest and the coarsest arrive early and the view fills from coarse to fine.

RFC 0006's memory budget counts every level. Under pressure the outermost ring shrinks first, then the inner ones, so the view keeps its full depth longest at the horizon's coarsest level. At k = 4 the estimate is about 5,100 coarse chunks beside the full-detail ones: 20 to 80 MiB of block data, 20 to 40 MiB of meshes.

### Drawing the levels

RFC 0007's arena holds every level's meshes; a record gains its level, and the draw-list builder culls a coarse chunk as a box 32 x 2^L blocks wide. Face-direction culling holds at every level, conservative by one cell. Origins are computed in 64-bit integers from the camera's block, as now, so a coarse chunk 16 km away is as exact as a near one. The opaque pass draws every level; the cutout pass only level 0.

### Occlusion culling

Before any of this is chosen for good, the second pull request measures where today's frame goes (budget below): PR #296 drew 160,000 quads as one instanced draw in 0.56 ms of GPU time, so a 22 ms frame, as the first flythrough measured, is not explained by its quad count, and culling only helps if quads are what costs.

Apple's two Vulkan drivers run every multi-draw command as a Metal draw encoded on the CPU, and both emulate the draw count and conditional rendering with a compute pass that splits the render pass and still encodes every potential draw. GPU-driven culling that writes draw commands, the usual way, is therefore slow here. This RFC culls on the CPU first:

1. **Horizon test over coarse columns.** Each frame the columns in view are walked front to back in azimuth bins, each bin keeping the highest elevation angle its columns' lowest surface reaches; a column whose highest surface, plus a margin for trees, stays under every nearer occluder in its bins is skipped. Occluders are under-estimated and tested columns over-estimated, so nothing visible goes. It reads the coarse surface ranges streaming already has. It does nothing for a camera above the terrain, and is sound only while caves do not open to the sky (RFC 0008's default).
2. **Visibility graph for full-detail chunks** (Checchi 2014): the mesher records which pairs of the chunk's faces an air path connects (15 bits); a search from the camera's chunk crosses only connected faces that face away from the camera. It culls caves and buildings, where the horizon test cannot.

If the measurements say the GPU still spends too much after these, a later pull request moves culling to the GPU through RFC 0007's open question 1: one instanced draw over fixed-size clusters whose instance count a compute pass writes, after a box test against last frame's depth. One draw carries the whole visible list, with no empty commands; it costs a render-pass split and a less standard vertex shader. Rejected for this hardware: hardware occlusion queries (a draw per query, results one to three frames late, open MoltenVK bugs) and the emulated draw count.

### Fog and the far edge

26.2's clear-weather fog is solid fog color from 1,024 blocks on, whatever the render distance: its environmental fog runs from 0 to 1,024 blocks. Copying that ends the world at 1,024 blocks. So the fog keeps 26.2's shape and colors and scales its distances with the view distance: the render-distance band at the view's end, D - clamp(D / 10, 4, 64) to D in 26.2, and the environmental haze's end stretched from 1,024 blocks to the view distance (open question 3 offers the alternatives). The queued fog RFC builds the fog; this RFC asks it not to fix 1,024 blocks.

### Settings and overlay

The view distance slider reaches 512 chunks (16,384 blocks); the quality is k, from 2 (sharp) to 8 (fast). The overlay shows, per level, the chunks resident and drawn, quads and commands, the ring radii, swaps waiting, and the chunks each culling step removed.

### API sketch

```cpp
namespace vx {

// A chunk of any level: its position on its level's grid, cells 2^level blocks wide.
struct LodPos {
    std::int32_t x = 0, y = 0, z = 0;
    std::uint8_t level = 0;   // 0 is RFC 0002's chunk; up to 5
};

[[nodiscard]] constexpr std::int32_t cell_blocks(std::uint8_t level) noexcept { return 1 << level; }
[[nodiscard]] LodPos parent(LodPos pos) noexcept;
[[nodiscard]] std::array<LodPos, 8> children(LodPos pos) noexcept;

// Which level is drawn where (pure functions, for the streamer and tests).
struct LodRings {
    double pixels_per_cell = 4.0;     // k, the quality setting
    std::uint32_t framebuffer_height = 1'440;
    float fov_y = 1.2217305F;         // 70 degrees
    std::int32_t view_distance = 512; // chunks of level 0
    std::int32_t near_distance = 8;   // RFC 0006's near cylinder, always level 0

    [[nodiscard]] double radius(std::uint8_t level) const noexcept;   // where the level starts, in blocks
    [[nodiscard]] std::uint8_t level_at(const Vec3d& eye, LodPos chunk) const noexcept;
};

// RFC 0006's Generator, with a level; level 0 is today's.
class Generator {
public:
    virtual ~Generator() = default;
    [[nodiscard]] virtual ChunkRange surface_range(ColumnPos column, std::uint8_t level,
                                                   GenScratch& scratch) const = 0;
    [[nodiscard]] virtual std::optional<BlockState> out_of_reach(LodPos pos) const noexcept = 0;
    [[nodiscard]] virtual ChunkPlan plan(LodPos pos, GenScratch& scratch) const = 0;
    [[nodiscard]] virtual ChunkStorage build(std::span<const ChunkPlan* const, 27> plans,
                                             GenScratch& scratch) const = 0;   // plans of one level
};

} // namespace vx
```

## Alternatives considered

- **Columns of vertical runs** (Distant Horizons): compact, and it reaches 65,536 blocks with a few hundred plain draws. But it is a second data model and mesher, overhangs turn into walls under its slice caps, caves need special cases, and heights are capped: everything cubic chunks avoid.
- **A heightfield clipmap beyond a few thousand blocks** (Veloren): the cheapest far geometry, a constant grid per ring. But it is a second renderer, smooth silhouettes looked out of place next to blocks in Voxel Farm's tests, and builds would not show.
- **Downsampling generated full-detail chunks only** (Distant Horizons' first path, Voxy): exact, and builds show. But the whole view must be generated at full detail first: hours for 16,384 blocks.
- **Skirts or border walls left on** (Distant Horizons, Cesium): no neighbor knowledge needed, but extra quads along every border, depth fighting, and broken water.
- **Dithered cross-fades between levels:** smooth, but `discard` stops Apple's hidden-surface removal for everything it touches.
- **Geomorphing** (CDLOD, the 0fps blocky variant): no pops at all, at the price of a mesh layout built for it.
- **Error-driven refinement** (Voxel Farm): flat land stays coarse, cliffs refine; fewer chunks, but pops that differ by place. A refinement for later, measured against rings.
- **GPU-driven traversal and draw counts** (Voxy, Cubyz): the usual high end, emulated on both Apple drivers with a render-pass split and every potential draw encoded.
- **Masked software occlusion** (Intel's library, Dagor's NEON port): general and fast, but it needs occluder triangles extracted from the voxels; the horizon test does that job for terrain with numbers streaming already has.

## Performance budget

On the reference machine, release build, at 2560 by 1440, k = 4 and a view distance of 512 chunks (16,384 blocks), measured by flythrough version 2 (this RFC's), which adds a phase at the full distance:

| What | Target |
|---|---|
| Frame time in `hover` and `cruise` | p99 under 16.7 ms (60 fps), including after the 10-minute run, since the reference machine has no fan |
| Quads drawn | under 1 million |
| Draw commands, both passes | under 4,000 |
| The whole view from nothing (`load`) | under 5 s |
| A coarse chunk generated, per worker | under 2 ms (Cubyz measured 5 times a full-detail chunk; RFC 0008 budgets 400 µs for a mixed one) |
| Horizon test, main thread | under 0.3 ms a frame |
| Visibility graph search | under 0.5 ms a frame |
| Mesh memory | under 256 MiB |
| Chunk data | within RFC 0006's 512 MiB budget |
| Holes within 256 blocks during `sprint` | none at p99, as RFC 0006 asks now |

The frame at 1,024 blocks measured 22 to 24 ms in the first flythrough and 4.6 ms at the median in a later run. Coarse levels keep the far terrain within today's quad count, but the target also needs the near frame down: which is why the measurements come first.

## Test plan

- **Rings (unit):** `level_at` never decreases with distance; radii follow the formula; for random cameras every pair of neighboring chunks drawn differs by at most one level; parents and children agree on every level; positions near the edge of the 32-bit world.
- **Coarse generation (unit):** chunks made on different threads in different orders are byte-identical at every level; a coarse chunk and the downsampled full-detail chunks it covers agree within one cell of height on the placeholder hills (the number measured, then held); `surface_range` at a level is never narrower than its chunks' surfaces.
- **Swaps (unit, deterministic streamer):** under random camera walks the drawn set covers the region in every frame: never a chunk and its child, never neither; a parent goes only with all its children that have faces uploaded.
- **Seams (GPU image test, both Apple drivers and lavapipe):** a seeded scene with levels 0 to 3 next to each other, seen from above and from the side, shows no background color through solid terrain, with border sets on; a control without them shows holes.
- **Culling (unit and image):** the horizon test and the graph never skip a chunk with a visible pixel (the image with culling equals the image without); controls with a planted wrong margin go red.
- **Flythrough version 2:** the budget above, with the per-level counters; CI's quick run at a small distance checks the swap invariants every sample.
- **By eye:** pops and the fade, the fog at the edge, the far colors, from the owner's seat.

## Pull request plan

1. This RFC, VISION.md's headline target, and the roadmap's order.
2. **Measure the frame:** GPU time per pass in the overlay and the flythrough, a quad-count sweep with the view distance slider, the cutout pass's share, the cost of a render-pass split and of an instanced box test on both drivers. Checks the accepted k = 4 against the numbers, and says whether culling must go to the GPU.
3. Levels: `LodPos`, parents and children, the rings from k, 2:1 balance; property tests.
4. The generator's level: the interface, the placeholder hills at any level, band-limited; determinism and agreement tests. RFC 0008's As built notes the change to its interface.
5. Meshing at a level: the scale in the origin, occlusion at level 0 only, the two border sets, flat colors past level 2.
6. Streaming the levels: records and regions per level, backlogs by size on screen, the memory budget per ring.
7. Swaps: never both, never neither; the fade from the fog color.
8. Drawing the levels: per-level draw lists, the arena's records, the overlay's counters, the slider to 512 chunks.
9. The horizon test.
10. The visibility graph.
11. The far fog's distances, with the fog RFC.
12. Flythrough version 2 and its report; the targets checked.
13. If pull request 2 or 12 says so: GPU culling on instanced clusters, with RFC 0007's open question 1 reopened.

Pull requests 2 to 8 can land on the placeholder hills before M4 (decided 2026-10-09); 9 to 12 are worth most on RFC 0008's terrain. **Order, decided 2026-10-19:** pull requests 3 to 8 come first, the owner choosing to build before measuring; pull request 2 follows them, ahead of 9, 10 and 13, the culling choices its numbers decide.

## Open questions

All settled on 2026-10-19: every recommendation below was accepted.

1. **Quality default, k** (taste: sharpness against frame rate). Recommendation: **4**, cells of 2 to 4 pixels and full detail to about 514 blocks, as a setting from 2 to 8. 2 keeps full detail to 1,028 blocks at about 3 times the quads; 8 drops full detail to the near cylinder.
2. **The far look** (taste). Recommendation: **textures to level 2, flat colors beyond**, as Distant Horizons and Luanti's draft do: cheaper, no moire at the edge, at the price of a change of look where fog mostly covers it. The other way: textures at every level.
3. **The fog at the edge** (taste). Recommendation: **26.2's fog with its distances scaled to the view distance**, which looks like 26.2's at its view's edge at any distance. The others: Distant Horizons' far fog from 0.4 of the distance (closes the view earlier, hides more), or a height fog that keeps mountain tops clear (Cubyz).
4. **Builds from afar** (game design). Recommendation: **yes**: edits travel up the levels (with M5) and those coarse chunks are saved (M8), as Cubyz does. Otherwise builds vanish past full detail.
5. **What 60 fps means** (target). Recommendation: **p99 frame time under 16.7 ms in `hover` and `cruise`, after the 10-minute run**: the fanless reference machine slows under sustained load (an M3 Air kept 73% of its GPU speed after 20 minutes in one test), so a cold run would flatter it.

## As built

Items 3 to 8 landed on 2026-10-19, built by Claude in one session with no review agents (planted bugs and property tests stood in). The app turns the levels on with `--horizon`, off by default until the fog (item 11) and the frame measurement (item 2).

- **Levels, generator, meshing, drawing** (#406 to #409): as the RFC says.
- **Grass caps** (#423): the RFC's grass capping the hills gave a column's top coarse cell the block its own top has at level 0. That top can lie up to half a cell under the ground, where it is dirt or stone: about half of every coarse level's columns showed bare tops until the full-detail ring reached them. A column's top coarse cell is now grass.
- **Streaming** (#410 to #414): one streamer per level, each single-level inside, records keyed by position on its own grid. **Full discs, not rings:** distance was three-dimensional at first, so a camera high up drew coarse chunks straight below it, where a ring cut by horizontal distance would have had a hole. Levels now go by column (below), so a ring would do. Each level streams `LodRings::stream_distance(level)` of its own chunks: the nearer of the view's bound and its parent's ring plus a chunk's diagonal and the rounding of the camera's chunk, property-tested against `lod_cover` (19 chunks per level at the defaults, 18 at the top). A ring is the later saving. Backlogs rank by each level's own chunk distance, its size on screen; each pass's room for jobs is shared between the levels in turn, so none starves.
- **Swaps** (#415 to #417): the world thread picks the chunks to draw once a frame (`lod_select`), from what it knows of each level: a chunk is ready once its mesh is sent, or has nothing to draw once its column's band is known. Where the target cannot be drawn yet, **what was drawn there before stays** if all of it still can be, not only the parent, so a camera that moves two levels in one step opens no hole. There is no pruning by coarse bands: the placeholder's coarse surface ranges are its cells', and the full-detail terrain can rise past them. The main thread applies each swap whole once every chunk it shows has a mesh in the arena. Shown belongs to the chunk, so a mesh that turns empty and back keeps it.
- **Levels and swaps by column** (#424, after flicker found in play): the RFC picks each chunk's level by the three-dimensional distance to its box and swaps a chunk at a time. A coarse cell is solid when its middle is under the ground, so a level's surface lies up to half a cell under the finer one's, and where the ground is just above a row's bottom it ends in the row below. That chunk's children are all empty, so it went at once, while the row above, empty at the coarse level, waited for its children with the finer surface: for a frame or two neither level drew the ground. Flying at view distance 512, 6 frames in 30 seconds showed bare ground within 300 blocks. The swap test above passed, because it counts a box whose children are empty as covered. Now a chunk's level is its column's, from the distance across, and the near cylinder is a disc of columns at level 0 at every height; `lod_select` walks columns, and a column gives way to its 4 children once all their chunks between the rows can be drawn, so every row of a column changes level in one swap. A high camera draws the ground below it at full detail within level 1's ring, as one on the ground does. A property test on terrain-like faces guards it (one surface row per column per level, the coarse ones up to half a cell lower, meshes arriving and some leaving for a step, a wandering camera): ground drawn stays drawn. The old swaps lost it 105 times there; a flight probe on the fix found no bare ground out to 1,024 blocks.
- **Border sets, on the coarse side** (#418, reverted by #419; then #421): the RFC has the finer chunk at a level border make a second set against the coarser chunk across. Those faces never show: rings are by distance, so the camera is always on the finer chunk's side of the face between two levels (a property test over 1,000 such faces, ground and high cameras). The faces that can show are the coarser chunk's toward the finer region, which its main mesh made against its coarse neighbor's cells. So each chunk above level 0 with faces meshes six **open sets**, its outermost slice toward each side as if nothing were drawn across, and draws a side's open set in place of that slice where its own level's neighbor is not drawn. No cross-level pins, no waiting on another level. In the headless horizon render it closes every crack: 3 pinholes before, none after.
- **512 chunks** (#420): `--view-distance` up to 512 with `--horizon`, the overlay's slider likewise; the overlay shows each level's meshes held and shown.

Still to come: the frame measured (item 2), the horizon test and visibility graph (9, 10), the fog (11), flythrough version 2 (12), GPU culling if needed (13).
