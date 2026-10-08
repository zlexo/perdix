#include <vx/core/diagnostic.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using vx::render::BlockImage;
using vx::render::BlockMips;
using vx::render::Texel;

constexpr std::size_t at(std::size_t x, std::size_t y) {
    return (y * 16) + x;
}

// Every texel different: red counts columns, green counts rows.
BlockImage asymmetric() {
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            image[at(x, y)] = {.r = static_cast<std::uint8_t>(x * 16),
                               .g = static_cast<std::uint8_t>(y * 16),
                               .b = 7,
                               .a = 255};
        }
    }
    return image;
}

// Level `level`'s texel at (x, y).
Texel mip(const BlockMips& mips, std::size_t level, std::size_t x, std::size_t y) {
    return mips[vx::render::block_texture_level_start[level] + (y * (std::size_t{16} >> level)) +
                x];
}

// How many texels of a level pass the alpha test (alpha 0.5 or more).
std::size_t passing(const BlockMips& mips, std::size_t level) {
    const std::size_t side = std::size_t{16} >> level;
    std::size_t count = 0;
    for (std::size_t texel = 0; texel < side * side; ++texel) {
        if (mips[vx::render::block_texture_level_start[level] + texel].a >= 128) {
            ++count;
        }
    }
    return count;
}

const std::filesystem::path assets = VX_BLOCK_TEXTURES_DIR;

std::vector<std::byte> file_bytes(const std::string& name) {
    std::ifstream file(assets / "test" / "textures" / "blocks" / name, std::ios::binary);
    REQUIRE(file);
    const std::vector<char> chars{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

std::string path_of(const std::string& name) {
    return (assets / "test" / "textures" / "blocks" / (name + ".png")).generic_string();
}

vx::TextureRef ref(std::string name, std::uint8_t turns, bool opaque, bool cutout) {
    return {.name = std::move(name),
            .quarter_turns = turns,
            .used_opaque = opaque,
            .used_cutout = cutout,
            .first_use = "assets/test/blocks/x.toml:3:7"};
}

std::vector<std::string> formatted(const std::vector<vx::Diagnostic>& diagnostics) {
    std::vector<std::string> lines;
    for (const vx::Diagnostic& diagnostic : diagnostics) {
        lines.push_back(vx::format(diagnostic));
    }
    return lines;
}

// A texture of `name` made from `image`, as the minecraft layer makes them.
vx::MadeTexture made_of(std::string name, const BlockImage& image) {
    vx::MadeTexture made{.name = std::move(name), .rgba = {}};
    std::memcpy(made.rgba.data(), image.data(), sizeof(image));
    return made;
}

} // namespace

TEST_CASE("a known PNG decodes to known bytes") {
    // pattern.png: red counts columns by 16, green rows by 16, blue is
    // x * y; alpha is 0 in the top-left 4 by 4, 100 on the diagonal, 255
    // elsewhere.
    const vx::Result<BlockImage> image =
        vx::render::decode_block_texture(file_bytes("pattern.png"));
    REQUIRE(image.has_value());
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            INFO(x << ", " << y);
            const std::uint8_t alpha = x < 4 && y < 4 ? 0 : (x == y ? 100 : 255);
            CHECK(image.value()[at(x, y)] == Texel{.r = static_cast<std::uint8_t>(x * 16),
                                                   .g = static_cast<std::uint8_t>(y * 16),
                                                   .b = static_cast<std::uint8_t>((x * y) % 256),
                                                   .a = alpha});
        }
    }
}

TEST_CASE("a PNG without alpha decodes as fully opaque") {
    const vx::Result<BlockImage> image = vx::render::decode_block_texture(file_bytes("rgb.png"));
    REQUIRE(image.has_value());
    CHECK(image.value()[at(3, 5)] == Texel{.r = 48, .g = 80, .b = 9, .a = 255});
}

