#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <volk.h>

#include <cstdint>

namespace vx::render {

// RFC 0001's bindless textures: the one descriptor set every pipeline binds
// as set 0. Binding 0 is an array of sampled images, partially bound, that
// shaders index by the push constant's `texture`; binding 1 is a sampler with
// nearest filtering, fixed in the layout. Textures are added before any frame
// that uses the set is in flight: changing a set in use needs update-after-
// bind, which the feature floor does not include.
class TextureTable {
public:
    [[nodiscard]] static Result<TextureTable> create(const Device& device);

    TextureTable(TextureTable&& other) noexcept;
    TextureTable& operator=(TextureTable&& other) noexcept;
    TextureTable(const TextureTable&) = delete;
    TextureTable& operator=(const TextureTable&) = delete;
    ~TextureTable();

    // Puts `view`, of an image in the layout shaders read, into the next
    // slot; the slot's index, for the push constant.
    [[nodiscard]] Result<std::uint32_t> add(VkImageView view);

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return layout_; }
    [[nodiscard]] VkDescriptorSet set() const noexcept { return set_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    // 1024 slots, or fewer where the device's limits say so: MoltenVK allows
    // 256 sampled images a shader stage.
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
    TextureTable() = default;
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE; // freed with the pool
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;
};

} // namespace vx::render
