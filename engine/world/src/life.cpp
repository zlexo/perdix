#include <vx/world/life.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace vx {
namespace {

constexpr double pi = std::numbers::pi;

constexpr double radians(double degrees) noexcept {
    return degrees * pi / 180.0;
}

// Minecraft 26.2's look control, from its jar: the head's yaw turns at most
// this much a tick, toward a point or back to the body; the pitch stays
// within this of level; a point holds this many ticks. A point nearer than
// the game's 1e-5 (as a float) across, or up and down, leaves the head's
// yaw, or its pitch, as it is.
constexpr double head_turn = radians(10.0);
constexpr double most_pitch = radians(40.0);
constexpr int point_hold = 2;
constexpr double least_offset = 9.999999747378752e-6;
// While the mob walks a path, its head is held within this of its body.
constexpr double most_off_body = radians(75.0);

// Strolling, from the jar (RandomStrollGoal, WaterAvoidingRandomStrollGoal,
// LandRandomPos, RandomPos): a chance of 1 in 60 at each choice, its 120
// ticks counted in choices; up to 10 tries at a spot up to 10 blocks away
// across and 7 up or down. All but 1 pick in 1,000 lift a spot inside the
// ground to its top, as a cow's stroll does; lifting gives up after 16
// blocks, our bound in place of the game's world height.
constexpr int stroll_one_in = 60;
constexpr int stroll_tries = 10;
constexpr int stroll_across = 10;
constexpr int stroll_up_down = 7;
constexpr float unlifted_chance = 0.001F;
constexpr int most_lift = 16;
// The leash, our rule: no spot farther than this from you.
constexpr double leash_length = 24.0;
// Walking at a target, as the game's move control and navigation do it:
// the walking yaw turns at most this much a tick, and the walk ends within
// this of the target, or once it covered less than this between two
// checks for being stuck, more than 100 ticks apart. The game's stuck
// distance is 25 times the mob's speed squared, 1 block for a cow; Perdix
// takes the cow's.
constexpr double walking_turn = radians(90.0);
// Auto-jump, from the jar (LocalPlayer.updateAutoJump): a move this short
// squared, as a float, gives no way to go, and the keys' way is taken; a
// way this far from the facing is backward; the probes run this far above
// the feet (the game's 0.51, a float), half his width either side, and
// reach 7 times his speed ahead; an obstacle must rise more than 0.5 and at
// most 1.2.
constexpr float least_way_squared = 0.001F;
constexpr float backward_limit = -0.15F;
constexpr double probe_height = 0.5099999904632568;
constexpr double probe_spread = 0.3;
constexpr double reach_per_speed = 7.0;
constexpr float least_rise = 0.5F;
constexpr float most_rise = 1.2F;
constexpr double arrival = 1.0;
constexpr std::uint64_t stuck_ticks = 100;
constexpr double stuck_distance = 1.0;

// Looking at a player, from the jar: the chance at each choice; how far a
// player is seen, sneaking hiding one to 0.8 of that but never under 2;
// and 40 ticks and up to 39 more, counted in choices, two ticks each, so
// halved and rounded up.
constexpr float look_chance = 0.02F;
constexpr double look_reach = 6.0;
constexpr double sneaking_seen = 0.8;
constexpr double least_reach = 2.0;
constexpr int look_least = 40;
constexpr int look_spread = 40;
// Looking around: the chance at each choice, and 20 ticks and up to 19
// more, counted every tick.
constexpr float around_chance = 0.02F;
constexpr int around_least = 20;
constexpr int around_spread = 20;

// Following, from the jar (FollowOwnerGoal, with the wolf's numbers): it
// starts 10 blocks away or more and ends within 2, and aims again at where
// you are every 10 ticks, 5 choices. Beyond 16 blocks Perdix sprints, our
// rule where the wolf teleports. Beyond 48 blocks in his own life he goes
// back to you, as following goes, until within 24.
constexpr double follow_from = 10.0;
constexpr double follow_to = 2.0;
constexpr int retarget_choices = 5;
constexpr double sprint_beyond = 16.0;
constexpr double back_from = 48.0;
constexpr double back_to = 24.0;

constexpr std::array goals = {LifeGoal::go_back, LifeGoal::follow, LifeGoal::stroll,
                              LifeGoal::look_at_player, LifeGoal::look_around};

// `from` turned toward `to` by at most `most`, the shorter way round.
double turned_toward(double from, double to, double most) noexcept {
    return wrap_turn(from + std::clamp(wrap_turn(to - from), -most, most));
}

double squared_distance(Vec3d a, Vec3d b) noexcept {
    const Vec3d d = b - a;
    return (d.x * d.x) + (d.y * d.y) + (d.z * d.z);
}

Vec3d eyes(const Player& body) noexcept {
    return body.position + Vec3d{.x = 0.0, .y = eye_height(body.pose), .z = 0.0};
}

Vec3d eyes(const Onlooker& someone) noexcept {
    return someone.feet + Vec3d{.x = 0.0, .y = someone.eye_height, .z = 0.0};
}

// The block holding `point`, while its coordinates are far from the ends of
// a block coordinate's range; a NaN holds none.
std::optional<BlockPos> block_at(Vec3d point) noexcept {
    constexpr double far = 1e9;
    const auto fits = [](double value) { return value > -far && value < far; };
    if (!fits(point.x) || !fits(point.y) || !fits(point.z)) {
        return std::nullopt;
    }
    return BlockPos{.x = static_cast<std::int32_t>(std::floor(point.x)),
                    .y = static_cast<std::int32_t>(std::floor(point.y)),
                    .z = static_cast<std::int32_t>(std::floor(point.z))};
}

Box cube_of(BlockPos block) noexcept {
    const Vec3d low{.x = static_cast<double>(block.x),
                    .y = static_cast<double>(block.y),
                    .z = static_cast<double>(block.z)};
    return {.min = low, .max = low + Vec3d{.x = 1.0, .y = 1.0, .z = 1.0}};
}

// Whether a block box fills `block` whole: a full block, to stand on.
bool full(BlockPos block, const BlockBoxes& world, std::vector<Box>& scratch) {
    const Box cube = cube_of(block);
    scratch.clear();
    world.collect(cube, scratch);
    return std::ranges::any_of(scratch, [&](const Box& box) {
        return box.min.x <= cube.min.x && box.min.y <= cube.min.y && box.min.z <= cube.min.z &&
               box.max.x >= cube.max.x && box.max.y >= cube.max.y && box.max.z >= cube.max.z;
    });
}

// Whether any block box reaches into `block`: solid, to be lifted out of.
bool solid(BlockPos block, const BlockBoxes& world, std::vector<Box>& scratch) {
    const Box cube = cube_of(block);
    scratch.clear();
    world.collect(cube, scratch);
    return overlaps_any(cube, scratch);
}

// The top of the highest block box reaching into `block`, or none.
std::optional<double> top_in(BlockPos block, const BlockBoxes& world, std::vector<Box>& scratch) {
    const Box cube = cube_of(block);
    scratch.clear();
    world.collect(cube, scratch);
    std::optional<double> top;
    for (const Box& box : scratch) {
        if (overlaps_any(cube, std::span(&box, 1))) {
            top = std::max(top.value_or(box.max.y), box.max.y);
        }
    }
    return top;
}

// The way an auto-jump looks: the move across, or, when it was under the
// game's least, the way the keys push at `speed`; none when that is too
// short as well.
std::optional<Vec3d> way_ahead(const MovementKeys& keys, double yaw, double speed,
                               Vec3d moved) noexcept {
    const auto squared = [](Vec3d v) { return static_cast<float>((v.x * v.x) + (v.z * v.z)); };
    const Vec3d move{.x = moved.x, .y = 0.0, .z = moved.z};
    if (squared(move) > least_way_squared) {
        return move;
    }
    const double forward = (keys.forward ? 1.0 : 0.0) - (keys.back ? 1.0 : 0.0);
    const double strafe = (keys.left ? 1.0 : 0.0) - (keys.right ? 1.0 : 0.0);
    const Vec3d facing{.x = -std::sin(yaw), .y = 0.0, .z = -std::cos(yaw)};
    const Vec3d left{.x = -std::cos(yaw), .y = 0.0, .z = std::sin(yaw)};
    const Vec3d pushed =
        ((facing * forward) + (left * strafe)) * (speed / std::hypot(forward, strafe));
    if (squared(pushed) > least_way_squared) {
        return pushed;
    }
    return std::nullopt;
}

// The box spanned by the ends of a probe, flat at their height.
Box spanned(Vec3d a, Vec3d b) noexcept {
    return {.min = {.x = std::min(a.x, b.x), .y = a.y, .z = std::min(a.z, b.z)},
            .max = {.x = std::max(a.x, b.x), .y = a.y, .z = std::max(a.z, b.z)}};
}

// The top of the first block box a probe crosses, in the order the game
// meets them: by its block, z first, then y, then x, each from the lowest;
// with a block over it, that block's top. None when no box crosses.
std::optional<double> obstacle_top(const Box& probe_a, const Box& probe_b, const BlockBoxes& world,
                                   std::vector<Box>& scratch) {
    const auto crosses = [](const Box& box, const Box& probe) {
        return box.min.x < probe.max.x && box.max.x > probe.min.x && box.min.y < probe.max.y &&
               box.max.y > probe.min.y && box.min.z < probe.max.z && box.max.z > probe.min.z;
    };
    scratch.clear();
    world.collect({.min = {.x = std::min(probe_a.min.x, probe_b.min.x),
                           .y = probe_a.min.y,
                           .z = std::min(probe_a.min.z, probe_b.min.z)},
                   .max = {.x = std::max(probe_a.max.x, probe_b.max.x),
                           .y = probe_a.max.y,
                           .z = std::max(probe_a.max.z, probe_b.max.z)}},
                  scratch);
    std::optional<double> top;
    std::optional<BlockPos> first;
    const auto order = [](const BlockPos& b) { return std::array{b.z, b.y, b.x}; };
    for (const Box& box : scratch) {
        const std::optional<BlockPos> block = block_at((box.min + box.max) * 0.5);
        if (block && (crosses(box, probe_a) || crosses(box, probe_b)) &&
            (!first || order(*block) < order(*first))) {
            top = box.max.y;
            first = block;
        }
    }
    if (!first) {
        return std::nullopt;
    }
    const std::optional<double> over =
        top_in({.x = first->x, .y = first->y + 1, .z = first->z}, world, scratch);
    return over ? over : top;
}

// Whether a standing body fits with its feet at `feet`.
bool room(Vec3d feet, const BlockBoxes& world, std::vector<Box>& scratch) {
    const Box body = body_box(feet, Pose::standing);
    scratch.clear();
    world.collect(body, scratch);
    return !overlaps_any(body, scratch);
}

// Whether the blocks leave a clear line from `from` to `to`: no block box
// the segment passes through.
bool clear_line(Vec3d from, Vec3d to, const BlockBoxes& world, std::vector<Box>& scratch) {
    const Box region{.min = {.x = std::min(from.x, to.x),
                             .y = std::min(from.y, to.y),
                             .z = std::min(from.z, to.z)},
                     .max = {.x = std::max(from.x, to.x),
                             .y = std::max(from.y, to.y),
                             .z = std::max(from.z, to.z)}};
    scratch.clear();
    world.collect(region, scratch);
    return std::ranges::none_of(scratch,
                                [&](const Box& box) { return segment_enters(box, from, to); });
}

} // namespace

