#pragma once

#include "one_shot.hpp"

#include <vx/core/result.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/device.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/shaders/block_texture_probe.hpp>

#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace vx::test {

// Where the probe samples the block texture array: the push constant of
// block_texture_probe.slang.
struct ProbeSample {
    float u = 0.0F;
    float v = 0.0F;
    float layer = 0.0F;
    float level = 0.0F;
};

// Draws block_texture_probe.slang into a one-pixel sRGB target and reads the
// pixel back: what a shader sampling set 0's block texture array, through
// binding 3's sampler, sees at one point, as the bytes an sRGB target stores.
class BlockTextureProbe {
public:
    [[nodiscard]] static Result<BlockTextureProbe> create(const render::Device& device,
                                                          VkDescriptorSetLayout set_layout) {
        Result<render::GraphicsPipeline> pipeline =
            render::GraphicsPipeline::create(device,
                                             {.spirv = shaders::block_texture_probe,
                                              .color_format = VK_FORMAT_R8G8B8A8_SRGB,
                                              .set_layout = set_layout,
                                              .push_constant_size = sizeof(ProbeSample),
                                              .push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT},
                                             "block texture probe");
        if (!pipeline.has_value()) {
            return pipeline.error();
        }
        Result<render::Image> target = render::Image::create(
            device, {.width = 1, .height = 1}, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, "probe target");
        if (!target.has_value()) {
            return target.error();
        }
        Result<render::Buffer> readback =
            render::Buffer::create(device, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   render::BufferMemory::readback, "probe readback");
        if (!readback.has_value()) {
            return readback.error();
        }
        return BlockTextureProbe(device, std::move(pipeline).value(), std::move(target).value(),
                                 std::move(readback).value());
    }

    // The pixel the probe draws at `sample`, with `set` bound as set 0.
    [[nodiscard]] std::array<std::uint8_t, 4> sample(VkDescriptorSet set, ProbeSample sample) {
        OneShot shot(*device_);
        VkCommandBuffer commands = shot.commands();
        barrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const VkRenderingAttachmentInfo color = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = target_.view(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}}},
        };
        const VkRenderingInfo rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = 1, .height = 1}},
            .layerCount = 1,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
            .pDepthAttachment = nullptr,
            .pStencilAttachment = nullptr,
        };
        vkCmdBeginRendering(commands, &rendering);
        const VkViewport viewport = {.x = 0.0F,
                                     .y = 0.0F,
                                     .width = 1.0F,
                                     .height = 1.0F,
                                     .minDepth = 0.0F,
                                     .maxDepth = 1.0F};
        const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = {.width = 1, .height = 1}};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(commands, pipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(sample), &sample);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
        barrier(commands, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy2 region = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext = nullptr,
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = 1, .height = 1, .depth = 1},
        };
        const VkCopyImageToBufferInfo2 copy = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
            .pNext = nullptr,
            .srcImage = target_.image(),
            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstBuffer = readback_.handle(),
            .regionCount = 1,
            .pRegions = &region,
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
        const VkDependencyInfo host = {
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
        vkCmdPipelineBarrier2(commands, &host);
        shot.submit_and_wait();
        vmaInvalidateAllocation(device_->allocator(), readback_.allocation(), 0, VK_WHOLE_SIZE);
        const auto bytes = readback_.mapping();
        return {std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
                std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
    }

private:
    BlockTextureProbe(const render::Device& device, render::GraphicsPipeline pipeline,
                      render::Image target, render::Buffer readback)
        : device_(&device), pipeline_(std::move(pipeline)), target_(std::move(target)),
          readback_(std::move(readback)) {}

    void barrier(VkCommandBuffer commands, VkPipelineStageFlags2 src_stage,
                 VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                 VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) const {
        const VkImageMemoryBarrier2 image = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target_.image(),
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        };
        const VkDependencyInfo dependency = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &image,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    const render::Device* device_;
    render::GraphicsPipeline pipeline_;
    render::Image target_;
    render::Buffer readback_;
};

} // namespace vx::test
