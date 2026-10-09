#include <vx/world/movement.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
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

// Whether `keys` push the player forward: W without S, whatever A and D.
bool pushes_forward(const MovementKeys& keys) {
    return keys.forward && !keys.back;
}

// The sprint's rules (RFC 0010), read before the tick moves the player;
// the double tap as Minecraft 26.2's jar has it.
void update_sprint(Player& player, const MovementKeys& held, const MovementNumbers& numbers) {
    // Shift held last tick, or S held now, closes the double tap's window.
    if (player.last_held.sneak || held.back) {
        player.sprint_window = 0;
    }
    // A sprint starts only with the player pushing forward and not crouched.
    if (!player.sprinting && pushes_forward(held) && player.pose != Pose::crouching) {
        // A fresh push forward opens the window, and one while it is open
        // sprints, on the ground or not. The window then runs out on its
        // own, so another push inside it sprints again.
        if (!pushes_forward(player.last_held)) {
            if (player.sprint_window > 0) {
                player.sprinting = true;
            } else {
                player.sprint_window = numbers.double_tap_ticks;
            }
        }
        if (held.sprint) {
            player.sprinting = true;
        }
    }
    // A sprint ends without a push forward, or after a move that ran into a
    // wall rather than grazing it; a crouch keeps it, as 26.2's does. A
    // sprint started against a wall ends here at once.
    const bool hit_a_wall = player.horizontal_collision && !player.minor_horizontal_collision;
    if (player.sprinting && (!pushes_forward(held) || hit_a_wall)) {
        player.sprinting = false;
    }
}

// Whether a move that hit a wall only grazed it: the way it went is less
// than `degrees` from the way the keys pointed. A move that went nowhere,
// stopped dead against the wall, has no way, and counts as a hit.
bool grazed(Vec3d wished, Vec3d moved, double degrees) {
    const double wished_length = std::hypot(wished.x, wished.z);
    const double moved_length = std::hypot(moved.x, moved.z);
    if (wished_length == 0.0 || moved_length == 0.0) {
        return false;
    }
    const double cosine =
        ((wished.x * moved.x) + (wished.z * moved.z)) / (wished_length * moved_length);
    return cosine > std::cos(degrees * std::numbers::pi / 180.0);
}

// A block coordinate holding `value`, when one does: NaN fails both
// comparisons, so it holds none.
std::optional<std::int32_t> block_coordinate(double value) noexcept {
    const double block = std::floor(value);
    constexpr auto lowest = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr auto highest = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    if (block >= lowest && block <= highest) {
        return static_cast<std::int32_t>(block);
    }
    return std::nullopt;
}

// Whether `inner` lies inside `outer`; a NaN anywhere makes it not.
bool contains(const Box& outer, const Box& inner) noexcept {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           inner.max.x <= outer.max.x && inner.max.y <= outer.max.y && inner.max.z <= outer.max.z;
}

// Of the blocks whose boxes in `boxes` overlap `region`, the one whose center
// is nearest `position`, ties to the greater y, then z, then x, as 26.2
// chooses the block that supports a player (from its jar).
std::optional<BlockPos> supporting_block(const Box& region, Vec3d position,
                                         std::span<const Box> boxes) {
    std::optional<BlockPos> best;
    double best_distance = 0.0;
    for (const Box& box : boxes) {
        const bool overlaps = box.max.x > region.min.x && box.min.x < region.max.x &&
                              box.max.y > region.min.y && box.min.y < region.max.y &&
                              box.max.z > region.min.z && box.min.z < region.max.z;
        const std::optional<std::int32_t> x = block_coordinate(box.min.x);
        const std::optional<std::int32_t> y = block_coordinate(box.min.y);
        const std::optional<std::int32_t> z = block_coordinate(box.min.z);
        if (!overlaps || !x || !y || !z) {
            continue;
        }
        const BlockPos block{.x = *x, .y = *y, .z = *z};
        const Vec3d to_center{.x = static_cast<double>(block.x) + 0.5 - position.x,
                              .y = static_cast<double>(block.y) + 0.5 - position.y,
                              .z = static_cast<double>(block.z) + 0.5 - position.z};
        const double distance =
            (to_center.x * to_center.x) + (to_center.y * to_center.y) + (to_center.z * to_center.z);
        const auto order = [](const BlockPos& b) { return std::array{b.y, b.z, b.x}; };
        if (!best || distance < best_distance ||
            (distance == best_distance && order(block) > order(*best))) {
            best = block;
            best_distance = distance;
        }
    }
    return best;
}

