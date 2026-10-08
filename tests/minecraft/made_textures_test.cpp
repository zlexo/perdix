#include "../core/png_bytes.hpp"
#include "../core/zip_bytes.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/install.hpp>
#include <vx/minecraft/made_textures.hpp>
#include <vx/minecraft/texture_map.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using vx::minecraft::Texels;
using vx::test::PngColor;

// A 16 by 16 texture's samples, one byte each, from a function of (x, y).
template <class Sample> std::string samples(std::size_t per_texel, Sample sample) {
    std::string out;
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            for (std::size_t channel = 0; channel < per_texel; ++channel) {
                out.push_back(static_cast<char>(sample(x, y, channel)));
            }
        }
    }
    return out;
}

// A color map whose every texel differs: red is x, green is y, blue 7.
std::string color_map() {
    std::string out;
    for (std::size_t y = 0; y < 256; ++y) {
        for (std::size_t x = 0; x < 256; ++x) {
            out += {static_cast<char>(x), static_cast<char>(y), '\x07'};
        }
    }
    return vx::test::make_png(256, 256, PngColor::rgb, out);
}

vx::test::ZipEntry block_entry(std::string_view name, std::string png) {
    return {.name = "assets/minecraft/textures/block/" + std::string(name) + ".png",
            .data = std::move(png),
            .flags = 8,
            .deflate = true};
}

vx::test::ZipEntry color_map_entry(std::string_view name, std::string png) {
    return {.name = "assets/minecraft/textures/colormap/" + std::string(name) + ".png",
            .data = std::move(png),
            .flags = 8,
            .deflate = true};
}

// An install whose jar holds these entries, laid out as the game's are:
// deflated, with their sizes after their data.
vx::minecraft::Install install_of(const std::vector<vx::test::ZipEntry>& entries) {
    vx::test::Zip zip = vx::test::make_zip(entries);
    vx::Result<vx::Archive> jar = vx::Archive::from_bytes("26.2.jar", std::move(zip.bytes));
    REQUIRE(jar.has_value());
    return {.jar = std::move(jar).value(), .path = "26.2.jar", .version = "26.2"};
}

vx::minecraft::TextureMap map_of(const std::string& text) {
    vx::minecraft::TextureMapLoad load =
        vx::minecraft::load_texture_map({.path = "assets/core/minecraft.toml", .text = text});
    REQUIRE(load.diagnostics.empty());
    REQUIRE(load.map.has_value());
    return *std::move(load.map);
}

// What make_textures made, and what it said, formatted.
struct Made {
    std::vector<vx::MadeTexture> textures;
    std::vector<std::string> messages;
};

Made make(const vx::minecraft::Install& install, const std::string& map) {
    std::vector<vx::Diagnostic> diagnostics;
    Made made{.textures = vx::minecraft::make_textures(install.jar, install.version, map_of(map),
                                                       diagnostics),
              .messages = {}};
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        made.messages.push_back(vx::format(diagnostic));
    }
    return made;
}

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

TEST_CASE("a texture is its block texture's texels, decoded from the jar") {
    const std::string png = vx::test::make_png(
        16, 16, PngColor::rgba, samples(4, [](std::size_t x, std::size_t y, std::size_t channel) {
            return channel == 3 ? 255 : (x * 16) + (y * channel);
        }));
    const vx::minecraft::Install install = install_of({block_entry("stone", png)});
    const Made made = make(install, "stone = \"stone\"\n");
    CHECK(made.messages.empty());
    REQUIRE(made.textures.size() == 1);
    CHECK(made.textures[0].name == "core:stone");
    CHECK(texel(made.textures[0].rgba, 3, 5) == std::vector<int>{48, 53, 58, 255});
    CHECK(texel(made.textures[0].rgba, 15, 15) == std::vector<int>{240, 255, 14, 255});
}

