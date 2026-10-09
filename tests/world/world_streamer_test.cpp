#include <vx/core/frustum.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/vec3d.hpp>
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
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

// Takes in every result the jobs sent, however many steps that takes.
void integrate_all(vx::WorldStreamer& streamer) {
    while (streamer.integrate()) {
    }
}

// One pass of the world loop: feed, run what was fed, integrate it. False
// once nothing was left to run.
bool step(WorldStreamer& streamer, vx::JobSystem& jobs) {
    streamer.feed();
    if (streamer.stats().jobs_in_flight == 0) {
        return false;
    }
    jobs.run_until_idle();
    integrate_all(streamer);
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
    const bool regenerating = streamer.stats().regenerating > 0;
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
            // While regenerating, it also waits for its neighbors' new plans.
            REQUIRE((!record.loaded || record.planned_neighbors < 26 || regenerating));
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
        integrate_all(streamer);
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
    integrate_all(streamer);
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
    integrate_all(streamer);
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
            integrate_all(streamer);
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
    integrate_all(streamer);
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
    integrate_all(streamer);
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
            integrate_all(streamer);
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
                    integrate_all(streamer);
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
    integrate_all(streamer);
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
        integrate_all(streamer);
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
        integrate_all(streamer);
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
            integrate_all(streamer);
        }
    }
    REQUIRE(count(streamer, ChunkState::building) > 0);
    REQUIRE(jobs.queued(vx::JobLevel::normal) > 0);
    streamer.cancel_queued();
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
    streamer.cancel_queued();
    CHECK(jobs.queued(vx::JobLevel::normal) == 0);
}

namespace {

// A viewer at the chunk's middle looking along +x, with RFC 0001's 70
// degree vertical field of view on a 16:9 screen.
vx::Viewer looking_east(ChunkPos chunk) {
    vx::Viewer viewer = vx::viewer_at(chunk);
    viewer.forward = {.x = 1.0, .y = 0.0, .z = 0.0};
    viewer.view = vx::Frustum({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = 1.0},
                              {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7002075382097097, 16.0 / 9.0, 0.05);
    return viewer;
}

} // namespace

TEST_CASE("chunks in view are built before nearer ones behind the camera") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    const CountingGenerator generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_viewer(looking_east(above_hills));
    settle(streamer, jobs);
    // Three chunks ahead scores 9, two behind 4 doubled twice, 16: the one
    // ahead is built first, at every height where both are.
    const auto built_at = [&](ChunkPos pos) -> std::optional<std::size_t> {
        const auto at = std::ranges::find(generator.built, pos);
        if (at == generator.built.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(at - generator.built.begin());
    };
    std::size_t compared = 0;
    for (std::int32_t y = above_hills.y - 3; y <= above_hills.y + 3; ++y) {
        const std::optional<std::size_t> ahead =
            built_at({.x = above_hills.x + 3, .y = y, .z = above_hills.z});
        const std::optional<std::size_t> behind =
            built_at({.x = above_hills.x - 2, .y = y, .z = above_hills.z});
        if (ahead && behind) {
            INFO("height ", y);
            CHECK(*ahead < *behind);
            ++compared;
        }
    }
    CHECK(compared > 0);
}

TEST_CASE("a new chunk or a turn of more than 15 degrees ranks the waiting jobs again") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    vx::Viewer viewer = looking_east(above_hills);
    CHECK(streamer.set_viewer(viewer));
    CHECK_FALSE(streamer.set_viewer(viewer));
    // 10 degrees: the scores stand. 20 degrees from the view they were made
    // for: ranked again.
    const auto turned = [](double degrees) {
        const double radians = degrees * 3.141592653589793 / 180.0;
        return vx::Vec3d{.x = std::cos(radians), .y = 0.0, .z = std::sin(radians)};
    };
    viewer.forward = turned(10.0);
    CHECK_FALSE(streamer.set_viewer(viewer));
    viewer.forward = turned(20.0);
    CHECK(streamer.set_viewer(viewer));
    // A step to the next chunk, looking the same way.
    viewer.chunk.x += 1;
    CHECK(streamer.set_viewer(viewer));
}

