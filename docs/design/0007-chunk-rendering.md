# RFC 0007: Chunk rendering at scale

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M3 Infinite |
| **Issue** | #20 |

## Summary

How thousands of chunk meshes live on the GPU and get drawn every frame: one buffer, suballocated, that holds every mesh; uploads within a per-frame budget; per-chunk positions relative to the camera; frustum and face-direction culling on the CPU; and one multi-draw indirect call per pass, with one command per run of visible face groups. At the north star's 1,024-block view distance that is up to about 9,400 meshes and, with the cutout pass, up to about 8,600 draws per frame, so this RFC budgets what each mesh and each draw costs.

## Motivation

RFC 0006 loads about 17,900 chunks at a 1,024-block view distance and draws about 9,400 of them; only drawn chunks are meshed, so at most about 9,400 carry a mesh. A meshing spike on the reference machine (a prototype of RFC 0003 with its merge key) put a typical terrain chunk at 819 quads (6.4 KiB) and a rough one at 1,849 quads (14.4 KiB). That is about 60 to 130 MiB of quads, on a machine where Metal recommends a GPU working set of 5,461 MiB (queried). Memory is a budget to keep, not the limit.

The draw count is the limit. Both Apple drivers run a multi-draw indirect call as one Metal draw per command, encoded on the CPU: MoltenVK 1.4.2 (`MVKCmdDrawIndexedIndirect::encode`) and KosmicKrisp (`kk_draw`) both loop over the commands. A Metal-only probe on the reference machine measured about 100 ns of CPU encoding and about 100 ns of GPU time per indexed indirect draw, whether the draw covers a quad or nothing. 10,000 draws cost about a millisecond on each side before the Vulkan drivers add their own work, and at the spike's mesh sizes an average draw covers only a few hundred quads. Multi-draw indirect saves a Vulkan call per draw, not the Metal cost per draw, so commands have to be few, and none may be empty.

## Goals and non-goals

Goals:

