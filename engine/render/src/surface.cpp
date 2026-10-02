#include <vx/render/surface.hpp>

#include <utility>

namespace vx::render {

Surface::Surface(Surface&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      surface_(std::exchange(other.surface_, VK_NULL_HANDLE)) {}

Surface& Surface::operator=(Surface&& other) noexcept {
    if (this != &other) {
        destroy();
        instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
        surface_ = std::exchange(other.surface_, VK_NULL_HANDLE);
    }
    return *this;
}

Surface::~Surface() {
    destroy();
}

void Surface::destroy() noexcept {
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
