#pragma once

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_registry.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

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

// Level 0 of every texture in the map that the player's Minecraft can make
// (RFC 0009): each layer read from the jar and decoded, 16 by 16, tinted
// with the plains color of its color map, or its own color, and drawn over
// the layers below it. A texture the jar cannot make, for a layer or a color
// map that is missing, or that cannot be read or decoded, or has the wrong
// size, is left out with a warning at the name in the map, and is then drawn
// from its own file. The warnings come in the order of the file, and name
// the game by `version`, such as an Install's: "Minecraft 26.2 has no
// block texture 'grass_block_side_overlay'".
[[nodiscard]] std::vector<MadeTexture> make_textures(const Archive& jar, std::string_view version,
                                                     const TextureMap& map,
                                                     std::vector<Diagnostic>& diagnostics);

} // namespace vx::minecraft