TEST_CASE("a tinted layer takes its color map's plains color, or its own color") {
    // Gray 200, as grass_block_top is gray in the game.
    const std::string gray =
        vx::test::make_png(16, 16, PngColor::gray,
                           samples(1, [](std::size_t, std::size_t, std::size_t) { return 200; }));
    const vx::minecraft::Install install =
        install_of({block_entry("grass_block_top", gray), color_map_entry("grass", color_map())});
    const Made made = make(install, R"(grass_top = { texture = "grass_block_top", tint = "grass" }
spruce = { texture = "grass_block_top", tint = "#619961" }
)");
    CHECK(made.messages.empty());
    REQUIRE(made.textures.size() == 2);
    // The color map's texel at (50, 173) is (50, 173, 7).
    CHECK(made.textures[0].name == "core:grass_top");
    CHECK(texel(made.textures[0].rgba, 0, 0) ==
          std::vector<int>{vx::minecraft::tinted(200, 50), vx::minecraft::tinted(200, 173),
                           vx::minecraft::tinted(200, 7), 255});
    CHECK(made.textures[1].name == "core:spruce");
    CHECK(texel(made.textures[1].rgba, 7, 9) ==
          std::vector<int>{vx::minecraft::tinted(200, 0x61), vx::minecraft::tinted(200, 0x99),
                           vx::minecraft::tinted(200, 0x61), 255});
}

TEST_CASE("layers are drawn bottom first, an overlay's holes leaving the layer under it") {
    // The game's grass side: an RGB side, and a gray overlay whose gray 0 is
    // transparent (a tRNS chunk), tinted. The overlay covers the top half.
    const std::string side = vx::test::make_png(
        16, 16, PngColor::rgb, samples(3, [](std::size_t x, std::size_t, std::size_t channel) {
            return channel == 0 ? 100 + x : 50;
        }));
    const std::string overlay = vx::test::make_png(
        16, 16, PngColor::gray,
        samples(1, [](std::size_t, std::size_t y, std::size_t) { return y < 8 ? 180 : 0; }), 0);
    const vx::minecraft::Install install = install_of(
        {block_entry("grass_block_side", side), block_entry("grass_block_side_overlay", overlay),
         color_map_entry("grass", color_map())});
    const Made made = make(
        install,
        R"(grass_side = ["grass_block_side", { texture = "grass_block_side_overlay", tint = "grass" }]
)");
    CHECK(made.messages.empty());
    REQUIRE(made.textures.size() == 1);
    const std::vector<int> covered = {vx::minecraft::tinted(180, 50),
                                      vx::minecraft::tinted(180, 173),
                                      vx::minecraft::tinted(180, 7), 255};
    CHECK(texel(made.textures[0].rgba, 4, 0) == covered);
    CHECK(texel(made.textures[0].rgba, 4, 7) == covered);
    CHECK(texel(made.textures[0].rgba, 4, 8) == std::vector<int>{104, 50, 50, 255});
    CHECK(texel(made.textures[0].rgba, 15, 15) == std::vector<int>{115, 50, 50, 255});
}

TEST_CASE("three layers, a color of its own on the middle one, are drawn in order") {
    // A blue base; over its top half, a gray overlay tinted red; over its
    // left half, a white one. Dropping or reordering any layer changes a
    // quarter of the texels.
    const std::string base = vx::test::make_png(
        16, 16, PngColor::rgb, samples(3, [](std::size_t, std::size_t, std::size_t channel) {
            return channel == 2 ? 200 : 20;
        }));
    const std::string middle = vx::test::make_png(
        16, 16, PngColor::gray,
        samples(1, [](std::size_t, std::size_t y, std::size_t) { return y < 8 ? 100 : 0; }), 0);
    const std::string top = vx::test::make_png(
        16, 16, PngColor::gray,
        samples(1, [](std::size_t x, std::size_t, std::size_t) { return x < 8 ? 250 : 0; }), 0);
    const vx::minecraft::Install install = install_of(
        {block_entry("base", base), block_entry("middle", middle), block_entry("top", top)});
    const Made made =
        make(install, R"(three = ["base", { texture = "middle", tint = "#FF0000" }, "top"]
)");
    CHECK(made.messages.empty());
    REQUIRE(made.textures.size() == 1);
    const Texels& texels = made.textures[0].rgba;
    CHECK(texel(texels, 3, 3) == std::vector<int>{250, 250, 250, 255});
    CHECK(texel(texels, 3, 12) == std::vector<int>{250, 250, 250, 255});
    CHECK(texel(texels, 12, 3) == std::vector<int>{100, 0, 0, 255});
    CHECK(texel(texels, 12, 12) == std::vector<int>{20, 20, 200, 255});
}

