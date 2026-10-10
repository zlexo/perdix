#include <vx/world/life.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <algorithm>
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

constexpr std::array goals = {LifeGoal::look_at_player, LifeGoal::look_around};

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

LookControl::LookControl(float yaw) noexcept
    : yaw_(static_cast<float>(wrap_turn(static_cast<double>(yaw)))) {}

void LookControl::look_at(Vec3d point) noexcept {
    point_ = point;
    hold_ = point_hold;
}

void LookControl::tick(Vec3d eye, float body_yaw) noexcept {
    // Level unless a point holds, as the game resets a mob's pitch each tick.
    pitch_ = 0.0F;
    if (hold_ <= 0) {
        yaw_ = static_cast<float>(
            turned_toward(static_cast<double>(yaw_), static_cast<double>(body_yaw), head_turn));
        return;
    }
    --hold_;
    const Vec3d to = point_ - eye;
    const double across = std::hypot(to.x, to.z);
    if (std::abs(to.x) > least_offset || std::abs(to.z) > least_offset) {
        // The engine's yaw of the way to the point: forward is (-sin, -cos).
        const double toward = std::atan2(-to.x, -to.z);
        yaw_ = static_cast<float>(turned_toward(static_cast<double>(yaw_), toward, head_turn));
    }
    if (std::abs(to.y) > least_offset || across > least_offset) {
        pitch_ = static_cast<float>(std::clamp(std::atan2(to.y, across), -most_pitch, most_pitch));
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
    look_.tick(eyes(body), body_yaw);
    ++ticks_;
    return {.input = {.held = {}, .yaw = walking_yaw_},
            .look_yaw = look_.yaw(),
            .look_pitch = look_.pitch()};
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
        static constexpr std::array eyes_only = {Claim::eyes};
        static constexpr std::array legs_and_eyes = {Claim::legs, Claim::eyes};
        return goal == LifeGoal::look_around ? std::span<const Claim>(legs_and_eyes)
                                             : std::span<const Claim>(eyes_only);
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
        if (running(goal)) {
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
        start(goal);
    }
}

bool Life::can_use(LifeGoal goal, const Player& body, const Onlooker& you, const BlockBoxes& world,
                   std::vector<Box>& scratch) {
    const auto chance = static_cast<float>(next() >> 40U) * 0x1p-24F;
    switch (goal) {
    case LifeGoal::look_at_player: {
        if (chance >= look_chance) {
            return false;
        }
        const double reach =
            std::max(look_reach * (you.sneaking ? sneaking_seen : 1.0), least_reach);
        return squared_distance(body.position, you.feet) <= reach * reach &&
               clear_line(eyes(body), eyes(you), world, scratch);
    }
    case LifeGoal::look_around:
        return chance < around_chance;
    }
    return false;
}

bool Life::can_go_on(LifeGoal goal, const Player& body, const Onlooker& you) const {
    switch (goal) {
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

void Life::start(LifeGoal goal) {
    running_[static_cast<std::size_t>(goal)] = true;
    ++starts_[static_cast<std::size_t>(goal)];
    switch (goal) {
    case LifeGoal::look_at_player:
        look_time_ = (look_least + static_cast<int>(next() % look_spread) + 1) / 2;
        return;
    case LifeGoal::look_around: {
        const double angle = 2.0 * pi * (static_cast<double>(next() >> 11U) * 0x1p-53);
        around_ = {.x = std::cos(angle), .y = 0.0, .z = std::sin(angle)};
        around_time_ = around_least + static_cast<int>(next() % around_spread);
        return;
    }
    }
}

void Life::stop(LifeGoal goal) {
    running_[static_cast<std::size_t>(goal)] = false;
}

void Life::tick_goal(LifeGoal goal, const Player& body, const Onlooker& you) {
    switch (goal) {
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

std::uint64_t Life::next() noexcept {
    // SplitMix64: small, fast, and good enough for a creature's whims.
    random_ += 0x9E37'79B9'7F4A'7C15U;
    std::uint64_t mixed = random_;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
    return mixed ^ (mixed >> 31U);
}

} // namespace vx
