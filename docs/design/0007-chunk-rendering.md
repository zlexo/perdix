# RFC 0007: Chunk rendering at scale

| | |
|---|---|
| **Status** | Accepted (2026-10-02) |
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
- **By hand on both Apple drivers:** the overlay counters, freeze culling, and an Xcode frame capture through MoltenVK (ADR 0002) showing one Metal draw per run. Debug sessions run GPU-assisted validation, which flags reads outside any buffer, on whichever Apple driver the SDK supports it for; which one that is was not confirmed while writing this RFC, so the first chunk-rendering pull request checks it on both.

## Pull request plan

1. This RFC.
2. Draw overhead benchmark: indexed indirect draws, empty and real, and instanced clusters, on KosmicKrisp and MoltenVK. Confirms open question 1 (merged runs) or triggers the switch to clusters.
3. `ChunkArena`: VMA virtual block, retire lists keyed by frame, statistics.
4. Uploads: direct and staged paths through RFC 0001's `upload`, the time budget, generations, and `upload_batch` for edits.
5. Draw list builder: frustum, face masks, runs, origins, commands; tests and nanobench.
6. Chunk pipeline and passes, building on RFC 0003's single-chunk draw; the lavapipe image test and its control.
7. Cutout pass.
8. Overlay counters and switches.
9. Compaction and repack, with the trace replay.

## Open questions

All settled on 2026-10-02: every recommendation below was accepted.

1. **Draw granularity on the Apple drivers.** Multi-draw indirect with merged runs, or one instanced draw over 64-quad clusters? Recommendation: **runs**, the standard path; switch only if the benchmark of PR 2 shows recording 10,000 draws takes over 2 ms on KosmicKrisp. Only the builder and the vertex shader would change.
2. **One arena buffer or several.** Recommendation: **one**, sized for rough terrain (165 MiB at 1,024 blocks) so a repack is rare. Several buffers would avoid the repack's pause and its peak memory, at the cost of a base address per chunk or a draw call per buffer. Revisit if repacks show up in play.

## As built

