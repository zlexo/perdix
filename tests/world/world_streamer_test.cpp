#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/versions.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

// RFC 0006's streaming, the data half, in RFC 0005's deterministic mode.

namespace {

using vx::BlockState;
using vx::ChunkEvent;
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

// The placeholder, noting each plan and build it is asked for. The
// deterministic mode runs every job on the test's thread.
class CountingGenerator final : public vx::Generator {
public:
    explicit CountingGenerator(const vx::Generator& inner) : inner_(&inner) {}

    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos column,
                                               vx::GenScratch& scratch) const override {
        return inner_->surface_range(column, scratch);
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override {
        return inner_->out_of_reach(pos);
    }
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& scratch) const override {
        planned.push_back(pos);
        return inner_->plan(pos, scratch);
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> plans,
                                         vx::GenScratch& scratch) const override {
        const ChunkPos pos = plans[13]->pos;
        built.push_back(pos);
        // Each neighbor's plan in RFC 0008's place: x fastest, then z, then y.
        for (std::int32_t at = 0; at < 27; ++at) {
            const vx::ChunkPlan* plan = plans[static_cast<std::size_t>(at)];
            if (at == 13 || plan == nullptr) {
                continue;
            }
            const ChunkPos place{.x = pos.x + (at % 3) - 1,
                                 .y = pos.y + (at / 9) - 1,
                                 .z = pos.z + ((at / 3) % 3) - 1};
            ++neighbor_plans;
            misplaced_plans += plan->pos == place ? 0U : 1U;
        }
        return inner_->build(plans, scratch);
    }

    mutable std::vector<ChunkPos> planned;
    mutable std::vector<ChunkPos> built;
    mutable std::size_t neighbor_plans = 0;
    mutable std::size_t misplaced_plans = 0;

private:
    const vx::Generator* inner_;
};

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

std::size_t count(const WorldStreamer& streamer, ChunkState state) {
    return streamer.stats().chunks[static_cast<std::size_t>(state)];
}

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
        switch (record.state) {
        case ChunkState::planned:
            REQUIRE((!record.loaded || record.planned_neighbors < 26));
            break;
        case ChunkState::build_queued:
            REQUIRE(record.loaded);
            REQUIRE(record.planned_neighbors == 26);
            break;
        case ChunkState::ready:
            REQUIRE(record.ready_neighbors < 26);
            break;
        case ChunkState::complete:
            REQUIRE(record.ready_neighbors == 26);
            break;
        default:
            break;
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

std::size_t count_events(const std::vector<ChunkEvent>& events, ChunkEvent::Kind kind) {
    std::size_t total = 0;
    for (const ChunkEvent& event : events) {
        if (event.kind == kind) {
            ++total;
        }
    }
    return total;
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

// Steps until the jobs fed for loaded chunks in `state` have run, their
// results waiting in the inbox, and returns the westmost such chunk.
std::optional<ChunkPos> until_ran(WorldStreamer& streamer, vx::JobSystem& jobs, ChunkState state) {
    for (int pass = 0; pass < 100; ++pass) {
        streamer.feed();
        std::optional<ChunkPos> westmost;
        const vx::ChunkRecords& records = streamer.records();
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index) && records[index].state == state && records[index].loaded &&
                (!westmost || records[index].pos.x < westmost->x)) {
                westmost = records[index].pos;
            }
        }
        jobs.run_until_idle();
        if (westmost) {
            return westmost;
        }
        streamer.integrate();
    }
    return std::nullopt;
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

