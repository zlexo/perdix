#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vx {

// A font's atlas is 128 by 128 texels: 16 rows of 16 cells of 8 by 8, the
// cell of code point c at column c % 16 and row c / 16.
inline constexpr int font_atlas_size = 128;
inline constexpr int font_cell_size = 8;

// An ASCII bitmap font (RFC 0012): a 128 by 128 RGBA8 atlas of 16 by 16 cells
// of 8 by 8 pixels, white ink on transparent, rows from the top, and each
// character's advance in pixels. The cells past code point 127 are not used.
struct Font {
    std::array<std::uint8_t, std::size_t{font_atlas_size} * font_atlas_size * 4> rgba{};
    std::array<std::uint8_t, 128> advances{};
};

} // namespace vx
