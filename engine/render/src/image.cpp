#include <vx/render/image.hpp>

#include "debug_name.hpp"
#include "vk_result.hpp"

#include <string>
#include <utility>

namespace vx::render {

Result<Image> Image::create(const Device& device, VkExtent2D extent, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect,
                            std::string_view name) {
    return make(device, extent, format, usage, aspect, name, 1, 1, VK_IMAGE_VIEW_TYPE_2D);
}

Result<Image> Image::create_array(const Device& device, VkExtent2D extent, VkFormat format,
                                  std::uint32_t levels, std::uint32_t layers,
                                  VkImageUsageFlags usage, std::string_view name) {
    return make(device, extent, format, usage, VK_IMAGE_ASPECT_COLOR_BIT, name, levels, layers,
                VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

Result<Image> Image::make(const Device& device, VkExtent2D extent, VkFormat format,
                          VkImageUsageFlags usage, VkImageAspectFlags aspect, std::string_view name,
                          std::uint32_t levels, std::uint32_t layers, VkImageViewType view_type) {
    Image image;
    image.device_ = device.handle();
    image.allocator_ = device.allocator();
    image.extent_ = extent;
    image.format_ = format;
    image.levels_ = levels;
    image.layers_ = layers;

    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = levels,
        .arrayLayers = layers,
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
        return Error{"vmaCreateImage failed for the image \"" + std::string(name) +
                     "\": " + vk_result_name(result)};
    }
    set_debug_name(image.device_, image.image_, name);

    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image.image_,
        .viewType = view_type,
        .format = format,
        .components = {},
        .subresourceRange =
            {
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = levels,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
    };
    if (const VkResult result = vkCreateImageView(image.device_, &view_info, nullptr, &image.view_);
        result != VK_SUCCESS) {
        return Error{"vkCreateImageView failed for the image \"" + std::string(name) +
                     "\": " + vk_result_name(result)};
    }
    set_debug_name(image.device_, image.view_, name);
    return image;
}

Image::Image(Image&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)), extent_(other.extent_),
      format_(other.format_), levels_(other.levels_), layers_(other.layers_) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        extent_ = other.extent_;
        format_ = other.format_;
        levels_ = other.levels_;
        layers_ = other.layers_;
    }
    return *this;
}

Image::~Image() {
    destroy();
}

void Image::destroy() noexcept {
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
