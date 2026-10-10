#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vx {

// One line of text to float over a body this frame (RFC 0012).
struct Tag {
    Vec3d anchor{}; // the middle of the text's top edge
    // Printable ASCII; anything else draws the missing glyph. The {} lets a
    // designated initializer leave it out on Linux Clang too.
    std::string_view text{}; // NOLINT(readability-redundant-member-init)
    bool discreet = false;   // a sneaking player's: drawn once, not seen through walls
};

// Where a player's tag goes: 0.5 above the top of its box, so 2.3 above the
// feet standing, and 1.875 crouching, where the box is 1.5 tall and the body
// sits 1/8 block lower.
[[nodiscard]] Vec3d tag_anchor(Vec3d feet, bool crouching) noexcept;

// The most characters of a tag drawn: the rest of a longer text is left out.
inline constexpr std::size_t max_tag_characters = 32;
// A tag's quads: its box, then one a character drawn.
inline constexpr std::size_t max_tag_vertices = 6 * (1 + max_tag_characters);

// One corner of a tag's quads, as the text pipeline pulls it: two float4s.
struct TagVertex {
    std::array<float, 3> position; // blocks, relative to the tag's block
    float ink;                     // 1 on a glyph, sampling the font; 0 on the box, a solid color
    std::array<float, 2> texel;    // on the font's atlas, 0 to 1
    std::array<float, 2> padding;
};

// How wide `text` is in the font's pixels: its advances added up, as the
// game measures a line.
[[nodiscard]] int text_width(std::string_view text,
                             std::span<const std::uint8_t, 128> advances) noexcept;

// Lays out `tag` as the game lays out a name tag: facing a camera that looks
// along `yaw` and `pitch` (RFC 0001), never rolled, 0.025 blocks to the
// font's pixel, its pixel rows counting down from the anchor, and centered
// by half its width, a fraction for an odd width. First its box, from 1
// pixel left of the first character to the end of the last one's advance and
// from 1 pixel above the line to 9 below it, a hair behind the glyphs; then
// each character's 8 by 8 cell of the atlas, from the line's top. Positions
// are relative to `origin`; the quads wind counter-clockwise seen from the
// camera. A space draws no quad. Returns how many vertices it wrote.
[[nodiscard]] std::size_t layout_tag(const Tag& tag, std::span<const std::uint8_t, 128> advances,
                                     float yaw, float pitch, BlockPos origin,
                                     std::span<TagVertex, max_tag_vertices> out) noexcept;

} // namespace vx