TEST_CASE("a file 17 by 16, or not a PNG at all, is refused with why") {
    const vx::Result<BlockImage> wide = vx::render::decode_block_texture(file_bytes("wide.png"));
    REQUIRE_FALSE(wide.has_value());
    CHECK(wide.error().message == "17 by 16 pixels; block textures are 16 by 16");
    const vx::Result<BlockImage> text = vx::render::decode_block_texture(file_bytes("not_png.png"));
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().message.starts_with("not a PNG file stb_image can read: "));
}

TEST_CASE("the texture array holds every layer of level 0, then every layer of each level") {
    const std::vector<vx::TextureRef> textures = {{},
                                                  ref("test:pattern", 0, true, false),
                                                  ref("test:pattern", 1, true, false),
                                                  ref("test:solid", 0, true, false)};
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(textures, assets, diagnostics);
    REQUIRE(bytes.size() == 4 * vx::render::block_texture_layer_bytes);

    // Each layer, worked out step by step: decoded, turned, filled, mipmapped.
    std::vector<BlockMips> expected = {vx::render::mipmaps(vx::render::checker())};
    for (const vx::TextureRef& texture : std::span(textures).subspan(1)) {
        BlockImage image = vx::render::turned(
            vx::render::decode_block_texture(file_bytes(texture.name.substr(5) + ".png")).value(),
            texture.quarter_turns);
        vx::render::fill_transparent(image);
        expected.push_back(vx::render::mipmaps(image));
    }
    // Level by level: level 0 of all four layers (256 texels each), then
    // level 1 of all four (64 each), and so on.
    std::size_t offset = 0;
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t texels = (std::size_t{16} >> level) * (std::size_t{16} >> level);
        for (std::size_t layer = 0; layer < 4; ++layer) {
            INFO("level " << level << ", layer " << layer);
            CHECK(std::memcmp(&bytes[offset],
                              &expected[layer][vx::render::block_texture_level_start[level]],
                              4 * texels) == 0);
            offset += 4 * texels;
        }
    }
    CHECK(offset == bytes.size());
    // pattern.png's 4 by 4 corner has alpha 0 and the rest of its diagonal
    // 100: 16 + 12 texels, and an opaque block uses it.
    CHECK(formatted(diagnostics) ==
          std::vector<std::string>{
              path_of("pattern") +
              ": warning: 28 texels are transparent (alpha below 0.5), and opaque blocks draw "
              "them solid; cutout blocks would show the holes"});
}

TEST_CASE("a made texture takes its file's place, turned, filled and mipmapped as a file is") {
    // test:gone has no file and needs none once made; test:pattern's file
    // would warn about its holes on an opaque block, and is not read.
    BlockImage holed = asymmetric();
    holed[at(3, 5)].a = 0;
    BlockImage other = asymmetric();
    other[at(0, 0)].r = 1;
    const std::vector<vx::MadeTexture> made = {made_of("test:gone", holed),
                                               made_of("test:pattern", other)};
    const std::vector<vx::TextureRef> textures = {{},
                                                  ref("test:gone", 0, true, false),
                                                  ref("test:gone", 1, true, false),
                                                  ref("test:pattern", 0, true, false)};
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(textures, assets, made, diagnostics);
    CHECK(formatted(diagnostics).empty());
    REQUIRE(bytes.size() == 4 * vx::render::block_texture_layer_bytes);

    std::vector<BlockMips> expected = {vx::render::mipmaps(vx::render::checker())};
    for (const auto& [image, turns] :
         {std::pair{holed, 0}, std::pair{holed, 1}, std::pair{other, 0}}) {
        BlockImage layer = vx::render::turned(image, turns);
        vx::render::fill_transparent(layer);
        expected.push_back(vx::render::mipmaps(layer));
    }
    std::size_t offset = 0;
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t texels = (std::size_t{16} >> level) * (std::size_t{16} >> level);
        for (std::size_t layer = 0; layer < 4; ++layer) {
            INFO("level " << level << ", layer " << layer);
            CHECK(std::memcmp(&bytes[offset],
                              &expected[layer][vx::render::block_texture_level_start[level]],
                              4 * texels) == 0);
            offset += 4 * texels;
        }
    }
}

