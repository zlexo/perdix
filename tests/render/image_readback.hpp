#pragma once

#include <vx/render/buffer.hpp>
#include <vx/render/image.hpp>

#include <volk.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vx::test {

// Records a copy of all of `image`, which shaders could read, into
// `readback`, laid out as upload_image takes it (level by level, every layer
// of a level before the next), and the barriers around it, so the host can
// read the copy once the queue is idle.
inline void record_image_readback(VkCommandBuffer commands, const render::Image& image,
                                  const render::Buffer& readback) {
    const VkImageMemoryBarrier2 to_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image(),
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = image.levels(),
                             .baseArrayLayer = 0,
                             .layerCount = image.layers()},
    };
    const VkDependencyInfo before = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_copy,
    };
    vkCmdPipelineBarrier2(commands, &before);
    std::vector<VkBufferImageCopy2> regions;
    VkDeviceSize offset = 0;
    for (std::uint32_t level = 0; level < image.levels(); ++level) {
        const std::uint32_t width = std::max(image.extent().width >> level, 1U);
        const std::uint32_t height = std::max(image.extent().height >> level, 1U);
        regions.push_back({
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext = nullptr,
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = level,
                                 .baseArrayLayer = 0,
                                 .layerCount = image.layers()},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = width, .height = height, .depth = 1},
        });
        offset += VkDeviceSize{width} * height * 4 * image.layers();
    }
    const VkCopyImageToBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcImage = image.image(),
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = readback.handle(),
        .regionCount = static_cast<std::uint32_t>(regions.size()),
        .pRegions = regions.data(),
    };
    vkCmdCopyImageToBuffer2(commands, &copy);
    const VkMemoryBarrier2 to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo after = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &after);
}

} // namespace vx::test
