#include <vx/render/upload.hpp>

#include "transition.hpp"
#include "vk_result.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace vx::render {
namespace {

// What upload_image takes: the color formats of four bytes a texel.
bool four_bytes_a_texel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

} // namespace

void UploadContext::finish(VkCommandBuffer commands) {
    if (!unfinished_) {
        return;
    }
    // Whatever touches the buffers next, vertex pulling or another copy, sees
    // the copies' writes.
    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
    unfinished_ = false;
}

Result<VkBuffer> UploadContext::stage(std::span<const std::byte> bytes) {
    Result<Buffer> made = Buffer::create(*device_, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         BufferMemory::staging);
    if (!made.has_value()) {
        return made.error();
    }
    Buffer staging = std::move(made).value();
    std::ranges::copy(bytes, staging.mapping().begin());
    if (const VkResult result =
            vmaFlushAllocation(device_->allocator(), staging.allocation(), 0, bytes.size());
        result != VK_SUCCESS) {
        return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
    }
    VkBuffer handle = staging.handle();
    staging_.push_back(std::move(staging));
    staged_bytes_ += bytes.size();
    return handle;
}

void UploadContext::reset() noexcept {
    staging_.clear();
    staged_bytes_ = 0;
    unfinished_ = false;
}

Result<void> upload(UploadContext& context, VkCommandBuffer commands, const Buffer& dst,
                    VkDeviceSize dst_offset, std::span<const std::byte> bytes) {
    if (dst_offset > dst.size() || bytes.size() > dst.size() - dst_offset) {
        return Error{"an upload of " + std::to_string(bytes.size()) + " bytes at offset " +
                     std::to_string(dst_offset) + " runs past the end of a buffer of " +
                     std::to_string(dst.size())};
    }
    if (bytes.empty()) {
        return {};
    }
    VmaAllocator allocator = context.device_->allocator();

    if (const std::span<std::byte> mapped = dst.mapping(); !mapped.empty()) {
        std::ranges::copy(bytes, mapped.subspan(static_cast<std::size_t>(dst_offset)).begin());
        // Does nothing where the memory is host-coherent, as unified memory is.
        if (const VkResult result =
                vmaFlushAllocation(allocator, dst.allocation(), dst_offset, bytes.size());
            result != VK_SUCCESS) {
            return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
        }
        return {};
    }

    const Result<VkBuffer> staging = context.stage(bytes);
    if (!staging.has_value()) {
        return staging.error();
    }
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .pNext = nullptr,
        .srcOffset = 0,
        .dstOffset = dst_offset,
        .size = bytes.size(),
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcBuffer = staging.value(),
        .dstBuffer = dst.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(commands, &copy);
    context.unfinished_ = true;
    return {};
}

Result<void> upload_image(UploadContext& context, VkCommandBuffer commands, const Image& image,
                          std::span<const std::byte> pixels) {
    if (!four_bytes_a_texel(image.format())) {
        return Error{"upload_image takes images of four bytes a texel"};
    }
    const VkExtent2D extent = image.extent();
    const std::size_t expected = std::size_t{extent.width} * extent.height * 4;
    if (pixels.size() != expected) {
        return Error{"an image of " + std::to_string(extent.width) + " by " +
                     std::to_string(extent.height) + " takes " + std::to_string(expected) +
                     " bytes of pixels, not " + std::to_string(pixels.size())};
    }
    const Result<VkBuffer> staging = context.stage(pixels);
    if (!staging.has_value()) {
        return staging.error();
    }

    // Every earlier command, reads of the image included, finishes before the
    // move, which discards what the image held.
    transition(commands, image.image(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .pNext = nullptr,
        .bufferOffset = 0,
        .bufferRowLength = 0, // rows with nothing between them
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
    const VkCopyBufferToImageInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .pNext = nullptr,
        .srcBuffer = staging.value(),
        .dstImage = image.image(),
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBufferToImage2(commands, &copy);
    // Shaders in any later command may sample it.
    transition(commands, image.image(), VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return {};
}

} // namespace vx::render
