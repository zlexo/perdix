# RFC 0001: Renderer bootstrap

| | |
|---|---|
| **Status** | Draft |
| **Milestone** | M1 First Cube |
| **Issue** | #8 |

## Summary

The Vulkan foundation that takes the engine from nothing to a textured cube you can fly around: the window, instance and device selection, the swapchain, frames in flight and their synchronization, GPU memory, shaders, a camera, and the tooling (validation, profiling, a debug overlay) that every later rendering feature relies on. The cube is small on purpose. The plumbing under it is the deliverable.

## Motivation

Every later milestone renders through this layer. The bugs that are hardest to find later (synchronization hazards, leaks, crashes on resize, precision loss far from the origin) are cheapest to prevent here, with validation clean from the first frame.

Three decisions made here shape everything after it and are expensive to change later:

- **How depth works.** View distance is the north star, so depth precision far away matters from day one.
- **Where positions live.** A world that is huge cannot be drawn with 32-bit float world coordinates. A million blocks from the origin, neighboring floats are a sixteenth of a block apart, one texel; at 10 million they are a whole block apart, and vertices snap and jitter.
- **How the GPU reads geometry.** The cube is drawn the way chunks will be: by pulling data from buffers, not through fixed vertex input.

## Goals and non-goals

Goals:

- A window with a Vulkan surface on macOS (KosmicKrisp and MoltenVK), Linux and Windows.
- Device selection that checks the feature floor of [ADR 0002](../adr/0002-vulkan.md) and, when a device falls short, names every missing feature.
- A swapchain that survives resizing, minimizing, and moving between displays with different scale factors.
- Two frames in flight, with zero validation errors, synchronization validation included.
- GPU memory through the Vulkan Memory Allocator, using unified memory directly where the GPU has it.
- Shaders compiled at build time.
- A fly camera with reversed-Z depth, an infinite far plane and camera-relative rendering.
- A textured cube, drawn through the bindless texture path every later system will use.
- Tooling: validation routed to the log, a debug name on every Vulkan object, Tracy CPU and GPU zones, and a Dear ImGui overlay.
- Headless rendering, so Linux CI can render frames with lavapipe and fail on any validation message.

Non-goals:

- Chunks, mesh formats and the chunk renderer: RFC 0003 and M3.
- GPU memory arenas, streaming uploads and culling: RFC 0007 (M3).
- Lighting (M6) and translucency (M7).
- Shader hot reload: a later tooling RFC.
- Multiple windows, HDR output, VR.
- Any second graphics API (ADR 0002).

## Prior art

- **Vulkan Guide** (vkguide.dev): VMA, dynamic rendering, ImGui integration. The frame loop here follows its structure.
- **Khronos Vulkan Tutorial and Vulkan Samples**: synchronization2, dynamic rendering, timeline semaphores, buffer device address, and the Khronos guidance on swapchain semaphore reuse.
- **zeux/niagara** (Arseny Kapoulkine): a compact modern Vulkan renderer built on volk and buffer device address; the model for keeping the C API readable without a heavy wrapper.
- **"Depth Precision Visualized"** (Nathan Reed, 2015): reversed float depth gives nearly uniform precision with distance; with an infinite projection the far plane disappears.
- **"Yet another blog explaining Vulkan synchronization"** (Hans-Kristian Arntzen): the mental model the synchronization code is written against.
- **Floating-point precision far from the origin.** Minecraft's Bedrock Edition uses 32-bit floats for positions: some blocks render wrongly from 131,072 blocks out, and from 16,777,216 every block snaps to even coordinates (Minecraft Wiki, "Bedrock Edition distance effects"). Unity's High Definition Render Pipeline renders camera-relative by default for the same reason. This is not what makes the Far Lands: those are an integer overflow in Minecraft's terrain noise at 12,550,824 blocks (Minecraft Wiki, "Far Lands"), which camera-relative rendering does not touch; RFC 0008's noise uses 64-bit lattice indices.

## Design

### Libraries

| Library | Role | Why |
|---|---|---|
| Vulkan SDK | headers, validation layers, shader compiler | a manual install (RFC 0000 lists them all); ships both KosmicKrisp and MoltenVK on macOS |
| volk | loads Vulkan entry points | calls go straight to the driver with no loader trampoline, and the code never links the loader statically |
| Vulkan Memory Allocator | GPU memory | suballocation, memory type selection and budget tracking, done right once |
| SDL3 or GLFW | window, input, surface | open question 1 |
| Dear ImGui | debug overlay | the standard; has a Vulkan backend with dynamic rendering |
| Tracy | profiler | CPU and GPU zones with nanosecond resolution, live |
| glm | float math for the camera | open question 4 |

