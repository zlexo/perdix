#pragma once

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>
#include <vx/render/texture_table.hpp>
#include <vx/world/coordinates.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tracy {
class VkCtx; // Tracy's GPU timing context, defined in profiling builds only
} // namespace tracy
struct ImDrawData;   // Dear ImGui's draw lists for one frame
struct ImGuiContext; // Dear ImGui's state: windows, input, fonts

namespace vx::render {

struct RendererDesc {
    // The window to draw into; empty draws headless, into an offscreen image.
    std::optional<SurfaceSource> surface;
    Extent framebuffer; // the window's size in pixels, or the offscreen image's
    // VK_LAYER_KHRONOS_validation with synchronization validation: debug builds.
    bool validation = false;
    PresentMode present_mode = PresentMode::fifo;
    // The ImGui context the overlay is built in, by the app, which keeps it
    // alive longer than the renderer; null when there is no overlay.
    ImGuiContext* overlay = nullptr;
    // RFC 0004's block texture array: every layer at every level, as
    // build_block_textures lays them out, such as a BlockPack's textures.
    // Uploaded at startup; empty binds an array of one layer, the checker.
    std::span<const std::byte> block_textures;
    // The chunk arena's room, in quads (RFC 0007): 32 MiB, until the app
    // sizes it from its view distance.
    std::uint32_t chunk_arena_quads = std::uint32_t{1} << 22U;
    // Tests set it to take the staged upload path on any device, as a
    // discrete GPU without host-visible device memory would.
    bool stage_chunk_uploads = false;
    // The most commands one indirect call draws, below the device's limit:
    // tests set it to split each pass's calls. 0 is the device's.
    std::uint32_t draw_call_limit = 0;
};

// A color in linear RGB, with alpha. The target's sRGB format encodes it.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// What one frame draws (RFC 0001): for now, cubes as a camera sees them.
struct FrameInputs {
    Color sky; // what the frame clears to
    Camera camera;
    // Where cubes stand: each one's block, its lowest corner. At most
    // Renderer::max_cubes.
    std::span<const glm::i64vec3> cubes;
    // The overlay's draw lists, from ImGui::GetDrawData(), drawn over the
    // frame; null draws none. Not const: drawing updates its textures' state.
    ImDrawData* overlay = nullptr;
    // What the frame changes in the chunk arena (RFC 0007), asked once the
    // frame's slot is free and, in a window, its image acquired. A skipped
    // frame does not ask, so nothing uploads or unloads while a window is
    // minimized. Null changes nothing.
    ChunkChanges* chunk_changes = nullptr;
};

// How block textures are filtered (RFC 0004).
enum class BlockFilter : std::uint8_t {
    // Texels crisp up close, mipmapped and anisotropic far away: the default.
    sharpened,
    // The same without anisotropic filtering, to see and measure what it adds.
    sharpened_isotropic,
    // The classic look: crisp up close, shimmering far away.
    nearest,
};

enum class Frame : std::uint8_t {
    drawn,
    skipped, // the framebuffer has no area, as when the window is minimized
};

// What only the renderer can measure of the last frame it drew, for RFC
// 0001's frame time graphs.
struct FrameTimes {
    // How long render() waited in the calls that wait for the GPU or the
    // display: for the frame slot, acquire, submit and present. Submit is
    // one, since MoltenVK waits there for its drawable. The rest of a
    // frame's time on the CPU is work.
    std::chrono::nanoseconds waited{0};
    // How long the GPU took for the newest frame whose slot has come round
    // again, frames_in_flight frames before this one: from the first of its
    // commands to the last, after the frame before it. Empty for the first
    // frames, when the queue writes no timestamps, and for the odd frame
    // whose timestamps come in late: on KosmicKrisp, about one frame in 200
    // drawn to a swapchain.
    std::optional<std::chrono::nanoseconds> gpu;
};

// Draws frames into a window's swapchain, or headless into an offscreen image
// (RFC 0001). Each frame clears its target to the sky and draws cubes and the
// chunks in the arena (RFC 0007) camera-relative, with reversed-Z depth and
// two frames in flight: the CPU records frame N + 1 while the GPU draws frame
// N.
class Renderer {
public:
    [[nodiscard]] static Result<Renderer> create(const RendererDesc& desc);