TEST_CASE("over the placeholder hills, the region streams in and every drawn chunk is complete") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    const vx::StreamingStats stats = streamer.stats();
    CHECK(stats.surfaces_known == stats.columns);
    CHECK(count(streamer, ChunkState::queued) == 0);
    CHECK(count(streamer, ChunkState::planning) == 0);
    CHECK(count(streamer, ChunkState::build_queued) == 0);
    CHECK(count(streamer, ChunkState::building) == 0);
    // Loaded chunks are built, plan ring chunks planned, drawn ones complete.
    const vx::ChunkRecords& records = streamer.records();
    std::size_t drawn = 0;
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!records.live(index)) {
            continue;
        }
        const ChunkRecord& record = records[index];
        if (record.drawn) {
            ++drawn;
            CHECK(record.state == ChunkState::complete);
        } else if (record.loaded) {
            CHECK(vx::counts_as_ready(record.state));
        } else if (record.state != ChunkState::planned) {
            // In the plan ring: planned, or uniform, ready with no job.
            CHECK(vx::counts_as_ready(record.state));
            CHECK(record.blocks == nullptr);
        }
    }
    CHECK(drawn > 27);
    // The near cylinder, a circle of radius 1 a chunk up and down, is drawn
    // and complete.
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if ((dx * dx) + (dz * dz) > 1) {
                    continue;
                }
                const ChunkRecord* record = streamer.find(
                    {.x = above_hills.x + dx, .y = above_hills.y + dy, .z = above_hills.z + dz});
                REQUIRE(record != nullptr);
                CHECK(record->drawn);
                CHECK(record->state == ChunkState::complete);
            }
        }
    }
    // Every drawn chunk was shown once.
    std::vector<ChunkEvent> events;
    streamer.take_events(events);
    CHECK(count_events(events, ChunkEvent::Kind::shown) == drawn);
    CHECK(count_events(events, ChunkEvent::Kind::hidden) == 0);
}

TEST_CASE("a near cylinder wider than the terrain disc streams in whole") {
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
    CHECK(count(streamer, ChunkState::queued) == 0);
    for (std::int32_t dz = -4; dz <= 4; ++dz) {
        for (std::int32_t dx = -4; dx <= 4; ++dx) {
            if ((dx * dx) + (dz * dz) > 16) {
                continue;
            }
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                INFO("chunk ", dx, ", ", dy, ", ", dz, " from the camera's");
                const ChunkRecord* record = streamer.find(
                    {.x = above_hills.x + dx, .y = above_hills.y + dy, .z = above_hills.z + dz});
                REQUIRE(record != nullptr);
                CHECK(record->drawn);
                CHECK(record->state == ChunkState::complete);
            }
        }
    }
}

TEST_CASE("one result at a time, a chunk is build-queued exactly at its 26th neighbor's plan and "
          "complete exactly at its 26th ready neighbor") {
    // One job slot: each step runs one job, and the rules are checked after
    // each.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({.slots = 1});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    // The camera's chunk: the step its last neighbor's plan lands and the
    // step it is build-queued; the same for ready neighbors and complete.
    int step_planned = -1;
    int step_build_queued = -1;
    int step_all_ready = -1;
    int step_complete = -1;
    for (int pass = 0; pass < 10'000 && step(streamer, jobs); ++pass) {
        check_rules(streamer);
        const ChunkRecord* own = streamer.find(above_hills);
        if (own == nullptr) {
            continue;
        }
        if (step_planned < 0 && own->planned_neighbors == 26) {
            step_planned = pass;
        }
        if (step_build_queued < 0 && own->state >= ChunkState::build_queued) {
            step_build_queued = pass;
        }
        if (step_all_ready < 0 && own->ready_neighbors == 26) {
            step_all_ready = pass;
        }
        if (step_complete < 0 && own->state == ChunkState::complete) {
            step_complete = pass;
        }
    }
    REQUIRE(step_planned >= 0);
    CHECK(step_build_queued == step_planned);
    REQUIRE(step_all_ready >= 0);
    CHECK(step_complete >= step_all_ready);
    CHECK(streamer.find(above_hills)->state == ChunkState::complete);
}

TEST_CASE("uniform chunks never create jobs") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    const CountingGenerator generator(placeholder);
    // One job at a time: surfaces arrive one by one, so chunks are made
    // before every surface around them is known.
    vx::JobSystem jobs({.slots = 1});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    // High above the hills, where the near cylinder is all air.
    streamer.set_camera({.x = 3, .y = 40, .z = 3});
    settle(streamer, jobs);
    vx::GenScratch scratch;
    for (const ChunkPos pos : generator.planned) {
        const vx::ChunkRange surface = placeholder.surface_range({.x = pos.x, .z = pos.z}, scratch);
        CHECK_FALSE(placeholder.out_of_reach(pos).has_value());
        CHECK(pos.y <= surface.max_y);
    }
    CHECK(generator.built.size() <= generator.planned.size());
    // The near cylinder around the camera has records, all uniform air.
    const ChunkRecord* own = streamer.find({.x = 3, .y = 40, .z = 3});
    REQUIRE(own != nullptr);
    CHECK(own->state == ChunkState::complete);
    CHECK(own->blocks == nullptr);
    CHECK(own->uniform == BlockState::air);
    CHECK_FALSE(generator.planned.empty());
}