Everything comes through `FetchContent` as in RFC 0000, except the Vulkan SDK.

We call the Vulkan C API directly, behind thin move-only C++ types (`Buffer`, `Image`, `Pipeline`) that own exactly one handle and its memory. Vulkan-Hpp was considered and left out: it is 14 headers, and including `vulkan.hpp` pulls in about 17 MiB of Vulkan headers (SDK 1.4.363.0), parsed again in every file that includes it, while the thin types give the same safety at a fraction of the cost.

### Layering

`engine/render` (`vx::render`) depends on `core` and `mesh`, never on the window library. The window lives in `app`. That raises a chicken-and-egg problem: a surface needs an instance, and the instance needs the window library's surface extensions. The renderer solves it without learning about windows:

```cpp
struct SurfaceSource {
    std::span<const char* const> instance_extensions;   // from the window library
    std::function<VkSurfaceKHR(VkInstance)> create;     // called after each instance is created
};
```

`app` fills this in from the window library; `render` creates the instance, then calls `create`. Headless runs pass no surface source at all.

### Instance

- API version 1.3.
- Extensions: the surface extensions from `SurfaceSource`; `VK_EXT_debug_utils` in debug builds. `VK_KHR_portability_enumeration` only for the portability fallback under device selection, and then always with the `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag: the extension alone lists no portability driver.
- Validation in debug builds: `VK_LAYER_KHRONOS_validation` with synchronization validation on. Messages go to the engine log with their message ID.

### Device selection

Selection is a pure function over the features a device reports, so it is unit tested without a GPU:

```cpp
struct FeatureReport {
    bool supported;
    std::vector<std::string_view> missing;   // e.g. "synchronization2", "multiDrawIndirect"
};

