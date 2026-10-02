#include <vx/render/offscreen_image.hpp>

#include "vk_result.hpp"

#include <utility>

namespace vx::render {

Result<OffscreenImage> OffscreenImage::create(const Device& device, VkExtent2D extent,
                                              VkFormat format) {
    OffscreenImage offscreen;
    offscreen.device_ = device.handle();
    offscreen.allocator_ = device.allocator();
    offscreen.extent_ = extent;
    offscreen.format_ = format;

    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (const VkResult result = vmaCreateImage(offscreen.allocator_, &image_info, &allocation_info,
                                               &offscreen.image_, &offscreen.allocation_, nullptr);
        result != VK_SUCCESS) {
        return Error{"vmaCreateImage failed for the offscreen image: " + vk_result_name(result)};
    }

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = offscreen.image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
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
    if (const VkResult result =
            vkCreateImageView(offscreen.device_, &view_info, nullptr, &offscreen.view_);
        result != VK_SUCCESS) {
        return Error{"vkCreateImageView failed for the offscreen image: " + vk_result_name(result)};
    }
    return offscreen;
}

OffscreenImage::OffscreenImage(OffscreenImage&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), extent_(other.extent_),
      format_(other.format_) {}

OffscreenImage& OffscreenImage::operator=(OffscreenImage&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        extent_ = other.extent_;
        format_ = other.format_;
    }
    return *this;
}

OffscreenImage::~OffscreenImage() {
    destroy();
}

void OffscreenImage::destroy() noexcept {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
