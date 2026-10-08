#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>
#include <vx/render/image.hpp>

#include <cstdint>

namespace vx::render {

// RFC 0004's block texture array on the GPU: one VK_FORMAT_R8G8B8A8_SRGB 2D
// array image of 16 by 16 texels, five mip levels and `layers` layers, which
// upload_image fills from build_block_textures' bytes in one copy. It can be
// sampled, copied into from staging, and copied out of, for tests and tools
// that read it back. More layers than the device holds in one image is an
// error naming both numbers: Vulkan guarantees 256 layers on a 1.3 device
// and 2,048 on a 1.4 device.
[[nodiscard]] Result<Image> create_block_texture_array(const Device& device, std::uint32_t layers);

} // namespace vx::render