[[nodiscard]] FeatureReport check_feature_floor(const DeviceFeatures& features);
```

The floor, which `check_feature_floor` checks item by item:

- Vulkan 1.3.
- `dynamicRendering`, `synchronization2`, `timelineSemaphore` and `bufferDeviceAddress`.
- `descriptorIndexing` with `runtimeDescriptorArray`, `descriptorBindingPartiallyBound` and `shaderSampledImageArrayNonUniformIndexing`, for the bindless texture array.
- `multiDrawIndirect` and `drawIndirectFirstInstance`, for RFC 0007's chunk passes: one indirect command per run of faces, whose `firstInstance` selects the chunk's origin.

Optional features are enabled when the device offers them, and logged at startup when it does not:

- `samplerAnisotropy`, for RFC 0004's block texture filtering. Without it, filtering stays trilinear.
- `fillModeNonSolid`, for RFC 0003's wireframe toggle. Without it, the overlay hides the toggle. KosmicKrisp does not offer it (SDK 1.4.363.0) and MoltenVK does, so on the reference machine the wireframe needs MoltenVK.

Among devices that pass the floor, one without `VK_KHR_portability_subset` beats one that lists it, and then a discrete GPU beats an integrated one. If none passes, startup fails with the device names and their missing features.

**Drivers on macOS.** The SDK installs two drivers for the same GPU: KosmicKrisp, which is conformant, and MoltenVK, a portability driver. The loader shows portability drivers only to an instance that opts in with the extension and flag above, and then lists MoltenVK first (LunarG's macOS guide for SDK 1.4.363.0). An engine that always opted in and kept the first device that passes would quietly run on MoltenVK. So:

1. The instance does not opt in, and selection sees conformant drivers only.
2. If no device passes the floor, or the instance fails with `VK_ERROR_INCOMPATIBLE_DRIVER` because no conformant driver is installed, the instance is created again, opted in. That is the path on Macs only MoltenVK supports: Intel Macs, and macOS before 26.
3. If the chosen device lists `VK_KHR_portability_subset`, device creation enables it, as the specification requires (VUID-VkDeviceCreateInfo-pProperties-04451).

`VK_DRIVER_FILES` overrides the choice: the loader then sees only the drivers it names. Pointed at MoltenVK's manifest, it hides KosmicKrisp and step 2 finds MoltenVK, which is how a driver bug is told apart from our own. A "Debugging the renderer" page documents both manifests.

**Device creation.** Release builds leave `robustBufferAccess2` and `pipelineRobustness` off. With either one enabled, KosmicKrisp runs a compute pass in front of every indexed indirect draw and splits the render pass around it (`requires_unroll_robustness` in its `kk_cmd_draw.c`, SDK 1.4.363.0), and RFC 0007 draws every chunk that way.

At startup the engine logs the GPU, driver name and version, API version, present mode and whether the portability fallback was taken, and the overlay shows the same.

### Swapchain

- Format: the first sRGB format offered, preferring `B8G8R8A8_SRGB`.
- Present mode: FIFO by default (always available, no tearing). Immediate, when offered, for benchmarking, switchable at runtime from the overlay.
- Image count: the minimum plus one, clamped to the maximum unless the maximum is 0, which means no limit.
- Size: the framebuffer size in pixels, never the window size in points, so high-DPI displays render at native resolution.
- Recreated on `VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR` or a resize event, passing the old swapchain. A zero-sized framebuffer (minimized) pauses rendering instead of recreating.

### Frames in flight

Two frames in flight: the CPU records frame N+1 while the GPU draws frame N. More frames would add a frame of input latency for throughput a cube does not need; this constant is revisited with numbers in M3.

- **Pacing:** one timeline semaphore. Frame N signals value N; before reusing frame slot N, the CPU waits for value N minus 2. One semaphore replaces a fence per frame.
- **Acquire:** a binary semaphore per frame slot.
- **Present:** a binary semaphore per *swapchain image*, not per frame slot. The presentation engine may still hold a semaphore after the frame slot comes around again; reusing it is a real hazard the validation layers report.
- **Barriers:** synchronization2 (`vkCmdPipelineBarrier2`) for every layout transition; dynamic rendering, so no render pass or framebuffer objects.
- **Per frame slot:** a command pool and a command buffer, reset as a pool each frame. No per-frame CPU heap allocations after the first frames.

### Memory

Through VMA with buffer device address enabled. The reference machine has unified memory: Apple GPUs offer memory that is both device-local and host-visible. Buffers that receive uploads follow VMA's documented pattern for this case, and also get `VK_BUFFER_USAGE_TRANSFER_DST_BIT`:

```cpp
const VmaAllocationCreateInfo upload_target{
    .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
             VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT,
    .usage = VMA_MEMORY_USAGE_AUTO,
};
```

VMA then prefers memory that is device-local and host-visible, and falls back to device-local memory alone when there is none or it is full. Sequential-write access alone would not do: with automatic usage it requires host-visible memory, so a discrete GPU would never stage.

So `upload` asks where the destination landed (`vmaGetAllocationMemoryProperties`), not what kind of GPU it runs on. Host-visible memory is written directly, on unified memory and on discrete GPUs with resizable BAR alike; anything else goes through a staging buffer:

```cpp
// Copies bytes into dst at dst_offset: through dst's mapping when its memory
// is host-visible (flushed if it is not coherent), otherwise into the frame
// slot's staging buffer, with the copy recorded into `cmd`.
void upload(UploadContext& ctx, VkCommandBuffer cmd, const Buffer& dst,
            VkDeviceSize dst_offset, std::span<const std::byte> bytes);
