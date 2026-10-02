#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

namespace vx::render {

// An image with its memory and a view: one the renderer draws into, such as
// headless rendering's color target (RFC 0001) or a depth attachment, or one
// it samples, a texture. `usage` says how it is used, such as a copy source
// for reading pixels back; `aspect` is the part the view sees, color or depth.
class Image {
public:
    [[nodiscard]] static Result<Image> create(const Device& device, VkExtent2D extent,
                                              VkFormat format, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect);

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }

private:
    Image() = default;
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
};

} // namespace vx::render
