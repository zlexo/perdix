#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <cstdint>
#include <span>
#include <vector>

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

// clip_move with step-up (RFC 0010). When the plain move hit something
// horizontally, and the box started it on the ground or landed during it,
// the box may move onto a surface instead: each top or bottom face of
// `obstacles` from 0 to `step_height` above the box's bottom (where it
// landed, if it did) is tried, lowest first, skipping the height the plain
// move reached, and the first move at that height that goes further
// horizontally than the plain one wins, ending on that surface. The floor at
// 0 counts, so a jump into a doorway walks in under the lintel. A full block
// 1 above is never stepped onto, and a step height of 0 or less steps
// nothing. What was cut short is told against `wanted`, as for the plain
// move.
[[nodiscard]] Move clip_move_stepping(const Box& box, Vec3d wanted, bool started_on_ground,
                                      double step_height, std::span<const Box> obstacles) noexcept;

// Whether `box` overlaps one of `obstacles` by more than the tolerance on
// every axis: whether it would be inside a block there.
[[nodiscard]] bool overlaps_any(const Box& box, std::span<const Box> obstacles) noexcept;

// The sneaking edge guard (RFC 0010). While the box is within `max_drop` of
// the ground and not moving up, `wanted`'s horizontal motion is cut back in
// steps of 0.05 blocks (to 0 once under 0.05) until the box, moved by it,
// would still have an obstacle within `max_drop` below its bottom: x alone,
// then z alone, then both together. A motion still over the edge after 256
// steps (12.8 blocks, far beyond walking speed) becomes 0, so the guard
// always ends.
[[nodiscard]] Vec3d guard_edge(const Box& box, Vec3d wanted, double max_drop,
                               std::span<const Box> obstacles) noexcept;

// How a move treats steps and edges (RFC 0010); the defaults do neither.
struct MoveRules {
    double step_height = 0.0;       // the highest surface risen onto without a jump
    bool started_on_ground = false; // step-up's other condition
    double edge_drop = 0.0;         // the edge guard's drop, or 0 for no guard
};

// The blocks movement collides with, and how slippery they are.
class BlockBoxes {
public:
    virtual ~BlockBoxes() = default;

    // Appends to `out` the collision box of every block that overlaps or
    // touches `region`.
    virtual void collect(const Box& region, std::vector<Box>& out) const = 0;
    // How slippery `block` is to stand on (RFC 0010): above 0 and at most 1,
    // 0.6 for default ground and air.
    [[nodiscard]] virtual double slipperiness(BlockPos block) const = 0;
};

// The region a move of `box` by `wanted` under `rules` gathers boxes from:
// the box swept along the move, down to the edge guard's drop and up to the
// step's height.
[[nodiscard]] Box gathered_region(const Box& box, Vec3d wanted, const MoveRules& rules) noexcept;

// A move against the world's blocks. The boxes it could touch, those of
// gathered_region, are gathered into `scratch`, which keeps its memory from
// one move to the next, so moves allocate only when they gather more boxes
// than any before them, and still holds them when the move returns; then the
// edge guard, if the rules ask for one, and clip_move_stepping. A move with a
// coordinate or a rule that is not finite does nothing, and asks the world
// for nothing; a negative drop or step height is none.
[[nodiscard]] Move move_box(const Box& box, Vec3d wanted, const BlockBoxes& world,
                            std::vector<Box>& scratch, const MoveRules& rules = {});

} // namespace vx