```

The destination offset lets RFC 0007 write each chunk mesh into its range of one shared arena.

### Geometry: vertex pulling

The pipeline has no vertex input state. The vertex shader reads its data from storage buffers through buffer device addresses passed in push constants, laid out for the camera-relative scheme below:

```cpp
struct DrawPush {                  // 96 of the 128 bytes Vulkan 1.3 guarantees
    glm::mat4 view_projection;     // offset 0: rotation and projection only
    VkDeviceAddress vertices;      // offset 64: the cube's vertices; RFC 0007's quads
    VkDeviceAddress origins;       // offset 72: one float4 per instance
    glm::vec3 camera_fraction;     // offset 80: the camera's position within its block
    std::uint32_t texture;         // offset 92: index into the bindless texture array
};
static_assert(sizeof(DrawPush) == 96 && offsetof(DrawPush, texture) == 92);
```

The shader's push constant block has the same offsets: 80 meets a `vec3`'s 16-byte alignment, and `texture` takes the 4 bytes after it. RFC 0007's `ChunkPush` is this layout without `texture`.

The cube is 36 vertices in a buffer, drawn as one instance whose origin is `origins[0]`. Chunks will replace the vertices with packed quads (RFC 0003) and draw many instances (RFC 0007), but the path (buffer addresses, a push constant, a vertex shader that decodes) stays the same.

### Textures: one bindless array

One global descriptor set holds an array of separate sampled images (descriptor indexing, partially bound) for textures in general, and a second binding for block textures: one 2D array image with a layer per block texture (RFC 0004). A quad's texture number selects a layer of that image. A layer is a third texture coordinate rather than a descriptor index, so it varies freely within a draw without non-uniform indexing.

How block textures are filtered and mipmapped is RFC 0004's. The cube samples one generated 16 by 16 checker texture with nearest filtering.

### Depth: reversed-Z with an infinite far plane

Depth is a 32-bit float. Near maps to 1 and infinity maps to 0, the depth test is `GREATER`, and depth clears to 0. Float precision is densest near zero, which reversed-Z puts at the far end, so precision stays nearly uniform with distance. With the far plane at infinity, the projection itself puts no limit on view distance; only the world streaming does.

```cpp
// Reversed-Z, infinite far plane, Vulkan clip space (y down, depth 0..1).
glm::mat4 perspective_reversed_infinite(float fov_y, float aspect, float z_near) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    return {
        {f / aspect, 0.0f, 0.0f, 0.0f},
        {0.0f, -f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, z_near, 0.0f},
    };
}
```

### Camera-relative rendering

The camera's position is stored on the CPU in 64-bit (`double` now, integer chunk coordinates later). Each frame splits it into the block the camera is in, as integers, and the camera's fraction within that block, from 0 to 1 on each axis. The GPU never sees a world position, and RFC 0007 draws chunks the same way:

- **View-projection:** rotation and projection only.
- **Origins:** each object's origin relative to the camera's block, computed on the CPU in 64-bit integers. A whole number of blocks is exact as a float up to 2^24 (16,777,216) blocks from the camera, far beyond any view distance. Each frame slot owns a host-visible buffer of origins, one `float4` per instance, rewritten every frame.
- **Fraction:** `camera_fraction`, subtracted from every vertex in the shader.

```glsl
// Sketched in GLSL; open question 2 picks the language.
vec3 p = origins[gl_InstanceIndex].xyz + vertices[gl_VertexIndex].position;
gl_Position = view_projection * vec4(p - camera_fraction, 1.0);
```

An origin plus a block corner is a whole number, exact as a float, so a corner shared by two chunks gets the same float from both, and neighbors meet without cracks. The first rounding comes when the fraction is subtracted, on values that are small near the viewer, where precision is visible, however far from the world origin the camera flies.

Controls: WASD to move, Space and Shift for up and down, Ctrl to go faster, raw mouse motion to look, Escape to release the mouse. Default vertical field of view: 70 degrees.

### Tooling

- **Validation** messages go to the log with their message ID. In debug builds a validation error is counted and shown in red in the overlay; in CI it fails the run.
- **Debug names** on every Vulkan object, so validation messages and GPU captures read in our terms ("cube vertices", not a handle number).
- **Tracy** zones from the first frame: acquire, record, submit, present; GPU zones through timestamp queries.
- **Dear ImGui overlay:** CPU and GPU frame time graphs, GPU and driver, present mode switch, camera position, validation error count.
- **GPU captures on macOS:** RenderDoc does not run there, so the debugging page documents Xcode's Metal frame capture through MoltenVK (`MTL_CAPTURE_ENABLED=1` with `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE`).

### Headless rendering and CI

A render target is either a swapchain image or an offscreen image. Without a surface source, the renderer draws into an offscreen image, which needs no window. A Linux CI job installs Mesa's lavapipe (a software Vulkan driver) and the validation layers, renders a few frames of the cube offscreen with synchronization validation on, and fails on any validation warning or error. It is the only non-Apple Vulkan driver every change runs on.

### API sketch

```cpp
namespace vx::render {

enum class PresentMode { fifo, immediate };

struct RendererDesc {
    std::optional<SurfaceSource> surface;   // empty: headless
    bool validation = true;                 // debug builds
    PresentMode present_mode = PresentMode::fifo;
};

struct FrameInputs {
    CameraView camera;                      // rotation, projection, the camera's block and fraction
    const ImDrawData* overlay = nullptr;
};

class Renderer {
public:
    // Fails with a readable reason, for example the missing device features.
    static CreateResult create(const RendererDesc& desc);