std::optional<Vec3d> stroll_spot(Vec3d feet, Vec3d leash, LifeRandom& random,
                                 const BlockBoxes& world, std::vector<Box>& scratch) {
    // A spot in the game's sense of pathfindable, without a path: its block
    // on a full block, as the game asks, and then room for his standing
    // body there, where the game would ask its pathfinder.
    const bool lifts = random.unit() >= unlifted_chance;
    for (int i = 0; i < stroll_tries; ++i) {
        const int across_x = random.below((2 * stroll_across) + 1) - stroll_across;
        const int up = random.below((2 * stroll_up_down) + 1) - stroll_up_down;
        const int across_z = random.below((2 * stroll_across) + 1) - stroll_across;
        const std::optional<BlockPos> found =
            block_at(feet + Vec3d{.x = static_cast<double>(across_x),
                                  .y = static_cast<double>(up),
                                  .z = static_cast<double>(across_z)});
        if (!found || !full({.x = found->x, .y = found->y - 1, .z = found->z}, world, scratch)) {
            continue;
        }
        BlockPos block = *found;
        int lifted = 0;
        while (lifts && lifted <= most_lift && solid(block, world, scratch)) {
            ++block.y;
            ++lifted;
        }
        const Vec3d spot{.x = static_cast<double>(block.x) + 0.5,
                         .y = static_cast<double>(block.y),
                         .z = static_cast<double>(block.z) + 0.5};
        if (lifted <= most_lift && room(spot, world, scratch) &&
            squared_distance(spot, leash) <= leash_length * leash_length) {
            return spot;
        }
    }
    return std::nullopt;
}