TEST_CASE("a texture the jar cannot make is left out, with a warning at the name it lacks") {
    const std::string gray =
        vx::test::make_png(16, 16, PngColor::gray,
                           samples(1, [](std::size_t, std::size_t, std::size_t) { return 90; }));
    const std::string tall = vx::test::make_png(16, 32, PngColor::gray, std::string(16 * 32, 'Z'));
    const vx::minecraft::Install install = install_of({
        block_entry("stone", gray),
        block_entry("grass_block_side", gray),
        block_entry("grass_block_top", gray),
        block_entry("water_still", tall),
        block_entry("broken", "not a PNG"),
        color_map_entry("foliage",
                        vx::test::make_png(16, 16, PngColor::gray, std::string(256, 'x'))),
    });
    // RFC 0009's example warning is the second line's. A layer that is
    // missing is not tinted, so its color map is not looked for: grass is
    // reported once, on grass_top.
    const Made made = make(install, R"(stone = "stone"
grass_side = ["grass_block_side", { texture = "grass_block_side_overlay", tint = "grass" }]
grass_top = { texture = "grass_block_top", tint = "grass" }
water = "water_still"
broken = "broken"
leaves = { texture = "grass_block_top", tint = "foliage" }
)");
    REQUIRE(made.textures.size() == 1);
    CHECK(made.textures[0].name == "core:stone");
    REQUIRE(made.messages.size() == 5);
    CHECK(made.messages[0] ==
          "assets/core/minecraft.toml:2:47: warning: Minecraft 26.2 has no block texture "
          "'grass_block_side_overlay'; core:grass_side is drawn from its own file");
    CHECK(made.messages[1] == "assets/core/minecraft.toml:3:51: warning: Minecraft 26.2 has no "
                              "color map 'grass'; core:grass_top is drawn from its own file");
    CHECK(made.messages[2] == "assets/core/minecraft.toml:4:9: warning: Minecraft 26.2's block "
                              "texture 'water_still' is 16 by 32 pixels, not 16 by 16; core:water "
                              "is drawn from its own file");
    CHECK(made.messages[3].starts_with(
        "assets/core/minecraft.toml:5:10: warning: Minecraft 26.2's block texture 'broken' is not "
        "a PNG file stb_image can read ("));
    CHECK(made.messages[3].ends_with("); core:broken is drawn from its own file"));
    CHECK(made.messages[4] == "assets/core/minecraft.toml:6:48: warning: Minecraft 26.2's color "
                              "map 'foliage' is 16 by 16 pixels, not 256 by 256; core:leaves is "
                              "drawn from its own file");
}

TEST_CASE("an entry that does not read back whole is a texture the jar cannot make") {
    const std::string png =
        vx::test::make_png(16, 16, PngColor::gray,
                           samples(1, [](std::size_t, std::size_t, std::size_t) { return 90; }));
    std::vector<vx::test::ZipEntry> entries = {
        {.name = "assets/minecraft/textures/block/stone.png", .data = png}};
    vx::test::Zip zip = vx::test::make_zip(entries);
    // The last byte of the stored entry's data, so its CRC-32 fails.
    const std::size_t data_end = zip.central_headers[0] - 1;
    zip.bytes[data_end] ^= std::byte{0xFF};
    vx::Result<vx::Archive> jar = vx::Archive::from_bytes("26.2.jar", std::move(zip.bytes));
    REQUIRE(jar.has_value());
    const vx::minecraft::Install install{
        .jar = std::move(jar).value(), .path = "26.2.jar", .version = "26.2"};
    const Made made = make(install, "stone = \"stone\"\n");
    CHECK(made.textures.empty());
    CHECK(made.messages ==
          std::vector<std::string>{
              "assets/core/minecraft.toml:1:9: warning: Minecraft 26.2's block texture 'stone' "
              "cannot be read (26.2.jar: 'assets/minecraft/textures/block/stone.png' fails its "
              "CRC-32 check); core:stone is drawn from its own file"});
}