TEST_CASE("made textures no layer uses are passed over") {
    const std::vector<vx::TextureRef> textures = {{}, ref("test:solid", 0, true, false)};
    const std::vector<vx::MadeTexture> made = {made_of("test:unused", asymmetric()),
                                               made_of("other:solid", asymmetric())};
    std::vector<vx::Diagnostic> plain;
    std::vector<vx::Diagnostic> with_made;
    const std::vector<std::byte> from_files =
        vx::render::build_block_textures(textures, assets, plain);
    CHECK_FALSE(from_files.empty());
    CHECK(vx::render::build_block_textures(textures, assets, made, with_made) == from_files);
    CHECK(with_made.empty());
}

TEST_CASE("of two made textures with one name, the first stands in") {
    const std::vector<vx::TextureRef> textures = {{}, ref("test:gone", 0, true, false)};
    const BlockImage first = asymmetric();
    BlockImage second = asymmetric();
    second[at(0, 0)].r = 1;
    const std::vector<vx::MadeTexture> made = {made_of("test:gone", first),
                                               made_of("test:gone", second)};
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(textures, assets, made, diagnostics);
    REQUIRE(bytes.size() == 2 * vx::render::block_texture_layer_bytes);
    // Level 0 of layer 1 comes right after the checker's.
    CHECK(std::memcmp(&bytes[sizeof(first)], first.data(), sizeof(first)) == 0);
}

TEST_CASE("a missing or bad texture file is an error, every one reported, and nothing comes back") {
    const std::vector<vx::TextureRef> textures = {{},
                                                  ref("test:wide", 0, true, false),
                                                  ref("test:gone", 0, true, false),
                                                  ref("test:not_png", 2, true, false),
                                                  ref("test:solid", 0, true, false)};
    std::vector<vx::Diagnostic> diagnostics;
    CHECK(vx::render::build_block_textures(textures, assets, diagnostics).empty());
    const std::vector<std::string> lines = formatted(diagnostics);
    REQUIRE(lines.size() == 3);
    // In name order.
    CHECK(lines[0] == path_of("gone") + ": error: no such file, for the texture test:gone (used at "
                                        "assets/test/blocks/x.toml:3:7)");
    CHECK(
        lines[1].starts_with(path_of("not_png") + ": error: not a PNG file stb_image can read: "));
    CHECK(lines[2] == path_of("wide") + ": error: 17 by 16 pixels; block textures are 16 by 16");
}

TEST_CASE("each texture is read once, however many turns of it there are") {
    // Three turns of a missing file: one error.
    const std::vector<vx::TextureRef> textures = {{},
                                                  ref("test:gone", 0, true, false),
                                                  ref("test:gone", 1, true, false),
                                                  ref("test:gone", 3, true, false)};
    std::vector<vx::Diagnostic> diagnostics;
    CHECK(vx::render::build_block_textures(textures, assets, diagnostics).empty());
    CHECK(diagnostics.size() == 1);
}

