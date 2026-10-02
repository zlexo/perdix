#include <vx/render/swapchain_choice.hpp>

#include <algorithm>

namespace vx::render {
namespace {

bool is_srgb(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

const char* format_name(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8_SRGB:
        return "R8G8B8_SRGB";
    case VK_FORMAT_B8G8R8_SRGB:
        return "B8G8R8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB:
        return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "B8G8R8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return "A8B8G8R8_SRGB_PACK32";
    default:
        return "another format";
    }
}

const char* present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "immediate";
    default:
        return "another present mode";
    }
}

// A currentExtent of this width means the swapchain's size decides the
// surface's, rather than the other way round.
constexpr std::uint32_t swapchain_decides = 0xFFFFFFFF;

} // namespace

Result<SwapchainChoice> choose_swapchain(const SurfaceSupport& support, Extent framebuffer,
                                         PresentMode wanted) {
    const VkSurfaceFormatKHR* format = nullptr;
    for (const VkSurfaceFormatKHR& offered : support.formats) {
        if (offered.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || !is_srgb(offered.format)) {
            continue;
        }
        if (offered.format == VK_FORMAT_B8G8R8A8_SRGB) {
            format = &offered;
            break;
        }
        if (format == nullptr) {
            format = &offered;
        }
    }
    if (format == nullptr) {
        return Error{"the window's surface offers no sRGB format"};
    }

    SwapchainChoice choice;
    choice.format = *format;

    if (wanted == PresentMode::immediate &&
        std::ranges::find(support.present_modes, VK_PRESENT_MODE_IMMEDIATE_KHR) !=
            support.present_modes.end()) {
        choice.present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    const VkSurfaceCapabilitiesKHR& capabilities = support.capabilities;
    choice.image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        choice.image_count = std::min(choice.image_count, capabilities.maxImageCount);
    }

    if (capabilities.currentExtent.width != swapchain_decides) {
        choice.extent = capabilities.currentExtent;
    } else {
        choice.extent = {
            .width = std::clamp(framebuffer.width, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width),
            .height = std::clamp(framebuffer.height, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height),
        };
    }
    choice.transform = capabilities.currentTransform;
    // Opaque where allowed; otherwise the lowest bit the surface supports.
    choice.composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        for (VkFlags bit = 1; bit != 0 && bit <= capabilities.supportedCompositeAlpha; bit <<= 1U) {
            if ((capabilities.supportedCompositeAlpha & bit) != 0) {
                choice.composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                break;
            }
        }
    }
    return choice;
}

std::string format_swapchain_report(const SwapchainChoice& choice) {
    return "Swapchain: " + std::to_string(choice.image_count) + " images of " +
           std::to_string(choice.extent.width) + "x" + std::to_string(choice.extent.height) + ", " +
           format_name(choice.format.format) + ", " + present_mode_name(choice.present_mode) + "\n";
}

} // namespace vx::render