// The block that holds the player up, after a move (26.2's way, from its
// jar): on the ground, the supporting block under the box's bottom; with
// none there, the one under where the box came from, unless the last search
// on the ground found none either. None in the air. The move left the boxes
// of `gathered` in `scratch`, so a search inside that region reads them
// rather than asking the world again, and finds the same block: every box
// that touches it is among them. A search that a rounding leaves a hair
// outside gathers its own.
void update_supporting(Player& player, Vec3d motion, const Box& gathered, const BlockBoxes& world,
                       std::vector<Box>& scratch) {
    if (!player.on_ground) {
        player.supporting.reset();
        player.ground_without_block = false;
        return;
    }
    constexpr double depth = 1e-6; // how far under the box's bottom a block is under it
    const Box body = body_box(player.position, player.pose);
    const Box under{.min = {.x = body.min.x, .y = body.min.y - depth, .z = body.min.z},
                    .max = {.x = body.max.x, .y = body.min.y, .z = body.max.z}};
    bool holds_gathered = true; // until a search gathers its own boxes
    const auto search = [&](const Box& region) {
        if (!holds_gathered || !contains(gathered, region)) {
            scratch.clear();
            world.collect(region, scratch);
            holds_gathered = false;
        }
        return supporting_block(region, player.position, scratch);
    };
    std::optional<BlockPos> found = search(under);
    if (!found && !player.ground_without_block) {
        const Vec3d back{.x = -motion.x, .y = 0.0, .z = -motion.z};
        found = search({.min = under.min + back, .max = under.max + back});
    }
    player.supporting = found;
    player.ground_without_block = !found;
}

// The slipperiness the player stands on (RFC 0010): of the block just over
// 0.5 below its feet (0.500001, as 26.2 reads it, so on a slab the block
// under the slab), in the supporting block's column, or under the player's
// position with none.
double slipperiness_under(const Player& player, const BlockBoxes& world,
                          const MovementNumbers& numbers) {
    constexpr double below_feet = 0.500001;
    const std::optional<std::int32_t> y = block_coordinate(player.position.y - below_feet);
    std::optional<std::int32_t> x;
    std::optional<std::int32_t> z;
    if (player.supporting) {
        x = player.supporting->x;
        z = player.supporting->z;
    } else {
        x = block_coordinate(player.position.x);
        z = block_coordinate(player.position.z);
    }
    if (!x || !y || !z) {
        return numbers.default_slipperiness;
    }
    return world.slipperiness({.x = *x, .y = *y, .z = *z});
}

// Space pressed twice toggles creative flight (RFC 0010, in the order 26.2's
// jar has it): a fresh press opens the window, and another while it is
// still open toggles. True when the toggle takes off.
bool update_flight(Player& player, const MovementKeys& held, const MovementNumbers& numbers) {
    if (!held.jump || player.last_held.jump) {
        return false;
    }
    const bool pressed_twice = player.fly_window > 0;
    player.fly_window = pressed_twice ? 0 : numbers.double_tap_ticks;
    if (!pressed_twice || !player.may_fly) {
        return false;
    }
    player.flying = !player.flying;
    return player.flying;
}

// A jump: at least 0.42 upward, never slowing a faster rise, as 26.2's does
// (from its jar); sprinting, 0.2 more along the facing, whatever the keys.
void jump(Player& player, Vec3d facing, const MovementNumbers& numbers) {
    player.velocity.y = std::max(numbers.jump_speed, player.velocity.y);
    if (player.sprinting) {
        player.velocity = player.velocity + (facing * numbers.sprint_jump_boost);
    }
}

// The keys' acceleration this tick: on the ground's terms if the tick began
// there, flying or not, as 26.2's (from its jar: a flying player begins a
// tick on the ground only when it takes off); else flying, or in the air.
double acceleration_now(const Player& player, bool started_on_ground, double slipperiness,
                        const MovementNumbers& numbers) {
    if (started_on_ground) {
        return ground_acceleration(slipperiness, player.sprinting, numbers);
    }
    if (player.flying) {
        return numbers.fly_acceleration * (player.sprinting ? numbers.fly_sprint_factor : 1.0);
    }
    return numbers.air_acceleration * (player.sprinting ? numbers.sprint_factor : 1.0);
}

// Counts the double-tap windows down by a tick, as each tick begins.
void count_down_windows(Player& player) {
    player.sprint_window = std::max(player.sprint_window - 1, 0);
    player.fly_window = std::max(player.fly_window - 1, 0);
}

// What Space and Shift add to a flying player's vertical speed before the
// move: 0.15 up, 0.15 down, nothing with both.
double climb(const MovementKeys& held, const MovementNumbers& numbers) {
    return (held.jump ? numbers.fly_vertical_speed : 0.0) -
           (held.sneak ? numbers.fly_vertical_speed : 0.0);
}

// The vertical speed after the move: flying keeps 0.6 of the speed it had
// before the move, with no gravity; otherwise gravity, then drag.
double vertical_after_move(double vertical, bool flying, const MovementNumbers& numbers) {
    if (flying) {
        return vertical * numbers.fly_vertical_drag;
    }
    return (vertical - numbers.gravity) * numbers.vertical_drag;
}

