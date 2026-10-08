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

// What a move did: how far the box went, and what cut it short.
struct Move {
    Vec3d motion;
    bool x_collision = false;        // x was cut short
    bool vertical_collision = false; // y was cut short
    bool z_collision = false;        // z was cut short
    bool on_ground = false;          // y was cut short moving down

    // x or z was cut short.
    [[nodiscard]] constexpr bool horizontal_collision() const noexcept {
        return x_collision || z_collision;
    }
};

// The move that went `motion` when `wanted` was asked for: each axis that
// went otherwise was cut short, and y cut short moving down is the ground.
[[nodiscard]] constexpr Move collided(Vec3d wanted, Vec3d motion) noexcept {
    return {.motion = motion,
            .x_collision = motion.x != wanted.x,
            .vertical_collision = motion.y != wanted.y,
            .z_collision = motion.z != wanted.z,
            .on_ground = motion.y != wanted.y && wanted.y < 0.0};
}

// Moves `box` by `wanted` among `obstacles` one axis at a time, as Minecraft
// does (RFC 0010): y first, then the horizontal axis with the larger wanted
// motion (x on a tie), then the other, each clipped from where the last left
// the box. Since y goes first, a box that walks off an edge still ends that
// move on the ground.
[[nodiscard]] Move clip_move(const Box& box, Vec3d wanted, std::span<const Box> obstacles) noexcept;

} // namespace vx
