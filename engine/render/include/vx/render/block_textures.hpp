#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
// (Castaño), so leaves keep their density at a distance.
[[nodiscard]] BlockMips mipmaps(const BlockImage& image);

// Layer 0: magenta and black squares, so a missing texture is visibly wrong.
[[nodiscard]] BlockImage checker() noexcept;

} // namespace vx::render