namespace {

// No terrain at all: every chunk out of reach and air, every column without
// a surface, so a wide region costs its columns' surface jobs and little else.
class NoTerrain final : public vx::Generator {
public:
    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos /*column*/,
                                               vx::GenScratch& /*scratch*/) const override {
        return {};
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos /*pos*/) const noexcept override {
        return BlockState::air;
    }
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& /*scratch*/) const override {
        return {.pos = pos};
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> /*plans*/,
                                         vx::GenScratch& /*scratch*/) const override {
        return vx::ChunkStorage::uniform(BlockState::air);
    }
};

// Each reading moves it on by a microsecond: a result costs one.
class TickingClock final : public vx::JobClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        at += std::chrono::microseconds(1);
        return at;
    }

    mutable std::chrono::steady_clock::time_point at{};
};

} // namespace

TEST_CASE("a burst of results is taken in by steps of half a millisecond and one result at most") {
    const NoTerrain generator;
    vx::JobSystem jobs({.slots = 16'384});
    vx::WakeEvent wake;
    const vx::RegionSettings wide{
        .view_distance = 60, .near_distance = 1, .near_height = 1, .hysteresis = 2};
    WorldStreamer streamer(generator, jobs, wake, wide);
    const TickingClock clock;
    streamer.set_clock(clock);
    streamer.set_camera(above_hills);
    streamer.feed();
    jobs.run_until_idle();
    const std::size_t burst = streamer.stats().jobs_in_flight;
    REQUIRE(burst >= 10'000);
    REQUIRE(streamer.results_waiting() == burst);
    std::size_t steps = 0;
    for (bool left = true; left; ++steps) {
        const std::size_t before = streamer.results_waiting();
        const std::chrono::steady_clock::time_point started = clock.at;
        left = streamer.integrate();
        const std::size_t taken = before - streamer.results_waiting();
        INFO("step ", steps);
        // 0.5 ms of results, the last of which went past it: 500 here.
        CHECK(taken <= 501);
        CHECK(clock.at - started <= std::chrono::microseconds(502));
        if (left) {
            CHECK(taken == 500);
        }
    }
    CHECK(streamer.results_waiting() == 0);
    CHECK(streamer.stats().jobs_in_flight == 0);
    CHECK(streamer.stats().surfaces_known == streamer.stats().columns);
    CHECK(steps == (burst + 499) / 500);
}

namespace {

// The placeholder's hills at version 0; at version 1 every chunk it builds is
// solid stone, so new blocks are told from old. Notes each chunk built at 1.
class VersionedTerrain final : public vx::Generator {
public:
    explicit VersionedTerrain(const vx::Generator& inner) : inner_(&inner) {}

    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos column,
                                               vx::GenScratch& scratch) const override {
        return inner_->surface_range(column, scratch);
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override {
        return inner_->out_of_reach(pos);
    }
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& scratch) const override {
        return inner_->plan(pos, scratch);
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> plans,
                                         vx::GenScratch& scratch) const override {
        if (version_ == 0) {
            return inner_->build(plans, scratch);
        }
        rebuilt.push_back(plans[13]->pos);
        return vx::ChunkStorage::uniform(stone);
    }
    [[nodiscard]] std::uint32_t version() const noexcept override { return version_; }

    std::uint32_t version_ = 0;
    mutable std::vector<ChunkPos> rebuilt;

private:
    const vx::Generator* inner_;
};

// The chunks holding blocks: those built, and not uniform.
std::vector<ChunkPos> with_blocks(const WorldStreamer& streamer) {
    std::vector<ChunkPos> out;
    const vx::ChunkRecords& records = streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index) && records[index].blocks != nullptr) {
            out.push_back(records[index].pos);
        }
    }
    return out;
}

} // namespace

