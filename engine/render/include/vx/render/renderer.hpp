#pragma once

#include <vx/core/result.hpp>
#include <vx/render/context.hpp>
#include <vx/render/swapchain.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <volk.h>

#include <cstdint>
#include <vector>

namespace vx::render {

struct RendererDesc {
    SurfaceSource surface; // the window to draw into
    Extent framebuffer;    // its size in pixels
    // VK_LAYER_KHRONOS_validation with synchronization validation: debug builds.
    bool validation = false;
    PresentMode present_mode = PresentMode::fifo;
};

// A color in linear RGB, with alpha. The swapchain encodes it to sRGB.
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

// Draws frames into the window. For now each frame clears it to one color
// (RFC 0001 plan item 4), with one frame in flight; item 5 overlaps two.
class Renderer {
public:
    [[nodiscard]] static Result<Renderer> create(const RendererDesc& desc);

    Renderer(Renderer&& other) noexcept;
    // Like the context it owns, it has no move assignment.
    Renderer& operator=(Renderer&& other) = delete;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    // The window's framebuffer has this size now; the swapchain follows on the
    // next frame.
    void resize(Extent framebuffer) noexcept { framebuffer_ = framebuffer; }

    // Clears the window to `clear` and presents it.
    [[nodiscard]] Result<Frame> render(Color clear);

    [[nodiscard]] const Context& context() const noexcept { return context_; }
    [[nodiscard]] const Swapchain& swapchain() const noexcept { return swapchain_; }

private:
    Renderer(Context context, Swapchain swapchain, Extent framebuffer, PresentMode present_mode);
    [[nodiscard]] Result<void> create_frame_objects();
    [[nodiscard]] Result<void> add_rendered_semaphores();
    [[nodiscard]] Result<void> recreate_swapchain();
    void destroy() noexcept;

    Context context_; // declared first, so it is destroyed last
    Swapchain swapchain_;
    Extent framebuffer_;
    Extent swapchain_framebuffer_; // the size the swapchain was made for
    PresentMode present_mode_;
    bool swapchain_stale_ = false;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkSemaphore image_acquired_ = VK_NULL_HANDLE;
    // One per swapchain image, not per frame: the presentation engine may hold
    // one after the frame that signaled it is done (RFC 0001).
    std::vector<VkSemaphore> image_rendered_;
    // A timeline semaphore: frame N signals N, and frame N + 1 waits for it.
    VkSemaphore frames_done_ = VK_NULL_HANDLE;
    std::uint64_t frame_ = 0;
};

} // namespace vx::render