    Renderer(Renderer&& other) noexcept;
    // Like the context it owns, it has no move assignment.
    Renderer& operator=(Renderer&& other) = delete;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    // The framebuffer has this size now; the target follows on the next frame.
    void resize(Extent framebuffer) noexcept { framebuffer_ = framebuffer; }

    // Presents in `mode` from the next frame, on a new swapchain. Immediate
    // falls back to FIFO where the surface does not offer it (RFC 0001), so
    // the swapchain's choice says what presentation got. Headless, there is
    // no swapchain, and the mode is only kept.
    void set_present_mode(PresentMode mode) noexcept;
    // The mode asked for, at creation or since.
    [[nodiscard]] PresentMode present_mode() const noexcept { return present_mode_; }

    // How many cubes one frame can draw.
    static constexpr std::size_t max_cubes = 256;

    // Clears the target to the sky, makes the frame's chunk changes, draws
    // the cubes and the arena's chunks as the camera sees them, and presents
    // the target when it is a window's. An error from the chunk changes or
    // the draw lists leaves staged copies that never ran: the renderer is
    // not drawn with again.
    [[nodiscard]] Result<Frame> render(const FrameInputs& inputs);

    // Headless only: the last frame's pixels, 8-bit RGBA, row after row.
    [[nodiscard]] Result<std::vector<std::uint8_t>> capture();

    // The last frame drawn's times.
    [[nodiscard]] const FrameTimes& frame_times() const noexcept { return times_; }

    [[nodiscard]] const Context& context() const noexcept { return context_; }
    // Set 0, for whatever else draws with it: tools and tests now, and the
    // chunk renderer (RFC 0007).
    [[nodiscard]] const TextureTable& textures() const noexcept { return textures_; }
    // The layers of the block texture array in set 0: the pack's, or the
    // checker's one.
    [[nodiscard]] std::uint32_t block_texture_layers() const noexcept {
        return block_textures_.layers();
    }

    // RFC 0003's milestone, one chunk, now in the arena with the rest:
    // `mesh` takes the place of the chunk set before, wherever that was,
    // once the frames in flight are done. An empty mesh leaves nothing to
    // draw. Outside any frame and its budget, for tools and tests; the world
    // streams in through FrameInputs::chunk_changes, and never where a
    // streamed chunk is: the generations set_chunk gives are its own.
    [[nodiscard]] Result<void> set_chunk(ChunkPos position, const ChunkMesh& mesh);
    // The overlay's switches for culling and merging (RFC 0007), from the
    // next frame.
    void set_draw_switches(const DrawSwitches& switches) noexcept {
        chunks_.set_switches(switches);
    }
    // Freeze culling (RFC 0007): cull as the next frame's camera sees, while
    // the camera moves on.
    void freeze_culling(bool frozen) noexcept { chunks_.set_frozen(frozen); }
    // How long each frame's chunk uploads may take (RFC 0007): 1 ms, raised
    // while a loading screen shows. A frame's first upload always goes.
    void set_chunk_budget(std::chrono::nanoseconds budget) noexcept {
        chunks_.uploads().set_budget(budget);
    }
    // The arena, its uploads and the last frame's culling, for the overlay.
    [[nodiscard]] const ChunkRenderer& chunks() const noexcept { return chunks_; }
    // How the chunk's textures are filtered, from the next frame.
    void set_block_filter(BlockFilter filter) noexcept { block_filter_ = filter; }
    [[nodiscard]] BlockFilter block_filter() const noexcept { return block_filter_; }
    // How much ambient occlusion darkens the chunk's corners: from 0, not at
    // all, to 1, as Minecraft's smooth lighting does, the default (RFC 0003,
    // open question 1: chosen by eye on the overlay's slider).
    void set_occlusion_strength(float strength) noexcept {
        occlusion_strength_ = std::clamp(strength, 0.0F, 1.0F);
    }
    [[nodiscard]] float occlusion_strength() const noexcept { return occlusion_strength_; }
    // Whether the chunk can be drawn as the edges of its quads' triangles:
    // where the device offers fillModeNonSolid (RFC 0003), which MoltenVK
    // does and KosmicKrisp does not.
    [[nodiscard]] bool wireframe_offered() const noexcept {
        return chunk_drawing_.wireframe.has_value();
    }
    // Draws the chunk as a wireframe from the next frame, where offered.
    void set_wireframe(bool on) noexcept { wireframe_ = on; }
    [[nodiscard]] bool wireframe() const noexcept { return wireframe_ && wireframe_offered(); }
    // Null when headless.
    [[nodiscard]] const Swapchain* swapchain() const noexcept {
        return swapchain_.has_value() ? &swapchain_.value() : nullptr;
    }

private:
    // The cube: its pipeline, its 36 vertices, and its texture, a generated
    // checker, in the texture table.
    struct Cube {
        GraphicsPipeline pipeline;
        Buffer vertices;
        Image checker;
        std::uint32_t checker_slot = 0;
    };

