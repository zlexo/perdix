#pragma once

#include <vx/core/result.hpp>
#include <vx/render/context.hpp>
#include <vx/render/offscreen_image.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx::render {

struct RendererDesc {
    // The window to draw into; empty draws headless, into an offscreen image.
    std::optional<SurfaceSource> surface;
    Extent framebuffer; // the window's size in pixels, or the offscreen image's
    // VK_LAYER_KHRONOS_validation with synchronization validation: debug builds.
    bool validation = false;
    PresentMode present_mode = PresentMode::fifo;
};

// A color in linear RGB, with alpha. The target's sRGB format encodes it.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

enum class Frame : std::uint8_t {
    drawn,
    skipped, // the framebuffer has no area, as when the window is minimized
};

// Draws frames into a window's swapchain, or headless into an offscreen image
// (RFC 0001). For now each frame clears its target to one color, with two
// frames in flight: the CPU records frame N + 1 while the GPU draws frame N.
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

    // Clears the target to `clear`, and presents it when it is a window's.
    [[nodiscard]] Result<Frame> render(Color clear);

    // Headless only: the last frame's pixels, 8-bit RGBA, row after row.
    [[nodiscard]] Result<std::vector<std::uint8_t>> capture();

    [[nodiscard]] const Context& context() const noexcept { return context_; }
    // Null when headless.
    [[nodiscard]] const Swapchain* swapchain() const noexcept {
        return swapchain_.has_value() ? &swapchain_.value() : nullptr;
    }

private:
    Renderer(Context context, std::optional<Swapchain> swapchain,
             std::optional<OffscreenImage> offscreen, Extent framebuffer, PresentMode present_mode);
    [[nodiscard]] Result<void> create_frame_objects();
    [[nodiscard]] Result<void> add_rendered_semaphores();
    [[nodiscard]] Result<void> recreate_target();
    void destroy() noexcept;

    Context context_;                    // declared first, so it is destroyed last
    std::optional<Swapchain> swapchain_; // exactly one of these two
    std::optional<OffscreenImage> offscreen_;
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
    // One per swapchain image, not per frame: the presentation engine may hold
    // one after the frame that signaled it is done (RFC 0001).
    std::vector<VkSemaphore> image_rendered_;
    // A timeline semaphore: frame N signals N.
    VkSemaphore frames_done_ = VK_NULL_HANDLE;
    std::uint64_t frame_ = 0;
};

} // namespace vx::render