bool auto_jumps(const Player& body, const TickInput& input, float look_pitch, Vec3d moved,
                const BlockBoxes& world, const MovementNumbers& numbers,
                std::vector<Box>& scratch) {
    const MovementKeys& keys = input.held;
    const bool moving = keys.forward != keys.back || keys.left != keys.right;
    if (!body.on_ground || keys.sneak || !moving || !std::isfinite(input.yaw) ||
        !std::isfinite(look_pitch) || !std::isfinite(moved.x) || !std::isfinite(moved.z)) {
        return false;
    }
    const auto yaw = static_cast<double>(input.yaw);
    const double speed = numbers.walk_acceleration * (body.sprinting ? numbers.sprint_factor : 1.0);
    const std::optional<Vec3d> way = way_ahead(keys, yaw, speed, moved);
    if (!way) {
        return false;
    }
    const double step = std::hypot(way->x, way->z);
    const Vec3d along = *way * (1.0 / step);
    const double ahead = std::cos(static_cast<double>(look_pitch)) *
                         ((-std::sin(yaw) * along.x) + (-std::cos(yaw) * along.z));
    if (static_cast<float>(ahead) < backward_limit) {
        return false;
    }

    // Room over his head: the block holding its top, and the one above.
    const Vec3d feet = body.position;
    const std::optional<BlockPos> head =
        block_at({.x = feet.x, .y = body_box(feet, body.pose).max.y, .z = feet.z});
    if (!head || top_in(*head, world, scratch) ||
        top_in({.x = head->x, .y = head->y + 1, .z = head->z}, world, scratch)) {
        return false;
    }

    // The two probes, each the box its ends span, 0.51 over the feet.
    const double reach = std::max(speed * reach_per_speed, step);
    const Vec3d start = feet + Vec3d{.x = 0.0, .y = probe_height, .z = 0.0};
    const Vec3d end = start + Vec3d{.x = moved.x, .y = 0.0, .z = moved.z} + (along * reach);
    const Vec3d side = Vec3d{.x = -along.z, .y = 0.0, .z = along.x} * probe_spread;
    const std::optional<double> top = obstacle_top(
        spanned(start - side, end - side), spanned(start + side, end + side), world, scratch);
    if (!top) {
        return false;
    }
    const auto rise = static_cast<float>(*top - feet.y);
    return rise > least_rise && rise <= most_rise;
}

