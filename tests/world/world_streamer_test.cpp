#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_records.hpp>
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
using vx::ChunkRecord;
using vx::ChunkState;
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

// Steps until no job is left, checking the rules after every step.
void settle(WorldStreamer& streamer, vx::JobSystem& jobs);

// Counted by hand, not with counts_as_planned and counts_as_ready, which
// the counts under test use.
bool planned_by_hand(ChunkState state) {
    return state == ChunkState::planned || state == ChunkState::build_queued ||
           state == ChunkState::building || state == ChunkState::ready ||
           state == ChunkState::complete;
}

bool ready_by_hand(ChunkState state) {
    return state == ChunkState::ready || state == ChunkState::complete;
}

// Neighbors past the world's edge, which count as planned and ready.
int outside_by_hand(ChunkPos pos) {
    const auto beyond = [](std::int64_t at) {
        return at < vx::lowest_chunk || at > vx::highest_chunk;
    };
    int outside = 0;
    for (std::int64_t dy = -1; dy <= 1; ++dy) {
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                if (beyond(pos.x + dx) || beyond(pos.y + dy) || beyond(pos.z + dz)) {
                    ++outside;
                }
            }
        }
    }
    return outside;
}

// RFC 0006's rules for every record: its counts are its neighbors' states,
// and its state follows from its counts and its place in the region.
void check_rules(const WorldStreamer& streamer) {
    const vx::ChunkRecords& records = streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!records.live(index)) {
            continue;
        }
        const ChunkRecord& record = records[index];
        INFO("chunk ", record.pos.x, ", ", record.pos.y, ", ", record.pos.z, " in state ",
             static_cast<int>(record.state));
        int planned = outside_by_hand(record.pos);
        int ready = planned;
        records.for_neighbors(record.pos, [&](std::uint32_t neighbor) {
            planned += planned_by_hand(records[neighbor].state) ? 1 : 0;
            ready += ready_by_hand(records[neighbor].state) ? 1 : 0;
        });
        REQUIRE(record.planned_neighbors == planned);
        REQUIRE(record.ready_neighbors == ready);
        if (record.drawn) {
            REQUIRE(record.loaded);
        }
    }
}

void settle(WorldStreamer& streamer, vx::JobSystem& jobs) {
    for (int pass = 0; pass < 10'000; ++pass) {
        if (!step(streamer, jobs)) {
            check_rules(streamer);
            return;
        }
        check_rules(streamer);
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

TEST_CASE("a drawn chunk stays drawn until it leaves the region grown for hysteresis") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    // A drawn chunk at the disc's west edge, 3 columns from the camera.
    std::optional<ChunkPos> edge;
    for (std::int32_t y = -16; y <= 16 && !edge; ++y) {
        const ChunkRecord* record =
            streamer.find({.x = above_hills.x - small.view_distance, .y = y, .z = above_hills.z});
        if (record != nullptr && record->drawn) {
            edge = record->pos;
        }
    }
    REQUIRE(edge.has_value());
    // East by the hysteresis, up to 5 columns away: still drawn.
    for (std::int32_t east = 1; east <= small.hysteresis; ++east) {
        streamer.set_camera({.x = above_hills.x + east, .y = above_hills.y, .z = above_hills.z});
        settle(streamer, jobs);
        const ChunkRecord* record = streamer.find(*edge);
        REQUIRE(record != nullptr);
        CHECK(record->drawn);
    }
    // One column more: no longer drawn, but loaded by its drawn neighbor.
    streamer.set_camera(
        {.x = above_hills.x + small.hysteresis + 1, .y = above_hills.y, .z = above_hills.z});
    settle(streamer, jobs);
    const ChunkRecord* left = streamer.find(*edge);
    REQUIRE(left != nullptr);
    CHECK_FALSE(left->drawn);
    CHECK(left->loaded);
    // Back over the hills: drawn again.
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    REQUIRE(streamer.find(*edge) != nullptr);
    CHECK(streamer.find(*edge)->drawn);
}
