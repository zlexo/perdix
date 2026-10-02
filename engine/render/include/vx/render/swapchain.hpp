#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>
#include <vx/render/swapchain_choice.hpp>

#include <volk.h>

#include <span>
#include <vector>

namespace vx::render {

// Reads what a surface supports on a physical device.
[[nodiscard]] SurfaceSupport query_surface_support(VkPhysicalDevice physical_device,
                                                   VkSurfaceKHR surface);

// Owns a VkSwapchainKHR and a view of each of its images, for rendering into.
class Swapchain {
public:
    // Creates the swapchain choose_swapchain picks for the surface. `previous`,
    // when there is one, is the swapchain this one replaces; Vulkan lets it
    // finish presenting, and the caller destroys it afterwards.
    [[nodiscard]] static Result<Swapchain> create(const Device& device, VkSurfaceKHR surface,
                                                  Extent framebuffer, PresentMode mode,
                                                  VkSwapchainKHR previous);

    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    ~Swapchain();

    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return swapchain_; }
    [[nodiscard]] const SwapchainChoice& choice() const noexcept { return choice_; }
    [[nodiscard]] std::span<const VkImage> images() const noexcept { return images_; }
    [[nodiscard]] std::span<const VkImageView> views() const noexcept { return views_; }

private:
    Swapchain() = default;
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    SwapchainChoice choice_;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
};

} // namespace vx::render
