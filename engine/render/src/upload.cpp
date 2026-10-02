#include <vx/render/upload.hpp>

#include "vk_result.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace vx::render {

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

    Result<Buffer> made = Buffer::create(*context.device_, bytes.size(),
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::staging);
    if (!made.has_value()) {
        return made.error();
    }
    Buffer staging = std::move(made).value();
    std::ranges::copy(bytes, staging.mapping().begin());
    if (const VkResult result =
            vmaFlushAllocation(allocator, staging.allocation(), 0, bytes.size());
        result != VK_SUCCESS) {
        return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
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
        .srcBuffer = staging.handle(),
        .dstBuffer = dst.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(commands, &copy);
    context.staging_.push_back(std::move(staging));
    context.staged_bytes_ += bytes.size();
    context.unfinished_ = true;
    return {};
}

} // namespace vx::render