TEST_CASE("a new generator version regenerates every chunk nearest first, and nothing leaves "
          "the screen meanwhile") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    VersionedTerrain generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    std::vector<ChunkEvent> events;
    streamer.take_events(events);
    const std::vector<ChunkPos> built = with_blocks(streamer);
    REQUIRE(built.size() > 27);
    const std::size_t records = streamer.records().size();
    generator.version_ = 1;
    settle(streamer, jobs);
    CHECK(streamer.stats().regenerating == 0);
    // Shown chunks kept their meshes until new blocks came: changed, never
    // hidden, and no record came or went.
    streamer.take_events(events);
    CHECK(count_events(events, ChunkEvent::Kind::hidden) == 0);
    CHECK(count_events(events, ChunkEvent::Kind::freed) == 0);
    CHECK(count_events(events, ChunkEvent::Kind::changed) > built.size());
    CHECK(streamer.records().size() == records);
    // Every chunk built before holds the new version's blocks.
    for (const ChunkPos pos : built) {
        const ChunkRecord* record = streamer.find(pos);
        REQUIRE(record != nullptr);
        INFO("chunk ", pos.x, ", ", pos.y, ", ", pos.z);
        CHECK(record->blocks == nullptr);
        CHECK(record->uniform == stone);
        CHECK(record->state == (record->drawn ? ChunkState::complete : record->state));
    }
    CHECK(streamer.stats().block_versions == 0);
    // Nearest first: the camera's chunk before any chunk three or more away.
    const auto camera_at = std::ranges::find(generator.rebuilt, above_hills);
    REQUIRE(camera_at != generator.rebuilt.end());
    for (auto at = generator.rebuilt.begin(); at != generator.rebuilt.end(); ++at) {
        const std::int32_t far =
            std::max({std::abs(at->x - above_hills.x), std::abs(at->y - above_hills.y),
                      std::abs(at->z - above_hills.z)});
        if (far >= 3) {
            CHECK(at > camera_at);
        }
    }
}

TEST_CASE("an edited chunk keeps its edit through a regeneration, its neighbors regenerate") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    VersionedTerrain generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    settle(streamer, jobs);
    constexpr BlockState glass = static_cast<BlockState>(7);
    const vx::BlockPos inside{.x = 100, .y = 70, .z = 100};
    REQUIRE(streamer.set_block(inside, glass));
    generator.version_ = 1;
    settle(streamer, jobs);
    CHECK(streamer.block(inside) == glass);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    CHECK(own->blocks != nullptr);
    CHECK(std::ranges::find(generator.rebuilt, above_hills) == generator.rebuilt.end());
    const ChunkRecord* east =
        streamer.find({.x = above_hills.x + 1, .y = above_hills.y, .z = above_hills.z});
    REQUIRE(east != nullptr);
    CHECK(east->uniform == stone);
}

TEST_CASE("results made for an older version are dropped, and their chunks start over") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    VersionedTerrain generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    streamer.set_camera(above_hills);
    // Surfaces, then plans, taken in; the builds of version 0 wait in the
    // inbox when the version changes.
    for (int pass = 0; pass < 3; ++pass) {
        streamer.feed();
        jobs.run_until_idle();
        if (pass < 2) {
            integrate_all(streamer);
        }
    }
    REQUIRE(count(streamer, ChunkState::building) > 0);
    REQUIRE(streamer.results_waiting() > 0);
    generator.version_ = 1;
    settle(streamer, jobs);
    // Nothing of version 0 was kept: every chunk built is the new stone.
    CHECK(streamer.stats().block_versions == 0);
    CHECK_FALSE(generator.rebuilt.empty());
    CHECK(count(streamer, ChunkState::queued) == 0);
    const ChunkRecord* own = streamer.find(above_hills);
    REQUIRE(own != nullptr);
    CHECK(own->state == ChunkState::complete);
    CHECK(own->uniform == stone);
}

