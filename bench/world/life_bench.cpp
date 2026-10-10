#include "../benchmarks.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/life.hpp>
#include <vx/world/movement.hpp>

#include <nanobench.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace vx::bench {
namespace {

// An endless floor whose top is y = 0, with a block standing on it every
// 4 blocks each way: steps for Perdix to climb or walk into wherever his
// life takes him. Each block is its own box.
class SteppedFloor final : public BlockBoxes {
public:
    [[nodiscard]] double slipperiness(BlockPos /*block*/) const override { return 0.6; }

    void collect(const Box& region, std::vector<Box>& out) const override {
        // The blocks p whose [p, p + 1] touches the region.
        const auto first = [](double low) { return static_cast<int>(std::ceil(low - 1.0)); };
        const auto last = [](double high) { return static_cast<int>(std::floor(high)); };
        for (int y = first(region.min.y); y <= std::min(last(region.max.y), 0); ++y) {
            for (int z = first(region.min.z); z <= last(region.max.z); ++z) {
                for (int x = first(region.min.x); x <= last(region.max.x); ++x) {
                    if (y == 0 && (x % 4 != 0 || z % 4 != 0)) {
                        continue;
                    }
                    const Vec3d low{.x = static_cast<double>(x),
                                    .y = static_cast<double>(y),
                                    .z = static_cast<double>(z)};
                    out.push_back({.min = low, .max = low + Vec3d{.x = 1.0, .y = 1.0, .z = 1.0}});
                }
            }
        }
    }
};

} // namespace

// RFC 0012's budget: under 5 microseconds a tick for Perdix's life, the
// auto-jump's look ahead included. Timed with the rest of his tick, the
// move (RFC 0010's own budget is 2) and the animation, as the world thread
// runs them, with you standing 5 blocks off so he strolls, looks at you
// and looks around. The scratch is kept from tick to tick, as the app
// keeps it.
void life(ankerl::nanobench::Bench& bench) {
    const SteppedFloor world;
    const MovementNumbers numbers;
    std::vector<Box> scratch;
    scratch.reserve(256);
    Player perdix;
    perdix.position = {.x = 2.5, .y = 0.0, .z = 2.5};
    perdix.on_ground = true;
    perdix.may_fly = false;
    Life life(1, 0.0F);
    BodyMotion motion = body_facing(0.0F, 0.0F);
    const Onlooker you{.feet = {.x = 2.5, .y = 0.0, .z = -2.5}};
    bench.batch(1).unit("tick").run("life: one tick of Perdix, his life, move and look ahead", [&] {
        const LifeTick ticked = life.tick(perdix, motion.body_yaw, you, world, scratch);
        const Vec3d before = perdix.position;
        tick(perdix, ticked.input, world, numbers, scratch);
        life.after_move(perdix, perdix.position - before, world, scratch);
        animate(motion, perdix.position - before, ticked.look_yaw, ticked.look_pitch);
        ankerl::nanobench::doNotOptimizeAway(perdix.position);
    });
}

} // namespace vx::bench
