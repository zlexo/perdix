#include <vx/core/job_system.hpp>
#include <vx/world/block_data_pool.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/versions.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// RFC 0005's job records: each names the versions its job pinned, and
// nothing a job reads goes before it reports back or its cancel succeeds.

namespace {

using vx::BlockState;
using vx::ChunkPlan;
using vx::ChunkStorage;
using vx::JobRecord;
using vx::JobRecords;
using vx::Version;
using vx::Versions;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr BlockState dirt = static_cast<BlockState>(2);

// Stone with dirt at block 0: a chunk with block data.
ChunkStorage dirt_on_stone() {
    ChunkStorage chunk = ChunkStorage::uniform(stone);
    chunk.set(0, dirt);
    return chunk;
}

// The block data packed arrays hold, over every class.
std::size_t block_data_in_use() {
    std::size_t total = 0;
    for (const vx::BlockDataClass& sizes : vx::block_data::stats()) {
        total += sizes.in_use;
    }
    return total;
}

// What a job saw, and its record: the deterministic mode runs jobs on the
// test's thread, so a plain vector collects the reports.
struct Report {
    const JobRecord* record = nullptr;
    BlockState seen = BlockState::air;
};

// Reads block 0 of its own chunk, the way a mesh job reads its pinned blocks,
// and reports back with its record.
struct ReadJob {
    const JobRecord* record;
    std::vector<Report>* reports;

    void operator()(vx::JobContext& /*context*/) const noexcept {
        const Version<ChunkStorage>* own = record->chunks[13];
        reports->push_back(
            {.record = record, .seen = own != nullptr ? own->value().get(0) : record->uniform[13]});
    }
};

// A mesh job's record: `own` at the middle, uniform stone all around.
JobRecord& mesh_record(JobRecords& records, Version<ChunkStorage>* own, std::uint32_t sequence) {
    JobRecord* record = records.acquire(1, sequence);
    REQUIRE(record != nullptr);
    for (std::size_t at = 0; at < 27; ++at) {
        records.read_chunk(*record, at, at == 13 ? own : nullptr, stone);
    }
    return *record;
}

} // namespace

TEST_CASE("a completion unpins the versions its record names, even after an edit replaced them") {
    const std::size_t data_before = block_data_in_use();
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(16, chunks, plans);
    vx::JobSystem jobs({.workers = 0});
    std::vector<Report> reports;
    reports.reserve(4);
    Version<ChunkStorage>* current = &chunks.make(dirt_on_stone());
    JobRecord& record = mesh_record(records, current, 1);
    CHECK(current->pins() == 1);
    REQUIRE(jobs.submit({}, ReadJob{&record, &reports}));
    // An edit lands before the job runs: it writes a copy, and the job reads
    // the version it was handed.
    chunks.write(current).set(0, stone);
    CHECK(chunks.frozen() == 1);
    jobs.run_until_idle();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].record == &record);
    CHECK(reports[0].seen == dirt);
    // The completion: the frozen version goes, the edit's copy stays.
    records.release(record);
    CHECK(chunks.frozen() == 0);
    CHECK(chunks.live() == 1);
    CHECK(current->current());
    CHECK(current->pins() == 0);
    CHECK(current->value().get(0) == stone);
    chunks.retire(*current);
    CHECK(block_data_in_use() == data_before);
}

TEST_CASE("an unloaded chunk is freed only after its last job reports back") {
    const std::size_t data_before = block_data_in_use();
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(16, chunks, plans);
    vx::JobSystem jobs({.workers = 0});
    std::vector<Report> reports;
    reports.reserve(4);
    // The chunk map leads to the chunk's current version.
    const vx::ChunkPos pos{.x = 3, .y = -1, .z = 7};
    vx::ChunkMap map;
    std::vector<Version<ChunkStorage>*> current_of{&chunks.make(dirt_on_stone())};
    REQUIRE(map.insert(pos, 0));
    JobRecord& first = mesh_record(records, current_of[0], 1);
    JobRecord& second = mesh_record(records, current_of[0], 2);
    REQUIRE(jobs.submit({}, ReadJob{&first, &reports}));
    REQUIRE(jobs.submit({}, ReadJob{&second, &reports}));
    CHECK(current_of[0]->pins() == 2);
    // It leaves the region: gone from the map, no longer current, its blocks
    // kept for the two jobs.
    REQUIRE(map.erase(pos));
    chunks.retire(*current_of[0]);
    current_of[0] = nullptr;
    CHECK(chunks.live() == 1);
    CHECK(chunks.frozen() == 1);
    CHECK(block_data_in_use() > data_before);
    jobs.run_until_idle();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].seen == dirt);
    CHECK(reports[1].seen == dirt);
    records.release(first);
    CHECK(chunks.live() == 1);
    CHECK(block_data_in_use() > data_before);
    records.release(second);
    CHECK(chunks.live() == 0);
    CHECK(chunks.frozen() == 0);
    CHECK(block_data_in_use() == data_before);
}