namespace {

// Plans carry the version they were made at, and a build's blocks say which
// version its plans were: uniform 100 plus the version, or 99 when the 27
// plans were of different versions. Deep rock needs plans too, as RFC
// 0008's caves will, so the near cylinder underground is built.
class PlanStamps final : public vx::Generator {
public:
    explicit PlanStamps(const vx::Generator& inner) : inner_(&inner) {}

    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos column,
                                               vx::GenScratch& scratch) const override {
        return inner_->surface_range(column, scratch);
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override {
        const std::optional<BlockState> inner = inner_->out_of_reach(pos);
        return inner == stone ? std::nullopt : inner;
    }
    // The version rides in z's high bits: streaming never reads a plan.
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& /*scratch*/) const override {
        return {.pos = {.x = pos.x,
                        .y = pos.y,
                        .z = pos.z + (static_cast<std::int32_t>(version_.load()) * stamp)}};
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> plans,
                                         vx::GenScratch& /*scratch*/) const override {
        const std::int32_t own = stamp_of(*plans[13]);
        for (const vx::ChunkPlan* plan : plans) {
            if (plan != nullptr && stamp_of(*plan) != own) {
                ++mixed;
                return vx::ChunkStorage::uniform(mixed_plans);
            }
        }
        return vx::ChunkStorage::uniform(built_at(static_cast<std::uint32_t>(own)));
    }
    [[nodiscard]] std::uint32_t version() const noexcept override { return version_.load(); }

    static constexpr BlockState mixed_plans = static_cast<BlockState>(99);
    static constexpr BlockState built_at(std::uint32_t version) {
        return static_cast<BlockState>(100 + version);
    }

    std::atomic<std::uint32_t> version_{0}; // up to 7
    mutable std::atomic<std::size_t> mixed{0};

private:
    static constexpr std::int32_t stamp = 1 << 27; // past any chunk's z
    static std::int32_t stamp_of(const vx::ChunkPlan& plan) {
        return (plan.pos.z + (stamp / 2)) / stamp;
    }

    const vx::Generator* inner_;
};

// Ready chunks that are neither air nor what version `version` builds.
std::size_t not_built_at(const WorldStreamer& streamer, std::uint32_t version) {
    std::size_t stale = 0;
    const vx::ChunkRecords& records = streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index) && vx::counts_as_ready(records[index].state) &&
            records[index].uniform != BlockState::air &&
            records[index].uniform != PlanStamps::built_at(version)) {
            ++stale;
        }
    }
    return stale;
}

} // namespace

TEST_CASE("a chunk that leaves while its rebuild runs and comes back after another version "
          "change regenerates from the newest plans") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    PlanStamps generator(placeholder);
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, small);
    // Underground, where the near cylinder's chunks are built.
    const ChunkPos deep{.x = 3, .y = -6, .z = 3};
    streamer.set_camera(deep);
    settle(streamer, jobs);
    REQUIRE(not_built_at(streamer, 0) == 0);
    generator.version_ = 1;
    // Until a rebuild of the near cylinder has run, its result unread.
    std::optional<ChunkPos> rebuilt;
    for (int pass = 0; pass < 100 && !rebuilt; ++pass) {
        streamer.feed();
        const vx::ChunkRecords& records = streamer.records();
        for (std::uint32_t index = 0; index < records.end_index() && !rebuilt; ++index) {
            const ChunkRecord& record = records[index];
            if (records.live(index) && record.regeneration == vx::Regeneration::rebuild &&
                record.job && record.pos.y <= deep.y + 1) {
                rebuilt = record.pos;
            }
        }
        jobs.run_until_idle();
        if (!rebuilt) {
            integrate_all(streamer);
        }
    }
    REQUIRE(rebuilt.has_value());
    // Up over the hills: it leaves while its job has run. The version
    // changes again, and the camera goes back down before the job reports.
    streamer.set_camera(above_hills);
    REQUIRE(streamer.find(*rebuilt) != nullptr);
    REQUIRE(streamer.find(*rebuilt)->state == ChunkState::unloading);
    generator.version_ = 2;
    streamer.feed();
    streamer.set_camera(deep);
    settle(streamer, jobs);
    CHECK(streamer.stats().regenerating == 0);
    CHECK(generator.mixed.load() == 0);
    CHECK(not_built_at(streamer, 2) == 0);
}

