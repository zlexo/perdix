#include <vx/render/image.hpp>

#include "debug_name.hpp"
#include "vk_result.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>

namespace vx::render {

Result<Image> Image::create(const Device& device, VkExtent2D extent, VkFormat format,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect,
                            std::string_view name) {
    return make(device, extent, format, usage, aspect, name, 1, 1, VK_IMAGE_VIEW_TYPE_2D);
}

Result<Image> Image::create_attachment(const Device& device, VkExtent2D extent, VkFormat format,
                                       VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                       VkSampleCountFlagBits samples, std::string_view name) {
    return make(device, extent, format, usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, aspect,
                name, 1, 1, VK_IMAGE_VIEW_TYPE_2D, samples, true);
}

Result<Image> Image::create_array(const Device& device, VkExtent2D extent, VkFormat format,
                                  std::uint32_t levels, std::uint32_t layers,
                                  VkImageUsageFlags usage, std::string_view name) {
    // Vulkan allows levels down to the one of 1 by 1, and as many layers as
    // the device holds in one image. Past either, the request is invalid:
    // KosmicKrisp aborts on too many layers, and a level past the last would
    // have upload_image shift a 32-bit size by 32 or more.
    const std::string image = "the image \"" + std::string(name) + "\"";
    if (extent.width == 0 || extent.height == 0) {
        return Error{image + " has no texels: " + std::to_string(extent.width) + " by " +
                     std::to_string(extent.height)};
    }
    const auto most_levels =
        static_cast<std::uint32_t>(std::bit_width(std::max(extent.width, extent.height)));
    if (levels == 0 || levels > most_levels) {
        return Error{image + " of " + std::to_string(extent.width) + " by " +
                     std::to_string(extent.height) + " has 1 to " + std::to_string(most_levels) +
                     " mip levels, not " + std::to_string(levels)};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    if (const std::uint32_t most = properties.limits.maxImageArrayLayers;
        layers == 0 || layers > most) {
        return Error{image + " has 1 to " + std::to_string(most) + " layers on this device, not " +
                     std::to_string(layers)};
    }
    return make(device, extent, format, usage, VK_IMAGE_ASPECT_COLOR_BIT, name, levels, layers,
                VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

Result<Image> Image::make(const Device& device, VkExtent2D extent, VkFormat format,
                          VkImageUsageFlags usage, VkImageAspectFlags aspect, std::string_view name,
                          std::uint32_t levels, std::uint32_t layers, VkImageViewType view_type,
                          VkSampleCountFlagBits samples, bool lazily) {
    Image image;
    image.device_ = device.handle();
    image.allocator_ = device.allocator();
    image.extent_ = extent;
    image.format_ = format;
    image.levels_ = levels;
    image.layers_ = layers;
    image.view_type_ = view_type;

    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    // A transient attachment in lazily allocated memory where the device
    // has it, which tile-based GPUs need never back; elsewhere as any image.
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = lazily ? VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED : VMA_MEMORY_USAGE_AUTO;
    VkResult created = vmaCreateImage(image.allocator_, &image_info, &allocation_info,
                                      &image.image_, &image.allocation_, nullptr);
    if (created == VK_ERROR_FEATURE_NOT_PRESENT && lazily) {
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        created = vmaCreateImage(image.allocator_, &image_info, &allocation_info, &image.image_,
                                 &image.allocation_, nullptr);
    }
    if (const VkResult result = created; result != VK_SUCCESS) {
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
      format_(other.format_), levels_(other.levels_), layers_(other.layers_),
      view_type_(other.view_type_) {}

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
        view_type_ = other.view_type_;
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
