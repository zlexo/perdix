#include <vx/core/vec3d.hpp>
#include <vx/mesh/name_tag.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>

// Every number below is RFC 0012's or the game's, not the code's.

namespace {

using vx::TagVertex;
using vx::Vec3d;

// Advances shaped like the game's font: 6 for most characters, 2 for i, 3
// for l, 4 for the space.
std::array<std::uint8_t, 128> game_like_advances() {
    std::array<std::uint8_t, 128> advances{};
    advances.fill(6);
    advances.at('i') = 2;
    advances.at('l') = 3;
    advances.at(' ') = 4;
    return advances;
}

const std::array<std::uint8_t, 128> advances = game_like_advances();

struct Laid {
    std::array<TagVertex, vx::max_tag_vertices> vertices{};
    std::size_t count = 0;
};

Laid lay(const vx::Tag& tag, float yaw = 0.0F, float pitch = 0.0F, vx::BlockPos origin = {}) {
    Laid laid;
    laid.count = vx::layout_tag(tag, advances, yaw, pitch, origin, laid.vertices);
    return laid;
}

Vec3d position(const TagVertex& vertex) {
    return {.x = static_cast<double>(vertex.position[0]),
            .y = static_cast<double>(vertex.position[1]),
            .z = static_cast<double>(vertex.position[2])};
}

double dot(Vec3d a, Vec3d b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3d cross(Vec3d a, Vec3d b) {
    return {.x = (a.y * b.z) - (a.z * b.y),
            .y = (a.z * b.x) - (a.x * b.z),
            .z = (a.x * b.y) - (a.y * b.x)};
}

// The extent of quad `quad` (0 the box) along x and y at yaw 0, in pixels
// from the anchor: left, right, top, bottom, with rows counting down.
std::array<double, 4> extent(const Laid& laid, std::size_t quad, const Vec3d& anchor) {
    std::array<double, 4> edges = {1e9, -1e9, 1e9, -1e9};
    for (std::size_t at = quad * 6; at < (quad + 1) * 6; ++at) {
        const Vec3d offset = position(laid.vertices.at(at)) - anchor;
        edges[0] = std::min(edges[0], offset.x / 0.025);
        edges[1] = std::max(edges[1], offset.x / 0.025);
        edges[2] = std::min(edges[2], -offset.y / 0.025);
        edges[3] = std::max(edges[3], -offset.y / 0.025);
    }
    return edges;
}

void check_near(double actual, double expected, double tolerance = 1e-4) {
    INFO(actual, " against ", expected);
    CHECK(std::abs(actual - expected) <= tolerance);
}

} // namespace

TEST_CASE("a tag floats 2.3 above a standing player's feet and 1.875 above a crouching one's") {
    const Vec3d feet{.x = 3.5, .y = 64.0, .z = -2.0};
    check_near(vx::tag_anchor(feet, false).y - feet.y, 2.3);
    check_near(vx::tag_anchor(feet, true).y - feet.y, 1.875);
    CHECK(vx::tag_anchor(feet, false).x == feet.x);
    CHECK(vx::tag_anchor(feet, true).z == feet.z);
}

TEST_CASE("Perdix is 32 pixels wide and its box 33 by 10 from 1 left of its first letter") {
    CHECK(vx::text_width("Perdix", advances) == 32);
    // Laid out relative to the block the anchor is in, as the renderer
    // does: floats are exact enough there.
    const Vec3d anchor{.x = 0.5, .y = 0.3, .z = 0.5};
    const Laid laid = lay({.anchor = anchor + Vec3d{.y = 66.0}, .text = "Perdix"}, 0.0F, 0.0F,
                          {.x = 0, .y = 66, .z = 0});
    REQUIRE(laid.count == 6 * (1 + 6));
    // The box: from -17 to 16 across, -1 to 9 down, the pen starting at -16.
    const std::array<double, 4> box = extent(laid, 0, anchor);
    check_near(box[0], -17.0);
    check_near(box[1], 16.0);
    check_near(box[2], -1.0);
    check_near(box[3], 9.0);
    // The first letter's cell from the pen, the line's top down to 8; the
    // fifth, i, at -16 plus four advances of 6.
    const std::array<double, 4> first = extent(laid, 1, anchor);
    check_near(first[0], -16.0);
    check_near(first[1], -8.0);
    check_near(first[2], 0.0);
    check_near(first[3], 8.0);
    check_near(extent(laid, 5, anchor)[0], 8.0);
    // P is 80: column 0, row 5 of the atlas's 16 by 16 cells of 8.
    float u_low = 1.0F;
    float v_low = 1.0F;
    float u_high = 0.0F;
    float v_high = 0.0F;
    for (std::size_t at = 6; at < 12; ++at) {
        u_low = std::min(u_low, laid.vertices.at(at).texel[0]);
        u_high = std::max(u_high, laid.vertices.at(at).texel[0]);
        v_low = std::min(v_low, laid.vertices.at(at).texel[1]);
        v_high = std::max(v_high, laid.vertices.at(at).texel[1]);
        CHECK(laid.vertices.at(at).ink == 1.0F);
    }
    CHECK(u_low == 0.0F);
    CHECK(u_high == 8.0F / 128.0F);
    CHECK(v_low == 40.0F / 128.0F);
    CHECK(v_high == 48.0F / 128.0F);
    for (std::size_t at = 0; at < 6; ++at) {
        CHECK(laid.vertices.at(at).ink == 0.0F);
    }
}

TEST_CASE("an odd width starts on half a pixel") {
    // Z, l, e, x: 6 + 3 + 6 + 6 = 21, so the pen starts at -10.5.
    const Vec3d anchor{.x = 0.0, .y = 0.0, .z = 0.0};
    const Laid laid = lay({.anchor = anchor, .text = "Zlex"});
    check_near(extent(laid, 0, anchor)[0], -11.5);
    check_near(extent(laid, 0, anchor)[1], 10.5);
    check_near(extent(laid, 1, anchor)[0], -10.5);
}

TEST_CASE("a space advances and draws nothing and a long text is cut at 32 characters") {
    const Vec3d anchor{};
    const Laid spaced = lay({.anchor = anchor, .text = "A B"});
    REQUIRE(spaced.count == 6 * 3);
    // 6 + 4 + 6 = 16: B's cell starts at -8 + 6 + 4.
    check_near(extent(spaced, 2, anchor)[0], 2.0);
    const std::string long_text(40, 'x');
    CHECK(lay({.anchor = anchor, .text = long_text}).count == 6 * (1 + 32));
    CHECK(vx::text_width(long_text, advances) == 32 * 6);
}

TEST_CASE("a character past ASCII draws the missing glyph at 127") {
    const Laid laid = lay({.anchor = {}, .text = "\xC3\xA9"}); // two bytes, both past 127
    REQUIRE(laid.count == 6 * 3);
    for (std::size_t at = 6; at < 18; ++at) {
        // 127: column 15, row 7.
        CHECK(laid.vertices.at(at).texel[0] >= 120.0F / 128.0F);
        CHECK(laid.vertices.at(at).texel[1] >= 56.0F / 128.0F);
        CHECK(laid.vertices.at(at).texel[1] <= 64.0F / 128.0F);
    }
}

TEST_CASE("a tag faces the camera at any yaw and pitch and its box sits just behind its glyphs") {
    std::mt19937 random(7);
    std::uniform_real_distribution<float> yaw(-3.2F, 3.2F);
    std::uniform_real_distribution<float> pitch(-1.5F, 1.5F);
    for (int view = 0; view < 40; ++view) {
        const float camera_yaw = yaw(random);
        const float camera_pitch = pitch(random);
        INFO("yaw ", camera_yaw, " pitch ", camera_pitch);
        const auto turn = static_cast<double>(camera_yaw);
        const auto tilt = static_cast<double>(camera_pitch);
        const Vec3d forward{.x = -std::sin(turn) * std::cos(tilt),
                            .y = std::sin(tilt),
                            .z = -std::cos(turn) * std::cos(tilt)};
        const Vec3d anchor{.x = 0.25, .y = 0.5, .z = 0.75};
        const Laid laid = lay({.anchor = anchor, .text = "Perdix"}, camera_yaw, camera_pitch);
        for (std::size_t triangle = 0; triangle < laid.count / 3; ++triangle) {
            const Vec3d a = position(laid.vertices.at(3 * triangle));
            const Vec3d b = position(laid.vertices.at((3 * triangle) + 1));
            const Vec3d c = position(laid.vertices.at((3 * triangle) + 2));
            const Vec3d normal = cross(b - a, c - a);
            // Counter-clockwise seen from the camera: toward it, square on.
            CHECK(dot(normal, forward) < -0.999 * std::sqrt(dot(normal, normal)));
        }
        // The glyphs lie in the anchor's plane, the box 0.01 pixel behind.
        for (std::size_t at = 0; at < laid.count; ++at) {
            const double depth = dot(position(laid.vertices.at(at)) - anchor, forward);
            check_near(depth, at < 6 ? 0.01 * 0.025 : 0.0, 1e-6);
        }
    }
}

TEST_CASE("a tag's positions are relative to the block given") {
    const Vec3d anchor{.x = 100.5, .y = 66.3, .z = -20.5};
    const Laid laid =
        lay({.anchor = anchor, .text = "Perdix"}, 0.0F, 0.0F, {.x = 100, .y = 66, .z = -21});
    // The first letter's top left: 16 pixels left of the anchor's 0.5.
    const Vec3d corner = position(laid.vertices.at(6));
    check_near(corner.x, 0.5 - (16 * 0.025));
    check_near(corner.y, 0.3);
    check_near(corner.z, 0.5);
}