TEST_CASE("warnings: holes an opaque block hides, and a cutout block with no holes") {
    // leaves.png, every other texel transparent: opaque blocks use one turn,
    // cutout blocks another, so it warns once, whichever comes first.
    // solid.png only cutout blocks use. Warnings stop nothing.
    for (const bool opaque_first : {false, true}) {
        INFO("opaque turn first: " << opaque_first);
        std::vector<vx::Diagnostic> diagnostics;
        const std::vector<vx::TextureRef> textures = {
            {},
            ref("test:leaves", 0, opaque_first, !opaque_first),
            ref("test:leaves", 1, !opaque_first, opaque_first),
            ref("test:solid", 0, false, true)};
        CHECK_FALSE(vx::render::build_block_textures(textures, assets, diagnostics).empty());
        CHECK(formatted(diagnostics) ==
              std::vector<std::string>{
                  path_of("leaves") +
                      ": warning: 128 texels are transparent (alpha below 0.5), and opaque "
                      "blocks draw them solid; cutout blocks would show the holes",
                  path_of("solid") +
                      ": warning: only cutout blocks use this texture and no texel is "
                      "transparent: an opaque block would draw it faster"});
    }

    // Used by cutout blocks alone, leaves.png is what cutout is for.
    std::vector<vx::Diagnostic> diagnostics;
    CHECK_FALSE(vx::render::build_block_textures(
                    std::vector<vx::TextureRef>{{}, ref("test:leaves", 0, false, true)}, assets,
                    diagnostics)
                    .empty());
    CHECK(diagnostics.empty());
}

TEST_CASE("a pack of no textures is the checker alone") {
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(std::vector<vx::TextureRef>{{}}, assets, diagnostics);
    REQUIRE(bytes.size() == vx::render::block_texture_layer_bytes);
    const BlockMips mips = vx::render::mipmaps(vx::render::checker());
    CHECK(std::memcmp(bytes.data(), mips.data(), bytes.size()) == 0);
    CHECK(diagnostics.empty());
}

TEST_CASE("an empty texture list, without even the checker, gives no bytes") {
    std::vector<vx::Diagnostic> diagnostics;
    CHECK(vx::render::build_block_textures({}, assets, diagnostics).empty());
    CHECK(diagnostics.empty());
}

TEST_CASE("a quarter turn moves the image's top edge to its right") {
    const BlockImage image = asymmetric();
    const BlockImage once = vx::render::turned(image, 1);
    // The left column, from the bottom up, becomes the top row.
    CHECK(once[at(0, 0)] == image[at(0, 15)]);
    CHECK(once[at(15, 0)] == image[at(0, 0)]);
    CHECK(once[at(15, 15)] == image[at(15, 0)]);
    CHECK(once[at(3, 5)] == image[at(5, 12)]);
    // Twice is a half turn, and four times is no turn at all.
    const BlockImage twice = vx::render::turned(image, 2);
    CHECK(twice[at(0, 0)] == image[at(15, 15)]);
    CHECK(twice[at(4, 9)] == image[at(11, 6)]);
    CHECK(vx::render::turned(once, 1) == twice);
    CHECK(vx::render::turned(image, 3) == vx::render::turned(twice, 1));
    CHECK(vx::render::turned(image, 4) == image);
    CHECK(vx::render::turned(image, 0) == image);
}

TEST_CASE("fully transparent texels take the color of the nearest visible ones, wrapping") {
    // Red in column 2, blue in column 10, the rest fully transparent.
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        image[at(2, y)] = {.r = 255, .g = 0, .b = 0, .a = 255};
        image[at(10, y)] = {.r = 0, .g = 0, .b = 255, .a = 255};
    }
    vx::render::fill_transparent(image);
    for (std::size_t y = 0; y < 16; ++y) {
        INFO("row " << y);
        // Nearer one color: that color, and still fully transparent.
        CHECK(image[at(4, y)] == Texel{.r = 255, .g = 0, .b = 0, .a = 0});
        CHECK(image[at(8, y)] == Texel{.r = 0, .g = 0, .b = 255, .a = 0});
        // Column 0 is two from red; through the edge, column 15 is three.
        CHECK(image[at(0, y)] == Texel{.r = 255, .g = 0, .b = 0, .a = 0});
        CHECK(image[at(15, y)] == Texel{.r = 255, .g = 0, .b = 0, .a = 0});
        // Halfway: column 6 is four from each. So is column 14, but only by
        // wrapping round to red; without wrapping it would be blue.
        // Half red and half blue in linear light is sRGB 188.
        CHECK(image[at(6, y)] == Texel{.r = 188, .g = 0, .b = 188, .a = 0});
        CHECK(image[at(14, y)] == Texel{.r = 188, .g = 0, .b = 188, .a = 0});
        // The visible texels are untouched.
        CHECK(image[at(2, y)] == Texel{.r = 255, .g = 0, .b = 0, .a = 255});
    }
}