TEST_CASE("a successful cancel unpins at once, and a job that ran unpins when it reports back") {
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(16, chunks, plans);
    vx::JobSystem jobs({.workers = 0});
    std::vector<Report> reports;
    reports.reserve(4);
    Version<ChunkStorage>& version = chunks.make(dirt_on_stone());
    JobRecord& canceled = mesh_record(records, &version, 1);
    const vx::JobHandle handle = jobs.submit({}, ReadJob{&canceled, &reports});
    REQUIRE(handle);
    REQUIRE(jobs.cancel(handle));
    records.release(canceled);
    CHECK(version.pins() == 0);
    CHECK(records.in_use() == 0);
    // A job that ran is too late to cancel: its pin stays until its
    // completion is handled, even with the chunk gone.
    JobRecord& ran = mesh_record(records, &version, 2);
    const vx::JobHandle done = jobs.submit({}, ReadJob{&ran, &reports});
    REQUIRE(done);
    jobs.run_until_idle();
    CHECK_FALSE(jobs.cancel(done));
    CHECK(version.pins() == 1);
    chunks.retire(version);
    CHECK(chunks.live() == 1);
    // Only the job that ran reported.
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].record == &ran);
    records.release(ran);
    CHECK(chunks.live() == 0);
}

TEST_CASE("a plan dropped while a build reads it lives until the build reports back") {
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(16, chunks, plans);
    // A build reads the 27 plans around its chunk; two neighbors are out of
    // reach and have none.
    std::array<Version<ChunkPlan>*, 27> around{};
    JobRecord* record = records.acquire(4, 9);
    REQUIRE(record != nullptr);
    for (std::size_t at = 0; at < around.size(); ++at) {
        if (at == 0 || at == 26) {
            records.read_plan(*record, at, nullptr);
            continue;
        }
        around[at] =
            &plans.make(ChunkPlan{.pos = {.x = static_cast<std::int32_t>(at), .y = 0, .z = 0}});
        records.read_plan(*record, at, around[at]);
    }
    CHECK(plans.live() == 25);
    CHECK(plans.pins() == 25);
    // Every neighbor's chunk is built: the plans are dropped, but the build
    // still reads them.
    for (Version<ChunkPlan>* plan : around) {
        if (plan != nullptr) {
            plans.retire(*plan);
        }
    }
    CHECK(plans.live() == 25);
    CHECK(record->plans[5]->value().pos.x == 5);
    records.release(*record);
    CHECK(plans.live() == 0);
    CHECK(plans.pins() == 0);
}

TEST_CASE("records run out at the pool's size and come back empty when released") {
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(2, chunks, plans);
    CHECK(records.capacity() == 2);
    JobRecord* first = records.acquire(1, 1);
    JobRecord* second = records.acquire(1, 2);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);
    CHECK(records.acquire(1, 3) == nullptr);
    CHECK(records.in_use() == 2);
    Version<ChunkStorage>& version = chunks.make(dirt_on_stone());
    records.read_chunk(*first, 13, &version, BlockState::air);
    records.read_chunk(*first, 14, nullptr, stone);
    records.release(*first);
    JobRecord* again = records.acquire(2, 4);
    REQUIRE(again == first);
    CHECK(again->load_id == 2);
    CHECK(again->sequence == 4);
    CHECK(again->chunks[13] == nullptr);
    CHECK(again->uniform[14] == BlockState::air);
    CHECK(version.pins() == 0);
    records.release(*again);
    records.release(*second);
    CHECK(records.in_use() == 0);
    chunks.retire(version);
}
