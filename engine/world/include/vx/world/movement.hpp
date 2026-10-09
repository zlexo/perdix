#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// RFC 0010's numbers: Minecraft Java Edition 26.2's values, kept as data so
// that later effects and attributes change values, not code. Distances are
// blocks, times ticks of 50 ms, speeds blocks per tick.
struct MovementNumbers {
    double walk_acceleration = 0.1;       // on default ground: the game's movement speed
    double sprint_factor = 1.3;           // on ground and air acceleration while sprinting
    double air_acceleration = 0.02;       // in the air, whatever is below
    double input_scale = 0.98;            // on each key axis
    double sneak_factor = 0.3;            // on each key axis while crouching
    double default_slipperiness = 0.6;    // air, and every block that sets none
    double air_drag = 0.91;               // horizontal speed kept after a tick begun in the air
    double gravity = 0.08;                // taken from vertical speed after each move
    double vertical_drag = 0.98;          // vertical speed kept, after gravity
    double step_height = 0.6;             // the highest surface stepped onto without a jump
    double momentum_threshold = 0.003;    // speeds below it become 0
    double jump_speed = 0.42;             // the vertical speed a jump leaves, at least
    double sprint_jump_boost = 0.2;       // added along the facing, jumping while sprinting
    int jump_cooldown = 10;               // ticks between jumps while Space stays held
    int double_tap_ticks = 7;             // the window for a second press of W or Space
    double minor_collision_degrees = 8.0; // a wall hit at a smaller angle keeps a sprint
    double fly_acceleration = 0.05;       // horizontal, flying
    double fly_sprint_factor = 2.0;       // on the flying acceleration while sprinting
    double fly_vertical_speed = 0.15;     // added by Space, or taken by Shift, flying
    double fly_vertical_drag = 0.6;       // vertical speed kept after a flying move
};

// The keys held during a tick.
struct MovementKeys {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool sneak = false;
    bool sprint = false;
};

// What a tick reads: the keys held when it runs, and the camera's yaw then
// (RFC 0001: 0 looks down -z, and positive turns left, seen from above).
struct TickInput {
    MovementKeys held;
    float yaw = 0.0F;
};

enum class Pose : std::uint8_t { standing, crouching };

// The player's body between ticks.
struct Player {
    Vec3d position; // the bottom center of its box
    Vec3d velocity;
    Pose pose = Pose::standing;
    bool sprinting = false;
    bool on_ground = false;            // the last move ended on the ground
    bool horizontal_collision = false; // the last move was cut short across x or z
    // Whether that cut was a graze, the move going less than 8 degrees from
    // where the keys pointed; a graze keeps a sprint.
    bool minor_horizontal_collision = false;
    int jump_cooldown = 0; // ticks before a held jump may jump again
    int sprint_window = 0; // ticks left for W's second press to start a sprint
    bool flying = false;   // in creative flight
    bool may_fly = true;   // in creative mode: Space pressed twice toggles flight
    int fly_window = 0;    // ticks left for Space's second press to toggle flight
    // The block that holds the player up, chosen after each move on the
    // ground (RFC 0010; 26.2's way, from its jar); none in the air.
    std::optional<BlockPos> supporting;
    bool ground_without_block = false; // the last move on the ground found none
    MovementKeys last_held;            // the last tick's keys, to tell a fresh press
};

// The box of a body at `position` in `pose`: 0.6 wide, and 1.8 tall standing
// or 1.5 crouching.
[[nodiscard]] Box body_box(Vec3d position, Pose pose) noexcept;

// How far above its feet a body in `pose` sees from: 1.62 standing, 1.27
// crouching.
[[nodiscard]] double eye_height(Pose pose) noexcept;

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

// Advances the player one tick (50 ms) with the input held during it, in
// Minecraft's order (RFC 0010): a sprint starts, with W held without S and the
// player not crouched, while the sprint key is held, or when W is pressed
// again within 6 ticks of a press, in the air too, unless Shift was held the
// tick before or S is; it stops when that push forward goes or when the last
// move ran into a wall at more than 8 degrees from where the keys pointed, and
// survives a crouch; Space pressed again within 6 ticks of a press toggles
// creative flight; speeds under the momentum threshold become 0; a player that
// began the tick on the ground with Space held jumps, once the cooldown since
// its last jump is over, and sprinting also pushes it along its facing, while
// a flying one rises with Space and sinks with Shift; the keys accelerate the
// player, on the ground, in the air or flying, along the facing the yaw gives;
// the box moves with collision and step-up, held back at edges while Shift is
// held and the player walks, and each component of the velocity a block cut
// short becomes 0; landing ends a flight; then gravity, and drag, on the
// ground if the tick began there, or flight's own drag. Last, the pose for the
// next tick: crouching while Shift is held and the player walks, or where only
// the crouching box fits. `scratch` holds the boxes a move gathers, from one
// tick to the next. A yaw that is not finite gives no facing: the tick then
// does nothing.
void tick(Player& player, const TickInput& input, const BlockBoxes& world,
          const MovementNumbers& numbers, std::vector<Box>& scratch);

} // namespace vx
