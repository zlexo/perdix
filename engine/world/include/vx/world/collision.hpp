#pragma once

#include <vx/core/vec3d.hpp>

#include <cstdint>
#include <span>

namespace vx {

// An axis-aligned box in blocks, `min` below `max` on every axis: a block's
// collision box, or the player's (RFC 0010).
struct Box {
    Vec3d min;
    Vec3d max;

    friend constexpr bool operator==(const Box&, const Box&) = default;
};

// The box moved by `offset`.
[[nodiscard]] constexpr Box moved(const Box& box, Vec3d offset) noexcept {
    return {.min = box.min + offset, .max = box.max + offset};
}

// The region `box` sweeps moving by `motion`: the box grown on each axis in
// the direction of that axis's motion.
[[nodiscard]] Box swept(const Box& box, Vec3d motion) noexcept;

enum class Axis : std::uint8_t { x, y, z };

// Boxes count as overlapping on an axis only by more than this, so a box
// resting against another's face, or a hair inside it from rounding, is not
// held back by it on the other axes.
inline constexpr double collision_tolerance = 1e-7;

// How far `box` can move along `axis` toward `wanted` (in blocks, either
// sign) before it would enter one of `obstacles`. Only an obstacle that
// overlaps the box on the other two axes and lies ahead of it on this one
// counts; one the box is already more than the tolerance inside does not
// hold it back. The result has wanted's sign or is 0, and is never longer
// than wanted.
[[nodiscard]] double clip_axis(const Box& box, Axis axis, double wanted,
                               std::span<const Box> obstacles) noexcept;

} // namespace vx
