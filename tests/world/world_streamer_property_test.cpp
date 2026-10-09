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

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

// RFC 0006's streaming under a random camera walk with edits: the rules
// hold after every step, and once the jobs are done the records are exactly
// the region, written out from the region's functions and the generator.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ChunkRange;
using vx::ChunkRecord;
using vx::ChunkState;
using vx::ColumnPos;
using vx::ColumnRuns;
using vx::Region;
using vx::WorldStreamer;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr vx::PlaceholderBlocks blocks{
    .grass = static_cast<BlockState>(3), .dirt = static_cast<BlockState>(2), .stone = stone};

using Chunk = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

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

// The rules every record keeps: its counts are its neighbors' states, and
// its state follows from its counts and flags.
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
        REQUIRE((!record.drawn || record.loaded));
        if (record.state == ChunkState::build_queued) {
            REQUIRE(record.loaded);
            REQUIRE(record.planned_neighbors == 26);
        }
        if (record.state == ChunkState::planned && record.loaded) {
            REQUIRE(record.planned_neighbors < 26);
        }
        if (record.state == ChunkState::complete) {
            REQUIRE(record.ready_neighbors == 26);
        }
        if (record.state == ChunkState::ready) {
            REQUIRE(record.ready_neighbors < 26);
        }
    }
}

// Each column's terrain band, from the generator's surface ranges.
class Bands {
public:
    explicit Bands(const vx::Generator& generator) : generator_(&generator) {}

    ChunkRange band(ColumnPos column) {
        std::array<ChunkRange, 4> sides = {surface({.x = column.x + 1, .z = column.z}),
                                           surface({.x = column.x - 1, .z = column.z}),
                                           surface({.x = column.x, .z = column.z + 1}),
                                           surface({.x = column.x, .z = column.z - 1})};
        return vx::terrain_band(surface(column), sides);
    }

    // What `region` holds of the column: its planned, loaded and drawn runs.
    std::array<ColumnRuns, 3> runs(const Region& region, ColumnPos column) {
        std::array<ChunkRange, 25> around{};
        for (std::int32_t dz = -2; dz <= 2; ++dz) {
            for (std::int32_t dx = -2; dx <= 2; ++dx) {
                around[static_cast<std::size_t>(((dz + 2) * 5) + dx + 2)] =
                    band({.x = column.x + dx, .z = column.z + dz});
            }
        }
        std::array<ChunkRange, 9> middle{};
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                middle[static_cast<std::size_t>(((dz + 1) * 3) + dx + 1)] =
                    around[static_cast<std::size_t>(((dz + 2) * 5) + dx + 2)];
            }
        }
        return {region.planned(column, around), region.loaded(column, middle),
                region.drawn(column, around[12])};
    }

private:
    ChunkRange surface(ColumnPos column) {
        const auto [at, added] = surfaces_.try_emplace({column.x, column.z});
        if (added) {
            at->second = generator_->surface_range(column, scratch_);
        }
        return at->second;
    }

    const vx::Generator* generator_;
    vx::GenScratch scratch_;
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkRange> surfaces_;
};

// The chunks the events say are shown: shown adds one, hidden takes it
// away, and a freed record was hidden first. `known` holds the records seen
// at the last call: each is still there, or was reported freed.
void follow_events(WorldStreamer& streamer, std::set<std::uint32_t>& shown,
                   std::map<std::uint32_t, ChunkPos>& known) {
    std::vector<vx::ChunkEvent> events;
    streamer.take_events(events);
    for (const vx::ChunkEvent& event : events) {
        switch (event.kind) {
        case vx::ChunkEvent::Kind::shown:
            REQUIRE(shown.insert(event.record).second);
            break;
        case vx::ChunkEvent::Kind::hidden:
            REQUIRE(shown.erase(event.record) == 1);
            break;
        case vx::ChunkEvent::Kind::freed:
            REQUIRE_FALSE(shown.contains(event.record));
            // A record made and freed between two calls was never known.
            if (const auto at = known.find(event.record); at != known.end()) {
                REQUIRE(at->second == event.pos);
                known.erase(at);
            }
            break;
        case vx::ChunkEvent::Kind::changed:
            break;
        }
    }
    const vx::ChunkRecords& records = streamer.records();
    for (const auto& [index, pos] : known) {
        REQUIRE(records.live(index));
        REQUIRE(records[index].pos == pos);
    }
    known.clear();
    // Exactly the complete chunks in the drawn region.
    std::size_t complete_and_drawn = 0;
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!records.live(index)) {
            continue;
        }
        known.emplace(index, records[index].pos);
        if (records[index].drawn && records[index].state == ChunkState::complete) {
            ++complete_and_drawn;
            REQUIRE(shown.contains(index));
        }
    }
    REQUIRE(shown.size() == complete_and_drawn);
}

