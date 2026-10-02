#include <vx/render/swapchain.hpp>

#include "vk_result.hpp"

#include <tracy/Tracy.hpp>

#include <cstdint>
#include <utility>

namespace vx::render {

SurfaceSupport query_surface_support(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    SurfaceSupport support;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &support.capabilities);

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr);
    support.formats.resize(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, support.formats.data());
    support.formats.resize(count);

    count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr);
    support.present_modes.resize(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count,
                                              support.present_modes.data());
    support.present_modes.resize(count);
    return support;
}

Result<Swapchain> Swapchain::create(const Device& device, VkSurfaceKHR surface, Extent framebuffer,
                                    PresentMode mode, VkSwapchainKHR previous) {
    ZoneScopedN("Create swapchain");
    const Result<SwapchainChoice> chosen = choose_swapchain(
        query_surface_support(device.physical_device(), surface), framebuffer, mode);
    if (!chosen.has_value()) {
        return chosen.error();
    }

    Swapchain swapchain;
    swapchain.device_ = device.handle();
    swapchain.choice_ = chosen.value();
    const SwapchainChoice& choice = swapchain.choice_;
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = surface,
        .minImageCount = choice.image_count,
        .imageFormat = choice.format.format,
        .imageColorSpace = choice.format.colorSpace,
        .imageExtent = choice.extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, // one queue does everything
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = choice.transform,
        .compositeAlpha = choice.composite_alpha,
        .presentMode = choice.present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = previous,
    };
    if (const VkResult result =
            vkCreateSwapchainKHR(swapchain.device_, &info, nullptr, &swapchain.swapchain_);
        result != VK_SUCCESS) {
        return Error{"vkCreateSwapchainKHR failed: " + vk_result_name(result)};
    }

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(swapchain.device_, swapchain.swapchain_, &count, nullptr);
    swapchain.images_.resize(count);
    vkGetSwapchainImagesKHR(swapchain.device_, swapchain.swapchain_, &count,
                            swapchain.images_.data());

    swapchain.views_.reserve(count);
    for (VkImage image : swapchain.images_) {
        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = choice.format.format,
            .components = {},
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult result =
                vkCreateImageView(swapchain.device_, &view_info, nullptr, &view);
            result != VK_SUCCESS) {
            return Error{"vkCreateImageView failed for a swapchain image: " +
                         vk_result_name(result)};
        }
        swapchain.views_.push_back(view);
    }
    return swapchain;
}

Swapchain::Swapchain(Swapchain&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      swapchain_(std::exchange(other.swapchain_, VK_NULL_HANDLE)), choice_(other.choice_),
      images_(std::move(other.images_)), views_(std::move(other.views_)) {}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        swapchain_ = std::exchange(other.swapchain_, VK_NULL_HANDLE);
        choice_ = other.choice_;
        images_ = std::move(other.images_);
        views_ = std::move(other.views_);
    }
    return *this;
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::destroy() noexcept {
    for (VkImageView view : views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    views_.clear();
    images_.clear(); // the swapchain owns its images
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