TEST_CASE("a build reads every neighbor's plan, each in its place") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    const CountingGenerator generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    REQUIRE_FALSE(generator.built.empty());
    // With the camera still, the neighbors with plans now had them then;
    // the others are uniform, and builds read a null plan for them.
    std::size_t with_plans = 0;
    const vx::ChunkRecords& records = streamer.records();
    for (const ChunkPos pos : generator.built) {
        records.for_neighbors(pos, [&](std::uint32_t neighbor) {
            with_plans += records[neighbor].plan != nullptr ? 1U : 0U;
        });
    }
    CHECK(with_plans > 0);
    CHECK(generator.neighbor_plans == with_plans);
    CHECK(generator.misplaced_plans == 0);
}

TEST_CASE("a chunk that leaves while its plan job runs is freed when the job reports back") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    const std::optional<ChunkPos> planning = until_ran(streamer, jobs, ChunkState::planning);
    REQUIRE(planning.has_value());
    // Its plan ran: too late to cancel, so it waits for the result.
    streamer.set_camera({.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z});
    REQUIRE(streamer.find(*planning) != nullptr);
    CHECK(streamer.find(*planning)->state == ChunkState::unloading);
    check_rules(streamer);
    streamer.integrate();
    CHECK(streamer.find(*planning) == nullptr);
    CHECK(count(streamer, ChunkState::unloading) == 0);
    CHECK(streamer.stats().plan_versions == 0);
    settle(streamer, jobs);
    CHECK(streamer.find(above_hills) == nullptr);
}

TEST_CASE("a chunk that leaves before its plan job starts is freed at once, its job canceled") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    // Step until plans are fed, and stop before they run.
    for (int pass = 0; pass < 100 && count(streamer, ChunkState::planning) == 0; ++pass) {
        streamer.feed();
        if (count(streamer, ChunkState::planning) == 0) {
            jobs.run_until_idle();
            streamer.integrate();
        }
    }
    REQUIRE(count(streamer, ChunkState::planning) > 0);
    streamer.set_camera({.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z});
    // Every queued job was canceled: nothing is left to wait for.
    CHECK(streamer.records().size() == 0);
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
    settle(streamer, jobs);
}

TEST_CASE("a chunk that comes back before its plan job reports keeps the job's plan") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    const std::optional<ChunkPos> planning = until_ran(streamer, jobs, ChunkState::planning);
    REQUIRE(planning.has_value());
    // Eight columns east of it the chunk is outside the region grown for
    // hysteresis, while the surfaces it needs stay known.
    streamer.set_camera({.x = planning->x + 8, .y = above_hills.y, .z = above_hills.z});
    REQUIRE(streamer.find(*planning) != nullptr);
    CHECK(streamer.find(*planning)->state == ChunkState::unloading);
    streamer.set_camera(above_hills);
    REQUIRE(streamer.find(*planning) != nullptr);
    CHECK(streamer.find(*planning)->state == ChunkState::planning);
    check_rules(streamer);
    streamer.integrate();
    REQUIRE(streamer.find(*planning) != nullptr);
    CHECK(vx::counts_as_planned(streamer.find(*planning)->state));
    CHECK(streamer.find(*planning)->plan != nullptr);
    settle(streamer, jobs);
}

TEST_CASE("a chunk that leaves while its build runs is freed when the job reports back") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    const std::optional<ChunkPos> built = until_ran(streamer, jobs, ChunkState::building);
    REQUIRE(built.has_value());
    // Its build ran: too late to cancel, so it waits for the result.
    streamer.set_camera({.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z});
    REQUIRE(streamer.find(*built) != nullptr);
    CHECK(streamer.find(*built)->state == ChunkState::unloading);
    check_rules(streamer);
    streamer.integrate();
    CHECK(streamer.find(*built) == nullptr);
    CHECK(count(streamer, ChunkState::unloading) == 0);
    settle(streamer, jobs);
    CHECK(streamer.find(above_hills) == nullptr);
}

