#include <vx/render/block_texture_array.hpp>

#include <vx/render/block_textures.hpp>

#include <string>

namespace vx::render {

Result<Image> create_block_texture_array(const Device& device, std::uint32_t layers) {
    if (layers == 0) {
        return Error{"a block texture array has one layer at least, the checker"};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    if (const std::uint32_t most = properties.limits.maxImageArrayLayers; layers > most) {
        return Error{"the block textures need " + std::to_string(layers) +
                     " layers, and this device holds " + std::to_string(most) +
                     " at most in one image"};
    }
    constexpr auto size = static_cast<std::uint32_t>(block_texture_size);
    return Image::create_array(device, {.width = size, .height = size}, VK_FORMAT_R8G8B8A8_SRGB,
                               static_cast<std::uint32_t>(block_texture_levels), layers,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               "block textures");
}

} // namespace vx::render
