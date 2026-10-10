#pragma once

#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>
#include <vx/world/movement.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace vx::test {

constexpr double pi = std::numbers::pi;

constexpr double radians(double degrees) {
    return degrees * pi / 180.0;
}

inline double degrees(double radians) {
    return radians * 180.0 / pi;
}

// The engine's yaw from `from` toward `to`, across.
inline double yaw_toward(Vec3d from, Vec3d to) {
    return std::atan2(-(to.x - from.x), -(to.z - from.z));
}

// Loose boxes and nothing else: a world with no floor but what a test lays.
class LooseWorld final : public BlockBoxes {
public:
    std::vector<Box> boxes;

    [[nodiscard]] double slipperiness(BlockPos /*block*/) const override { return 0.6; }

    void collect(const Box& region, std::vector<Box>& out) const override {
        for (const Box& box : boxes) {
            if (box.max.x >= region.min.x && box.min.x <= region.max.x &&
                box.max.y >= region.min.y && box.min.y <= region.max.y &&
                box.max.z >= region.min.z && box.min.z <= region.max.z) {
                out.push_back(box);
            }
        }
    }
};

// Perdix living: each step runs his life, then RFC 0010's tick with what it
// chose, then RFC 0012's animation, as the app runs him. He starts on the
// ground at the origin.
template <class World> struct SceneOf {
    explicit SceneOf(std::uint64_t seed, float facing = 0.0F)
        : life(seed, facing), motion(body_facing(facing, 0.0F)) {
        perdix.velocity.y = -0.0784;
        perdix.on_ground = true;
    }

    World world;
    MovementNumbers numbers;
    Player perdix;
    Life life;
    BodyMotion motion;
    Onlooker you;
    std::vector<Box> scratch;

    LifeTick step() {
        const LifeTick ticked = life.tick(perdix, motion.body_yaw, you, world, scratch);
        const Vec3d before = perdix.position;
        tick(perdix, ticked.input, world, numbers, scratch);
        animate(motion, perdix.position - before, ticked.look_yaw, ticked.look_pitch);
        return ticked;
    }
};

// On a flat floor that goes on for good.
using Scene = SceneOf<FlatWorld>;

// On a pillar a block across, its top at y = 0, with nothing else to stand
// on, so his strolls never take him anywhere: he stands in the middle of
// its top.
struct PillarScene : SceneOf<LooseWorld> {
    explicit PillarScene(std::uint64_t seed) : SceneOf<LooseWorld>(seed) {
        world.boxes.push_back(block_box(0.0, -1.0, 0.0));
        perdix.position = {.x = 0.5, .y = 0.0, .z = 0.5};
    }
};

// Whether the life's choices fall on a tick that starts at `tick`: every
// second tick, and the first two.
inline bool chooses(std::uint64_t tick) {
    return tick % 2 == 0 || tick < 2;
}

// The choices at which a goal could start, and how many times it did. A
// goal could start at a choice where it was not running, or where it ended:
// not running after it, or started again.
struct Tally {
    int chances = 0;
    int starts = 0;

    void count(std::uint64_t tick, bool before, bool after, bool started) {
        if (!chooses(tick)) {
            return;
        }
        if (!before || !after || started) {
            ++chances;
        }
        if (started) {
            ++starts;
        }
    }

    // Whether the starts lie inside the 99% interval for `chance` at each
    // chance.
    [[nodiscard]] bool fits(double chance) const {
        const double mean = chances * chance;
        const double spread = std::sqrt(chances * chance * (1.0 - chance));
        return std::abs(starts - mean) <= 2.576 * spread;
    }
};

// One spell of a goal: when it started and how long it ran, in ticks.
struct Spell {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

// Runs `ticks` ticks, tallying `goal`'s chances and keeping its spells. A
// spell ends where the goal stops or starts again.
struct Watch {
    LifeGoal goal = LifeGoal::look_around;
    Tally tally{};
    std::vector<Spell> spells{};

    template <class World> void run(SceneOf<World>& scene, std::uint64_t ticks) {
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            step(scene, tick);
        }
    }

    template <class World> void step(SceneOf<World>& scene, std::uint64_t tick) {
        const bool before = scene.life.running(goal);
        const std::uint64_t starts = scene.life.starts(goal);
        static_cast<void>(scene.step());
        const bool after = scene.life.running(goal);
        const bool started = scene.life.starts(goal) > starts;
        tally.count(tick, before, after, started);
        if (before && (!after || started)) {
            spells.back().length = tick - spells.back().start;
        }
        if (started) {
            spells.push_back({.start = tick, .length = 0});
        }
    }
};

} // namespace vx::test
