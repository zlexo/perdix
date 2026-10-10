#include "../core/png_bytes.hpp"
#include "player_minecraft.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/font_file.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/world/font.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using vx::test::PngColor;

// A texel as {r, g, b, a}.
using Texel = std::array<int, 4>;

// A 128 by 128 atlas, every texel transparent white as the game's are, and
// what a test inks into the cells, as RGBA samples for make_png.
class Atlas {
public:
    void ink(int code, int x, int y, Texel color = {255, 255, 255, 255}) {
        const std::size_t at = at_of(code, x, y);
        for (std::size_t channel = 0; channel < 4; ++channel) {
            samples_[at + channel] = static_cast<char>(color.at(channel));
        }
    }
    // A glyph whose ink is `width` columns wide, on its fourth row.
    void glyph(char code, int width) {
        for (int x = 0; x < width; ++x) {
            ink(code, x, 3);
        }
    }
    [[nodiscard]] const std::string& samples() const { return samples_; }
    [[nodiscard]] std::string png() const {
        return vx::test::make_png(128, 128, PngColor::rgba, samples_);
    }

private:
    static std::size_t at_of(int code, int x, int y) {
        const int column = ((code % 16) * 8) + x;
        const int row = ((code / 16) * 8) + y;
        return 4 * ((static_cast<std::size_t>(row) * 128) + static_cast<std::size_t>(column));
    }

    std::string samples_ = [] {
        std::string out;
        for (int texel = 0; texel < 128 * 128; ++texel) {
            out.append("\xFF\xFF\xFF\0", 4);
        }
        return out;
    }();
};

std::span<const std::byte> bytes_of(const std::string& text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

vx::Font read(const std::string& png) {
    vx::Result<vx::Font> font = vx::minecraft::read_font(bytes_of(png));
    if (!font.has_value()) {
        FAIL(font.error().message);
    }
    return std::move(font).value();
}

// The texel at (x, y) of a code point's cell.
Texel texel(const vx::Font& font, int code, int x, int y) {
    const int column = ((code % 16) * 8) + x;
    const int row = ((code / 16) * 8) + y;
    const auto at = 4 * ((static_cast<std::size_t>(row) * 128) + static_cast<std::size_t>(column));
    return {font.rgba[at], font.rgba[at + 1], font.rgba[at + 2], font.rgba[at + 3]};
}

int width_of(const vx::Font& font, std::string_view text) {
    int width = 0;
    for (const char c : text) {
        width += font.advances.at(static_cast<std::size_t>(c));
    }
    return width;
}

} // namespace

TEST_CASE("a printable character advances its ink width plus one") {
    Atlas atlas;
    atlas.glyph('A', 5);
    atlas.glyph('i', 1);
    atlas.glyph('l', 2);
    atlas.glyph('@', 6);
    atlas.glyph('W', 8);
    // The rightmost ink in any row counts.
    atlas.ink('g', 6, 7);
    atlas.ink('q', 2, 0);
    atlas.ink('q', 4, 5);
    const vx::Font font = read(atlas.png());
    CHECK(font.advances['A'] == 6);
    CHECK(font.advances['i'] == 2);
    CHECK(font.advances['l'] == 3);
    CHECK(font.advances['@'] == 7);
    CHECK(font.advances['W'] == 9);
    CHECK(font.advances['g'] == 8);
    CHECK(font.advances['q'] == 6);
    // An empty glyph still advances 1.
    CHECK(font.advances['~'] == 1);
    CHECK(font.advances['!'] == 1);
}

TEST_CASE("ink is any alpha above 0 whatever its color") {
    Atlas atlas;
    atlas.ink('a', 6, 2, {255, 255, 255, 1});
    // Transparent white is no ink; opaque black is.
    atlas.ink('b', 7, 4, {255, 255, 255, 0});
    atlas.ink('b', 3, 4, {0, 0, 0, 255});
    const vx::Font font = read(atlas.png());
    CHECK(font.advances['a'] == 8);
    CHECK(font.advances['b'] == 5);
}

TEST_CASE("Perdix is 32 pixels wide in glyphs of the game's ink widths") {
    // RFC 0012's arithmetic: P, e, r, d and x are 5 wide in ink, i is 1.
    Atlas atlas;
    for (const char c : std::string_view("Perdx")) {
        atlas.glyph(c, 5);
    }
    atlas.glyph('i', 1);
    CHECK(width_of(read(atlas.png()), "Perdix") == 32);
}

TEST_CASE("the space advances 4 and draws nothing") {
    Atlas atlas;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            atlas.ink(' ', x, y);
        }
    }
    const vx::Font font = read(atlas.png());
    CHECK(font.advances[' '] == 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(texel(font, ' ', x, y) == Texel{0, 0, 0, 0});
        }
    }
}

