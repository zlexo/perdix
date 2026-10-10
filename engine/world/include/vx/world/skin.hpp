#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vx {

// A player's arms (RFC 0012): wide ones 4 texels across, slim ones 3. The
// skin says which; the model, the skin file and the renderer read it.
enum class Arms : std::uint8_t { wide, slim };

// A player skin is 64 by 64 texels, in Minecraft's layout since 1.8.
inline constexpr int skin_size = 64;

// A player skin: RGBA8 texels, rows from the top, and the arms it is drawn
// on.
struct Skin {
    std::array<std::uint8_t, std::size_t{skin_size} * skin_size * 4> rgba{};
    Arms arms = Arms::wide;
};

} // namespace vx
