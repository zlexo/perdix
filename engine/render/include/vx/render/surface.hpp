#pragma once

#include <volk.h>

namespace vx::render {

// Owns a VkSurfaceKHR. The instance it was made on must outlive it. Empty when
// running headless.
class Surface {
public:
    Surface() = default;
    Surface(VkInstance instance, VkSurfaceKHR surface) noexcept
        : instance_(instance), surface_(surface) {}

    Surface(Surface&& other) noexcept;
    Surface& operator=(Surface&& other) noexcept;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface();

    [[nodiscard]] VkSurfaceKHR handle() const noexcept { return surface_; }

private:
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
};

} // namespace vx::render