TEST_CASE("a chunk that leaves before its build starts is freed at once, its job canceled") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    // Step until builds are queued, and stop before they run.
    std::optional<ChunkPos> queued;
    bool saw_planning = false;
    for (int pass = 0; pass < 100 && !queued; ++pass) {
        streamer.feed();
        // A chunk whose plan job is fed is planning until the job reports.
        saw_planning = saw_planning || count(streamer, ChunkState::planning) > 0;
        const vx::ChunkRecords& records = streamer.records();
        for (std::uint32_t index = 0; index < records.end_index() && !queued; ++index) {
            if (records.live(index) && records[index].state == ChunkState::building) {
                queued = records[index].pos;
            }
        }
        if (!queued) {
            jobs.run_until_idle();
            streamer.integrate();
        }
    }
    REQUIRE(queued.has_value());
    CHECK(saw_planning);
    REQUIRE(streamer.stats().jobs_in_flight > 0);
    streamer.set_camera({.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z});
    // Every queued job was canceled: nothing is left to wait for.
    CHECK(streamer.find(*queued) == nullptr);
    CHECK(count(streamer, ChunkState::unloading) == 0);
    CHECK(streamer.stats().jobs_in_flight == 0);
    check_rules(streamer);
    settle(streamer, jobs);
}

TEST_CASE("a streamer destroyed with jobs queued or results unread cancels and unpins them all") {
    // Debug builds also assert, as each streamer goes, that no job record
    // and no pin is left, and the job system that no job is.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    for (int round = 0; round < 2; ++round) {
        const bool ran = round == 1;
        INFO("results unread: ", ran);
        {
            WorldStreamer streamer(generator, jobs, wake, small);
            streamer.set_camera(above_hills);
            for (int pass = 0; pass < 100 && count(streamer, ChunkState::building) == 0; ++pass) {
                streamer.feed();
                if (count(streamer, ChunkState::building) == 0) {
                    jobs.run_until_idle();
                    streamer.integrate();
                }
            }
            REQUIRE(count(streamer, ChunkState::building) > 0);
            if (ran) {
                jobs.run_until_idle();
            }
        }
        REQUIRE(jobs.queued(vx::JobLevel::normal) == 0);
    }
}

TEST_CASE("a chunk that comes back before its job reports keeps the job's result") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    const std::optional<ChunkPos> built = until_ran(streamer, jobs, ChunkState::building);
    REQUIRE(built.has_value());
    // Eight columns east of it the chunk is outside the region grown for
    // hysteresis, while the surfaces it needs stay known.
    streamer.set_camera({.x = built->x + 8, .y = above_hills.y, .z = above_hills.z});
    REQUIRE(streamer.find(*built) != nullptr);
    CHECK(streamer.find(*built)->state == ChunkState::unloading);
    streamer.set_camera(above_hills);
    REQUIRE(streamer.find(*built) != nullptr);
    CHECK(streamer.find(*built)->state == ChunkState::building);
    check_rules(streamer);
    streamer.integrate();
    REQUIRE(streamer.find(*built) != nullptr);
    CHECK(vx::counts_as_ready(streamer.find(*built)->state));
    settle(streamer, jobs);
}

TEST_CASE("a complete chunk goes back to ready when a neighbor unloads") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    std::vector<ChunkPos> complete;
    const vx::ChunkRecords& records = streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index) && records[index].state == ChunkState::complete) {
            complete.push_back(records[index].pos);
        }
    }
    // Four chunks east: the far west edge leaves, and chunks next to it stay.
    streamer.set_camera({.x = above_hills.x + 4, .y = above_hills.y, .z = above_hills.z});
    check_rules(streamer);
    std::size_t back_to_ready = 0;
    for (const ChunkPos pos : complete) {
        const ChunkRecord* record = streamer.find(pos);
        if (record != nullptr && record->state == ChunkState::ready) {
            ++back_to_ready;
            CHECK(record->ready_neighbors < 26);
        }
    }
    CHECK(back_to_ready > 0);
}

TEST_CASE("at the world's edges every drawn chunk completes, with nothing past them to wait for") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    // A corner of the world, a chunk in from its west edge, and its top.
    const std::array<ChunkPos, 3> cameras = {
        ChunkPos{.x = vx::highest_chunk, .y = above_hills.y, .z = vx::lowest_chunk},
        ChunkPos{.x = vx::lowest_chunk + 1, .y = above_hills.y, .z = above_hills.z},
        ChunkPos{.x = above_hills.x, .y = vx::highest_chunk, .z = above_hills.z}};
    for (const ChunkPos camera : cameras) {
        INFO("camera at chunk ", camera.x, ", ", camera.y, ", ", camera.z);
        WorldStreamer streamer(generator, jobs, wake, small);
        streamer.set_camera(camera);
        settle(streamer, jobs);
        const ChunkRecord* own = streamer.find(camera);
        REQUIRE(own != nullptr);
        CHECK(own->drawn);
        const vx::ChunkRecords& records = streamer.records();
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index) && records[index].drawn) {
                CHECK(records[index].state == ChunkState::complete);
            }
        }
    }
}

