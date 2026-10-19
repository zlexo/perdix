#pragma once

#include <cstdint>

namespace vx::render {

// A color as Minecraft keeps one: 8 bits a channel, sRGB-encoded.
struct Srgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    friend constexpr bool operator==(const Srgb8&, const Srgb8&) = default;
};

// RFC 0013's fog ramps for a view, in blocks: Minecraft 26.2's two, each
// linear from its start to its end. The haze runs on the distance from the
// eye, from 0; the band on the larger of the distance across and the
// height difference, at the view's edge.
struct FogRamps {
    float haze_end = 0.0F;
    float band_start = 0.0F;
    float band_end = 0.0F;

    friend constexpr bool operator==(const FogRamps&, const FogRamps&) = default;
};

// The ramps for a view of `view_blocks`. For any view 26.2 can have, up to
// 512 blocks, its own: the haze to 1,024 blocks, the band from D -
// clamp(D / 10, 4, 64) to D. Past 512 blocks the haze ends at 2D and the
// band is D / 10 wide, so the edge keeps the look of 26.2's at its
// farthest view, where 1,024 is 2D and the band is D / 10.
[[nodiscard]] FogRamps fog_ramps(double view_blocks) noexcept;

// How much fog a point gets, 0 to 1, as the shaders work it out: the larger
// of the haze at its distance from the eye and the band at `cylinder`, the
// larger of its distance across and its height difference.
[[nodiscard]] float fog_amount(const FogRamps& ramps, float distance, float cylinder) noexcept;

// 26.2's noon overworld (the overworld's visual attributes, which its day
// timeline multiplies by white from tick 133 to 11,867): the fog and sky
// colors, in one place for a later sky RFC.
struct FogLook {
    Srgb8 fog{.r = 0xC0, .g = 0xD8, .b = 0xFF};
    Srgb8 sky{.r = 0x78, .g = 0xA7, .b = 0xFF};
};

// The fog color for a view of `view_blocks`. Under 512 blocks 26.2 moves it
// toward the sky color by 1 - (0.25 + 0.75 c / 32)^0.25, c the view in its
// chunks of 16 blocks, a channel at a time and rounded down, in its float
// arithmetic: 15% of the way at 12 chunks, none from 32.
[[nodiscard]] Srgb8 fog_color(const FogLook& look, double view_blocks) noexcept;

// How much of the fog color 26.2's daytime sky has where a view ray rises
// by `rise`, the sine of its elevation, 0 to 1. Its sky is a disc 16 blocks
// above the eye, 512 across from its middle, its distance from the eye
// interpolated from 16 at the middle to its rim's, and fogged from 0 to 512
// blocks; under the rim's elevation, 1.79 degrees, the fog color shows.
[[nodiscard]] float sky_fog_amount(float rise) noexcept;

// A frame's fog (RFC 0013): the color it mixes toward, its ramps, and the
// sky's color, which fades into it toward the horizon: 26.2's noon sky
// unless given.
struct Fog {
    Srgb8 color;
    FogRamps ramps;
    Srgb8 sky = FogLook{}.sky;
};

} // namespace vx::render
