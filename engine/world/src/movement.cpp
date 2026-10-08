#include <vx/world/movement.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace vx {
namespace {

// The keys as two lengths: forward (W less S) and strafe (A less D, left
// positive), each scaled, and crouching scaled again; a vector longer than 1
// is cut to 1, a shorter one is kept, so W with A is a little faster than W
// alone, as in the game.
struct KeyInput {
    double forward = 0.0;
    double strafe = 0.0;
};

KeyInput key_input(const MovementKeys& keys, bool crouching, const MovementNumbers& numbers) {
    const auto axis = [](bool positive, bool negative) {
        return (positive ? 1.0 : 0.0) - (negative ? 1.0 : 0.0);
    };
    const double scale = numbers.input_scale * (crouching ? numbers.sneak_factor : 1.0);
    KeyInput input{.forward = axis(keys.forward, keys.back) * scale,
                   .strafe = axis(keys.left, keys.right) * scale};
    const double length = std::hypot(input.forward, input.strafe);
    if (length > 1.0) {
        input.forward /= length;
        input.strafe /= length;
    }
    return input;
}

} // namespace

Box body_box(Vec3d position, Pose pose) noexcept {
    constexpr double half_width = 0.3;
    const double height = pose == Pose::crouching ? 1.5 : 1.8;
    return {.min = {.x = position.x - half_width, .y = position.y, .z = position.z - half_width},
            .max = {.x = position.x + half_width,
                    .y = position.y + height,
                    .z = position.z + half_width}};
}

double ground_acceleration(double slipperiness, bool sprinting,
                           const MovementNumbers& numbers) noexcept {
    // The game's 0.216 over the slipperiness cubed: 0.216 is 0.6 cubed.
    constexpr double formula_slipperiness = 0.6;
    const double ratio = formula_slipperiness / slipperiness;
    const double slow_start = std::min(1.0, ratio * ratio * ratio);
    return numbers.walk_acceleration * (sprinting ? numbers.sprint_factor : 1.0) * slow_start;
}

double ground_drag(double slipperiness, const MovementNumbers& numbers) noexcept {
    return slipperiness * numbers.air_drag;
}

void tick(Player& player, const TickInput& input, const BlockBoxes& world,
          const MovementNumbers& numbers, std::vector<Box>& scratch) {
    // A yaw that is not finite gives no facing, and a NaN facing would turn
    // the velocity NaN for good, even with no key held: such a tick does
    // nothing, as a move that is not finite does.
    if (!std::isfinite(input.yaw)) {
        return;
    }
    const bool started_on_ground = player.on_ground;
    Vec3d& velocity = player.velocity;
    if (player.jump_cooldown > 0) {
        --player.jump_cooldown;
    }

    // Speeds left from the last tick that are too small to matter stop: the
    // horizontal one as a whole, since 1.21.5, and the vertical one alone.
    const double threshold = numbers.momentum_threshold;
    if ((velocity.x * velocity.x) + (velocity.z * velocity.z) < threshold * threshold) {
        velocity.x = 0.0;
        velocity.z = 0.0;
    }
    if (std::abs(velocity.y) < threshold) {
        velocity.y = 0.0;
    }

    // The jump: from the ground, with Space held, once the cooldown is over.
    // Sprinting, it also pushes the player along its facing, whatever the
    // other keys. Letting go of Space ends the cooldown.
    const auto yaw = static_cast<double>(input.yaw);
    const Vec3d facing{.x = -std::sin(yaw), .y = 0.0, .z = -std::cos(yaw)};
    if (!input.held.jump) {
        player.jump_cooldown = 0;
    } else if (started_on_ground && player.jump_cooldown == 0) {
        velocity.y = numbers.jump_speed;
        if (player.sprinting) {
            velocity = velocity + (facing * numbers.sprint_jump_boost);
        }
        player.jump_cooldown = numbers.jump_cooldown;
    }

    // The keys accelerate the player along the facing the yaw gives.
    const double slipperiness = numbers.default_slipperiness;
    const double acceleration =
        started_on_ground
            ? ground_acceleration(slipperiness, player.sprinting, numbers)
            : numbers.air_acceleration * (player.sprinting ? numbers.sprint_factor : 1.0);
    const KeyInput keys = key_input(input.held, player.pose == Pose::crouching, numbers);
    const Vec3d left{.x = -std::cos(yaw), .y = 0.0, .z = std::sin(yaw)};
    velocity = velocity + (((facing * keys.forward) + (left * keys.strafe)) * acceleration);

    // The move, with step-up; what a block cut short stops.
    const Move move = move_box(body_box(player.position, player.pose), velocity, world, scratch,
                               {.step_height = numbers.step_height,
                                .started_on_ground = started_on_ground,
                                .edge_drop = 0.0});
    player.position = player.position + move.motion;
    if (move.x_collision) {
        velocity.x = 0.0;
    }
    if (move.vertical_collision) {
        velocity.y = 0.0;
    }
    if (move.z_collision) {
        velocity.z = 0.0;
    }
    player.on_ground = move.on_ground;
    player.horizontal_collision = move.horizontal_collision();

    // Gravity, then drag: on the ground's terms if the tick began there.
    velocity.y = (velocity.y - numbers.gravity) * numbers.vertical_drag;
    const double drag = started_on_ground ? ground_drag(slipperiness, numbers) : numbers.air_drag;
    velocity.x *= drag;
    velocity.z *= drag;
}

} // namespace vx
