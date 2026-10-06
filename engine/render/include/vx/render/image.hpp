#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

#include <cstdint>
#include <string_view>

namespace vx::render {

// An image with its memory and a view: one the renderer draws into, such as
// headless rendering's color target (RFC 0001) or a depth attachment, or one
// it samples, a texture. `usage` says how it is used, such as a copy source
// for reading pixels back; `aspect` is the part the view sees, color or depth.
// `name` is the debug name of the image and its view, which validation
// messages and GPU captures show (RFC 0001), and errors too.
class Image {
public:
    // One mip level and one layer.
    [[nodiscard]] static Result<Image> create(const Device& device, VkExtent2D extent,
                                              VkFormat format, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect, std::string_view name);
    // A color image of `levels` mip levels, each half the one before, and
    // `layers` layers, whose view shaders sample as an array of 2D images,
    // even with one layer: RFC 0004's block texture array. Levels run from 1
    // to the one of 1 by 1, layers from 1 to the device's maxImageArrayLayers;
    // anything else is an error, before Vulkan sees it.
    [[nodiscard]] static Result<Image> create_array(const Device& device, VkExtent2D extent,
                                                    VkFormat format, std::uint32_t levels,
                                                    std::uint32_t layers, VkImageUsageFlags usage,
                                                    std::string_view name);

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t levels() const noexcept { return levels_; }
    [[nodiscard]] std::uint32_t layers() const noexcept { return layers_; }
    // 2D, or 2D_ARRAY for create_array's images, even of one layer.
    [[nodiscard]] VkImageViewType view_type() const noexcept { return view_type_; }

private:
    Image() = default;
    [[nodiscard]] static Result<Image> make(const Device& device, VkExtent2D extent,
                                            VkFormat format, VkImageUsageFlags usage,
                                            VkImageAspectFlags aspect, std::string_view name,
                                            std::uint32_t levels, std::uint32_t layers,
                                            VkImageViewType view_type);
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t levels_ = 1;
    std::uint32_t layers_ = 1;
    VkImageViewType view_type_ = VK_IMAGE_VIEW_TYPE_2D;
};

} // namespace vx::render