void settle(WorldStreamer& streamer, vx::JobSystem& jobs) {
    for (int pass = 0; pass < 10'000; ++pass) {
        streamer.feed();
        if (streamer.stats().jobs_in_flight == 0) {
            return;
        }
        jobs.run_until_idle();
        streamer.integrate();
        check_rules(streamer);
    }
    FAIL("streaming never settled");
}

// Each record's load id, by position: a record freed and made again has a
// new one.
std::map<Chunk, std::uint32_t> load_ids(const WorldStreamer& streamer) {
    std::map<Chunk, std::uint32_t> ids;
    const vx::ChunkRecords& records = streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index)) {
            const ChunkPos pos = records[index].pos;
            ids.emplace(Chunk{pos.x, pos.y, pos.z}, records[index].load_id);
        }
    }
    return ids;
}

} // namespace

TEST_CASE("under a random camera walk with edits, the rules hold at every step, and settled, the "
          "records are the region") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const vx::RegionSettings settings{
        .view_distance = 3, .near_distance = 2, .near_height = 1, .hysteresis = 2};
    vx::JobSystem jobs({.slots = 64});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, settings);
    Bands bands(generator);
    std::mt19937 random(0x5EED);
    std::uniform_int_distribution<int> roll(0, 99);
    std::uniform_int_distribution<std::int32_t> step(-3, 3);
    ChunkPos camera{.x = 3, .y = 2, .z = 3};
    std::set<Chunk> edited;
    std::set<std::uint32_t> shown;
    std::map<std::uint32_t, ChunkPos> known;
    std::size_t settled_checks = 0;
    std::size_t moves_with_jobs_queued = 0;
    for (int move = 0; move < 40; ++move) {
        // Mostly short moves, sometimes a jump, sometimes a climb.
        const int kind = roll(random);
        if (kind < 70) {
            camera.x += step(random);
            camera.z += step(random);
        } else if (kind < 85) {
            camera.x += 9 * (step(random) >= 0 ? 1 : -1);
        } else {
            camera.y += step(random);
        }
        streamer.set_camera(camera);
        check_rules(streamer);
        follow_events(streamer, shown, known);
        // A few passes of the world loop, or all of them; now and then the
        // camera moves while jobs wait in the queue, so their cancels win.
        const bool all = roll(random) < 40;
        const int passes = all ? 10'000 : roll(random) % 4;
        for (int pass = 0; pass < passes; ++pass) {
            streamer.feed();
            if (streamer.stats().jobs_in_flight == 0) {
                break;
            }
            if (!all && roll(random) < 50) {
                camera.x += step(random);
                camera.z += step(random);
                streamer.set_camera(camera);
                ++moves_with_jobs_queued;
                check_rules(streamer);
            }
            jobs.run_until_idle();
            streamer.integrate();
            check_rules(streamer);
            follow_events(streamer, shown, known);
        }
        // An edit next to the camera, where a chunk is ready.
        const vx::BlockPos target{.x = (camera.x * vx::chunk_size) + roll(random) % 32,
                                  .y = (camera.y * vx::chunk_size) + roll(random) % 32,
                                  .z = (camera.z * vx::chunk_size) + roll(random) % 32};
        if (roll(random) < 30 && streamer.set_block(target, stone)) {
            const ChunkPos chunk = vx::chunk_of(target);
            edited.emplace(chunk.x, chunk.y, chunk.z);
        }
        if (!all) {
            continue;
        }
        // Settled: the records are the region, written out.
        ++settled_checks;
        const Region region(camera, settings);
        const Region kept = region.grown(settings.hysteresis);
        std::set<Chunk> wanted;
        std::set<Chunk> allowed;
        const std::int32_t reach = settings.view_distance + settings.hysteresis + 2;
        for (std::int32_t z = camera.z - reach; z <= camera.z + reach; ++z) {
            for (std::int32_t x = camera.x - reach; x <= camera.x + reach; ++x) {
                const ColumnPos column{.x = x, .z = z};
                if (!kept.column_planned(column)) {
                    continue;
                }
                const std::array<ColumnRuns, 3> now = bands.runs(region, column);
                const std::array<ColumnRuns, 3> grown = bands.runs(kept, column);
                for (const ChunkRange& run : grown[0].runs()) {
                    for (std::int32_t y = run.min_y; y <= run.max_y; ++y) {
                        allowed.emplace(x, y, z);
                        const ChunkRecord* record = streamer.find({.x = x, .y = y, .z = z});
                        INFO("chunk ", x, ", ", y, ", ", z);
                        if (now[0].contains(y)) {
                            wanted.emplace(x, y, z);
                            REQUIRE(record != nullptr);
                        }
                        if (record == nullptr) {
                            continue;
                        }
                        // Flags: set inside the region, kept inside the grown one.
                        CHECK((!now[1].contains(y) || record->loaded));
                        CHECK((!now[2].contains(y) || record->drawn));
                        CHECK((!record->loaded || grown[1].contains(y)));
                        CHECK((!record->drawn || grown[2].contains(y)));
                        // Settled: drawn chunks are complete, loaded ones built.
                        if (record->drawn) {
                            INFO("state ", static_cast<int>(record->state), ", ready neighbors ",
                                 int{record->ready_neighbors});
                            CHECK(record->state == ChunkState::complete);
                        } else if (record->loaded) {
                            CHECK(vx::counts_as_ready(record->state));
                        }
                    }
                }
            }
        }
        // Nothing outside the grown region but edited chunks, and no blocks or
        // plans but the records': nothing reads a frozen version once settled.
        const vx::ChunkRecords& records = streamer.records();
        std::size_t with_blocks = 0;
        std::size_t with_plans = 0;
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (!records.live(index)) {
                continue;
            }
            const ChunkRecord& record = records[index];
            with_blocks += record.blocks != nullptr ? 1U : 0U;
            with_plans += record.plan != nullptr ? 1U : 0U;
            const Chunk at{record.pos.x, record.pos.y, record.pos.z};
            INFO("chunk ", record.pos.x, ", ", record.pos.y, ", ", record.pos.z);
            CHECK(record.state != ChunkState::unloading);
            CHECK((allowed.contains(at) || record.edited));
        }
        CHECK(streamer.stats().block_versions == with_blocks);
        CHECK(streamer.stats().plan_versions == with_plans);
        CHECK(wanted.size() > 50);
        // Every edited chunk is still there, with its edit.
        for (const auto& [x, y, z] : edited) {
            const ChunkRecord* record = streamer.find({.x = x, .y = y, .z = z});
            REQUIRE(record != nullptr);
            CHECK(record->edited);
        }
    }
    CHECK(settled_checks > 8);
    CHECK(moves_with_jobs_queued > 3);
    CHECK_FALSE(edited.empty());
    settle(streamer, jobs);
}