    // What draws chunks (RFC 0003, RFC 0007): a pipeline for each pass,
    // from one shader, and the index buffer every command shares, 0, 1, 2,
    // 2, 3, 0 for each quad, as many as a chunk can have.
    struct ChunkDrawing {
        GraphicsPipeline opaque;
        GraphicsPipeline cutout;
        std::optional<GraphicsPipeline> wireframe; // where the device offers it
        Buffer indices;
    };

    Renderer(Context context, std::optional<Swapchain> swapchain, std::optional<Image> offscreen,
             Image depth, TextureTable textures, Image block_textures, Cube cube,
             ChunkDrawing chunk_drawing, ChunkRenderer chunks, Extent framebuffer,
             PresentMode present_mode);
    [[nodiscard]] Result<void> create_frame_objects();
    [[nodiscard]] Result<void> upload_at_startup(std::span<const std::byte> block_textures);
    [[nodiscard]] Result<void> add_rendered_semaphores();
    [[nodiscard]] Result<void> recreate_target();
    // Makes the target again where the swapchain is stale or the framebuffer
    // changed size since it was made.
    [[nodiscard]] Result<void> refresh_target();
    [[nodiscard]] Result<void> start_overlay(ImGuiContext* overlay);
    void destroy() noexcept;

    Context context_;                    // declared first, so it is destroyed last
    std::optional<Swapchain> swapchain_; // exactly one of these two
    std::optional<Image> offscreen_;
    Image depth_;           // beside either target, the size of its images
    TextureTable textures_; // RFC 0001's one global descriptor set
    // RFC 0004's block texture array, in the set's binding 2: the layers of
    // RendererDesc::block_textures, or the checker alone without them.
    Image block_textures_;
    Cube cube_;
    ChunkDrawing chunk_drawing_;
    ChunkRenderer chunks_;
    // Where set_chunk's chunk is, and the generation its next mesh takes.
    std::optional<ChunkPos> set_chunk_at_;
    std::uint32_t set_chunk_generation_ = 0;
    BlockFilter block_filter_ = BlockFilter::sharpened;
    float occlusion_strength_ = 1.0F;
    bool wireframe_ = false;
    Extent framebuffer_;
    Extent target_framebuffer_; // the size the target was made for
    PresentMode present_mode_;
    bool swapchain_stale_ = false;

    // RFC 0001: two, since more would add a frame of input latency.
    static constexpr std::size_t frames_in_flight = 2;

    // What one frame in flight owns. Frame N uses slot N % frames_in_flight
    // and reuses it once frame N - frames_in_flight is done.
    struct FrameSlot {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkSemaphore image_acquired = VK_NULL_HANDLE;
    };
    std::array<FrameSlot, frames_in_flight> slots_{};
    // Each slot's cube origins, relative to the camera's block, rewritten
    // every frame: one float4 for each of up to max_cubes instances.
    std::vector<Buffer> origins_;
    // One per swapchain image, not per frame: the presentation engine may hold
    // one after the frame that signaled it is done (RFC 0001).
    std::vector<VkSemaphore> image_rendered_;
    // A timeline semaphore: frame N signals N.
    VkSemaphore frames_done_ = VK_NULL_HANDLE;
    std::uint64_t frame_ = 0;
    // Each frame slot's GPU time, read when the slot comes round again.
    GpuTimer gpu_timer_;
    FrameTimes times_;
    // Tracy's timing of the queue, for RFC 0001's GPU zones: null without
    // VX_PROFILE, or when the queue writes no timestamps.
    tracy::VkCtx* gpu_profiler_ = nullptr;
    // The context ImGui's Vulkan backend was started on: null without one.
    ImGuiContext* overlay_ = nullptr;
};

} // namespace vx::render
