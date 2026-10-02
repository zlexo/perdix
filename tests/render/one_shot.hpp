#pragma once

#include <vx/render/device.hpp>

#include <volk.h>

namespace vx::test {

// One command buffer, recorded, submitted and waited for.
class OneShot {
public:
    explicit OneShot(const render::Device& device) : device_(device) {
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = device.queue_family(),
        };
        vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool_);
        const VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = pool_,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkAllocateCommandBuffers(device.handle(), &allocate, &commands_);
        const VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        vkBeginCommandBuffer(commands_, &begin);
    }
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;
    OneShot(OneShot&&) = delete;
    OneShot& operator=(OneShot&&) = delete;
    ~OneShot() { vkDestroyCommandPool(device_.handle(), pool_, nullptr); }

    [[nodiscard]] VkCommandBuffer commands() const { return commands_; }

    void submit_and_wait() {
        vkEndCommandBuffer(commands_);
        const VkCommandBufferSubmitInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .pNext = nullptr,
            .commandBuffer = commands_,
            .deviceMask = 0,
        };
        const VkSubmitInfo2 submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = 0,
            .pWaitSemaphoreInfos = nullptr,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &buffer_info,
            .signalSemaphoreInfoCount = 0,
            .pSignalSemaphoreInfos = nullptr,
        };
        vkQueueSubmit2(device_.queue(), 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(device_.queue());
    }

private:
    const render::Device& device_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
};

} // namespace vx::test
