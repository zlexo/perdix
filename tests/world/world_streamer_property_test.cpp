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
#include <tuple>
#include <utility>
#include <vector>

// RFC 0006's streaming: once the jobs are done the records are exactly the
// region, written out from the region's functions and the generator.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ChunkRange;
using vx::ChunkRecord;
using vx::ColumnPos;
using vx::ColumnRuns;
using vx::Region;
using vx::WorldStreamer;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr vx::PlaceholderBlocks blocks{
    .grass = static_cast<BlockState>(3), .dirt = static_cast<BlockState>(2), .stone = stone};

using Chunk = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

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

void settle(WorldStreamer& streamer, vx::JobSystem& jobs) {
    for (int pass = 0; pass < 10'000; ++pass) {
        streamer.feed();
        if (streamer.stats().jobs_in_flight == 0) {
            return;
        }
        jobs.run_until_idle();
        streamer.integrate();
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