TEST_CASE("from a standing start the records are exactly the region, and crossing a chunk border "
          "back and forth loads and unloads nothing after the first time") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const vx::RegionSettings settings{
        .view_distance = 3, .near_distance = 2, .near_height = 1, .hysteresis = 2};
    vx::JobSystem jobs({});
    vx::WakeEvent wake;
    WorldStreamer streamer(generator, jobs, wake, settings);
    Bands bands(generator);
    const ChunkPos here{.x = 3, .y = 2, .z = 3};
    streamer.set_camera(here);
    settle(streamer, jobs);
    // Nothing kept yet: every flag and record is the region's, no more.
    const Region region(here, settings);
    const std::int32_t reach = settings.view_distance + 2;
    std::size_t planned = 0;
    for (std::int32_t z = here.z - reach; z <= here.z + reach; ++z) {
        for (std::int32_t x = here.x - reach; x <= here.x + reach; ++x) {
            const std::array<ColumnRuns, 3> runs = bands.runs(region, {.x = x, .z = z});
            for (const ChunkRange& run : runs[0].runs()) {
                for (std::int32_t y = run.min_y; y <= run.max_y; ++y) {
                    INFO("chunk ", x, ", ", y, ", ", z);
                    const ChunkRecord* record = streamer.find({.x = x, .y = y, .z = z});
                    REQUIRE(record != nullptr);
                    ++planned;
                    CHECK(record->loaded == runs[1].contains(y));
                    CHECK(record->drawn == runs[2].contains(y));
                }
            }
        }
    }
    CHECK(streamer.records().size() == planned);
    // Over the border east and back: after that, nothing comes or goes,
    // and no record is freed and made again.
    const ChunkPos east{.x = here.x + 1, .y = here.y, .z = here.z};
    streamer.set_camera(east);
    settle(streamer, jobs);
    streamer.set_camera(here);
    settle(streamer, jobs);
    const std::map<Chunk, std::uint32_t> ids = load_ids(streamer);
    for (int crossing = 0; crossing < 4; ++crossing) {
        streamer.set_camera(crossing % 2 == 0 ? east : here);
        settle(streamer, jobs);
        CHECK(load_ids(streamer) == ids);
    }
}