    void resize(Extent framebuffer_size);
    void render(const FrameInputs& frame);
    void set_present_mode(PresentMode mode);
    [[nodiscard]] const DeviceInfo& device_info() const;
};

} // namespace vx::render
```

`CreateResult` follows whatever RFC 0000 settles for errors (open question 1 there).

## Alternatives considered

- **vk-bootstrap** for instance, device and swapchain setup: saves a few hundred lines. Open question 3.
- **Vulkan-Hpp** (`vk::raii`): safer handles for free, but `vulkan_raii.hpp` brings about 19 MiB of headers into every translation unit that includes it. Thin hand-written types win on compile time.
- **Classic vertex input** instead of vertex pulling: simpler for one cube, but a dead end for packed chunk quads, and a second path to maintain.
- **Standard Z** (near 0, far 1): precision collapses with distance, which works against the north star.
- **World-space float positions**: fine near the origin, but float precision loss makes them jitter far from it, by a whole block at 10 million blocks. Retrofitting camera-relative rendering touches every system that produces positions, so it is done first.
- **Three frames in flight:** smoother when the CPU spikes, at the cost of a frame of latency. Revisited with numbers in M3.
- **A dedicated transfer queue:** not worth it for one cube, and RFC 0007 settles it for chunks: copies share the graphics queue, because KosmicKrisp exposes a single queue and a transfer queue would only pay off on discrete GPUs we cannot measure.

## Performance budget

On the reference machine, release build, cube scene:

- CPU time per frame (record and submit): under 0.5 ms.
- GPU time per frame at 1440p: under 0.5 ms.
- Startup to first presented frame: under 500 ms, not counting the first shader cache build.
- Zero CPU heap allocations per frame after the first frames, checked with Tracy's memory profiling.

## Test plan

- **Unit tests** (no GPU): `check_feature_floor` names every missing feature; for one GPU reported by both drivers, ranking picks the device without `VK_KHR_portability_subset`; the reversed infinite projection maps the near plane to depth 1 and points far away toward 0; camera-relative conversion keeps a position near the camera within float precision at 10 million blocks from the origin, and two neighboring chunks give their shared corner the same float.
- **CI:** the lavapipe job renders offscreen with synchronization validation on and fails on any message. Control: a pull-request-local change that drops one barrier must turn the job red before the job is trusted.
- **By hand on the reference machine:** a plain run picks KosmicKrisp, and `VK_DRIVER_FILES` set to MoltenVK's manifest runs on MoltenVK through the fallback; resize, minimize and restore; switch present modes; fly to 10 million blocks and confirm the cube does not jitter; capture one frame in Xcode.

## Pull request plan

1. This RFC.
2. `engine/render` skeleton: Vulkan SDK detection in CMake, volk and VMA, instance and debug messenger; `check_feature_floor` with its tests.
3. Device selection with the portability fallback, logical device and queue; the startup device report.
4. Window in `app`, `SurfaceSource`, swapchain, clear to a color; resize and minimize.
5. Frames in flight: timeline semaphore pacing, per-image present semaphores; synchronization validation clean.
6. Offscreen render target and the lavapipe CI job, with its control. Early, so every later pull request runs on it.
7. Shader build step and the first pipeline: a triangle through vertex pulling.
8. Reversed-Z depth, the fly camera and camera-relative rendering; the cube.
9. The bindless texture array and sampler; the textured cube.
10. Tracy CPU and GPU zones.
11. The Dear ImGui overlay.
12. Debug names everywhere, and the "Debugging the renderer" page (driver switching, Xcode capture).

## Open questions

1. **SDL3 or GLFW** for the window and input. Recommendation: **SDL3**. It will also cover audio and gamepads, so it is one platform layer instead of three libraries later. Cost: a clean build of SDL3 adds about a minute, mostly felt on CI.
2. **Slang or GLSL** for shaders. Recommendation: **Slang**. Khronos hosts it, the Vulkan SDK ships its compiler, the Khronos Vulkan Tutorial ships its shaders in both, and its modules and generics keep a growing shader codebase organized. GLSL has more voxel-specific examples online.
3. **Own setup code or vk-bootstrap.** Recommendation: **own code**. It is a few hundred lines, written once, it is the part of Vulkan worth understanding, and it lets device selection explain itself precisely.
4. **glm or our own math types.** Recommendation: **glm** for float math now, configured for Vulkan's depth range. Integer world coordinates are our own types in the world layer either way.

## As built

Not built yet.
