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
- **Where positions live.** A world that is huge cannot be drawn with 32-bit float world coordinates; Minecraft's Far Lands are the famous example of what happens far from the origin.
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
- GPU memory arenas for streaming, transfer queues and culling: M3.
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
- **Minecraft's Far Lands**: what float precision does to a world far from its origin. Camera-relative rendering is the standard cure.

## Design

### Libraries

| Library | Role | Why |
|---|---|---|
| Vulkan SDK | headers, validation layers, shader compiler | the only manual install; ships both KosmicKrisp and MoltenVK on macOS |
| volk | loads Vulkan entry points | calls go straight to the driver with no loader trampoline, and the code never links the loader statically |
| Vulkan Memory Allocator | GPU memory | suballocation, memory type selection and budget tracking, done right once |
| SDL3 or GLFW | window, input, surface | open question 1 |
| Dear ImGui | debug overlay | the standard; has a Vulkan backend with dynamic rendering |
| Tracy | profiler | CPU and GPU zones with nanosecond resolution, live |
| glm | float math for the camera | open question 4 |

Everything comes through `FetchContent` as in RFC 0000, except the Vulkan SDK.

We call the Vulkan C API directly, behind thin move-only C++ types (`Buffer`, `Image`, `Pipeline`) that own exactly one handle and its memory. Vulkan-Hpp was considered and left out: its single header is several megabytes and costs compile time in every file that includes it, while the thin types give the same safety at a fraction of the cost.

### Layering

`engine/render` (`vx::render`) depends on `core` and `mesh`, never on the window library. The window lives in `app`. That raises a chicken-and-egg problem: a surface needs an instance, and the instance needs the window library's surface extensions. The renderer solves it without learning about windows:

```cpp
struct SurfaceSource {
    std::span<const char* const> instance_extensions;   // from the window library
    std::function<VkSurfaceKHR(VkInstance)> create;     // called once the instance exists
};
```

`app` fills this in from the window library; `render` creates the instance, then calls `create`. Headless runs pass no surface source at all.

### Instance

- API version 1.3.
- Extensions: the surface extensions from `SurfaceSource`; `VK_EXT_debug_utils` in debug builds; `VK_KHR_portability_enumeration` when present, so MoltenVK's device is listed.
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

The floor: Vulkan 1.3; `dynamicRendering`, `synchronization2`, `timelineSemaphore`, `bufferDeviceAddress`, `descriptorIndexing` with `runtimeDescriptorArray`, `descriptorBindingPartiallyBound` and `shaderSampledImageArrayNonUniformIndexing`; `multiDrawIndirect`. `samplerAnisotropy` is not part of the floor: it is enabled when the device offers it, for RFC 0004's texture filtering. Among devices that pass, a discrete GPU beats an integrated one. If none passes, startup fails with the device names and their missing features.

On macOS both KosmicKrisp and MoltenVK can expose the same GPU. The engine does not choose between drivers; the Vulkan loader does, through `VK_DRIVER_FILES`. Switching drivers to tell a driver bug from our own is an environment variable, documented in a "Debugging the renderer" page.

At startup the engine logs the GPU, driver name and version, API version and present mode, and the overlay shows the same.

### Swapchain

- Format: the first sRGB format offered, preferring `B8G8R8A8_SRGB`.
- Present mode: FIFO by default (always available, no tearing). Immediate, when offered, for benchmarking, switchable at runtime from the overlay.
- Image count: the minimum plus one, clamped to the maximum.
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

Through VMA with buffer device address enabled. The reference machine has unified memory: Apple GPUs offer memory that is both device-local and host-visible. VMA's automatic usage with sequential-write host access picks that memory when it exists, so uploads on the reference machine write straight into GPU memory, while discrete GPUs go through a staging buffer. One upload helper hides the difference:

```cpp
// Copies bytes into a GPU buffer: a direct write on unified memory,
// a staging copy recorded into `cmd` otherwise.
void upload(UploadContext& ctx, VkCommandBuffer cmd, const Buffer& dst, std::span<const std::byte> bytes);
```

### Geometry: vertex pulling

The pipeline has no vertex input state. The vertex shader reads its data from a storage buffer through a buffer device address passed in push constants:

```cpp
struct DrawPush {                  // 128 bytes is the guaranteed minimum
    glm::mat4 view_projection;     // 64 bytes
    VkDeviceAddress vertices;      // 8 bytes
    std::uint32_t texture;         // index into the bindless texture array
};
```

The cube is 36 vertices in a buffer. Chunks will replace them with packed quads (RFC 0003), but the path (a buffer address, a push constant, a vertex shader that decodes) stays the same.

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

The camera's position is stored on the CPU in 64-bit (`double` now, integer chunk coordinates later). The view matrix the GPU sees has rotation only. Every object's position is converted to "relative to the camera" on the CPU, in 64-bit, before it becomes a 32-bit float. Floats on the GPU therefore stay small near the viewer, where precision is visible, however far from the origin the camera flies.

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
    CameraView camera;                      // rotation, camera-relative origin, projection
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
- **Vulkan-Hpp** (`vk::raii`): safer handles for free, but a multi-megabyte header in every translation unit. Thin hand-written types win on compile time.
- **Classic vertex input** instead of vertex pulling: simpler for one cube, but a dead end for packed chunk quads, and a second path to maintain.
- **Standard Z** (near 0, far 1): precision collapses with distance, which works against the north star.
- **World-space float positions**: fine near the origin, jitter far from it. Retrofitting camera-relative rendering touches every system that produces positions, so it is done first.
- **Three frames in flight:** smoother when the CPU spikes, at the cost of a frame of latency. Revisited with numbers in M3.
- **A dedicated transfer queue:** not worth it for one cube; M3's streaming RFC owns it.

## Performance budget

On the reference machine, release build, cube scene:

- CPU time per frame (record and submit): under 0.5 ms.
- GPU time per frame at 1440p: under 0.5 ms.
- Startup to first presented frame: under 500 ms, not counting the first shader cache build.
- Zero CPU heap allocations per frame after the first frames, checked with Tracy's memory profiling.

## Test plan

- **Unit tests** (no GPU): `check_feature_floor` names every missing feature; the reversed infinite projection maps the near plane to depth 1 and points far away toward 0; camera-relative conversion keeps a position near the camera within float precision at 10 million blocks from the origin.
- **CI:** the lavapipe job renders offscreen with synchronization validation on and fails on any message. Control: a pull-request-local change that drops one barrier must turn the job red before the job is trusted.
- **By hand on the reference machine:** run on KosmicKrisp and on MoltenVK; resize, minimize and restore; switch present modes; fly to 10 million blocks and confirm the cube does not jitter; capture one frame in Xcode.

## Pull request plan

1. This RFC.
2. `engine/render` skeleton: Vulkan SDK detection in CMake, volk and VMA, instance and debug messenger; `check_feature_floor` with its tests.
3. Device selection, logical device and queue; the startup device report.
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
