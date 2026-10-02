# RFC 0003: Chunk meshing

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M2 First Chunk |
| **Issue** | #16 |

## Summary

How a chunk of blocks (RFC 0002) becomes GPU geometry: which faces are visible, how neighboring faces merge into larger quads, the 8-byte packed quad that the vertex shader expands (RFC 0001's vertex pulling), per-vertex ambient occlusion, and the speed and size budget per chunk.

## Motivation

Meshing is the engine's main CPU cost while flying (every chunk that comes into view) and while building (every edit). Its output is the main GPU memory and bandwidth cost at long view distances. Both scale with view distance, so both are budgeted here.

How much the approach matters, for one 32 by 32 by 32 chunk:

| Approach | Faces emitted |
|---|---|
| Every face of every solid block | up to 196,608 |
| Only faces between a solid block and air | about 1,000 to 3,000 for typical terrain |
| Faces merged into larger quads | 1.3 to 1.8 times fewer with occlusion in the merge key, 2.1 to 3.5 times fewer without it (measured on two seeded terrain chunks) |
| Worst case (a 3D checkerboard, nothing merges) | 98,304 |

## Goals and non-goals

Goals:

- Emit only faces that can be seen: where a block that draws faces meets one that does not hide them, including across chunk borders.
- Merge neighboring faces that look identical (same texture, same ambient occlusion) into one quad.
- Per-vertex ambient occlusion in the style of Minecraft's smooth lighting.
- An 8-byte quad format, decoded by the vertex shader.
- Output grouped by face direction, so the renderer can skip whole groups that face away from the camera.
- A pure function with no global state and no allocation in the hot path, ready to run on worker threads (M3).
- Remeshing after an edit fast enough to feel instant.

Non-goals:

- Blocks that are not full cubes (slabs, stairs, plants, fences): a block models RFC.
- Translucent blocks and water: M7.
- Light from the sun and torches: M6. The quad keeps spare bits, and the lighting RFC decides whether light lives in the quad or in a volume the shaders sample.
- Level of detail for distant chunks: M9.
- Uploading meshes and drawing many chunks: M3.

## Prior art

- **Mikola Lysenko, "Meshing in a Minecraft Game"** (0fps.net, 30 June 2012): face culling and greedy meshing. Its **Part 2** (7 July 2012) meshes each block type and face direction separately: merging only faces with the same merge key.
- **Mikola Lysenko, "Ambient occlusion for Minecraft-like worlds"** (0fps.net, 3 July 2013): per-vertex occlusion from three neighbors, the four occlusion values in the merge key, and choosing each quad's diagonal by comparing the occlusion sums of its two diagonals.
- **cgerikj/binary-greedy-meshing**: face culling and merging with 64-bit column masks, one bit per block, instead of per-block loops.
- **Sodium** (a Minecraft renderer mod): chunk geometry grouped by facing, so faces pointing away from the camera are skipped per chunk.
- **Minecraft**: per-face culling and smooth lighting, without merging. The look to match; the cost to beat.

## Design

### Input: a padded chunk

The mesher reads a plain array of 34 by 34 by 34 states: the chunk plus a one-block border taken from its 26 neighbors. Faces need the 6 face neighbors (RFC 0002's `copy_face`); ambient occlusion at the chunk's edges and corners also needs the 12 edge and 8 corner neighbors, which are 392 single-block reads in total. A neighbor that is not loaded counts as air, and the chunk is remeshed when it arrives.

What a block looks like comes from a per-state table built by the block registry (RFC 0004). The types live in the world layer (`<vx/world/block_visual.hpp>`): the registry that fills them is world code, and the world cannot include mesh headers (ADR 0003).

```cpp
namespace vx {

enum class Opacity : std::uint8_t { air, opaque, cutout };   // cutout: leaves, glass

struct BlockVisual {
    Opacity opacity;
    std::array<std::uint16_t, 6> texture;   // texture layer per face
};

} // namespace vx
```

### Face culling with column bitmasks

For each axis, every row of 34 blocks along that axis becomes two 64-bit masks, one bit per block: `draws` (the block has faces: opaque or cutout) and `hides` (the block hides the faces next to it: opaque). A face is visible where a block that draws sits next to one that does not hide, which is two shifts and an and-not for a whole row:

```cpp
// Bit i is block i along the row, padding included (bits 0 and 33).
const std::uint64_t pos_faces = draws & ~(hides >> 1);   // next block along +axis does not hide it
const std::uint64_t neg_faces = draws & ~(hides << 1);   // previous block along -axis does not hide it
// Keep bits 1..32: faces of this chunk's own blocks, not the border's.
constexpr std::uint64_t inner = 0x1'FFFF'FFFEull;
```

With only opaque blocks the two masks are equal. Cutout blocks (leaves, glass) draw faces but do not hide a neighbor's. Whether faces between two cutout blocks of the same kind are drawn is open question 2; if they are culled, that comparison runs only on rows that contain cutouts, so all-opaque terrain pays nothing for it.

### Ambient occlusion

Each corner of a visible face looks at the three blocks next to it on the face's open side: two edge neighbors and one diagonal. Following Lysenko:

```cpp
// 3 is fully lit, 0 is the darkest corner.
constexpr int corner_ao(bool side1, bool side2, bool corner) noexcept {
    return (side1 && side2) ? 0 : 3 - (side1 + side2 + corner);
}
```

Only opaque blocks occlude. A face's four corner values, 2 bits each, become part of its merge key and of its quad.

### Merging

Within one slice (all faces of one direction at one depth), faces merge when their merge key matches: same texture layer and same four occlusion values. The merge runs on bitmasks too: take the lowest set bit of a row, extend the run along the row while the bits stay set and the keys match, then extend the run row by row while the next row holds the same run with the same key, and clear what was merged. A slice of uniform terrain becomes a handful of quads in a few dozen bit operations.

Culling and merging want rows in different directions. Culling's rows run along the face normal, one bit per depth; merging's rows lie inside a slice. Between the two, each visible face bit moves into the row of its slice. A prototype on the reference machine spent about 4 ns per face on this step, about a third of the worst case's time (98,304 faces, 0.39 ms of 1.1 ms). The same prototype measured an alternative: build only the rows along x, and get the faces in the y and z directions by and-not between neighboring x rows, which already lie inside their slices, so only the x faces need moving. That halved the fixed cost of building masks, about 25 µs per chunk whatever the chunk holds. The implementation pull request picks between the two with benchmarks.

Textures repeat across a merged quad, which works because blocks use a texture array (RFC 0001) rather than an atlas, where repeating would bleed into neighboring tiles.

Merging can be switched off at runtime in the debug overlay, to measure what it saves and to debug it.

### The quad

```cpp
namespace vx {

// One visible rectangle of faces. The vertex shader rebuilds its four
// corners; the chunk's camera-relative origin comes from per-draw data.
struct Quad {
    std::uint32_t geometry;   // x:5 y:5 z:5 | width-1:5 height-1:5 | face:3 | spare:4
    std::uint32_t material;   // texture layer:16 | AO 4 corners x 2 bits | spare:8
};
static_assert(sizeof(Quad) == 8);

} // namespace vx
```

Four 32-byte vertices with float position, normal and texture coordinates would be 128 bytes for the same face; the packed quad is 16 times smaller, and size is what limits how many chunks fit on the GPU.

Positions are local to the chunk; the chunk's origin, relative to the camera (RFC 0001), comes from per-draw data. The face's normal and its texture coordinates are derived from the face direction and the quad size in the shader.

The position is a block, 0 to 31 on each axis, so 5 bits per axis suffice: of the blocks whose faces the quad covers, the one with the smallest coordinates. For -x, -y and -z faces, that block's minimum corner is also the quad's. A +x, +y or +z face lies one block further along its normal, so the vertex shader adds 1 along the normal for those three directions. Storing the quad's own corner would not fit: the +x face of a block at x = 31 lies at x = 32. Width and height run along fixed axes per face direction:

| Face | Width along | Height along |
|---|---|---|
| +x, -x | z | y |
| +y, -y | x | z |
| +z, -z | x | y |

The four occlusion values sit in bits 16 to 23 of `material`, two bits per corner, corner 0 first. In the face's width and height axes, measured from the quad's minimum corner, corner 0 is at (0, 0), corner 1 at (width, 0), corner 2 at (width, height) and corner 3 at (0, height). The vertex shader picks the winding per face direction, so every face is front-facing from outside the block.

### Drawing a quad

Quads are drawn with one static index buffer shared by every chunk, holding the pattern 0, 1, 2, 2, 3, 0 for quad 0, then the same offset by 4 for each following quad. RFC 0007 makes the indices 32-bit, since 16-bit ones would stop at 16,384 quads per draw, and sizes the buffer for the worst case of 98,304 quads: 98,304 × 6 indices × 4 bytes = 2.25 MiB. The vertex shader finds its quad as `vertex_index >> 2` and its corner from `vertex_index & 3`, so each corner is shaded once and reused through the vertex cache.

The pattern splits every quad along the diagonal through corners 0 and 2. Where the four occlusion values are not coplanar, the two possible splits interpolate them differently, so a fixed split shades the same arrangement of blocks differently depending on which way the quad is turned: a lone dark corner stays in its own triangle under one split and smears in a stripe along the diagonal under the other. Lysenko compares the sums of the two diagonals, `a00 + a11 > a01 + a10`, and splits along the one with the larger sum, the brighter one, which keeps a lone dark corner in its own triangle. His a00 and a11 are corners 0 and 2 here:

```cpp
// In the vertex shader. ao[c] is the occlusion value of corner c, 3 fully lit.
// rotate: the diagonal through corners 1 and 3 is brighter, so every vertex
// plays the next corner, (c + 1) & 3, which moves the split onto it.
const bool rotate = ao[1] + ao[3] > ao[0] + ao[2];
```

A tie keeps the split through corners 0 and 2, where Lysenko's code takes the other one. Equal sums mean the four values are coplanar, and then both splits interpolate them identically, so a tie only has to be broken the same way every time. The index pattern stays fixed: no extra bits, no second index buffer.

### Output

```cpp
namespace vx {

enum class Pass : std::uint8_t { opaque, cutout };

struct ChunkMesh {
    // ranges[pass][face], faces in the order +x, -x, +y, -y, +z, -z.
    std::array<std::array<std::span<const Quad>, 6>, 2> ranges;
};

// Pure: reads the padded chunk and the visual table, writes into reusable
// scratch owned by the calling worker. No allocation once the scratch is warm.
ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals, MeshScratch& scratch);

} // namespace vx
```

The spans point into `scratch`, so they are valid only until the next call that uses it. A caller that keeps the quads copies them out first: RFC 0005's mesh job copies them into a result buffer before its worker takes another job.

Cutout faces (leaves, glass) get ranges of their own because they need an alpha test. Apple's GPUs handle fragments that may be discarded less efficiently in hidden surface removal, and Apple asks for them to be drawn after all opaque geometry; separate ranges let RFC 0007 do exactly that.

Grouping by face direction lets the renderer skip the ranges that face away from the camera: for a chunk entirely above the camera, its upward faces cannot be seen. That roughly halves the quads drawn, at no cost here beyond keeping twelve ranges instead of one. RFC 0007 stores the ranges in GPU memory as +x, +y, +z, -x, -y, -z, an order in which the faces a camera sees form fewer draw commands, and reorders them when it uploads a mesh.

### Edits

Changing a block remeshes its chunk, plus each neighbor whose padded border holds that block: 1 neighbor for a block on a face of the chunk, 3 on an edge and 7 at a corner, so up to 8 meshes for one edit. At the prototype's speed that is about 0.4 to 0.5 ms (8 × 49 µs for typical terrain, 8 × 63 µs for rough); at the 150 µs budget below it is up to 1.2 ms on one thread. From M3, worker threads can mesh the 8 in parallel.

## Alternatives considered

- **Culled faces, no merging:** simplest, and each face can carry its own lighting with no merge rules. On the measured terrain chunks, 1.3 to 1.8 times more quads to store and draw. Kept as the runtime toggle.
- **Classic greedy meshing over block loops** (0fps): the same output as the bitmask version, several times slower to compute.
- **Four or six vertices per face, float formats:** easy to read and debug, 16 to 24 times the memory (four or six 32-byte vertices against one 8-byte quad).
- **Six vertices per quad without an index buffer:** one buffer fewer, but every corner is shaded 1.5 times as often.
- **Meshing on the GPU in compute:** possible later; it moves work off the CPU but makes edits and debugging harder. Revisit only if profiling shows CPU meshing limits streaming.

## Performance budget

On the reference machine, release build, single thread, measured with nanobench on fixed, seeded test chunks:

| Case | Target | Prototype |
|---|---|---|
| Typical terrain chunk (surface, mixed blocks) | under 150 µs | 49 µs (rougher terrain: 63 µs) |
| Flat layered chunk | under 50 µs | 36 µs |
| Worst case: 3D checkerboard | under 2 ms | 1.1 ms |
| Quad output, typical terrain chunk | under 32 KiB | 6.4 KiB (rougher terrain: 14.4 KiB) |

The prototype column comes from a throwaway implementation of culling, occlusion and merging with texture and occlusion in the merge key, measured on the reference machine on 2026-10-02 (Apple Clang 17, `-O2 -mcpu=apple-m3`, median of seven runs). Its face counts matched a naive per-block check in every case, and its merged quads passed the coverage property below. Flat terrain has the least headroom, because about 25 µs of every mesh goes into building masks whatever the chunk holds; the x-rows-only layout described under Merging halves that. The implementation's own benchmarks replace this column.

## Test plan

- **Single block in empty space:** 6 quads, every corner fully lit.
- **Two neighboring blocks of one kind:** 6 quads with merging (four of them 2 wide), 10 without.
- **A full chunk surrounded by air:** 6 quads of 32 by 32.
- **Quad encoding:** every field round-trips at its limits (positions 0 and 31, sizes 1 and 32, all six faces), and the +x, +y and +z quads of a block at 31 decode to corners at 32.
- **Borders:** an opaque neighbor chunk hides this chunk's border faces; an unloaded one does not.
- **Ambient occlusion:** known corner arrangements give the values in Lysenko's table.
- **Diagonal:** for all 256 combinations of four 2-bit occlusion values, the decoded quad's two triangles share the diagonal that a reference implementation of Lysenko's comparison picks (ties through corners 0 and 2), and keep their winding.
- **Coverage property:** for many random chunks (seeded), the merged mesh covers exactly the faces the unmerged mesh produces, each once, with the same texture and occlusion. On every chunk it tries, this one test checks that merging never adds, drops or changes a face.
- **Look, by hand:** the chunk renders with merging on and off and looks identical; occlusion darkens inner corners the way Minecraft's smooth lighting does.

## Pull request plan

1. This RFC.
2. `engine/mesh` layer: `Quad`, its encoding and decoding, with tests.
3. Padded chunk assembly from a chunk and its 26 neighbors, with tests.
4. Face culling with column bitmasks, without merging, with tests and the first meshing benchmarks.
5. Ambient occlusion, with tests.
6. Merging, with the coverage property test and before and after quad counts.
7. Drawing one chunk: upload, the shared index buffer, the decoding vertex shader. The M2 milestone image.
8. Overlay toggles: wireframe, merging, ambient occlusion, quad counts. Wireframe draws with `VK_POLYGON_MODE_LINE`, which needs the `fillModeNonSolid` feature. Like `samplerAnisotropy`, it is outside RFC 0001's floor: enabled when the device offers it, and without it the toggle is hidden.

## Open questions

1. **How ambient occlusion looks.** Recommendation: on by default, four levels as in Minecraft's smooth lighting, with its strength on a slider in the overlay so the final value is chosen by eye.
2. **Faces between leaves (and other cutout blocks of one kind).** Minecraft has two looks: "Fast" leaves render opaque, and "Fancy" leaves are see-through and also draw the faces between two leaves. Culled cutout leaves sit between them: see-through like Fancy, without the faces inside a canopy. Recommendation: **cull them** by default. Fewer quads and less overdraw, which serves the north star. Drawing them, as Fancy does, can be a setting later. Culling them needs to know which cutouts are "of one kind", which the visual table does not say yet: if this is the answer, `BlockVisual` gains a cull group that the registry (RFC 0004) fills, and two cutouts with the same nonzero group hide each other's shared faces.

## As built

Not built yet.
