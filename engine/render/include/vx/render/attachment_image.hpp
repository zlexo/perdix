#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

namespace vx::render {

// An image the renderer draws into, with its memory and a view: the color
// target of headless rendering (RFC 0001), or a depth attachment. `usage`
// says how else it is used, such as a copy source for reading pixels back;
// `aspect` is the part the view sees, color or depth.
class AttachmentImage {
public:
    [[nodiscard]] static Result<AttachmentImage> create(const Device& device, VkExtent2D extent,
                                                        VkFormat format, VkImageUsageFlags usage,
                                                        VkImageAspectFlags aspect);

    AttachmentImage(AttachmentImage&& other) noexcept;
    AttachmentImage& operator=(AttachmentImage&& other) noexcept;
    AttachmentImage(const AttachmentImage&) = delete;
    AttachmentImage& operator=(const AttachmentImage&) = delete;
    ~AttachmentImage();

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }

private:
    AttachmentImage() = default;
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
