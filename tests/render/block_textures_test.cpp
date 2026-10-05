#include <vx/render/block_textures.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

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

} // namespace

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