LookControl::LookControl(float yaw) noexcept
    : yaw_(static_cast<float>(wrap_turn(static_cast<double>(yaw)))) {}

void LookControl::look_at(Vec3d point) noexcept {
    point_ = point;
    hold_ = point_hold;
}

void LookControl::tick(Vec3d eye, float body_yaw, bool walking) noexcept {
    // Level unless a point holds, as the game resets a mob's pitch each tick.
    pitch_ = 0.0F;
    const auto body = static_cast<double>(body_yaw);
    if (hold_ > 0) {
        --hold_;
        const Vec3d to = point_ - eye;
        const double across = std::hypot(to.x, to.z);
        if (std::abs(to.x) > least_offset || std::abs(to.z) > least_offset) {
            // The engine's yaw of the way to the point: forward is (-sin, -cos).
            const double toward = std::atan2(-to.x, -to.z);
            yaw_ = static_cast<float>(turned_toward(static_cast<double>(yaw_), toward, head_turn));
        }
        if (std::abs(to.y) > least_offset || across > least_offset) {
            pitch_ =
                static_cast<float>(std::clamp(std::atan2(to.y, across), -most_pitch, most_pitch));
        }
    } else {
        yaw_ = static_cast<float>(turned_toward(static_cast<double>(yaw_), body, head_turn));
    }
    if (walking) {
        const double off =
            std::clamp(wrap_turn(static_cast<double>(yaw_) - body), -most_off_body, most_off_body);
        yaw_ = static_cast<float>(wrap_turn(body + off));
    }
}