TEST_CASE("a texture with nothing visible is left as it is") {
    BlockImage image{};
    image[at(3, 3)] = {.r = 9, .g = 9, .b = 9, .a = 0};
    const BlockImage before = image;
    vx::render::fill_transparent(image);
    CHECK(image == before);
}

TEST_CASE("level 1 of a one-texel black and white checker is sRGB 188, not 128") {
    // Averaging the bytes would give 128, 22% of the light; in linear light
    // the average is 50%, which sRGB stores as 188.
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            const std::uint8_t value = (x + y) % 2 == 0 ? 255 : 0;
            image[at(x, y)] = {.r = value, .g = value, .b = value, .a = 255};
        }
    }
    const BlockMips mips = vx::render::mipmaps(image);
    CHECK(std::ranges::equal(std::span(mips).first(256), image));
    for (std::size_t level = 1; level < 5; ++level) {
        INFO("level " << level);
        CHECK(mip(mips, level, 0, 0) == Texel{.r = 188, .g = 188, .b = 188, .a = 255});
        CHECK(mip(mips, level, (std::size_t{16} >> level) - 1, 0) ==
              Texel{.r = 188, .g = 188, .b = 188, .a = 255});
    }
}

TEST_CASE("a texture of one color keeps it exactly at every level, for every 8-bit value") {
    // Averaging in linear light and storing back as sRGB must give back the
    // byte it started from.
    for (int value = 0; value < 256; ++value) {
        const Texel color{.r = static_cast<std::uint8_t>(value),
                          .g = static_cast<std::uint8_t>(255 - value),
                          .b = static_cast<std::uint8_t>(value / 2),
                          .a = 255};
        BlockImage image{};
        image.fill(color);
        const BlockMips mips = vx::render::mipmaps(image);
        INFO("value " << value);
        CHECK(std::ranges::all_of(mips, [&](const Texel& texel) { return texel == color; }));
    }
}

TEST_CASE("mipmaps weight color by alpha, so transparent texels add no color") {
    // One opaque red texel in each 2 by 2 block, three fully transparent
    // green ones.
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            image[at(x, y)] = x % 2 == 0 && y % 2 == 0 ? Texel{.r = 255, .g = 0, .b = 0, .a = 255}
                                                       : Texel{.r = 0, .g = 255, .b = 0, .a = 0};
        }
    }
    const BlockMips mips = vx::render::mipmaps(image);
    // Red, at a quarter alpha before the coverage step, which then scales
    // the alpha: a quarter of level 0 passes the alpha test, so a quarter
    // of every level does, up to one texel.
    for (std::size_t level = 1; level < 5; ++level) {
        INFO("level " << level);
        const Texel first = mip(mips, level, 0, 0);
        CHECK(first.r == 255);
        CHECK(first.g == 0);
    }
}

TEST_CASE("where every texel is fully transparent, the filled colors stand in") {
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            const std::uint8_t value = (x + y) % 2 == 0 ? 255 : 0;
            image[at(x, y)] = {.r = value, .g = value, .b = value, .a = 0};
        }
    }
    const BlockMips mips = vx::render::mipmaps(image);
    // No weights at all: the plain average in linear light, alpha still 0.
    CHECK(mip(mips, 1, 0, 0) == Texel{.r = 188, .g = 188, .b = 188, .a = 0});
    CHECK(mip(mips, 4, 0, 0) == Texel{.r = 188, .g = 188, .b = 188, .a = 0});
}