- Every chunk mesh in one GPU buffer; memory that a frame in flight may still read is never reused.
- Uploads written straight into GPU memory on unified memory and staged on discrete GPUs, within a per-frame time budget.
- Per pass, one `vkCmdDrawIndexedIndirect` call (split only at the driver's `maxDrawIndirectCount`), one command per run of adjacent visible face groups, and no empty commands.
- Frustum and face-direction culling that never removes a visible face: a scene renders identically with culling on and off.
- One code path on KosmicKrisp, MoltenVK and lavapipe, without `drawIndirectCount`.
- Fragmentation measured, and compacted without stalling a frame.
- Overlay counters: chunks and quads drawn, draws, memory used and fragmentation.

Non-goals:

- Which chunks are loaded, and when: RFC 0006.
- Threads, the queue that delivers finished meshes, and canceling stale jobs: RFC 0005.
- Occlusion culling and level of detail: M9.
- Translucency and water: M7. Lighting: M6.
- The device, frames in flight and the upload helper: RFC 0001.

## Prior art

- **Sodium** (0.9.2, September 2026): chunk sections grouped into regions of 8 by 4 by 8 sections, one multi-draw per region, a shared quad index buffer, base vertex into the region's buffer, and the region's offset from the camera in push constants. Each section's quads are grouped by facing in the order +X, +Y, +Z, -X, -Y, -Z, and consecutive visible groups merge into one draw. Its allocator (PR #3634) incrementally defragments a set of shared arenas; it replaced one that issue #2683 calls slow and prone to fragmentation. We take the facing order, the merging, TLSF and budgeted compaction.
- **zeux/niagara**: culling in compute (frustum, two-phase occlusion against a depth pyramid, level of detail), drawn with `vkCmdDrawIndexedIndirectCount`, per-draw data found through `gl_DrawID`. The model for M9. We avoid `gl_DrawID`: KosmicKrisp uploads per-draw data to provide it, and MoltenVK binds a buffer per draw.
- **Vulkan Guide**, GPU-driven chapter: per-object data indexed through `firstInstance`, deliberately without `DrawIndirectCount`. We use `firstInstance` the same way.
- **Khronos Vulkan-Samples, `multi_draw_indirect`**: CPU-built and GPU-built commands side by side; culled objects become zero-instance commands.
- **VMA's virtual allocator** and **sebbbi's OffsetAllocator**: two TLSF-style offset allocators, compared under alternatives.

## Design

### What the drivers do

Read from the sources Vulkan SDK 1.4.363.0 ships (KosmicKrisp at its `vulkan-sdk-1.4.363.0` tag, MoltenVK 1.4.2), and from Mesa 26.2 for lavapipe:

| | KosmicKrisp | MoltenVK | lavapipe |
|---|---|---|---|
| `multiDrawIndirect`, `drawIndirectFirstInstance` | yes | yes | yes |
| A multi-draw runs as | one Metal draw per command, encoded on the CPU | the same | natively, in software |
| `drawIndirectCount` | emulated: a compute pass zeroes the commands past the count, and all `maxDrawCount` commands are still encoded | no | yes |
| `maxDrawIndirectCount` | 65,535 | 2^30 | 2^32 - 1 |
| `VK_EXT_memory_budget` | yes | yes | yes |

All three offer memory that is device-local, host-visible, coherent and cached. KosmicKrisp has a single queue. Metal allows buffers up to 4 GiB on the reference machine (queried).

### The arena

One `VkBuffer` with a device address holds every mesh. VMA creates it with sequential-write host access and a transfer allowed instead (VMA's documented upload pattern): on the reference machine it lands in device-local, host-visible memory and is written directly; on a discrete GPU it may land in device-local memory only, and uploads are staged.

A VMA virtual block (TLSF) suballocates the buffer. Each chunk's mesh is one allocation holding its 12 face groups back to back: opaque +x, +y, +z, -x, -y, -z, then cutout (leaves, glass) in the same order. The split is the one RFC 0004 asks of RFC 0003's mesher, so cutout quads can be drawn after every opaque one. Records live in a dense array, with a hash map from chunk position to index:

```cpp
// One chunk's mesh in the arena. The draw lists are rebuilt from these every
// frame, so moving a mesh only changes first_quad.
struct ChunkMeshRecord {
    ChunkPos pos;
    std::uint32_t generation;              // a newer mesh of this chunk replaces an older one
    std::uint32_t first_quad;              // offset in the arena, in 8-byte quads
    std::array<std::uint16_t, 12> count;   // quads per group, in arena order
    VmaVirtualAllocation allocation;
};
```

A face group holds at most 32,768 quads (one direction of a checkerboard of two different cutout blocks, which do not hide each other), so 16-bit counts still suffice. An empty mesh has no allocation and no record.

**Freeing.** A replaced, removed or moved mesh's range goes on the current frame slot's retire list, and back to the allocator when RFC 0001's pacing has waited for that slot again (frame N waits for frame N minus 2). No frame in flight reads a range after it is reused.

**Capacity.** Set at startup from the rough-terrain estimate in the performance budget, scaled to the configured view distance, plus 25%, and capped at a quarter of the device's memory budget. If the arena fills anyway, it is repacked into a buffer 1.5 times larger: every live mesh is copied on the GPU, the old buffer is retired like a range, and the overlay counts the repack. If the cap stops it from growing, uploads wait, the overlay shows the arena as full, and RFC 0006 decides what to load.

**Fragmentation and compaction.** Fragmentation is 1 minus the largest free range divided by the total free space, sampled a few times per second (VMA finds the largest free range by walking the block). Compaction starts when an allocation fails although enough space is free in total, or when fragmentation passes 0.5 with less than a quarter of the arena free. Each frame it moves the mesh with the highest offset (the arena keeps its meshes ordered by offset) into the lowest free range that fits (VMA's `MIN_OFFSET` strategy), until fragmentation falls below 0.2 or no move lowers an offset. A move is a `vkCmdCopyBuffer` inside the arena, recorded before the chunk passes, so this frame already draws from the new place. Moves are capped at 8 MiB and a fixed count per frame, because the lowest-offset search walks every block. The Metal probe copies 8 MiB inside a shared buffer in 0.19 ms of GPU time.

### Uploads

Finished meshes arrive in the order RFC 0005 delivers them and are handled on the main thread, which renders (RFC 0005) and alone owns the arena:

1. An update older than the record's generation is dropped: two remeshes of one chunk can finish out of order.
2. An empty mesh removes the record. Otherwise: allocate, write the 12 groups, retire the old range.
3. **Host-visible arena** (unified memory): `memcpy` into the mapped arena. Writes to coherent memory made before a submission are visible to it, so no barrier is needed. A 32 KiB mesh copies in about 0.7 µs on an idle reference machine.
4. **Arena not host-visible** (most discrete GPUs): write into the frame slot's staging ring and record a copy region, executed before the chunk passes behind one barrier. Copies share the graphics queue: KosmicKrisp has no other, and a transfer queue would only pay off on discrete hardware we cannot measure.

The budget is time: 1 ms per frame by default, plus the staging ring's free space. The first write to a page costs several times more than later ones (about seven times in the probe), so bytes would be the wrong unit while the arena is fresh. Meshes that do not fit wait, in order, on the main thread for the next frame; a loading screen raises the budget. An edit's batch (RFC 0005) goes ahead of waiting streaming meshes and is uploaded whole in the frame it arrives, outside the budget: at most 8 meshes, microseconds on unified memory. Every write goes through RFC 0001's `upload` helper, which takes the destination offset.

### Per-chunk data and the vertex shader

Each command's `vertexOffset` is four times the first quad of its run, so `gl_VertexIndex >> 2` is the quad's index in the whole arena, and RFC 0003's shared index buffer (the pattern 0, 1, 2, 2, 3, 0, offset by 4 per quad; 32-bit, sized for the 196,608-quad worst case, because one command can cover all of a chunk's cutout groups: 4.5 MiB) serves every command with `firstIndex` 0. `firstInstance` is the chunk's slot in this frame's array of origins, which is why `drawIndirectFirstInstance` is in RFC 0001's feature floor.

The origin is computed on the CPU in 64-bit, as RFC 0001 requires, relative to the block the camera is in. That makes it an integer, exact as a float, and the camera's fraction is subtracted per vertex: a corner shared by two chunks gets the same float from both, so neighbors meet without cracks.

```cpp
struct ChunkPush {                  // 96 of the 128 bytes (RFC 0001)
    glm::mat4 view_projection;      // rotation and projection only
    VkDeviceAddress quads;          // the arena
    VkDeviceAddress origins;        // this frame: one float4 per visible chunk
    glm::vec3 camera_fraction;      // the camera's position within its block
};
```

The vertex shader, sketched (RFC 0001 picks the shading language):

```glsl
Quad q = quads[gl_VertexIndex >> 2];
vec3 p = origins[gl_InstanceIndex].xyz + quad_corner(q, gl_VertexIndex & 3);   // RFC 0003
gl_Position = view_projection * vec4(p - camera_fraction, 1.0);
```

The origins and the commands live in one host-visible buffer per frame slot, grown when a frame needs more.

### Culling

Each frame the main thread walks the records once:

- **Frustum:** the chunk's 32-block cube against the near plane and the four side planes. The far plane is at infinity (RFC 0001); streaming bounds the distance.
- **Face direction:** with `o` the chunk's corner relative to the camera's block, +x quads can face the camera only if `o.x <= 0`, and -x quads only if `o.x >= -31`; likewise for y and z. Integer tests, conservative by one block.

Why the CPU: on the Apple drivers a command built on the GPU still becomes a Metal draw encoded on the CPU, a count makes KosmicKrisp encode every potential command (each costing GPU time even when empty), and MoltenVK has no count at all. The CPU knows the exact count, so no command is empty; the code is unit tested without a GPU; and walking 20,000 records is a small part of a frame (target below). GPU culling earns its place with occlusion culling, which needs the depth buffer on the GPU: M9.

### Draw commands

For each visible chunk, the builder walks the six opaque groups in arena order and merges consecutive groups that are visible or empty into runs; each run becomes one command. A camera outside a chunk's extent on all three axes faces three of its groups; in this order those form 1.75 runs on average over the eight octants, against 2.5 in RFC 0003's order (+x, -x, +y, ...) and 3 without merging.

```cpp
const VkDrawIndexedIndirectCommand cmd{
    .indexCount = 6 * run_quads, .instanceCount = 1, .firstIndex = 0,
    .vertexOffset = static_cast<std::int32_t>(4 * run_first_quad),
    .firstInstance = origin_slot};
```

`vertexOffset` is a signed 32-bit value, which caps the arena at 2^29 quads (4 GiB), the same as Metal's buffer limit here and above any budget in this RFC.

Two passes, in the order Apple's GPUs want (RFC 0004): every opaque run first, with a pipeline that never discards, then the cutout runs, built the same way from the six cutout groups, with alpha testing. Apple's hidden surface removal stays efficient as long as all opaque geometry is submitted before anything alpha-tested, and opaque draws need no sorting (WWDC20, "Harness Apple GPUs with Metal"). Chunks without cutout quads add nothing to the second pass.

Both Apple drivers add hidden work to indirect draws in some configurations (read from their sources): a compute pass in front of them, or an upload per draw. The chunk pipelines avoid every trigger: triangle lists, 32-bit indices, primitive restart off, the default first-vertex provoking mode, no multiview and no `gl_DrawID`. Release builds leave `robustBufferAccess2` and `pipelineRobustness` off.

### Overlay

Counters every frame: meshes resident, chunks drawn, opaque and cutout draws, quads resident and drawn, quads skipped by face-direction culling, arena capacity, used bytes, largest free range and fragmentation, uploads and backlog, bytes moved by compaction, repacks. Switches: frustum culling off, face-direction culling off, run merging off, and freeze culling (keep one frame's culling results while the camera moves on, to see what was culled).

### API sketch

```cpp
namespace vx::render {

struct ChunkMeshUpdate {
    ChunkPos pos;
    std::uint32_t generation;   // increases with every remesh of the chunk
    ChunkMesh mesh;             // RFC 0003's ranges, in a buffer from RFC 0005's MeshResultPool
};

// Owned by Renderer (RFC 0001); main thread only (RFC 0005).
class ChunkRenderer {
public:
    // Copies the quads; the mesh memory is free again on return. False when
    // this frame's budget is spent: offer the update again next frame.
    bool upload(const ChunkMeshUpdate& update);
    // RFC 0005's edit batch: all of it, this frame, outside the budget.
    void upload_batch(std::span<const ChunkMeshUpdate> batch);
    void remove(ChunkPos pos);
    [[nodiscard]] const ChunkRenderStats& stats() const noexcept;

    // From Renderer::render: free retired ranges, cull, build the commands,
    // then record the copies and both passes.
    void record(VkCommandBuffer cmd, const CameraView& camera, std::uint64_t frame);
};

} // namespace vx::render
```

## Alternatives considered

- **Commands built by a compute pass,** drawn with `vkCmdDrawIndexedIndirectCount`: missing on MoltenVK, and emulated on KosmicKrisp by encoding every potential command. On the GPU an empty command costs about as much as a real one. Returns with occlusion culling in M9.
- **Instanced clusters, one draw per pass:** the CPU writes an 8-byte record per 64 quads, and one `vkCmdDrawIndexed` with an instance per cluster replaces thousands of commands. Partial clusters waste vertex work, and the shader is less standard. Open question 1.
- **One command per chunk, all six groups:** 1 command per chunk instead of 1.75, but twice the quads drawn, since every group is drawn whichever way it faces.
- **`gl_DrawID` for per-draw data:** a per-draw upload on KosmicKrisp and a per-draw buffer bind on MoltenVK.
- **sebbbi's OffsetAllocator:** O(1) allocation and an O(1) estimate of the largest free range, but a new dependency its author calls an early prototype, with no lowest-offset placement for compaction. VMA is already a dependency (RFC 0001), maintained, and TLSF too.
- **A free list of our own:** full control, but Sodium's experience (issue #2683) is that a simple allocator is slow and fragments.
- **Several arena buffers** (Sodium's current allocator): growth without a repack, at the cost of a base address per chunk or a call per buffer. Open question 2.
- **16-bit indices:** half the index bytes, but runs longer than 16,384 quads would have to be split. 4.5 MiB of 32-bit indices, allocated once, is simpler.
- **Front-to-back sorting:** unnecessary for opaque geometry on Apple GPUs. It may help GPUs without hidden surface removal, and can be added to the builder without format changes.

## Performance budget

What the north star asks of this layer, from RFC 0006's drawn region and the meshing spike's sizes (replaced by measurements from generated terrain in M4):

| View distance | 1,024 blocks | 512 blocks |
|---|---|---|
| Chunks with a mesh, at most (RFC 0006's drawn region) | 9,400 | 4,500 |
| Mesh memory, typical terrain (6.4 KiB per mesh) | 59 MiB | 28 MiB |
| Mesh memory, rough terrain (14.4 KiB per mesh) | 132 MiB | 64 MiB |
| Arena capacity: rough terrain plus 25% | 165 MiB | 80 MiB |
| Visible meshed chunks (26% of columns, a 94-degree horizontal view) | 2,500 | 1,200 |
| Opaque draws, 1.75 per chunk | 4,300 | 2,100 |
| Cutout draws in forests, at most one per opaque draw | up to 4,300 | up to 2,100 |
| Draw overhead, Metal alone (about 100 ns each on CPU and GPU), both passes | 0.43 to 0.86 ms each | 0.21 to 0.42 ms each |
| Quads drawn, half of each mesh after face-direction culling | 1.0 to 2.3 million | 0.5 to 1.1 million |

The 94 degrees follow from RFC 0001's 70-degree vertical field of view on the reference display (2560 by 1664); the flythrough's 2560 by 1440 (RFC 0006) gives 102 degrees and about 28.5% of columns, slightly more draws. RFC 0003's 32 KiB per chunk would make the 1,024-block arena about 370 MiB; a repack covers that case. Draws and quads both grow with the square of the view distance, and level of detail (M9) cuts both at the far end; this RFC keeps the cost per chunk and per draw small.

Targets on the reference machine, release build:

| What | Target | Measured with |
|---|---|---|
| Build the draw lists for 20,000 meshes | under 0.5 ms | nanobench, synthetic records |
| Record both passes at 10,000 draws, per driver | under 2 ms CPU | Tracy, the benchmark of PR 2 |
| Uploads | 1 ms per frame (the default budget) | Tracy |
| Arena | no repack during the M3 benchmark flight at the capacity above | overlay, the M3 benchmark flight |
| Fragmentation | no allocation fails during a 30-minute flight | allocation trace replay |

## Test plan

- **Runs (unit):** for all 64 visibility masks and all 64 patterns of empty groups, with random group sizes, the commands cover exactly the quads of the visible groups, each once.
- **Face direction (unit):** for random chunks, cameras and quads, every quad that faces the camera is in a group the mask keeps.
- **Frustum (unit):** a chunk with any sample point inside the frustum is never culled.
- **Arena (unit, fake timeline):** no range is handed out again before the frame that retired it completes; compaction and repack keep every mesh's bytes (checksums); a fragmented failure starts compaction.
- **Uploads (unit, fake clock):** stale generations are dropped, the budget holds, empty meshes free their range.
- **Command splitting (unit):** with a fake `maxDrawIndirectCount` of 3, calls split and no command is lost.
- **CI on lavapipe,** synchronization validation on: a seeded scene of a few hundred chunks renders identically with culling on and off, and with the staging path forced (lavapipe, like Apple GPUs, offers host-visible device memory, so staging only runs when forced). Control: a planted sign error in the face mask must turn the job red.
- **Compaction stress (CI):** a deliberately small arena, many moves and repacks, the same image.
- **By hand on both Apple drivers:** the overlay counters, freeze culling, and an Xcode frame capture through MoltenVK (ADR 0002) showing one Metal draw per run. Debug sessions on KosmicKrisp run GPU-assisted validation, which the SDK supports there and not on MoltenVK, and which flags reads outside any buffer.

## Pull request plan

1. This RFC.
2. Draw overhead benchmark: indexed indirect draws, empty and real, and instanced clusters, on KosmicKrisp and MoltenVK. Settles open question 1.
3. `ChunkArena`: VMA virtual block, retire lists keyed by frame, statistics.
4. Uploads: direct and staged paths through RFC 0001's `upload`, the time budget, generations, and `upload_batch` for edits.
5. Draw list builder: frustum, face masks, runs, origins, commands; tests and nanobench.
6. Chunk pipeline and passes, building on RFC 0003's single-chunk draw; the lavapipe image test and its control.
7. Cutout pass.
8. Overlay counters and switches.
9. Compaction and repack, with the trace replay.

## Open questions

1. **Draw granularity on the Apple drivers.** Multi-draw indirect with merged runs, or one instanced draw over 64-quad clusters? Recommendation: **runs**, the standard path; switch only if the benchmark of PR 2 shows recording 10,000 draws takes over 2 ms on KosmicKrisp. Only the builder and the vertex shader would change.
2. **One arena buffer or several.** Recommendation: **one**, sized for rough terrain (165 MiB at 1,024 blocks) so a repack is rare. Several buffers would avoid the repack's pause and its peak memory, at the cost of a base address per chunk or a draw call per buffer. Revisit if repacks show up in play.

## As built

Not built yet.
