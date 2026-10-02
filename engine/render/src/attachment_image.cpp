#include <vx/render/attachment_image.hpp>

#include "vk_result.hpp"

#include <utility>

namespace vx::render {

Result<AttachmentImage> AttachmentImage::create(const Device& device, VkExtent2D extent,
                                                VkFormat format, VkImageUsageFlags usage,
                                                VkImageAspectFlags aspect) {
    AttachmentImage image;
    image.device_ = device.handle();
    image.allocator_ = device.allocator();
    image.extent_ = extent;
    image.format_ = format;

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
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (const VkResult result = vmaCreateImage(image.allocator_, &image_info, &allocation_info,
                                               &image.image_, &image.allocation_, nullptr);
        result != VK_SUCCESS) {
        return Error{"vmaCreateImage failed for an attachment image: " + vk_result_name(result)};
    }

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image.image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {},
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    if (const VkResult result = vkCreateImageView(image.device_, &view_info, nullptr, &image.view_);
        result != VK_SUCCESS) {
        return Error{"vkCreateImageView failed for an attachment image: " + vk_result_name(result)};
    }
    return image;
}

AttachmentImage::AttachmentImage(AttachmentImage&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), extent_(other.extent_),
      format_(other.format_) {}

AttachmentImage& AttachmentImage::operator=(AttachmentImage&& other) noexcept {
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

AttachmentImage::~AttachmentImage() {
    destroy();
}

void AttachmentImage::destroy() noexcept {
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
