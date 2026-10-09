#include "flythrough.hpp"

#include <vx/core/vec3d.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

// RFC 0006's benchmark path: each phase a function of time, starting where
// the one before it ends.

namespace {

using vx::app::FlythroughPath;
using vx::app::FlythroughPlan;
using vx::app::Phase;

double distance(vx::Vec3d a, vx::Vec3d b) {
    const vx::Vec3d d = b - a;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// Where the camera looks along the ground, RFC 0001's way.
vx::Vec3d facing(float yaw) {
    return {.x = -std::sin(static_cast<double>(yaw)),
            .y = 0.0,
            .z = -std::cos(static_cast<double>(yaw))};
}

} // namespace

TEST_CASE("the flythrough's phases go in RFC 0006's order") {
    std::vector<std::string_view> names;
    for (const Phase phase : vx::app::flythrough_phases) {
        names.push_back(vx::app::phase_name(phase));
    }
    CHECK(names == std::vector<std::string_view>{"load", "hover", "cruise", "turns", "sprint",
                                                 "climb", "teleport"});
    const FlythroughPlan plan = FlythroughPlan::full();
    CHECK(plan.length(Phase::load).until_complete);
    CHECK(plan.length(Phase::teleport).until_complete);
    CHECK_FALSE(plan.length(Phase::cruise).until_complete);
    CHECK(plan.length(Phase::cruise).seconds == 30.0);
}

TEST_CASE("each phase starts where the one before it ends, for either plan") {
    for (const FlythroughPlan& plan : {FlythroughPlan::full(), FlythroughPlan::quick()}) {
        const FlythroughPath path(plan);
        for (std::size_t at = 1; at < vx::app::phase_count; ++at) {
            const Phase before = vx::app::flythrough_phases[at - 1];
            const Phase phase = vx::app::flythrough_phases[at];
            INFO(vx::app::phase_name(phase));
            const vx::Vec3d end = path.pose(before, plan.length(before).seconds).eye;
            const vx::Vec3d begin = path.pose(phase, 0.0).eye;
            if (phase == Phase::teleport) {
                CHECK(begin.x == doctest::Approx(end.x + 1'000'000.0));
                CHECK(begin.y == doctest::Approx(160.0));
                CHECK(begin.z == doctest::Approx(end.z));
            } else {
                CHECK(distance(begin, end) < 1e-9);
            }
        }
    }
}

TEST_CASE("cruise goes east at 64 blocks a second, sprint at 512, and the climb to y = 2,000") {
    const FlythroughPath path(FlythroughPlan::full());
    CHECK(path.pose(Phase::load, 0.0).eye == vx::Vec3d{.x = 0.0, .y = 160.0, .z = 0.0});
    const vx::Vec3d cruise = path.pose(Phase::cruise, 10.0).eye - path.pose(Phase::cruise, 0.0).eye;
    CHECK(cruise.x == doctest::Approx(640.0));
    CHECK(cruise.z == 0.0);
    CHECK(facing(path.pose(Phase::cruise, 1.0).yaw).x == doctest::Approx(1.0));
    const vx::Vec3d sprint = path.pose(Phase::sprint, 2.0).eye - path.pose(Phase::sprint, 0.0).eye;
    CHECK(sprint.x == doctest::Approx(1'024.0));
    CHECK(path.pose(Phase::climb, 15.0).eye.y == doctest::Approx(2'000.0));
    CHECK(path.pose(Phase::climb, 0.0).pitch == doctest::Approx(-std::numbers::pi / 4.0));
    // Past its length, a phase holds its end.
    CHECK(path.pose(Phase::cruise, 100.0).eye == path.pose(Phase::cruise, 30.0).eye);
}

TEST_CASE("the turns keep 64 blocks a second, swing the view 90 degrees each way, and end on the "
          "line") {
    const FlythroughPath path(FlythroughPlan::full());
    for (double t = 0.0; t < 14.9; t += 0.37) {
        INFO("at ", t, " s");
        const vx::Vec3d a = path.pose(Phase::turns, t).eye;
        const vx::Vec3d b = path.pose(Phase::turns, t + 0.01).eye;
        CHECK(distance(a, b) / 0.01 == doctest::Approx(64.0).epsilon(0.005));
        // The camera looks where it flies.
        const vx::Vec3d look = facing(path.pose(Phase::turns, t + 0.005).yaw);
        CHECK((look.x * (b.x - a.x)) + (look.z * (b.z - a.z)) ==
              doctest::Approx(distance(a, b)).epsilon(0.01));
    }
    // A quarter of an S in, the view has swung to +z; three quarters, to -z.
    const double quarter = FlythroughPath::turn_period / 4.0;
    CHECK(facing(path.pose(Phase::turns, quarter).yaw).z == doctest::Approx(1.0));
    CHECK(facing(path.pose(Phase::turns, 3.0 * quarter).yaw).z == doctest::Approx(-1.0));
    // Two whole S-curves bring it back to the line it left.
    CHECK(std::abs(path.pose(Phase::turns, 15.0).eye.z) < 0.05);
    CHECK(path.pose(Phase::turns, 15.0).eye.x > path.pose(Phase::turns, 0.0).eye.x + 100.0);
}

TEST_CASE("the quick plan takes the same path for less time") {
    const FlythroughPath full(FlythroughPlan::full());
    const FlythroughPath quick(FlythroughPlan::quick());
    CHECK(quick.pose(Phase::cruise, 0.5).eye == full.pose(Phase::cruise, 0.5).eye);
    CHECK(distance(quick.pose(Phase::turns, 0.3).eye - quick.pose(Phase::turns, 0.0).eye,
                   full.pose(Phase::turns, 0.3).eye - full.pose(Phase::turns, 0.0).eye) < 1e-9);
    // It stops sooner: half a second of cruise, not thirty.
    CHECK(quick.pose(Phase::cruise, 30.0).eye.x == doctest::Approx(32.0));
}

TEST_CASE("the full plan is RFC 0006's, and the quick one CI's: small, every frame, checked") {
    const FlythroughPlan full = FlythroughPlan::full();
    CHECK(full.sample_every == 30);
    CHECK(full.view_distance == 32);
    CHECK(full.width == 2'560);
    CHECK(full.height == 1'440);
    CHECK_FALSE(full.check);
    const FlythroughPlan quick = FlythroughPlan::quick();
    CHECK(quick.sample_every == 1);
    CHECK(quick.view_distance == 16);
    CHECK(quick.check);
    CHECK(quick.width < full.width);
}
