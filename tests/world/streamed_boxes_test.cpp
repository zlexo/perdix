#include <vx/core/job_system.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/movement.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/streamed_boxes.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <vector>

// RFC 0010's moves over RFC 0006's streamed world: a player lands on the
// placeholder's hills, and chunks not loaded yet hold the player out.

namespace {

constexpr vx::PlaceholderBlocks blocks{.grass = static_cast<vx::BlockState>(3),
                                       .dirt = static_cast<vx::BlockState>(2),
                                       .stone = static_cast<vx::BlockState>(1)};

constexpr vx::RegionSettings small{
    .view_distance = 3, .near_distance = 1, .near_height = 1, .hysteresis = 2};

// Takes in every result the jobs sent, however many steps that takes.
void integrate_all(vx::WorldStreamer& streamer) {
    while (streamer.integrate()) {
    }
}

void settle(vx::WorldStreamer& streamer, vx::JobSystem& jobs) {
    for (int pass = 0; pass < 10'000; ++pass) {
        streamer.feed();
        if (streamer.stats().jobs_in_flight == 0) {
            return;
        }
        jobs.run_until_idle();
        integrate_all(streamer);
    }
    FAIL("streaming never settled");
}

} // namespace

TEST_CASE("a player dropped over the streamed hills lands on the ground there") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    vx::WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera({.x = 3, .y = 2, .z = 3});
    settle(streamer, jobs);
    const vx::StreamedBoxes world(streamer, {});
    const std::int32_t ground = generator.height(100, 100);
    vx::Player player;
    player.position = {.x = 100.5, .y = ground + 10.0, .z = 100.5};
    std::vector<vx::Box> scratch;
    for (int tick = 0; tick < 100 && !player.on_ground; ++tick) {
        vx::tick(player, {}, world, {}, scratch);
    }
    CHECK(player.on_ground);
    CHECK(player.position.y == doctest::Approx(static_cast<double>(ground)));
}

TEST_CASE("a chunk not loaded yet is solid, so a player cannot fall into it") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    vx::WorldStreamer streamer(generator, jobs, wake, small);
    // Nothing streamed: every chunk is a solid cube.
    const vx::StreamedBoxes world(streamer, {});
    std::vector<vx::Box> found;
    world.collect({.min = {.x = 0.2, .y = 0.2, .z = 0.2}, .max = {.x = 0.8, .y = 1.8, .z = 0.8}},
                  found);
    REQUIRE(found.size() == 1);
    CHECK(found[0].min == vx::Vec3d{.x = 0.0, .y = 0.0, .z = 0.0});
    CHECK(found[0].max == vx::Vec3d{.x = 32.0, .y = 32.0, .z = 32.0});
    // A region across a chunk corner finds the eight chunks around it.
    found.clear();
    world.collect({.min = {.x = -0.5, .y = -0.5, .z = -0.5}, .max = {.x = 0.5, .y = 0.5, .z = 0.5}},
                  found);
    CHECK(found.size() == 8);
}
