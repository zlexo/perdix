#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <vk_mem_alloc.h>
#include <volk.h>

namespace vx::render {

// An image the renderer draws into without a window (RFC 0001's headless
// rendering), with its memory and a view. It is a color attachment, and a
// copy source so its pixels can be read back.
class OffscreenImage {
public:
    [[nodiscard]] static Result<OffscreenImage> create(const Device& device, VkExtent2D extent,
                                                       VkFormat format);

    OffscreenImage(OffscreenImage&& other) noexcept;
    OffscreenImage& operator=(OffscreenImage&& other) noexcept;
    OffscreenImage(const OffscreenImage&) = delete;
    OffscreenImage& operator=(const OffscreenImage&) = delete;
    ~OffscreenImage();

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }

private:
    OffscreenImage() = default;
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
