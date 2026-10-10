#include "../core/png_bytes.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/skin_file.hpp>
#include <vx/world/skin.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using vx::test::PngColor;

// A texel as {r, g, b, a}.
using Texel = std::array<int, 4>;

// An image's RGBA samples, from a function of (x, y) giving each texel.
template <class Color> std::string samples(int width, int height, Color color) {
    std::string out;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (const int channel : color(x, y)) {
                out.push_back(static_cast<char>(channel));
            }
        }
    }
    return out;
}

Texel texel(const vx::Skin& skin, int x, int y) {
    const auto at = 4 * ((static_cast<std::size_t>(y) * 64) + static_cast<std::size_t>(x));
    return {skin.rgba[at], skin.rgba[at + 1], skin.rgba[at + 2], skin.rgba[at + 3]};
}

// Compares a skin with what is expected of every texel, and names the first
// few that differ.
template <class Expected> void check_texels(const vx::Skin& skin, Expected expected) {
    int wrong = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Texel want = expected(x, y);
            const Texel got = texel(skin, x, y);
            if (got != want && ++wrong <= 4) {
                FAIL_CHECK("texel (" << x << ", " << y << ") is " << got[0] << " " << got[1] << " "
                                     << got[2] << " " << got[3] << ", not " << want[0] << " "
                                     << want[1] << " " << want[2] << " " << want[3]);
            }
        }
    }
    CHECK(wrong == 0);
}

// A jar holding these default skins, deflated as the game's entries are:
// each a name under assets/minecraft/textures/entity/player/ and its PNG.
vx::Archive jar_of(const std::vector<std::pair<std::string, std::string>>& skins) {
    std::vector<vx::test::ZipEntry> entries;
    for (const auto& [name, png] : skins) {
        entries.push_back({.name = "assets/minecraft/textures/entity/player/" + name,
                           .data = png,
                           .flags = 8,
                           .deflate = true});
    }
    vx::Result<vx::Archive> jar =
        vx::Archive::from_bytes("26.2.jar", vx::test::make_zip(entries).bytes);
    REQUIRE(jar.has_value());
    return std::move(jar).value();
}

} // namespace

TEST_CASE("the default skins are the game's nine") {
    CHECK(vx::minecraft::default_skin_names ==
          std::array<std::string_view, 9>{"alex", "ari", "efe", "kai", "makena", "noor", "steve",
                                          "sunny", "zuri"});
}

TEST_CASE("a default skin is read from the jar's folder for its arms") {
    const auto gray = [](int value) {
        return vx::test::make_png(64, 64, PngColor::gray,
                                  std::string(64 * 64, static_cast<char>(value)));
    };
    const vx::Archive jar = jar_of({{"slim/alex.png", gray(40)}, {"wide/alex.png", gray(80)}});
    const vx::Result<vx::Skin> slim = vx::minecraft::default_skin(jar, "alex", vx::Arms::slim);
    REQUIRE(slim.has_value());
    CHECK(slim.value().arms == vx::Arms::slim);
    CHECK(texel(slim.value(), 9, 9) == Texel{40, 40, 40, 255});
    const vx::Result<vx::Skin> wide = vx::minecraft::default_skin(jar, "alex", vx::Arms::wide);
    REQUIRE(wide.has_value());
    CHECK(wide.value().arms == vx::Arms::wide);
    CHECK(texel(wide.value(), 9, 9) == Texel{80, 80, 80, 255});
}

TEST_CASE("a palette default skin with a transparent entry decodes to its colors as they are") {
    // As the game's are: a palette, one entry transparent, the hat empty. The
    // game draws them unchanged, so a transparent texel stays transparent even
    // on the base layer, here at (0, 0), which no face uses.
    const auto color = [](int x, int y) {
        if (x >= 8 && x < 16 && y >= 8 && y < 16) {
            return Texel{200, 150, 100, 255}; // the face
        }
        if (x >= 20 && x < 28 && y >= 20 && y < 32) {
            return Texel{50, 60, 70, 255}; // the body's front
        }
        if (x == 21 && y == 37) {
            return Texel{90, 91, 92, 255}; // one jacket texel
        }
        return Texel{10, 20, 30, 0};
    };
    const std::string png = vx::test::make_palette_png(64, 64, 4, samples(64, 64, color));
    REQUIRE_FALSE(png.empty());
    const vx::Archive jar = jar_of({{"wide/noor.png", png}});
    const vx::Result<vx::Skin> skin = vx::minecraft::default_skin(jar, "noor", vx::Arms::wide);
    REQUIRE(skin.has_value());
    check_texels(skin.value(), color);
    CHECK(texel(skin.value(), 0, 0) == Texel{10, 20, 30, 0});
    CHECK(texel(skin.value(), 40, 8) == Texel{10, 20, 30, 0});
}

TEST_CASE("a default skin the jar cannot give is an error that says which and why") {
    const std::string square =
        vx::test::make_png(64, 64, PngColor::gray, std::string(64 * 64, 'x'));
    std::vector<vx::test::ZipEntry> entries = {
        {.name = "assets/minecraft/textures/entity/player/slim/kai.png",
         .data = vx::test::make_png(64, 32, PngColor::gray, std::string(64 * 32, 'x'))},
        {.name = "assets/minecraft/textures/entity/player/slim/noor.png", .data = "GIF89a"},
        {.name = "assets/minecraft/textures/entity/player/wide/zuri.png", .data = square},
    };
    vx::test::Zip zip = vx::test::make_zip(entries);
    // The last byte of zuri's stored data, so that its CRC-32 fails.
    zip.bytes[zip.central_headers[0] - 1] ^= std::byte{0xFF};
    vx::Result<vx::Archive> archive = vx::Archive::from_bytes("26.2.jar", std::move(zip.bytes));
    REQUIRE(archive.has_value());
    const vx::Archive& jar = archive.value();
    const auto message = [&](std::string_view name, vx::Arms arms) {
        const vx::Result<vx::Skin> skin = vx::minecraft::default_skin(jar, name, arms);
        REQUIRE_FALSE(skin.has_value());
        return skin.error().message;
    };
    CHECK(message("bob", vx::Arms::wide) ==
          "the game has no default skin named 'bob', only alex, ari, efe, kai, makena, noor, "
          "steve, sunny, zuri");
    CHECK(message("Steve", vx::Arms::wide)
              .starts_with("the game has no default skin named "
                           "'Steve'"));
    CHECK(message("steve", vx::Arms::wide) ==
          "26.2.jar has no entry 'assets/minecraft/textures/entity/player/wide/steve.png'");
    // The slim one is no stand-in for the wide one.
    CHECK(message("kai", vx::Arms::wide) ==
          "26.2.jar has no entry 'assets/minecraft/textures/entity/player/wide/kai.png'");
    // A legacy size is no default skin: the game draws its defaults as they are.
    CHECK(message("kai", vx::Arms::slim) ==
          "'assets/minecraft/textures/entity/player/slim/kai.png' is 64 by 32 pixels, not 64 by "
          "64");
    CHECK(message("noor", vx::Arms::slim)
              .starts_with("'assets/minecraft/textures/entity/player/slim/noor.png' is not a PNG "
                           "file stb_image can read ("));
    CHECK(message("zuri", vx::Arms::wide) ==
          "26.2.jar: 'assets/minecraft/textures/entity/player/wide/zuri.png' fails its CRC-32 "
          "check");
}