Life::Life(std::uint64_t seed, float facing) noexcept
    : random_(seed), look_(facing), walking_yaw_(facing) {}

LifeTick Life::tick(const Player& body, float body_yaw, const Onlooker& you,
                    const BlockBoxes& world, std::vector<Box>& scratch) {
    // The goals are chosen every second tick, and on the first two, as a
    // mob's are; each running goal then has its tick. Between choices, only
    // the goals that need every tick have one: looking around.
    if (ticks_ % 2 == 0 || ticks_ < 2) {
        choose(body, you, world, scratch);
        for (const LifeGoal goal : goals) {
            if (running(goal)) {
                tick_goal(goal, body, you);
            }
        }
    } else if (running(LifeGoal::look_around)) {
        tick_goal(LifeGoal::look_around, body, you);
    }
    // Then, as the game's mob does, the walk at a target, then the head;
    // and the jump the last move's auto-jump found.
    MovementKeys held = walk(body, you);
    look_.tick(eyes(body), body_yaw, target_.has_value());
    held.jump = jump_next_;
    jump_next_ = false;
    ++ticks_;
    last_ = {.input = {.held = held, .yaw = walking_yaw_},
             .look_yaw = look_.yaw(),
             .look_pitch = look_.pitch()};
    return last_;
}

void Life::after_move(const Player& body, Vec3d moved, const BlockBoxes& world,
                      std::vector<Box>& scratch) {
    jump_next_ = auto_jumps(body, last_.input, last_.look_pitch, moved, world, numbers_, scratch);
}

void Life::set_mode(LifeMode mode) noexcept {
    mode_ = mode;
    for (const LifeGoal goal : goals) {
        if (running(goal) && !in_mode(goal)) {
            stop(goal);
        }
    }
}

bool Life::in_mode(LifeGoal goal) const noexcept {
    switch (mode_) {
    case LifeMode::life:
        return goal != LifeGoal::follow;
    case LifeMode::follow:
        return goal != LifeGoal::go_back;
    case LifeMode::still:
        return goal == LifeGoal::look_at_player;
    }
    return false;
}

bool Life::running(LifeGoal goal) const noexcept {
    return running_[static_cast<std::size_t>(goal)];
}

std::uint64_t Life::starts(LifeGoal goal) const noexcept {
    return starts_[static_cast<std::size_t>(goal)];
}