// The pose for the next tick (RFC 0010): crouching while Shift is held, or
// where the standing box does not fit and the crouching one does, as under
// a ceiling lower than 1.8; standing otherwise. A position that is not
// finite has no blocks around it, and the world is not asked, as for a move.
Pose next_pose(Vec3d position, bool sneak, const BlockBoxes& world, std::vector<Box>& scratch) {
    if (sneak) {
        return Pose::crouching;
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        return Pose::standing;
    }
    const auto fits = [&](Pose pose) {
        const Box box = body_box(position, pose);
        scratch.clear();
        world.collect(box, scratch);
        return !overlaps_any(box, scratch);
    };
    return !fits(Pose::standing) && fits(Pose::crouching) ? Pose::crouching : Pose::standing;
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

double eye_height(Pose pose) noexcept {
    return pose == Pose::crouching ? 1.27 : 1.62;
}

Box tick_reach(const Player& player) noexcept {
    const Box body = body_box(player.position, player.pose);
    constexpr double past_speed = 2.0;
    const Vec3d grown{.x = std::abs(player.velocity.x) + past_speed,
                      .y = std::abs(player.velocity.y) + past_speed,
                      .z = std::abs(player.velocity.z) + past_speed};
    return {.min = body.min - grown, .max = body.max + grown};
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
    count_down_windows(player);
    update_sprint(player, input.held, numbers);
    const auto yaw = static_cast<double>(input.yaw);
    const Vec3d facing{.x = -std::sin(yaw), .y = 0.0, .z = -std::cos(yaw)};
    // Taking off from the ground jumps too, as 26.2's toggle does; then,
    // flying, Space rises and Shift sinks, all before the threshold.
    if (update_flight(player, input.held, numbers) && started_on_ground) {
        jump(player, facing, numbers);
    }
    // This tick's mode, which a landing ends only after the move.
    const bool flying = player.flying;
    if (flying) {
        velocity.y += climb(input.held, numbers);
    }
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

    // The jump: from the ground, with Space held, once the cooldown is over,
    // flying or not, as 26.2's (a flying player is on the ground only on the
    // tick it takes off). Letting go of Space ends the cooldown.
    if (!input.held.jump) {
        player.jump_cooldown = 0;
    } else if (started_on_ground && player.jump_cooldown == 0) {
        jump(player, facing, numbers);
        player.jump_cooldown = numbers.jump_cooldown;
    }

    // The keys accelerate the player along the facing the yaw gives, on the
    // ground as slippery as the block it stands on.
    const double slipperiness = started_on_ground ? slipperiness_under(player, world, numbers)
                                                  : numbers.default_slipperiness;
    const double acceleration = acceleration_now(player, started_on_ground, slipperiness, numbers);
    const KeyInput keys = key_input(input.held, player.pose == Pose::crouching, numbers);
    const Vec3d left{.x = -std::cos(yaw), .y = 0.0, .z = std::sin(yaw)};
    const Vec3d wished = (facing * keys.forward) + (left * keys.strafe);
    velocity = velocity + (wished * acceleration);

    // The move, with step-up, held back at an edge while Shift is held and
    // the player walks; what a block cut short stops.
    const Box body = body_box(player.position, player.pose);
    const MoveRules rules{.step_height = numbers.step_height,
                          .started_on_ground = started_on_ground,
                          .edge_drop = input.held.sneak && !flying ? numbers.step_height : 0.0};
    const Vec3d wanted = velocity;
    const Move move = move_box(body, wanted, world, scratch, rules);
    player.position = player.position + move.motion;
    if (move.x_collision) {
        velocity.x = 0.0;
    }
    // Flying, a block cuts the move short but not the vertical speed: flight
    // keeps 0.6 of what it was before the move, so a landing leaves the
    // player pressing on the ground, as standing does.
    if (move.vertical_collision && !flying) {
        velocity.y = 0.0;
    }
    if (move.z_collision) {
        velocity.z = 0.0;
    }
    player.on_ground = move.on_ground;
    // On the ground, the search for the block underfoot reads the boxes the
    // move gathered; in the air there is none.
    update_supporting(player, move.motion,
                      move.on_ground ? gathered_region(body, wanted, rules) : Box{}, world,
                      scratch);
    if (flying && move.on_ground) {
        player.flying = false; // landing ends a flight
    }
    player.horizontal_collision = move.horizontal_collision();
    player.minor_horizontal_collision =
        player.horizontal_collision && grazed(wished, move.motion, numbers.minor_collision_degrees);

    // Gravity, then drag: on the ground's terms if the tick began there,
    // flying or not. Flying, no gravity, and the vertical speed from before
    // the move keeps 0.6.
    velocity.y = vertical_after_move(velocity.y, flying, numbers);
    const double drag = started_on_ground ? ground_drag(slipperiness, numbers) : numbers.air_drag;
    velocity.x *= drag;
    velocity.z *= drag;

    player.pose = next_pose(player.position, input.held.sneak && !player.flying, world, scratch);
    player.last_held = input.held;
}

} // namespace vx
