#include <vx/minecraft/made_textures.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vx::minecraft {
ColorMapTexel color_map_texel(float temperature, float downfall) noexcept {
    // Java's (int) of a double, which is 0 for NaN: clamping lets NaN through.
    const auto java_int = [](double value) {
        return std::isnan(value) ? 0 : static_cast<int>(value);
    };
    const auto t = static_cast<double>(std::clamp(temperature, 0.0F, 1.0F));
    const double d = static_cast<double>(std::clamp(downfall, 0.0F, 1.0F)) * t;
    return {.x = java_int((1.0 - t) * 255.0), .y = java_int((1.0 - d) * 255.0)};
}

void draw_over(Texels& lower, const Texels& upper) noexcept {
    for (std::size_t texel = 0; texel < upper.size(); texel += 4) {
        if (upper[texel + 3] >= alpha_test_passes) {
            std::copy_n(upper.begin() + static_cast<std::ptrdiff_t>(texel), 4,
                        lower.begin() + static_cast<std::ptrdiff_t>(texel));
        }
    }
}

} // namespace vx::minecraft
