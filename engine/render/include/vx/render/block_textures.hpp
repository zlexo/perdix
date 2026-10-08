#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/world/block_registry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace vx::render {

// Block textures (RFC 0004): 16 by 16 texels, RGBA8 with sRGB color, and
// five mip levels made for pixel art.
inline constexpr int block_texture_size = 16;
inline constexpr int block_texture_levels = 5;
// The texels of levels 0 to 4 (16x16, 8x8, 4x4, 2x2, 1x1), and the first of
// each level among them.
inline constexpr std::size_t block_texture_texels = 341;
inline constexpr std::array<std::size_t, 5> block_texture_level_start = {0, 256, 320, 336, 340};
// One layer of the texture array, every level.
inline constexpr std::size_t block_texture_layer_bytes = block_texture_texels * 4; // 1,364

struct Texel {
    std::uint8_t r = 0; // sRGB
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0; // linear: 0 is fully transparent

    friend constexpr bool operator==(const Texel&, const Texel&) = default;
};

// Level 0 of a block texture, row by row from the top, each row from the left.
using BlockImage = std::array<Texel, 256>;
// Every level of a block texture, level 0 first.
using BlockMips = std::array<Texel, block_texture_texels>;

// The image turned clockwise by quarter turns: each moves its top edge to
// its right.
[[nodiscard]] BlockImage turned(const BlockImage& image, int quarter_turns) noexcept;

// Gives every fully transparent texel the color of the nearest visible
// texels, spreading outward a ring at a time and wrapping at the edges,
// because textures repeat. Alpha stays 0, so filters that reach into a
// transparent texel blend toward its neighbors' color instead of black.
void fill_transparent(BlockImage& image);

// Levels 1 to 4, each straight from level 0: colors averaged in linear
// light, weighted by alpha (a plain average where every texel is fully
// transparent). When the alpha test can discard texels, each level's alpha
// is then scaled so the share of texels that pass matches level 0's
// (Castaño), with ties at the test broken in an ordered-dither order, so
// leaves keep their density at a distance.
[[nodiscard]] BlockMips mipmaps(const BlockImage& image);

// Layer 0: magenta and black squares, so a missing texture is visibly wrong.
[[nodiscard]] BlockImage checker() noexcept;

// A PNG file's bytes as a block image, RGBA8 whatever the file stores, or
// why not: not a PNG, or not 16 by 16.
[[nodiscard]] Result<BlockImage> decode_block_texture(std::span<const std::byte> png);

// Every layer the block registry lists, at every level, on the CPU (RFC
// 0004): each texture decoded once, then for each layer turned, filled and
// mipmapped. The texture core:oak_log is the file
// <assets>/core/textures/blocks/oak_log.png. Laid out for one buffer to
// image copy with a region per level: every layer of level 0, then every
// layer of level 1, and so on. Problems are added to `diagnostics`; after
// an error nothing comes back.
[[nodiscard]] std::vector<std::byte> build_block_textures(std::span<const TextureRef> textures,
                                                          const std::filesystem::path& assets,
                                                          std::vector<Diagnostic>& diagnostics);
// The same, with textures made from something other than their files, such
// as the player's own Minecraft (RFC 0009): a texture among `made` takes its
// level 0 from there, and its file is neither read nor warned about. Made
// textures that no layer uses are passed over, and of two with one name,
// the first is used.
[[nodiscard]] std::vector<std::byte> build_block_textures(std::span<const TextureRef> textures,
                                                          const std::filesystem::path& assets,
                                                          std::span<const MadeTexture> made,
                                                          std::vector<Diagnostic>& diagnostics);

} // namespace vx::render