Pull request plan item 2, the draw overhead benchmark (#247), landed as one pull request: `vx_draw_bench`, then the same program timing MoltenVK's encoding in its submit call.

- **`vx_draw_bench` is a program of its own:** 10,000 indexed indirect commands of 16 quads each, the same commands empty, and one instanced draw of 2,500 clusters of 64 quads covering the same 160,000 quads, placed by `firstInstance`; GPU times are medians over 60 frames after 5. MoltenVK keeps recorded commands in a list of its own and encodes them at submit, so the submit is timed apart from the wait. The program runs at user-interactive QoS, and CI's lavapipe job runs it once as a `vulkan` test.
- **Recording 10,000 commands takes 0.63 to 0.72 ms of CPU on KosmicKrisp;** MoltenVK records them in microseconds and pays 0.63 to 0.75 ms in `vkQueueSubmit2` (release build, reference machine, nothing else running, three runs a driver).
- **On the GPU a command costs about 130 ns on KosmicKrisp and 40 ns on MoltenVK,** where the Metal probe measured 100. 10,000 real commands took 2.45 ms on KosmicKrisp against 0.57 ms for the clusters, and 0.64 against 0.58 ms on MoltenVK. With builds running, the GPU numbers swung by up to four times, so benchmarks run on a quiet machine.
- **Open question 1 stands: merged runs.** Recording 10,000 draws stayed under 0.75 ms on both drivers, against the 2 ms that would have switched to clusters.

Item 3, the arena (#248), landed as one pull request: the arena, a fix for upload targets on MoltenVK, and a random test of every statistic.

- **`ArenaSpace` counts quads, not bytes:** a VMA virtual block (TLSF) over the arena, so an allocation's offset is its first quad. A range freed while a frame slot records waits on that slot's retire list. A random test of 20,000 allocations, retires and frames checks live, retired and free quads, the largest free range and the fragmentation against a model of every quad's owner; a mutant that took the smallest free range for the largest had passed the fixed cases.
- **Upload targets on MoltenVK were never host-visible:** for host writes VMA scores cached device-local, host-visible memory the same as device-local memory the CPU cannot write and takes the first, which on MoltenVK is the latter, so every upload target was staged though Apple's GPUs share one memory. On an integrated GPU upload targets now also prefer host-visible, coherent memory; discrete GPUs keep VMA's choice.
- **The app sizes the arena from the view distance** by the RFC's rough-terrain estimate plus a quarter: 21.7 million quads (165 MiB) at 32 chunks, at least 1,048,576 quads and at most 2^29. Item 9 caps it at a quarter of the device's memory budget and grows it when it fills. In the first flythrough report, meshes peaked at 28.1 MiB.

Item 4, uploads (#249), landed as two pull requests: `ChunkUploads`, with generations and the budget; `ArenaWriter`, in place or staged, with two fixes. A third fix landed with item 6.

- **Uploads reorder each mesh's twelve groups** from RFC 0003's order (+x, -x, +y, ...) into the arena's (+x, +y, +z, -x, -y, -z). An update no newer than the chunk's mesh is dropped, also after an empty mesh removed it. The budget is wall-clock time, 1 ms by default, and empty meshes and drops cost nothing.
- **An edit's batch goes in whole whatever the budget, or not at all** (`full`) when one of its meshes finds no range. The review found a small mesh taking the hole a large one in the same batch needed, so a batch now places its largest meshes first, and an update made stale by another in its own batch keeping its range for good. Mutants survived the first tests until the random test, 5,000 uploads, removals and frames, offered stale updates and batches.
- **A frame's first upload always goes,** so a frame whose budget was spent before it uploaded still makes progress, and a budget of 0 uploads one mesh a frame. Staged writes go through RFC 0001's upload context, one per frame slot, whose staging buffers are freed when the slot comes round, rather than a staging ring; tests force that path with an arena in device-local memory alone.

Item 5, the draw lists (#250), landed as two pull requests: the view frustum in `core`; the draw lists, with their tests and benchmark.

- **The frustum is in doubles, relative to the camera,** with the far plane at infinity, and conservative: only a box wholly outside one plane is out. `view_frustum(camera, aspect)`, built from the rows of the view rotation the GPU uses, came with item 6.
- **Runs merge the drawn groups and the empty groups between them,** one command each; the opaque and cutout passes get lists of their own, and a pass splits into calls at `maxDrawIndirectCount`.
- **Building the draw lists for 20,000 meshes takes 0.19 ms** (budget 0.5 ms), release build, reference machine. The tests cover all 64 masks of drawn groups by all 64 patterns of empty ones, 200,000 random quads that can face the camera, and 3,000 random views in which no chunk with a point in view is culled.

Item 6, the chunk pipeline and passes (#251), landed as five pull requests: a frame's first upload always going; `view_frustum`; `ChunkRenderer`; chunks drawn from the arena, each pass one indirect call; the seeded scene test.

- **The app hands each frame's changes through `ChunkChanges::apply(ChunkUploader&)`** (decided 2026-10-09), an interface it implements, in place of the sketch's `upload`, `upload_batch` and `remove` on the renderer. `render()` calls it after the slot wait and the swapchain acquire, so staged copies always reach a frame that is submitted.
- **The upload budget starts after the swapchain acquire.** Counted from the slot wait, as first built, it let a window under FIFO, whose acquire can block for most of a frame, upload nothing.
- **`ChunkRenderer` begins a frame, applies the changes and prepares the draw lists,** writing origins and commands into a host-visible buffer per frame slot that starts at 4 KiB and doubles. Arenas past 2^29 quads are refused, since `vertexOffset` is signed. The push constants carry RFC 0003's occlusion strength and RFC 0004's filter beside the RFC's 96 bytes, padded to 112 of the 128. RFC 0003's `set_chunk` goes through the arena as a batch of one with generations of its own, so it is not for streamed chunks.
- **A seeded scene of 243 chunks draws pixel for pixel the same** with culling and merging on or off, staged with no budget, one mesh a frame, and with each pass split into calls of seven; culling takes out 84 chunks and 51,387 quads facing away. Seven planted bugs each turn it red, the RFC's face-mask sign error among them. Removing the barrier after staged copies is not caught: synchronization validation does not follow reads through buffer device addresses, and Metal orders the copy anyway.
- **GPU-assisted validation works on KosmicKrisp and not on MoltenVK,** whose instrumented shaders fail to compile; on KosmicKrisp a planted out-of-bounds read of an origin gave 20 errors. The 22 chunk, arena and upload Vulkan tests pass on each driver alone, and Vulkan tests time out after 300 s, since a hung GPU had hung CTest.

Item 7, the cutout pass (#252), landed with item 6: every opaque run, then every cutout run with alpha testing, each pass one multi-draw indirect call.

- **The review found the cutout pass drawn from the opaque commands' offset passing every test,** since the scene test compared the pipeline with itself; it now also counts the cutout blocks' pixels.

Item 8, the overlay (#253), landed as one pull request: freeze culling, then the counters and switches.

- **The counters:** meshes and quads in the arena; chunks drawn and culled; opaque and cutout draws; quads drawn and facing away; the arena's use, largest free range and fragmentation, sampled four times a second because the statistics walk the free ranges; the frame's uploads, drops and refusals, and since RFC 0006's item 7 the meshes waiting on the main thread. Compaction's counters come with item 9. Each switch is tested through ImGui on SDL's dummy video driver.
- **Freeze culling culls from the frozen view and draws from the live camera,** each origin moved from the frozen camera's block to the current one's, exactly while the camera has moved under 2^24 blocks. Three freeze mutants survived until a test moved and turned the camera; a label that showed refusals as the backlog was renamed.
- **In the streamed app** (release build, Minecraft 26.2's textures, 2940 by 1846) the overlay read 40 frames a second, 12.7 ms of GPU time a frame on average with peaks of 33 ms, 1,616 chunks and 854,000 quads drawn, and 247,000 quads facing away. Distant hills alias: distance fog and an MSAA switch come first, through a short RFC after M3 (the owner's call, 2026-10-09).

Item 9, compaction and repack, landed as six pull requests: the arena space's lowest-offset allocations; compaction and repacks in the uploads; the upkeep that decides between them; the 30-minute flight replayed; the GPU's copies in the chunk renderer, with the overlay's line.

- **Compaction starts only when an upload is refused** and the meshes, packed, would leave room for it. The design above also started it when the fragmentation passed 0.5 with under a quarter of the arena free. Replayed 30-minute flights showed that trigger moving meshes on up to 6% of frames, 0.17 ms of the main thread on average and up to about 1 ms, without one refusal it prevented: VMA's TLSF placed every streaming mesh on its own at 5% headroom over the flight's peak and more, and with flights that crossed from small meshes to large ones too. At 2% headroom or less every variant, compaction or none, needed one repack.
- **A frame moves at most 8 meshes and 8 MiB:** VMA finds each one's lowest free range by walking every range of the block, 20 to 85 µs a move in the replays, so a compaction frame costs 0.2 to 0.7 ms (release build, reference machine).
- **Compaction runs until the highest mesh finds no lower range that fits, then settles:** the ranges it emptied come back once every frame slot has come round, and the refused uploads try again in that frame; a refusal still there after that repacks. Repacking at once would grow an arena the moves had just made room in: the first GPU test caught exactly that.
- **A repack makes an arena 1.5 times larger,** or large enough for the live meshes and the refused upload with a quarter to spare, copies every mesh into it packed from the start, one copy region for each run of neighbors, and keeps the old buffer until the frame slot that copied out of it comes round. Each frame reads the arena's address after the upkeep, for its push constants.
- **The arena's limit is a quarter of the largest budget VMA reports for a device-local heap** (80% of the heap, since the device does not enable VK_EXT_memory_budget), at most RFC 0007's 2^29 quads; the startup size keeps to it too. At the limit a refusal leaves the arena full, which the overlay shows and RFC 0006's memory budget answers.
- **The 30-minute trace is a model, not a recording:** the camera cruises, sprints, hovers, turns and twice teleports a million blocks; each chunk border crossed loads the disc's new columns and unloads those past the hysteresis; each column draws 2 to 4 chunks of rough terrain's sizes, 1,849 quads on average with a long tail; a few chunks a second are remeshed; at most 64 meshes go in a frame. At the app's capacity it uploads 1.27 million meshes with the arena 94% full at its peak, and nothing is refused; at 90% of that capacity it repacks, and no upload waits more than 6 frames. A debug build replays both in 5 seconds.
- **The overlay adds a line:** the MiB compaction moved, whether it is moving, the repacks, the limit, and whether the arena is full.
- **An independent review** (Opus) found no runtime defect: the copies within one buffer never overlap, the barrier covers the vertex shader's reads, and the old arena outlives every frame that reads it, each checked with planted mutants. It found one merge blocker, designated initializers out of order in a test, which GCC and Linux Clang reject and Apple Clang accepts; a refused batch's ranges retired instead of returned at once, which hid room from the upkeep and caused a repack that was not needed; and no test that could see the push constant reading the arena's address before the upkeep. All three were fixed, the last with a test of the frame a repack happens in. At the limit, compaction is not tried again until a frame passes without a refusal: RFC 0006's memory budget answers a full arena by lowering the view distance, which unloads meshes and makes room.

GitHub Actions was blocked by billing while M3 was built, so these pull requests merged on local runs of CI's jobs; GCC, AddressSanitizer, ThreadSanitizer and lavapipe have not built them yet, so the scene test and the draw benchmark have not run on lavapipe.
