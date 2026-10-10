#include "../benchmarks.hpp"

#include <vx/mesh/player_model.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/skin.hpp>

#include <nanobench.h>

#include <array>

namespace vx::bench {

// RFC 0012's budget: under 5 microseconds to pose one body. Each run walks
// the body on a little and turns its head, so no two poses are the same.
void player_model(ankerl::nanobench::Bench& bench) {
    std::array<BodyVertex, player_vertices> vertices{};
    PlayerPose pose{.feet = {.x = 10.5, .y = 64.0, .z = -3.25},
                    .limb_amount = 0.863,
                    .body_yaw = 0.4F,
                    .arms = Arms::slim};
    bench.batch(1).unit("body").run("player model: posing one walking body", [&] {
        pose.limb_position += 0.863;
        pose.age += 1.0;
        pose.head_yaw += 0.01F;
        pose_player(pose, {.x = 10, .y = 64, .z = -4}, vertices);
        ankerl::nanobench::doNotOptimizeAway(vertices);
    });
}

} // namespace vx::bench