void Life::choose(const Player& body, const Onlooker& you, const BlockBoxes& world,
                  std::vector<Box>& scratch) {
    const auto claims = [](LifeGoal goal) -> std::span<const Claim> {
        static constexpr std::array legs = {Claim::legs};
        static constexpr std::array eyes = {Claim::eyes};
        static constexpr std::array legs_and_eyes = {Claim::legs, Claim::eyes};
        switch (goal) {
        case LifeGoal::go_back:
        case LifeGoal::follow:
            return legs_and_eyes;
        case LifeGoal::stroll:
            return legs;
        case LifeGoal::look_at_player:
            return eyes;
        case LifeGoal::look_around:
            return legs_and_eyes;
        }
        return {};
    };
    // First the running goals that can no longer go on stop, and what the
    // stopped goals held is free.
    for (const LifeGoal goal : goals) {
        if (running(goal) && !can_go_on(goal, body, you)) {
            stop(goal);
        }
    }
    for (std::optional<LifeGoal>& holder : holders_) {
        if (holder && !running(*holder)) {
            holder.reset();
        }
    }
    // Then, in their order, the goals that are not running start when
    // everything they claim is free or held by a goal after them, and their
    // chance comes; those holders stop.
    for (const LifeGoal goal : goals) {
        if (running(goal) || !in_mode(goal)) {
            continue;
        }
        const bool claimable = std::ranges::all_of(claims(goal), [&](Claim claim) {
            const std::optional<LifeGoal>& holder = holders_[static_cast<std::size_t>(claim)];
            return !holder || *holder > goal;
        });
        if (!claimable || !can_use(goal, body, you, world, scratch)) {
            continue;
        }
        for (const Claim claim : claims(goal)) {
            std::optional<LifeGoal>& holder = holders_[static_cast<std::size_t>(claim)];
            if (holder) {
                stop(*holder);
            }
            holder = goal;
        }
        start(goal, body);
    }
}

bool Life::can_use(LifeGoal goal, const Player& body, const Onlooker& you, const BlockBoxes& world,
                   std::vector<Box>& scratch) {
    switch (goal) {
    case LifeGoal::go_back:
        return squared_distance(body.position, you.feet) > back_from * back_from;
    case LifeGoal::follow:
        return squared_distance(body.position, you.feet) >= follow_from * follow_from;
    case LifeGoal::stroll: {
        if (random_.below(stroll_one_in) != 0) {
            return false;
        }
        const std::optional<Vec3d> spot =
            stroll_spot(body.position, you.feet, random_, world, scratch);
        if (spot) {
            spot_ = *spot;
        }
        return spot.has_value();
    }
    case LifeGoal::look_at_player: {
        if (random_.unit() >= look_chance) {
            return false;
        }
        const double reach =
            std::max(look_reach * (you.sneaking ? sneaking_seen : 1.0), least_reach);
        return squared_distance(body.position, you.feet) <= reach * reach &&
               clear_line(eyes(body), eyes(you), world, scratch);
    }
    case LifeGoal::look_around:
        return random_.unit() < around_chance;
    }
    return false;
}

bool Life::can_go_on(LifeGoal goal, const Player& body, const Onlooker& you) const {
    switch (goal) {
    case LifeGoal::go_back:
        return target_.has_value() && squared_distance(body.position, you.feet) > back_to * back_to;
    case LifeGoal::follow:
        return target_.has_value() &&
               squared_distance(body.position, you.feet) > follow_to * follow_to;
    case LifeGoal::stroll:
        return target_.has_value();
    case LifeGoal::look_at_player:
        // Within the plain reach, sneaking or not, and with no look at
        // the line of sight: a look once begun lasts through a wall.
        return squared_distance(body.position, you.feet) <= look_reach * look_reach &&
               look_time_ > 0;
    case LifeGoal::look_around:
        return around_time_ >= 0;
    }
    return false;
}

