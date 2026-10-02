#pragma once

#include <volk.h>

#include <cstdint>
#include <string_view>

namespace vx::render {

// The object type of each kind of handle the engine names.
template <typename Handle> struct ObjectType;

template <> struct ObjectType<VkDevice> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_DEVICE;
};
template <> struct ObjectType<VkQueue> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_QUEUE;
};
template <> struct ObjectType<VkBuffer> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_BUFFER;
};
template <> struct ObjectType<VkImage> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE;
};
template <> struct ObjectType<VkImageView> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE_VIEW;
};
template <> struct ObjectType<VkSampler> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_SAMPLER;
};
template <> struct ObjectType<VkDescriptorSetLayout> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
};
template <> struct ObjectType<VkDescriptorPool> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
};
template <> struct ObjectType<VkDescriptorSet> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_SET;
};
template <> struct ObjectType<VkShaderModule> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_SHADER_MODULE;
};
template <> struct ObjectType<VkPipelineLayout> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
};
template <> struct ObjectType<VkPipeline> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE;
};
template <> struct ObjectType<VkSemaphore> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_SEMAPHORE;
};
template <> struct ObjectType<VkCommandPool> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_COMMAND_POOL;
};
template <> struct ObjectType<VkCommandBuffer> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_COMMAND_BUFFER;
};
template <> struct ObjectType<VkQueryPool> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_QUERY_POOL;
};
template <> struct ObjectType<VkSwapchainKHR> {
    static constexpr VkObjectType value = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
};

// Names a Vulkan object of `device`, so validation messages and GPU captures
// show "cube vertices" where they would show a handle (RFC 0001). Does
// nothing when the instance has no VK_EXT_debug_utils.
void set_debug_name(VkDevice device, VkObjectType type, std::uint64_t handle,
                    std::string_view name);

template <typename Handle>
void set_debug_name(VkDevice device, Handle handle, std::string_view name) {
    // Every handle is a pointer on the 64-bit platforms the engine targets.
    set_debug_name(device, ObjectType<Handle>::value, reinterpret_cast<std::uint64_t>(handle),
                   name);
}

} // namespace vx::render
