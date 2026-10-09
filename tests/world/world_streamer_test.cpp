#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

// RFC 0006's streaming, the data half, in RFC 0005's deterministic mode.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::WorldStreamer;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr vx::PlaceholderBlocks blocks{
    .grass = static_cast<BlockState>(3), .dirt = static_cast<BlockState>(2), .stone = stone};

// A small region: a disc of 3 columns, a near cylinder 1 out and 1 up and
// down, hysteresis 2.
constexpr vx::RegionSettings small{
    .view_distance = 3, .near_distance = 1, .near_height = 1, .hysteresis = 2};

// Over the hills near block (100, 70, 100).
constexpr ChunkPos above_hills{.x = 3, .y = 2, .z = 3};

// One pass of the world loop: feed, run what was fed, integrate it. False
// once nothing was left to run.
bool step(WorldStreamer& streamer, vx::JobSystem& jobs) {
    streamer.feed();
    if (streamer.stats().jobs_in_flight == 0) {
        return false;
    }
    jobs.run_until_idle();
    streamer.integrate();
    return true;
}

// Steps until no job is left.
void settle(WorldStreamer& streamer, vx::JobSystem& jobs) {
    for (int pass = 0; pass < 10'000; ++pass) {
        if (!step(streamer, jobs)) {
            return;
        }
    }
    FAIL("streaming never settled");
}

// The columns that get surfaces, counted by hand: those whose square of
// half side 2 comes within the wider of the disc and the near cylinder,
// grown for hysteresis.
std::vector<vx::ColumnPos> ring(ChunkPos camera, const vx::RegionSettings& settings) {
    const std::int32_t radius =
        std::max(settings.view_distance, settings.near_distance) + settings.hysteresis;
    std::vector<vx::ColumnPos> columns;
    for (std::int32_t dz = -radius - 2; dz <= radius + 2; ++dz) {
        for (std::int32_t dx = -radius - 2; dx <= radius + 2; ++dx) {
            const std::int32_t near_x = std::max(0, std::abs(dx) - 2);
            const std::int32_t near_z = std::max(0, std::abs(dz) - 2);
            if ((near_x * near_x) + (near_z * near_z) <= radius * radius) {
                columns.push_back({.x = camera.x + dx, .z = camera.z + dz});
            }
        }
    }
    return columns;
}

} // namespace

TEST_CASE("every column within reach of the region gets the generator's surface range") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    const std::vector<vx::ColumnPos> columns = ring(above_hills, small);
    // Exactly those columns: as many, and each of them known.
    CHECK(streamer.stats().columns == columns.size());
    CHECK(streamer.stats().surfaces_known == columns.size());
    vx::GenScratch scratch;
    for (const vx::ColumnPos pos : columns) {
        INFO("column ", pos.x, ", ", pos.z);
        const std::optional<vx::ChunkRange> surface = streamer.surface(pos);
        REQUIRE(surface.has_value());
        CHECK(*surface == generator.surface_range(pos, scratch));
    }
}

TEST_CASE("a near cylinder wider than the terrain disc widens the ring of columns") {
    // RFC 0006's slider goes down to a view distance of 4, under the near
    // cylinder's 8: the columns that get surfaces follow the wider shape.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    const vx::RegionSettings narrow{
        .view_distance = 1, .near_distance = 4, .near_height = 1, .hysteresis = 1};
    WorldStreamer streamer(generator, jobs, wake, narrow);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    CHECK(streamer.stats().columns == ring(above_hills, narrow).size());
    // The cylinder's edge, 4 columns out and 1 more for hysteresis, and the
    // 2 its chunks' plans and bands reach past it.
    CHECK(streamer.surface({.x = above_hills.x + 7, .z = above_hills.z}).has_value());
    CHECK_FALSE(streamer.surface({.x = above_hills.x + 8, .z = above_hills.z}).has_value());
}

TEST_CASE("a column that leaves the ring goes: its queued job is canceled, and a result that "
          "comes back late is dropped") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    streamer.feed();
    REQUIRE(streamer.stats().jobs_in_flight > 0);
    // Far east before any job runs: every queued surface job is canceled.
    const ChunkPos east{.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z};
    streamer.set_camera(east);
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
    // East's jobs run, then the camera goes and comes back before their
    // results are read: the columns there are new, and wait for new jobs.
    streamer.feed();
    jobs.run_until_idle();
    streamer.set_camera(above_hills);
    streamer.set_camera(east);
    streamer.integrate();
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(streamer.stats().surfaces_known == 0);
    settle(streamer, jobs);
    CHECK(streamer.stats().columns == ring(east, small).size());
    CHECK(streamer.stats().surfaces_known == streamer.stats().columns);
}
