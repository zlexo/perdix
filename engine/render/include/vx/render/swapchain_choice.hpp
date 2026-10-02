#pragma once

#include <vx/core/result.hpp>

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vx::render {

// A size in pixels.
struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// FIFO is always available and never tears; immediate, where the surface offers
// it, presents as soon as a frame is done, for benchmarks.
enum class PresentMode : std::uint8_t { fifo, immediate };

// What a surface supports on a device, as Vulkan reports it. A plain struct, so
// the choice below is tested without a GPU.
struct SurfaceSupport {
    VkSurfaceCapabilitiesKHR capabilities = {
        .minImageCount = 0,
        .maxImageCount = 0,
        .currentExtent = {},
        .minImageExtent = {},
        .maxImageExtent = {},
        .maxImageArrayLayers = 0,
        .supportedTransforms = 0,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, // no zero value to default to
        .supportedCompositeAlpha = 0,
        .supportedUsageFlags = 0,
    };
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

struct SwapchainChoice {
    VkSurfaceFormatKHR format{};
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    std::uint32_t image_count = 0;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
};

// RFC 0001's swapchain on a surface: the first sRGB format offered, preferring
// B8G8R8A8_SRGB; the wanted present mode where offered, else FIFO; one image
// more than the minimum, within the maximum; and the framebuffer's size in
// pixels, unless the surface fixes its own. The image keeps the surface's
// current transform and is opaque where the surface allows it. Fails without
// an sRGB format.
[[nodiscard]] Result<SwapchainChoice> choose_swapchain(const SurfaceSupport& support,
                                                       Extent framebuffer, PresentMode wanted);

// The startup report's line about the swapchain: its images, size, format and
// present mode, ending in a newline.
[[nodiscard]] std::string format_swapchain_report(const SwapchainChoice& choice);

} // namespace vx::render