void Life::start(LifeGoal goal, const Player& body) {
    running_[static_cast<std::size_t>(goal)] = true;
    ++starts_[static_cast<std::size_t>(goal)];
    switch (goal) {
    case LifeGoal::go_back:
    case LifeGoal::follow:
        // The first tick aims at you.
        retarget_ = 0;
        return;
    case LifeGoal::stroll:
        target_ = spot_;
        stuck_from_ = body.position;
        stuck_checked_ = ticks_;
        return;
    case LifeGoal::look_at_player:
        look_time_ = (look_least + random_.below(look_spread) + 1) / 2;
        return;
    case LifeGoal::look_around: {
        const double angle = 2.0 * pi * random_.unit_double();
        around_ = {.x = std::cos(angle), .y = 0.0, .z = std::sin(angle)};
        around_time_ = around_least + random_.below(around_spread);
        return;
    }
    }
}

void Life::stop(LifeGoal goal) {
    running_[static_cast<std::size_t>(goal)] = false;
    if (goal == LifeGoal::go_back || goal == LifeGoal::follow || goal == LifeGoal::stroll) {
        target_.reset();
    }
}

void Life::tick_goal(LifeGoal goal, const Player& body, const Onlooker& you) {
    switch (goal) {
    case LifeGoal::go_back:
    case LifeGoal::follow:
        // Eyes on you, and every 5 choices a walk at where you are, its
        // check for being stuck begun again, as a new path's is.
        look_.look_at(eyes(you));
        if (--retarget_ <= 0) {
            retarget_ = retarget_choices;
            target_ = you.feet;
            stuck_from_ = body.position;
            stuck_checked_ = ticks_;
        }
        return;
    case LifeGoal::stroll:
        return; // the walk does it
    case LifeGoal::look_at_player:
        look_.look_at(eyes(you));
        --look_time_;
        return;
    case LifeGoal::look_around:
        --around_time_;
        look_.look_at(eyes(body) + around_);
        return;
    }
}

MovementKeys Life::walk(const Player& body, const Onlooker& you) {
    if (!target_) {
        return {};
    }
    // The game's navigation is done at its last node; here, within a block
    // of the target. And it is stuck when it covered too little between two
    // checks, as it walks straight into a wall too high to climb.
    if (squared_distance(body.position, *target_) <= arrival * arrival) {
        target_.reset();
        return {};
    }
    if (ticks_ - stuck_checked_ > stuck_ticks) {
        if (squared_distance(body.position, stuck_from_) < stuck_distance * stuck_distance) {
            target_.reset();
            return {};
        }
        stuck_from_ = body.position;
        stuck_checked_ = ticks_;
    }
    // The move control: the walking yaw turns toward the target by at most
    // 90 degrees, and he walks on; following you from far off, he sprints,
    // and RFC 0010's sprint then lasts until he stops.
    const Vec3d to = *target_ - body.position;
    walking_yaw_ = static_cast<float>(
        turned_toward(static_cast<double>(walking_yaw_), std::atan2(-to.x, -to.z), walking_turn));
    const bool far = squared_distance(body.position, you.feet) > sprint_beyond * sprint_beyond;
    return {.forward = true,
            .sprint = far && (running(LifeGoal::follow) || running(LifeGoal::go_back))};
}

std::uint64_t LifeRandom::next() noexcept {
    state_ += 0x9E37'79B9'7F4A'7C15U;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
    return mixed ^ (mixed >> 31U);
}

int LifeRandom::below(int bound) noexcept {
    return static_cast<int>(next() % static_cast<std::uint64_t>(bound));
}

float LifeRandom::unit() noexcept {
    return static_cast<float>(next() >> 40U) * 0x1p-24F;
}

double LifeRandom::unit_double() noexcept {
    return static_cast<double>(next() >> 11U) * 0x1p-53;
}

} // namespace vx