TEST_CASE("a corner edit changes 8 chunks and an inner one 1") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    std::vector<ChunkEvent> events;
    streamer.take_events(events);
    // The camera's chunk starts at block (96, 64, 96).
    const vx::BlockPos corner{.x = 96, .y = 64, .z = 96};
    REQUIRE(streamer.set_block(corner, stone));
    streamer.take_events(events);
    CHECK(count_events(events, ChunkEvent::Kind::changed) == 8);
    CHECK(streamer.block(corner) == stone);
    // The far corner reaches the 7 neighbors on the other sides.
    REQUIRE(streamer.set_block({.x = 127, .y = 95, .z = 127}, stone));
    streamer.take_events(events);
    CHECK(count_events(events, ChunkEvent::Kind::changed) == 8);
    const std::uint32_t first_version = streamer.find(above_hills)->version;
    REQUIRE(streamer.set_block({.x = 100, .y = 70, .z = 100}, stone));
    streamer.take_events(events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ChunkEvent::Kind::changed);
    CHECK(events[0].pos == above_hills);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    CHECK(own->edited);
    CHECK(own->version > first_version);
    // Only ready chunks take edits.
    CHECK_FALSE(streamer.set_block({.x = 100'000, .y = 70, .z = 100}, stone));
}

TEST_CASE("an edited chunk survives leaving the region, and shows again when the camera returns") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    const vx::BlockPos inside{.x = 100, .y = 70, .z = 100};
    REQUIRE(streamer.set_block(inside, stone));
    std::vector<ChunkEvent> events;
    streamer.take_events(events);
    streamer.set_camera({.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z});
    settle(streamer, jobs);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    CHECK(own->edited);
    CHECK_FALSE(own->drawn);
    CHECK(streamer.block(inside) == stone);
    streamer.take_events(events);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    CHECK(streamer.find(above_hills)->state == ChunkState::complete);
    CHECK(streamer.block(inside) == stone);
}

TEST_CASE("with worker threads the region streams in as in the deterministic mode") {
    // CI's sanitizers watch what the jobs read on the workers.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem alone({});
    vx::JobSystem workers({.workers = 3});
    vx::WakeEvent wake;
    WorldStreamer expected(generator, alone, wake, small);
    WorldStreamer streamer(generator, workers, wake, small);
    expected.set_camera(above_hills);
    settle(expected, alone);
    streamer.set_camera(above_hills);
    for (int pass = 0; pass < 1'000; ++pass) {
        streamer.feed();
        if (streamer.stats().jobs_in_flight == 0) {
            break;
        }
        // Every job reports back, in no set order, before the next pass.
        workers.wait_until_idle();
        streamer.integrate();
        check_rules(streamer);
    }
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(streamer.stats().chunks == expected.stats().chunks);
    CHECK(streamer.records().size() == expected.records().size());
}

TEST_CASE("fed with room for a few jobs, the streamer queues no more, and still streams in") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    streamer.feed(5);
    CHECK(streamer.stats().jobs_in_flight == 5);
    CHECK(jobs.queued(vx::JobLevel::normal) == 5);
    streamer.feed(0);
    CHECK(jobs.queued(vx::JobLevel::normal) == 5);
    for (int pass = 0; pass < 10'000 && streamer.stats().jobs_in_flight > 0; ++pass) {
        jobs.run_until_idle();
        streamer.integrate();
        streamer.feed(5);
        check_rules(streamer);
        REQUIRE(streamer.stats().jobs_in_flight <= 5);
    }
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(count(streamer, ChunkState::queued) == 0);
    CHECK(streamer.stats().surfaces_known == streamer.stats().columns);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    CHECK(own->state == ChunkState::complete);
}

