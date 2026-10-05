#include <vx/render/texture_table.hpp>

#include "debug_name.hpp"
#include "vk_result.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace vx::render {
namespace {

constexpr std::uint32_t wanted_capacity = 1024;
// The set, its layout and pool, and the sampler in it, as validation
// messages and GPU captures name them.
constexpr std::string_view debug_name = "texture table";
constexpr std::string_view block_sampler_name = "block texture sampler";

} // namespace

Result<TextureTable> TextureTable::create(const Device& device) {
    TextureTable table;
    table.device_ = device.handle();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    // Each texture counts against the fragment stage's resources, and so do
    // the block texture array and each color attachment the stage draws to:
    // one, in every pipeline so far. Samplers do not count.
    table.capacity_ =
        std::min({wanted_capacity, properties.limits.maxPerStageDescriptorSampledImages - 1,
                  properties.limits.maxDescriptorSetSampledImages - 1,
                  properties.limits.maxPerStageResources - 2});

    // Nearest filtering and repeating coordinates, for pixel art; how block
    // textures filter and mip is RFC 0004's.
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.0F,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0F,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0F,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (const VkResult result =
            vkCreateSampler(table.device_, &sampler_info, nullptr, &table.sampler_);
        result != VK_SUCCESS) {
        return Error{"vkCreateSampler failed: " + vk_result_name(result)};
    }
    set_debug_name(table.device_, table.sampler_, debug_name);

    // RFC 0004's block textures: linear filtering between texels and between
    // mip levels, repeating, as merged quads repeat a texture. Anisotropic
    // filtering and the sharpened coordinate come with item 8.
    VkSamplerCreateInfo block_sampler_info = sampler_info;
    block_sampler_info.magFilter = VK_FILTER_LINEAR;
    block_sampler_info.minFilter = VK_FILTER_LINEAR;
    block_sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (const VkResult result =
            vkCreateSampler(table.device_, &block_sampler_info, nullptr, &table.block_sampler_);
        result != VK_SUCCESS) {
        return Error{"vkCreateSampler failed: " + vk_result_name(result)};
    }
    set_debug_name(table.device_, table.block_sampler_, block_sampler_name);

    const std::array bindings = {
        VkDescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = table.capacity_,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = &table.sampler_,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        },
        VkDescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = &table.block_sampler_,
        },
    };
    // Slots no shader reads need not hold an image.
    const std::array<VkDescriptorBindingFlags, 4> binding_flags = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = nullptr,
        .bindingCount = static_cast<std::uint32_t>(binding_flags.size()),
        .pBindingFlags = binding_flags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = 0,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    if (const VkResult result =
            vkCreateDescriptorSetLayout(table.device_, &layout_info, nullptr, &table.layout_);
        result != VK_SUCCESS) {
        return Error{"vkCreateDescriptorSetLayout failed: " + vk_result_name(result)};
    }
    set_debug_name(table.device_, table.layout_, debug_name);

    const std::array pool_sizes = {
        VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             .descriptorCount = table.capacity_ + 1},
        VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 2},
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1,
        .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    if (const VkResult result =
            vkCreateDescriptorPool(table.device_, &pool_info, nullptr, &table.pool_);
        result != VK_SUCCESS) {
        return Error{"vkCreateDescriptorPool failed: " + vk_result_name(result)};
    }
    set_debug_name(table.device_, table.pool_, debug_name);
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = table.pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &table.layout_,
    };
    if (const VkResult result = vkAllocateDescriptorSets(table.device_, &allocate, &table.set_);
        result != VK_SUCCESS) {
        return Error{"vkAllocateDescriptorSets failed: " + vk_result_name(result)};
    }
    set_debug_name(table.device_, table.set_, debug_name);
    return table;
}

Result<std::uint32_t> TextureTable::add(VkImageView view) {
    if (size_ == capacity_) {
        return Error{"the texture table is full, at " + std::to_string(capacity_) + " textures"};
    }
    const VkDescriptorImageInfo image = {
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set_,
        .dstBinding = 0,
        .dstArrayElement = size_,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return size_++;
}

void TextureTable::set_block_textures(VkImageView view) {
    const VkDescriptorImageInfo image = {
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set_,
        .dstBinding = 2,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

TextureTable::TextureTable(TextureTable&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      sampler_(std::exchange(other.sampler_, VK_NULL_HANDLE)),
      block_sampler_(std::exchange(other.block_sampler_, VK_NULL_HANDLE)),
      layout_(std::exchange(other.layout_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      set_(std::exchange(other.set_, VK_NULL_HANDLE)), size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

TextureTable& TextureTable::operator=(TextureTable&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        block_sampler_ = std::exchange(other.block_sampler_, VK_NULL_HANDLE);
        layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        set_ = std::exchange(other.set_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
}

TextureTable::~TextureTable() {
    destroy();
}

void TextureTable::destroy() noexcept {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr); // frees the set
        pool_ = VK_NULL_HANDLE;
        set_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (block_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, block_sampler_, nullptr);
        block_sampler_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