namespace {

// PlanStamps on workers, whose first build waits until the test lets it go:
// a build that runs on through a version change.
class HeldBuild final : public vx::Generator {
public:
    explicit HeldBuild(const PlanStamps& stamps) : stamps_(&stamps) {}

    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos column,
                                               vx::GenScratch& scratch) const override {
        return stamps_->surface_range(column, scratch);
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override {
        return stamps_->out_of_reach(pos);
    }
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& scratch) const override {
        return stamps_->plan(pos, scratch);
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> plans,
                                         vx::GenScratch& scratch) const override {
        {
            std::unique_lock guard(lock_);
            if (!held_) {
                held_ = ChunkPos{.x = plans[13]->pos.x, .y = plans[13]->pos.y, .z = 0};
                released_.wait(guard, [this] { return open_; });
            }
        }
        return stamps_->build(plans, scratch);
    }
    [[nodiscard]] std::uint32_t version() const noexcept override { return stamps_->version(); }

    // The held build's chunk, once one is held: x and y (z carries the
    // version).
    [[nodiscard]] std::optional<ChunkPos> held() const {
        const std::scoped_lock guard(lock_);
        return held_;
    }
    void release() {
        const std::scoped_lock guard(lock_);
        open_ = true;
        released_.notify_all();
    }

private:
    const PlanStamps* stamps_;
    mutable std::mutex lock_;
    mutable std::condition_variable released_;
    mutable std::optional<ChunkPos> held_;
    bool open_ = false;
};

// Passes until the held build is the only job left, or two seconds passed.
bool until_only_the_held_build(WorldStreamer& streamer) {
    for (int pass = 0; pass < 2'000; ++pass) {
        streamer.feed();
        integrate_all(streamer);
        if (streamer.stats().jobs_in_flight == 1 && streamer.results_waiting() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST_CASE("while a build of the old version runs, its neighbors do not read its plan") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    PlanStamps stamps(placeholder);
    HeldBuild generator(stamps);
    vx::JobSystem jobs({.workers = 2});
    vx::WakeEvent wake;
    {
        WorldStreamer streamer(generator, jobs, wake, small);
        streamer.set_camera(above_hills);
        REQUIRE(until_only_the_held_build(streamer));
        const std::optional<ChunkPos> held = generator.held();
        REQUIRE(held.has_value());
        const vx::ChunkRecords& records = streamer.records();
        std::optional<ChunkPos> building;
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index) && records[index].state == ChunkState::building) {
                building = records[index].pos;
            }
        }
        REQUIRE(building.has_value());
        CHECK(building->x == held->x);
        CHECK(building->y == held->y);
        // A new version while it runs: the rest regenerates around it.
        stamps.version_ = 1;
        const bool only_held = until_only_the_held_build(streamer);
        const std::size_t mixed_meanwhile = stamps.mixed.load();
        generator.release();
        for (int pass = 0; pass < 10'000; ++pass) {
            streamer.feed();
            if (streamer.stats().jobs_in_flight == 0) {
                break;
            }
            jobs.wait_until_idle();
            integrate_all(streamer);
        }
        CHECK(only_held);
        CHECK(mixed_meanwhile == 0);
        CHECK(stamps.mixed.load() == 0);
        CHECK(streamer.stats().regenerating == 0);
        CHECK(not_built_at(streamer, 1) == 0);
    }
    jobs.wait_until_idle();
}
