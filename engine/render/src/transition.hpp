#pragma once

#include <volk.h>

namespace vx::render {

// Records one image barrier over the image's first mip level and layer: a
// layout transition, and the execution and memory dependency around it.
void transition(VkCommandBuffer commands, VkImage image, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to,
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

} // namespace vx::render
