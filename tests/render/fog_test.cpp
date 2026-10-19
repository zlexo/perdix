#include <vx/render/fog.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace {

using vx::render::FogRamps;
using vx::render::Srgb8;

// Minecraft 26.2's ramps for a view of `blocks`, worked out apart, in its
// float arithmetic: the haze to 1,024, the band from D - clamp(D / 10, 4,
// 64) to D.
FogRamps game_ramps(int blocks) {
    const auto view = static_cast<float>(blocks);
    return {.haze_end = 1'024.0F,
            .band_start = view - std::clamp(view / 10.0F, 4.0F, 64.0F),
            .band_end = view};
}

} // namespace

TEST_CASE("up to 26.2's farthest view, 512 blocks, the fog's ramps are the game's") {
    for (int blocks = 4; blocks <= 512; ++blocks) {
        CAPTURE(blocks);
        const FogRamps ours = vx::render::fog_ramps(blocks);
        const FogRamps game = game_ramps(blocks);
        CHECK(ours.haze_end == game.haze_end);
        CHECK(ours.band_start == doctest::Approx(game.band_start).epsilon(1e-6));
        CHECK(ours.band_end == game.band_end);
    }
}

TEST_CASE("past 512 blocks the haze ends at twice the view and the band starts at nine tenths") {
    for (const int blocks : {513, 640, 641, 1'024, 2'048, 16'384}) {
        CAPTURE(blocks);
        const FogRamps ramps = vx::render::fog_ramps(blocks);
        CHECK(ramps.haze_end == doctest::Approx(2.0 * blocks));
        CHECK(ramps.band_start == doctest::Approx(0.9 * blocks));
        CHECK(ramps.band_end == doctest::Approx(blocks));
        // So the edge looks like 26.2's at its farthest: a quarter at half
        // the view, 45% at nine tenths, solid at the edge.
        const auto at = [&ramps, blocks](double share) {
            const auto distance = static_cast<float>(share * blocks);
            return vx::render::fog_amount(ramps, distance, distance);
        };
        CHECK(at(0.5) == doctest::Approx(0.25));
        CHECK(at(0.9) == doctest::Approx(0.45));
        CHECK(at(1.0) == 1.0F);
    }
    // The two meet at 512: 26.2's farthest is where the scaling starts.
    CHECK(vx::render::fog_ramps(512.0) == game_ramps(512));
}

TEST_CASE("the fog is the larger of the haze on the distance and the band on the cylinder") {
    const FogRamps ramps{.haze_end = 1'000.0F, .band_start = 400.0F, .band_end = 500.0F};
    CHECK(vx::render::fog_amount(ramps, 0.0F, 0.0F) == 0.0F);
    CHECK(vx::render::fog_amount(ramps, 250.0F, 100.0F) == doctest::Approx(0.25));
    // The band from a point's cylinder, not its distance.
    CHECK(vx::render::fog_amount(ramps, 300.0F, 450.0F) == doctest::Approx(0.5));
    CHECK(vx::render::fog_amount(ramps, 450.0F, 300.0F) == doctest::Approx(0.45));
    CHECK(vx::render::fog_amount(ramps, 600.0F, 500.0F) == 1.0F);
    CHECK(vx::render::fog_amount(ramps, 2'000.0F, 0.0F) == 1.0F);
}

TEST_CASE("under 512 blocks the fog color moves toward the sky's as 26.2 moves it") {
    // Worked out in 26.2's float arithmetic, a channel at a time rounded down.
    const std::array<std::pair<double, Srgb8>, 6> expected{{
        {32.0, {.r = 173, .g = 203, .b = 255}},
        {192.0, {.r = 181, .g = 208, .b = 255}},
        {384.0, {.r = 188, .g = 213, .b = 255}},
        {496.0, {.r = 191, .g = 215, .b = 255}},
        {512.0, {.r = 192, .g = 216, .b = 255}},
        {16'384.0, {.r = 192, .g = 216, .b = 255}},
    }};
    for (const auto& [blocks, color] : expected) {
        CAPTURE(blocks);
        CHECK(vx::render::fog_color({}, blocks) == color);
    }
}

TEST_CASE("26.2's daytime sky turns to the fog color toward the horizon, as its disc is fogged") {
    // The disc 16 blocks up, 512 across, fogged from 0 to 512 blocks, its
    // distance from 16 at the middle to 512.25 at the rim: at 90 degrees 16
    // blocks away, at 30 degrees 27.7 out, at 2 degrees 458 out.
    const auto at = [](double degrees) {
        return vx::render::sky_fog_amount(
            static_cast<float>(std::sin(degrees * std::numbers::pi / 180.0)));
    };
    CHECK(at(90.0) == doctest::Approx(16.0 / 512.0).epsilon(1e-4));
    CHECK(at(30.0) == doctest::Approx(0.0837).epsilon(2e-3));
    CHECK(at(10.0) == doctest::Approx(0.2030).epsilon(2e-3));
    CHECK(at(5.0) == doctest::Approx(0.3775).epsilon(2e-3));
    CHECK(at(3.0) == doctest::Approx(0.6092).epsilon(2e-3));
    CHECK(at(2.0) == doctest::Approx(0.8990).epsilon(2e-3));
    // From the rim, 1.79 degrees, down: the fog color.
    CHECK(at(1.78) == 1.0F);
    CHECK(at(0.0) == 1.0F);
    CHECK(at(-30.0) == 1.0F);
    // Never less as the ray sinks.
    for (double degrees = 89.0; degrees > 0.0; degrees -= 1.0) {
        CHECK(at(degrees) >= at(degrees + 1.0));
    }
}
