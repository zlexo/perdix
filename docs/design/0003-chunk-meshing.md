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
| Faces merged into larger quads | a further 2 to 10 times fewer, depending on how varied the surface is |
| Worst case (a 3D checkerboard, nothing merges) | 98,304 |

## Goals and non-goals

Goals:

- Emit only faces that can be seen: between a solid block and a non-solid one, including across chunk borders.
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

- **Mikola Lysenko, "Meshing in a Minecraft Game"** (0fps.net, 2012): face culling, greedy meshing, and merging only faces with the same merge key.
- **Mikola Lysenko, "Ambient occlusion for Minecraft-like worlds"** (0fps.net, 2013): per-vertex occlusion from three neighbors, and flipping a quad's diagonal so occlusion interpolates evenly.
- **cgerikj/binary-greedy-meshing**: face culling and merging with 64-bit column masks, one bit per block, instead of per-block loops.
- **Sodium** (a Minecraft renderer mod): chunk geometry grouped by facing, so faces pointing away from the camera are skipped per chunk.
- **Minecraft**: per-face culling and smooth lighting, without merging. The look to match; the cost to beat.

## Design

### Input: a padded chunk

The mesher reads a plain array of 34 by 34 by 34 states: the chunk plus a one-block border taken from its 26 neighbors. Faces need the 6 face neighbors (RFC 0002's `copy_face`); ambient occlusion at the chunk's edges and corners also needs the 12 edge and 8 corner neighbors, which are 392 single-block reads in total. A neighbor that is not loaded counts as air, and the chunk is remeshed when it arrives.

What a block looks like comes from a per-state table built by the block registry:

```cpp
enum class Opacity : std::uint8_t { air, opaque, cutout };   // cutout: leaves, glass

struct BlockVisual {
    Opacity opacity;
    std::array<std::uint16_t, 6> texture;   // texture layer per face
};
```

### Face culling with column bitmasks

For each axis, every row of 34 blocks along that axis becomes one 64-bit mask, one bit per block, set when the block is solid. A face is visible where a solid block has a non-solid neighbor, which is two shifts and an and-not for a whole row:

```cpp
// Bit i is block i along the row, padding included (bits 0 and 33).
const std::uint64_t pos_faces = solid & ~(solid >> 1);   // solid, next block along +axis is not
const std::uint64_t neg_faces = solid & ~(solid << 1);   // solid, previous block along -axis is not
// Keep bits 1..32: faces of this chunk's own blocks, not the border's.
constexpr std::uint64_t inner = 0x1'FFFF'FFFEull;
```

Cutout blocks (leaves, glass) are solid for their own faces but do not hide a neighbor's face. Whether faces between two cutout blocks of the same kind are drawn is open question 2.

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

Textures repeat across a merged quad, which works because blocks use a texture array (RFC 0001) rather than an atlas, where repeating would bleed into neighboring tiles.

Merging can be switched off at runtime in the debug overlay, to measure what it saves and to debug it.

### The quad

```cpp
// One visible rectangle of faces. The vertex shader rebuilds its four
// corners; the chunk's camera-relative origin comes from per-draw data.
struct Quad {
    std::uint32_t geometry;   // x:5 y:5 z:5 | width-1:5 height-1:5 | face:3 | spare:4
    std::uint32_t material;   // texture layer:16 | AO 4 corners x 2 bits | spare:8
};
static_assert(sizeof(Quad) == 8);
```

Four 32-byte vertices with float position, normal and texture coordinates would be 128 bytes for the same face; the packed quad is 16 times smaller, and size is what limits how many chunks fit on the GPU.

Positions are local to the chunk, so 5 bits per axis suffice; the chunk's origin, relative to the camera (RFC 0001), comes from per-draw data. The face's normal and its texture coordinates are derived from the face direction and the quad size in the shader.

### Drawing a quad

Quads are drawn with one static index buffer shared by every chunk, holding the pattern 0, 1, 2, 2, 3, 0 for quad 0, then the same offset by 4 for each following quad. The vertex shader finds its quad as `vertex_index >> 2` and its corner as `vertex_index & 3`, so each corner is shaded once and reused through the vertex cache.

Ambient occlusion needs the quad split along the diagonal whose corners are more alike, or the darkening smears in a visible stripe. The index pattern stays fixed; the shader rotates which corner each vertex plays when the other diagonal is the better split. No extra bits, no second index buffer.

### Output

```cpp
struct ChunkMesh {
    std::array<std::span<const Quad>, 6> by_face;   // +x, -x, +y, -y, +z, -z
};

// Pure: reads the padded chunk and the visual table, writes into reusable
// scratch owned by the calling worker. No allocation once the scratch is warm.
ChunkMesh mesh_chunk(const PaddedChunk& blocks, std::span<const BlockVisual> visuals, MeshScratch& scratch);
```

Grouping by face direction lets the renderer skip the groups that face away from the camera: for a chunk entirely above the camera, its upward faces cannot be seen. That roughly halves the quads drawn, at no cost here beyond keeping six ranges.

### Edits

Changing a block remeshes its chunk, plus each neighbor whose border it touches (up to 7 chunks for a corner block). At the budget below, that is well under a millisecond.

## Alternatives considered

- **Culled faces, no merging:** simplest, and each face can carry its own lighting with no merge rules. Two to ten times more quads to store and draw. Kept as the runtime toggle.
- **Classic greedy meshing over block loops** (0fps): the same output as the bitmask version, several times slower to compute.
- **Four or six vertices per face, float formats:** easy to read and debug, 16 times the memory.
- **Six vertices per quad without an index buffer:** one buffer fewer, but every corner is shaded 1.5 times as often.
- **Meshing on the GPU in compute:** possible later; it moves work off the CPU but makes edits and debugging harder. Revisit only if profiling shows CPU meshing limits streaming.

## Performance budget

On the reference machine, release build, single thread, measured with nanobench on fixed, seeded test chunks:

| Case | Target |
|---|---|
| Typical terrain chunk (surface, mixed blocks) | under 150 µs |
| Flat layered chunk | under 50 µs |
| Worst case: 3D checkerboard | under 2 ms |
| Quad output, typical terrain chunk | under 32 KiB |

## Test plan

- **Single block in empty space:** 6 quads, every corner fully lit.
- **Two neighboring blocks of one kind:** 6 quads with merging (four of them 2 wide), 10 without.
- **A full chunk surrounded by air:** 6 quads of 32 by 32.
- **Borders:** a solid neighbor chunk hides this chunk's border faces; an unloaded one does not.
- **Ambient occlusion:** known corner arrangements give the values in Lysenko's table, and the diagonal choice is checked for each case.
- **Coverage property:** for many random chunks (seeded), the merged mesh covers exactly the faces the unmerged mesh produces, each once, with the same texture and occlusion. This one test proves merging never adds, drops or changes a face.
- **Look, by hand:** the chunk renders with merging on and off and looks identical; occlusion darkens inner corners the way Minecraft's smooth lighting does.

## Pull request plan

1. This RFC.
2. `engine/mesh` layer: `Quad`, its encoding and decoding, with tests.
3. Padded chunk assembly from a chunk and its 26 neighbors, with tests.
4. Face culling with column bitmasks, without merging, with tests and the first meshing benchmarks.
5. Ambient occlusion, with tests.
6. Merging, with the coverage property test and before and after quad counts.
7. Drawing one chunk: upload, the shared index buffer, the decoding vertex shader. The M2 milestone image.
8. Overlay toggles: wireframe, merging, ambient occlusion, quad counts.

## Open questions

1. **How ambient occlusion looks.** Recommendation: on by default, four levels as in Minecraft's smooth lighting, with its strength on a slider in the overlay so the final value is chosen by eye.
2. **Faces between leaves (and other cutout blocks of one kind).** Recommendation: **cull them** by default, like Minecraft's "fast" leaves. Fewer quads and less overdraw, which serves the north star. "Fancy" leaves, with the inner faces drawn, can be a setting later.

## As built

Not built yet.
