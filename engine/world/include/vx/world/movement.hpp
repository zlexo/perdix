#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <cstdint>

namespace vx {

// RFC 0010's numbers: Minecraft Java Edition 26.2's values, kept as data so
// that later effects and attributes change values, not code. Distances are
// blocks, times ticks of 50 ms, speeds blocks per tick.
struct MovementNumbers {
    double walk_acceleration = 0.1;    // on default ground: the game's movement speed
    double sprint_factor = 1.3;        // on ground and air acceleration while sprinting
    double air_acceleration = 0.02;    // in the air, whatever is below
    double input_scale = 0.98;         // on each key axis
    double sneak_factor = 0.3;         // on each key axis while crouching
    double default_slipperiness = 0.6; // air, and every block that sets none
    double air_drag = 0.91;            // horizontal speed kept after a tick begun in the air
    double gravity = 0.08;             // taken from vertical speed after each move
    double vertical_drag = 0.98;       // vertical speed kept, after gravity
    double step_height = 0.6;          // the highest surface stepped onto without a jump
    double momentum_threshold = 0.003; // speeds below it become 0
};

enum class Pose : std::uint8_t { standing, crouching };

// The box of a body at `position` in `pose`: 0.6 wide, and 1.8 tall standing
// or 1.5 crouching.
[[nodiscard]] Box body_box(Vec3d position, Pose pose) noexcept;

// The horizontal acceleration on the ground, on a block of `slipperiness`
// (more than 0, at most 1): the walking acceleration, more sprinting, times
// (0.6 / slipperiness) cubed, never more than 1, so slippery blocks are slow
// to get going on. The 0.6 is the game's formula's own, whatever the
// default slipperiness.
[[nodiscard]] double ground_acceleration(double slipperiness, bool sprinting,
                                         const MovementNumbers& numbers) noexcept;

// The horizontal speed kept after a tick begun on a block of `slipperiness`:
// 0.546 on default ground, 0.8918 on ice.
[[nodiscard]] double ground_drag(double slipperiness, const MovementNumbers& numbers) noexcept;

} // namespace vx
