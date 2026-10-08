#include <vx/minecraft/made_textures.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using vx::minecraft::Texels;

// The texel at (x, y) of a 16 by 16 texture, as {r, g, b, a}.
std::vector<int> texel(const Texels& texels, std::size_t x, std::size_t y) {
    const std::size_t at = 4 * ((y * 16) + x);
    return {texels[at], texels[at + 1], texels[at + 2], texels[at + 3]};
}

} // namespace

TEST_CASE("the game reads a color map where temperature and downfall put it, in double") {
    using vx::minecraft::color_map_texel;
    using vx::minecraft::ColorMapTexel;
    // Plains, RFC 0009's tint: (50, 173).
    CHECK(color_map_texel(vx::minecraft::plains_temperature, vx::minecraft::plains_downfall) ==
          ColorMapTexel{.x = 50, .y = 173});
    // Worked out apart, in Python with the same float to double widening.
    CHECK(color_map_texel(0.95F, 0.9F) == ColorMapTexel{.x = 12, .y = 36});
    CHECK(color_map_texel(0.7F, 0.8F) == ColorMapTexel{.x = 76, .y = 112});
    CHECK(color_map_texel(0.25F, 0.8F) == ColorMapTexel{.x = 191, .y = 203});
    // Widened to double: in float, 1 - 0.2 rounds to just above 0.8, which
    // gives 204; in double, 203.
    CHECK(color_map_texel(0.2F, 0.0F) == ColorMapTexel{.x = 203, .y = 255});
    // Clamped to 0 to 1 first.
    CHECK(color_map_texel(2.0F, 0.0F) == ColorMapTexel{.x = 0, .y = 255});
    CHECK(color_map_texel(-0.5F, -1.0F) == ColorMapTexel{.x = 255, .y = 255});
    CHECK(color_map_texel(1.0F, 1.0F) == ColorMapTexel{.x = 0, .y = 0});
    // Not a number: clamping lets it through, and Java's (int) makes it 0.
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(color_map_texel(nan, 0.5F) == ColorMapTexel{.x = 0, .y = 0});
    CHECK(color_map_texel(0.5F, nan) == ColorMapTexel{.x = 127, .y = 0});
}

TEST_CASE("a tint multiplies the stored values and rounds to the nearest") {
    using vx::minecraft::tinted;
    CHECK(tinted(255, 145) == 145);
    CHECK(tinted(0, 200) == 0);
    CHECK(tinted(128, 128) == 64);  // 64.25
    CHECK(tinted(1, 128) == 1);     // 0.502
    CHECK(tinted(1, 127) == 0);     // 0.498
    CHECK(tinted(200, 189) == 148); // 148.24: grass_block_top's gray by #91BD59's green
    for (int channel = 0; channel < 256; ++channel) {
        for (int tint = 0; tint < 256; ++tint) {
            const double exact = static_cast<double>(channel) * tint / 255.0;
            const auto nearest = static_cast<int>(std::floor(exact + 0.5));
            if (tinted(static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(tint)) !=
                nearest) {
                FAIL_CHECK(channel << " x " << tint << " gives "
                                   << int{tinted(static_cast<std::uint8_t>(channel),
                                                 static_cast<std::uint8_t>(tint))}
                                   << ", not " << nearest);
            }
        }
    }
}

TEST_CASE("an upper texel replaces the one under it when it passes the alpha test") {
    Texels lower{};
    lower.fill(10);
    Texels upper{};
    for (std::size_t texel = 0; texel < 256; ++texel) {
        upper[4 * texel] = 200;
        upper[(4 * texel) + 1] = 201;
        upper[(4 * texel) + 2] = 202;
    }
    upper[3] = 255;
    upper[7] = 128;
    upper[11] = 127;
    upper[15] = 0;
    vx::minecraft::draw_over(lower, upper);
    CHECK(texel(lower, 0, 0) == std::vector<int>{200, 201, 202, 255});
    // Alpha included: 128 passes, and the texel keeps it.
    CHECK(texel(lower, 1, 0) == std::vector<int>{200, 201, 202, 128});
    CHECK(texel(lower, 2, 0) == std::vector<int>{10, 10, 10, 10});
    CHECK(texel(lower, 3, 0) == std::vector<int>{10, 10, 10, 10});
}