TEST_CASE("a control character is the game's missing glyph") {
    Atlas atlas;
    atlas.glyph('\n', 8);
    const vx::Font font = read(atlas.png());
    for (const int code : {0, 9, 10, 31, 127}) {
        INFO("code point ", code);
        CHECK(font.advances.at(static_cast<std::size_t>(code)) == 6);
        // A box outline 5 wide and 8 tall at the cell's left, the rest clear.
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool edge = x < 5 && (x == 0 || x == 4 || y == 0 || y == 7);
                CHECK(texel(font, code, x, y) ==
                      (edge ? Texel{255, 255, 255, 255} : Texel{0, 0, 0, 0}));
            }
        }
    }
}

TEST_CASE("the atlas keeps the texels it is given") {
    Atlas atlas;
    atlas.glyph('A', 5);
    // A cell past 127 is kept too, though no character uses it.
    atlas.ink(200, 1, 1, {10, 20, 30, 40});
    const vx::Font font = read(atlas.png());
    CHECK(texel(font, 'A', 2, 3) == Texel{255, 255, 255, 255});
    CHECK(texel(font, 'A', 2, 5) == Texel{255, 255, 255, 0});
    CHECK(texel(font, 200, 1, 1) == Texel{10, 20, 30, 40});
}

TEST_CASE("an atlas stored as the game's reads as white ink on transparent white") {
    // ascii.png in 26.2: a palette of two white entries, the first
    // transparent, one bit a texel.
    Atlas atlas;
    atlas.glyph('A', 5);
    const std::string png = vx::test::make_palette_png(128, 128, 1, atlas.samples());
    REQUIRE_FALSE(png.empty());
    const vx::Font font = read(png);
    CHECK(font.advances['A'] == 6);
    CHECK(texel(font, 'A', 4, 3) == Texel{255, 255, 255, 255});
    CHECK(texel(font, 'A', 5, 3) == Texel{255, 255, 255, 0});
}

TEST_CASE("a font of another size or not a PNG is refused") {
    for (const auto& [width, height] : {std::pair{256, 256}, std::pair{128, 64}}) {
        const std::string png = vx::test::make_png(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), PngColor::gray,
            std::string(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 'x'));
        const vx::Result<vx::Font> font = vx::minecraft::read_font(bytes_of(png));
        REQUIRE_FALSE(font.has_value());
        CHECK(font.error().message == "is " + std::to_string(width) + " by " +
                                          std::to_string(height) + " pixels, not 128 by 128");
    }
    const vx::Result<vx::Font> text = vx::minecraft::read_font(bytes_of("not a PNG"));
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().message.starts_with("is not a PNG file stb_image can read ("));
}

TEST_CASE("the default font is the jar's ascii.png, or an error that names it") {
    const auto jar_of = [](std::string png) {
        const std::vector<vx::test::ZipEntry> entries = {
            {.name = "assets/minecraft/textures/font/ascii.png",
             .data = std::move(png),
             .flags = 8,
             .deflate = true}};
        vx::Result<vx::Archive> jar =
            vx::Archive::from_bytes("26.2.jar", vx::test::make_zip(entries).bytes);
        REQUIRE(jar.has_value());
        return std::move(jar).value();
    };
    Atlas atlas;
    atlas.glyph('A', 5);
    const vx::Result<vx::Font> font = vx::minecraft::default_font(jar_of(atlas.png()));
    REQUIRE(font.has_value());
    CHECK(font.value().advances['A'] == 6);

    const vx::Result<vx::Font> tall = vx::minecraft::default_font(jar_of(
        vx::test::make_png(128, 64, PngColor::gray, std::string(std::size_t{128} * 64, 'x'))));
    REQUIRE_FALSE(tall.has_value());
    CHECK(tall.error().message ==
          "'assets/minecraft/textures/font/ascii.png' is 128 by 64 pixels, not 128 by 128");

    vx::Result<vx::Archive> empty =
        vx::Archive::from_bytes("26.2.jar", vx::test::make_zip({}).bytes);
    REQUIRE(empty.has_value());
    const vx::Result<vx::Font> missing = vx::minecraft::default_font(empty.value());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message ==
          "26.2.jar has no entry 'assets/minecraft/textures/font/ascii.png'");
}

TEST_CASE("the game's own font, when asked") {
    // A local check against the real game, never in CI: with the variable set,
    // finding no jar fails the test.
    if (!vx::test::player_minecraft_asked()) {
        MESSAGE(vx::test::not_asked);
        return;
    }
    std::vector<vx::Diagnostic> diagnostics;
    const vx::Result<vx::minecraft::Install> install =
        vx::minecraft::find_install(vx::test::player_query(), diagnostics);
    if (!install.has_value()) {
        FAIL(install.error().message);
    }
    const vx::Result<vx::Font> font = vx::minecraft::default_font(install.value().jar);
    if (!font.has_value()) {
        FAIL(font.error().message);
    }
    // RFC 0012's widths, and the tag over you.
    CHECK(width_of(font.value(), "Perdix") == 32);
    CHECK(width_of(font.value(), "Zlex") == 21);
    CHECK(font.value().advances[' '] == 4);
}
