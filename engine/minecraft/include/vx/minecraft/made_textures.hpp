#pragma once

#include <vx/world/block_registry.hpp>

#include <cstdint>

namespace vx::minecraft {

// A 16 by 16 texture's texels, RGBA8 with sRGB color, as MadeTexture holds
// them.
using Texels = decltype(MadeTexture::rgba);

// Where the game reads a color map for a biome (RFC 0009), 256 by 256 texels:
// x from its temperature, y from its downfall times its temperature, each
// clamped to 0 to 1 as a float and then widened to double, as the game does.
// One that is not a number reads 0, as Java's (int) of NaN does.
struct ColorMapTexel {
    int x = 0;
    int y = 0;

    friend constexpr bool operator==(const ColorMapTexel&, const ColorMapTexel&) = default;
};
[[nodiscard]] ColorMapTexel color_map_texel(float temperature, float downfall) noexcept;

// Plains, whose colors the core pack's textures take until biomes come
// (M4): read at (50, 173).
inline constexpr float plains_temperature = 0.8F;
inline constexpr float plains_downfall = 0.4F;

// A color channel multiplied by a tint's, on the stored values, as the
// game's shader does, rounded to the nearest: c × t / 255.
[[nodiscard]] constexpr std::uint8_t tinted(std::uint8_t channel, std::uint8_t tint) noexcept {
    return static_cast<std::uint8_t>(((channel * tint) + 127) / 255);
}

// Draws `upper` over `lower`, as the game draws an overlay: each texel of
// `upper` that passes the engine's alpha test (alpha 128 or more) replaces
// the one under it, alpha included; any other leaves it.
void draw_over(Texels& lower, const Texels& upper) noexcept;

} // namespace vx::minecraft
