#include <vx/mesh/name_tag.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vx {
namespace {

// 26.2's name tag (RFC 0012): 0.025 blocks a pixel of the font; the box one
// pixel out on the left, from one above the line to nine below, and a
// hundredth of a pixel behind the glyphs; a glyph's 8 by 8 cell from the
// line's top, since the default font's ascent is 7.
constexpr double tag_scale = 0.025;
constexpr double box_behind = 0.01;
constexpr int cell = 8;
constexpr int cells_across = 16;
constexpr int atlas = cell * cells_across;

// What a character draws: itself in the ASCII grid, or the missing glyph,
// which the font keeps at 127.
std::size_t glyph_of(char character) noexcept {
    const auto code = static_cast<unsigned char>(character);
    return code < 128 ? code : 127;
}

// The camera's right and up, and its forward, as RFC 0001's camera turns:
// by its yaw around +y, then its pitch around its own x axis.
struct Billboard {
    Vec3d right;
    Vec3d up;
    Vec3d forward;
};

Billboard billboard(float yaw, float pitch) noexcept {
    const double sin_yaw = std::sin(static_cast<double>(yaw));
    const double cos_yaw = std::cos(static_cast<double>(yaw));
    const double sin_pitch = std::sin(static_cast<double>(pitch));
    const double cos_pitch = std::cos(static_cast<double>(pitch));
    return {.right = {.x = cos_yaw, .y = 0.0, .z = -sin_yaw},
            .up = {.x = sin_pitch * sin_yaw, .y = cos_pitch, .z = sin_pitch * cos_yaw},
            .forward = {.x = -cos_pitch * sin_yaw, .y = sin_pitch, .z = -cos_pitch * cos_yaw}};
}

std::array<float, 3> floats(Vec3d v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// Writes one quad from pixel (left, top) to (right, bottom) of the tag, its
// rows counting down, at `depth` along the camera's forward: two triangles
// counter-clockwise seen from the camera.
struct QuadWriter {
    Vec3d anchor; // relative to the tag's block
    Billboard axes;
    std::span<TagVertex, max_tag_vertices> out;
    std::size_t next = 0;

    void quad(double left, double top, double right, double bottom, double depth, float ink,
              std::array<float, 4> texels) noexcept {
        const auto corner = [&](double x, double y) {
            return anchor + (axes.right * (x * tag_scale)) + (axes.up * (-y * tag_scale)) +
                   (axes.forward * (depth * tag_scale));
        };
        // Top left, bottom left, bottom right, then bottom right, top right,
        // top left: down the left edge first, as the camera sees it.
        const std::array<Vec3d, 4> corners = {corner(left, top), corner(left, bottom),
                                              corner(right, bottom), corner(right, top)};
        const std::array<std::array<float, 2>, 4> uv = {{{texels[0], texels[1]},
                                                         {texels[0], texels[3]},
                                                         {texels[2], texels[3]},
                                                         {texels[2], texels[1]}}};
        for (const std::size_t at : {0U, 1U, 2U, 2U, 3U, 0U}) {
            out[next++] = {.position = floats(corners.at(at)),
                           .ink = ink,
                           .texel = uv.at(at),
                           .padding = {0.0F, 0.0F}};
        }
    }
};

} // namespace

Vec3d tag_anchor(Vec3d feet, bool crouching) noexcept {
    // The box's height plus 0.5: 1.8 standing, 1.5 crouching, where the
    // whole body also sits 1/8 block lower.
    const double above = crouching ? 1.5 + 0.5 - 0.125 : 1.8 + 0.5;
    return feet + Vec3d{.x = 0.0, .y = above, .z = 0.0};
}

int text_width(std::string_view text, std::span<const std::uint8_t, 128> advances) noexcept {
    int width = 0;
    for (const char character : text.substr(0, max_tag_characters)) {
        width += advances[glyph_of(character)];
    }
    return width;
}

std::size_t layout_tag(const Tag& tag, std::span<const std::uint8_t, 128> advances, float yaw,
                       float pitch, BlockPos origin,
                       std::span<TagVertex, max_tag_vertices> out) noexcept {
    const std::string_view text = tag.text.substr(0, max_tag_characters);
    const int width = text_width(text, advances);
    QuadWriter writer{.anchor = tag.anchor - Vec3d{.x = static_cast<double>(origin.x),
                                                   .y = static_cast<double>(origin.y),
                                                   .z = static_cast<double>(origin.z)},
                      .axes = billboard(yaw, pitch),
                      .out = out};
    // The pen starts half the width to the left, a float, so an odd width
    // starts on half a pixel.
    double pen = -static_cast<double>(width) / 2.0;
    writer.quad(pen - 1.0, -1.0, pen + width, 9.0, box_behind, 0.0F, {0.0F, 0.0F, 0.0F, 0.0F});
    constexpr float texel = 1.0F / atlas;
    for (const char character : text) {
        const std::size_t glyph = glyph_of(character);
        if (character != ' ') {
            const std::size_t cell_column = glyph % cells_across;
            const std::size_t cell_row = glyph / cells_across;
            const auto column = static_cast<float>(cell_column * cell);
            const auto row = static_cast<float>(cell_row * cell);
            writer.quad(
                pen, 0.0, pen + cell, cell, 0.0, 1.0F,
                {column * texel, row * texel, (column + cell) * texel, (row + cell) * texel});
        }
        pen += advances[glyph];
    }
    return writer.next;
}

} // namespace vx