TEST_CASE("a mesh job's pins hold a complete chunk's neighborhood as it was") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    // Only a complete chunk is meshed: one with no record is not.
    CHECK(streamer.pin_neighborhood(
              {.x = above_hills.x + 40, .y = above_hills.y, .z = above_hills.z}, 1) == nullptr);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    REQUIRE(own->state == ChunkState::complete);
    vx::JobRecord* pins = streamer.pin_neighborhood(above_hills, 7);
    REQUIRE(pins != nullptr);
    CHECK(pins->load_id == own->load_id);
    CHECK(pins->sequence == 7);
    // In a build's order, x fastest, then z, then y: [13] is the chunk.
    for (std::int32_t at = 0; at < 27; ++at) {
        const ChunkPos pos{.x = above_hills.x + (at % 3) - 1,
                           .y = above_hills.y + (at / 9) - 1,
                           .z = above_hills.z + ((at / 3) % 3) - 1};
        INFO("neighbor ", at);
        const ChunkRecord* record = streamer.find(pos);
        REQUIRE(record != nullptr);
        const auto index = static_cast<std::size_t>(at);
        CHECK(pins->chunks[index] == record->blocks);
        if (record->blocks == nullptr) {
            CHECK(pins->uniform[index] == record->uniform);
        }
    }
    // An edit while pinned writes a copy: the job keeps reading the old
    // blocks, which go once released.
    const vx::Version<vx::ChunkStorage>* before = pins->chunks[13];
    REQUIRE(before != nullptr);
    REQUIRE(streamer.set_block({.x = 100, .y = 70, .z = 100}, stone));
    CHECK(streamer.find(above_hills)->blocks != before);
    CHECK(pins->chunks[13] == before);
    const std::size_t versions = streamer.stats().block_versions;
    streamer.release(*pins);
    CHECK(streamer.stats().block_versions == versions - 1);
}

TEST_CASE("Tracy's counts: results waiting, pins on plans and chunks, and copies on write") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    // Each job's result waits from when it ran until integrate takes it.
    streamer.feed();
    const std::uint32_t queued = jobs.queued(vx::JobLevel::normal);
    REQUIRE(queued > 0);
    CHECK(streamer.results_waiting() == 0);
    jobs.run_until_idle();
    CHECK(streamer.results_waiting() == queued);
    streamer.integrate();
    CHECK(streamer.results_waiting() == 0);
    // A queued build pins its neighbors' plans.
    for (int pass = 0; pass < 100 && count(streamer, ChunkState::building) == 0; ++pass) {
        streamer.feed();
        if (count(streamer, ChunkState::building) == 0) {
            jobs.run_until_idle();
            streamer.integrate();
        }
    }
    REQUIRE(count(streamer, ChunkState::building) > 0);
    CHECK(streamer.pins() > 0);
    settle(streamer, jobs);
    CHECK(streamer.pins() == 0);
    // A mesh job pins the blocks of each chunk around it that has any.
    vx::JobRecord* pins = streamer.pin_neighborhood(above_hills, 1);
    REQUIRE(pins != nullptr);
    const auto pinned = static_cast<std::uint64_t>(std::ranges::count_if(
        pins->chunks, [](const vx::Version<vx::ChunkStorage>* chunk) { return chunk != nullptr; }));
    REQUIRE(pinned > 0);
    CHECK(streamer.pins() == pinned);
    // An edit to a pinned chunk is a copy; once released, edits write in place.
    CHECK(streamer.copies() == 0);
    REQUIRE(streamer.set_block({.x = 100, .y = 70, .z = 100}, stone));
    CHECK(streamer.copies() == 1);
    streamer.release(*pins);
    CHECK(streamer.pins() == 0);
    REQUIRE(streamer.set_block({.x = 101, .y = 70, .z = 100}, stone));
    CHECK(streamer.copies() == 1);
}

TEST_CASE("canceling the queued jobs leaves none queued, and may run twice") {
    // Debug builds also assert, as the streamer goes, that every pin of the
    // canceled builds came back.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    for (int pass = 0; pass < 100 && count(streamer, ChunkState::building) == 0; ++pass) {
        streamer.feed();
        if (count(streamer, ChunkState::building) == 0) {
            jobs.run_until_idle();
            streamer.integrate();
        }
    }
    REQUIRE(count(streamer, ChunkState::building) > 0);
    REQUIRE(jobs.queued(vx::JobLevel::normal) > 0);
    streamer.cancel_queued();
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
    streamer.cancel_queued();
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
}
