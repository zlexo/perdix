#include <vx/render/fog.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vx::render {

FogRamps fog_ramps(double view_blocks) noexcept {
    const double view = std::max(view_blocks, 0.0);
    const double haze = std::max(1'024.0, 2.0 * view);
    const double band = std::max(4.0, view / 10.0);
    return {.haze_end = static_cast<float>(haze),
            .band_start = static_cast<float>(view - band),
            .band_end = static_cast<float>(view)};
}

namespace {

// 26.2's ramp: 0 up to `start`, 1 from `end`, linear between.
float ramp(float at, float start, float end) noexcept {
    if (at <= start) {
        return 0.0F;
    }
    if (at >= end) {
        return 1.0F;
    }
    return (at - start) / (end - start);
}

} // namespace

float fog_amount(const FogRamps& ramps, float distance, float cylinder) noexcept {
    return std::max(ramp(distance, 0.0F, ramps.haze_end),
                    ramp(cylinder, ramps.band_start, ramps.band_end));
}

Srgb8 fog_color(const FogLook& look, double view_blocks) noexcept {
    // 26.2 counts the view in its chunks, at most the sky's fog end over
    // 16: 32. Its arithmetic is in floats, the power in doubles.
    const float chunks = std::min(32.0F, static_cast<float>(std::max(view_blocks, 0.0) / 16.0));
    const float near = 0.25F + ((chunks / 32.0F) * 0.75F);
    const float toward = 1.0F - static_cast<float>(std::pow(static_cast<double>(near), 0.25));
    const auto channel = [toward](std::uint8_t from, std::uint8_t to) {
        const float step = toward * static_cast<float>(int{to} - int{from});
        return static_cast<std::uint8_t>(int{from} + static_cast<int>(std::floor(step)));
    };
    return {.r = channel(look.fog.r, look.sky.r),
            .g = channel(look.fog.g, look.sky.g),
            .b = channel(look.fog.b, look.sky.b)};
}

} // namespace vx::render
