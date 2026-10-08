#pragma once

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/context.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>
#include <vx/render/texture_table.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <volk.h>

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
// (RFC 0001). For now each frame clears its target to the sky and draws cubes
// camera-relative, with reversed-Z depth and two frames in flight: the CPU
// records frame N + 1 while the GPU draws frame N.
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

    // Clears the target to the sky, draws the cubes as the camera sees them,
    // and presents the target when it is a window's.
    [[nodiscard]] Result<Frame> render(const FrameInputs& inputs);

    // Headless only: the last frame's pixels, 8-bit RGBA, row after row.
    [[nodiscard]] Result<std::vector<std::uint8_t>> capture();

    // The last frame drawn's times.
    [[nodiscard]] const FrameTimes& frame_times() const noexcept { return times_; }

    [[nodiscard]] const Context& context() const noexcept { return context_; }
    // Set 0, for whatever else draws with it: tools and tests now, and the
    // chunk renderer (RFC 0007).
    [[nodiscard]] const TextureTable& textures() const noexcept { return textures_; }
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

    Renderer(Context context, std::optional<Swapchain> swapchain, std::optional<Image> offscreen,
             Image depth, TextureTable textures, Image block_textures, Cube cube,
             Extent framebuffer, PresentMode present_mode);
    [[nodiscard]] Result<void> create_frame_objects();
    [[nodiscard]] Result<void> upload_at_startup();
    [[nodiscard]] Result<void> add_rendered_semaphores();
    [[nodiscard]] Result<void> recreate_target();
    [[nodiscard]] Result<void> start_overlay(ImGuiContext* overlay);
    void destroy() noexcept;

    Context context_;                    // declared first, so it is destroyed last
    std::optional<Swapchain> swapchain_; // exactly one of these two
    std::optional<Image> offscreen_;
    Image depth_;           // beside either target, the size of its images
    TextureTable textures_; // RFC 0001's one global descriptor set
    // RFC 0004's block texture array, in the set's binding 2: the checker
    // alone until the core pack brings block textures.
    Image block_textures_;
    Cube cube_;
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
