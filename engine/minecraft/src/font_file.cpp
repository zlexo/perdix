#include <vx/minecraft/font_file.hpp>

#include "png.hpp"

#include <vx/core/result.hpp>
#include <vx/world/font.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx::minecraft {
namespace {

constexpr int cells_per_row = font_atlas_size / font_cell_size;

// The game's space provider (font/include/space.json) gives the space this
// advance and no glyph; it comes before the bitmaps in font/default.json.
constexpr std::uint8_t space_advance = 4;

// What the game draws for a character none of its fonts has (FontSet,
// SpecialGlyphs.MISSING): a white box outline 5 wide and 8 tall, at the top
// of the line as the ASCII glyphs are, advancing its width plus 1.
constexpr int missing_width = 5;
constexpr int missing_height = 8;
constexpr std::uint8_t missing_advance = missing_width + 1;

// The first byte of the texel at (x, y) of `code`'s cell.
constexpr std::size_t texel_at(int code, int x, int y) noexcept {
    const int column = ((code % cells_per_row) * font_cell_size) + x;
    const int row = ((code / cells_per_row) * font_cell_size) + y;
    return 4 *
           ((static_cast<std::size_t>(row) * font_atlas_size) + static_cast<std::size_t>(column));
}

void clear_cell(Font& font, int code) noexcept {
    for (int y = 0; y < font_cell_size; ++y) {
        std::fill_n(font.rgba.begin() + static_cast<std::ptrdiff_t>(texel_at(code, 0, y)),
                    4 * font_cell_size, std::uint8_t{0});
    }
}

void draw_missing_glyph(Font& font, int code) noexcept {
    clear_cell(font, code);
    for (int y = 0; y < missing_height; ++y) {
        for (int x = 0; x < missing_width; ++x) {
            if (x == 0 || x == missing_width - 1 || y == 0 || y == missing_height - 1) {
                std::fill_n(font.rgba.begin() + static_cast<std::ptrdiff_t>(texel_at(code, x, y)),
                            4, std::uint8_t{255});
            }
        }
    }
}

// One more than the rightmost column of the cell with any ink, 0 when it has
// none. Ink is any alpha above 0, whatever the color: the game reads an RGBA
// atlas's alpha (BitmapProvider, NativeImage.getLuminanceOrAlpha).
int ink_width(const Font& font, int code) noexcept {
    for (int x = font_cell_size - 1; x >= 0; --x) {
        for (int y = 0; y < font_cell_size; ++y) {
            if (font.rgba[texel_at(code, x, y) + 3] != 0) {
                return x + 1;
            }
        }
    }
    return 0;
}

} // namespace

Result<Font> read_font(std::span<const std::byte> png) {
    constexpr std::array<ImageSize, 1> size = {
        ImageSize{.width = font_atlas_size, .height = font_atlas_size}};
    const Result<Image> image = decode_png(png, size);
    if (!image) {
        return image.error();
    }
    Font font;
    std::ranges::copy(image.value().rgba, font.rgba.begin());
    for (std::size_t index = 0; index < font.advances.size(); ++index) {
        const auto code = static_cast<int>(index);
        std::uint8_t& advance = font.advances[index];
        if (code == ' ') {
            clear_cell(font, code);
            advance = space_advance;
        } else if (code < ' ' || code == 127) {
            // The game's ASCII grid leaves these cells out, and its jar's
            // unifont include is empty.
            draw_missing_glyph(font, code);
            advance = missing_advance;
        } else {
            // The game scales the ink width by the bitmap's height over its
            // cell's, 8 over 8 here, and rounds it to the nearest.
            advance = static_cast<std::uint8_t>(ink_width(font, code) + 1);
        }
    }
    return font;
}

} // namespace vx::minecraft