TEST_CASE("an alpha-tested texture keeps level 0's coverage to within one texel at every level") {
    // Leaves: seeded random holes at several densities, fully transparent or
    // fully opaque, and some with soft edges between.
    std::mt19937 generator(11);
    for (const double density : {0.15, 0.3, 0.5, 0.7, 0.85}) {
        for (int round = 0; round < 8; ++round) {
            std::bernoulli_distribution solid(density);
            std::uniform_int_distribution<int> soft(0, 255);
            BlockImage image{};
            for (Texel& texel : image) {
                const auto alpha = static_cast<std::uint8_t>(
                    solid(generator) ? 255 : (round % 2 == 0 ? 0 : soft(generator) / 2));
                texel = {.r = 40, .g = 120, .b = 30, .a = alpha};
            }
            vx::render::fill_transparent(image);
            const BlockMips mips = vx::render::mipmaps(image);
            const double coverage = static_cast<double>(passing(mips, 0)) / 256.0;
            for (std::size_t level = 1; level < 5; ++level) {
                const std::size_t side = std::size_t{16} >> level;
                const double expected = coverage * static_cast<double>(side * side);
                INFO("density " << density << ", round " << round << ", level " << level
                                << ": expected " << expected << ", got " << passing(mips, level));
                CHECK(std::abs(static_cast<double>(passing(mips, level)) - expected) <= 1.0);
            }
        }
    }
}

TEST_CASE("texels of equal alpha at the test take turns, in an ordered-dither pattern") {
    // Every other row opaque: every texel of level 1 has alpha 0.5, half of
    // level 0 passes, so half of level 1 must, and one scale cannot split
    // them. The Bayer order picks a checkerboard, not the top half.
    BlockImage image{};
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            image[at(x, y)] = {
                .r = 40, .g = 120, .b = 30, .a = y % 2 == 0 ? std::uint8_t{255} : std::uint8_t{0}};
        }
    }
    const BlockMips mips = vx::render::mipmaps(image);
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            INFO("level 1 at " << x << ", " << y);
            CHECK((mip(mips, 1, x, y).a >= 128) == ((x + y) % 2 == 0));
        }
    }
}

TEST_CASE("a texture the alpha test cannot discard keeps its alpha") {
    // Every texel at 0.5 or more: nothing to keep, so nothing is scaled.
    BlockImage image = asymmetric();
    for (std::size_t texel = 0; texel < image.size(); texel += 3) {
        image[texel].a = 128;
    }
    const BlockMips mips = vx::render::mipmaps(image);
    // The first 2 by 2 block holds one 128 and three 255s.
    CHECK(mip(mips, 1, 0, 0).a == static_cast<std::uint8_t>(std::lround((128 + 255 * 3) / 4.0)));
}

TEST_CASE("the checker is magenta and black squares of 8 texels, fully opaque") {
    const BlockImage checker = vx::render::checker();
    const Texel magenta{.r = 255, .g = 0, .b = 255, .a = 255};
    const Texel black{.r = 0, .g = 0, .b = 0, .a = 255};
    CHECK(checker[at(0, 0)] == magenta);
    CHECK(checker[at(7, 7)] == magenta);
    CHECK(checker[at(8, 0)] == black);
    CHECK(checker[at(0, 8)] == black);
    CHECK(checker[at(15, 15)] == magenta);
}

TEST_CASE("a texture file that cannot be read is an error that says so, not an empty file") {
    // A folder where the texture should be: it opens, then fails on the first
    // read, which the old reader took for the end of an empty file.
    std::random_device random;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("vx-unreadable-" + std::to_string(random()) + "-" + std::to_string(random()));
    std::filesystem::create_directories(root / "test" / "textures" / "blocks" / "stone.png");
    const std::vector<vx::TextureRef> textures = {{}, ref("test:stone", 0, true, false)};
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(textures, root, diagnostics);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(bytes.empty());
    CHECK(formatted(diagnostics) ==
          std::vector<std::string>{
              (root / "test" / "textures" / "blocks" / "stone.png").generic_string() +
              ": error: the file cannot be read, for the texture test:stone (used at "
              "assets/test/blocks/x.toml:3:7)"});
}
