#include <vx/world/chunk_records.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

// RFC 0006's chunk records: a pool at stable indices, each record counting
// the planned and ready records around it.

namespace {

using vx::ChunkPos;
using vx::ChunkRecords;
using vx::ChunkState;

constexpr ChunkPos middle{.x = 10, .y = 4, .z = -7};

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

// Every live record's counts against its neighbors' states.
void check_counts(const ChunkRecords& records) {
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!records.live(index)) {
            continue;
        }
        const ChunkPos pos = records[index].pos;
        INFO("chunk ", pos.x, ", ", pos.y, ", ", pos.z);
        int planned = 0;
        int ready = 0;
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const std::optional<std::uint32_t> neighbor =
                        records.find({.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz});
                    if ((dx != 0 || dy != 0 || dz != 0) && neighbor) {
                        planned += planned_by_hand(records[*neighbor].state) ? 1 : 0;
                        ready += ready_by_hand(records[*neighbor].state) ? 1 : 0;
                    }
                }
            }
        }
        REQUIRE(records[index].planned_neighbors == planned);
        REQUIRE(records[index].ready_neighbors == ready);
    }
}

// The 26 positions around `pos`.
std::vector<ChunkPos> around(ChunkPos pos) {
    std::vector<ChunkPos> out;
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    out.push_back({.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz});
                }
            }
        }
    }
    return out;
}

// The counts of each record around `pos`, in around()'s order.
std::vector<std::array<std::uint8_t, 2>> neighbor_counts(const ChunkRecords& records,
                                                         ChunkPos pos) {
    std::vector<std::array<std::uint8_t, 2>> counts;
    for (const ChunkPos near : around(pos)) {
        if (const std::optional<std::uint32_t> at = records.find(near)) {
            counts.push_back({records[*at].planned_neighbors, records[*at].ready_neighbors});
        }
    }
    return counts;
}

} // namespace

TEST_CASE("a new record counts the planned and ready records already around it") {
    ChunkRecords records;
    const std::vector<ChunkPos> near = around(middle);
    // Neighbors in every state, in turn: 8 states over 26 neighbors.
    for (std::size_t at = 0; at < near.size(); ++at) {
        const std::uint32_t index = records.add(near[at], static_cast<std::uint32_t>(at + 1));
        records.set_state(index, static_cast<ChunkState>(at % 8), [](std::uint32_t) {});
    }
    const std::uint32_t own = records.add(middle, 100);
    CHECK(records[own].state == ChunkState::queued);
    CHECK(records[own].load_id == 100);
    // Of each 8 neighbors, planned to complete are planned (5), ready and
    // complete are ready (2); the last 2 neighbors are queued and planning.
    CHECK(records[own].planned_neighbors == 15);
    CHECK(records[own].ready_neighbors == 6);
    check_counts(records);
}

TEST_CASE("a state change moves the neighbors' counts only when what it counts as changes") {
    ChunkRecords records;
    for (const ChunkPos pos : around(middle)) {
        records.add(pos, 1);
    }
    const std::uint32_t own = records.add(middle, 2);
    // Each step: the state, and whether planned and ready move.
    struct Step {
        ChunkState state;
        int planned;
        int ready;
    };
    const std::array<Step, 8> steps = {
        Step{ChunkState::planning, 0, 0},     Step{ChunkState::planned, 1, 0},
        Step{ChunkState::build_queued, 1, 0}, Step{ChunkState::building, 1, 0},
        Step{ChunkState::ready, 1, 1},        Step{ChunkState::complete, 1, 1},
        Step{ChunkState::ready, 1, 1},        Step{ChunkState::unloading, 0, 0}};
    int planned_before = 0;
    int ready_before = 0;
    for (const Step& step : steps) {
        INFO("to state ", static_cast<int>(step.state));
        std::vector<std::uint32_t> changed;
        records.set_state(own, step.state,
                          [&](std::uint32_t neighbor) { changed.push_back(neighbor); });
        const bool moved = step.planned != planned_before || step.ready != ready_before;
        CHECK(changed.size() == (moved ? 26U : 0U));
        for (const ChunkPos pos : around(middle)) {
            const std::optional<std::uint32_t> neighbor = records.find(pos);
            REQUIRE(neighbor.has_value());
            CHECK(records[*neighbor].planned_neighbors == step.planned);
            CHECK(records[*neighbor].ready_neighbors == step.ready);
        }
        planned_before = step.planned;
        ready_before = step.ready;
    }
    check_counts(records);
}

TEST_CASE("a removed record is no longer counted, and its index is reused") {
    ChunkRecords records;
    for (const ChunkPos pos : around(middle)) {
        records.add(pos, 1);
    }
    const std::uint32_t own = records.add(middle, 2);
    records.set_state(own, ChunkState::ready, [](std::uint32_t) {});
    REQUIRE(records.size() == 27);
    std::vector<std::uint32_t> changed;
    records.remove(own, [&](std::uint32_t neighbor) { changed.push_back(neighbor); });
    CHECK(changed.size() == 26);
    CHECK_FALSE(records.live(own));
    CHECK_FALSE(records.find(middle).has_value());
    CHECK(records.size() == 26);
    check_counts(records);
    // The next record takes the freed index, fresh.
    const ChunkPos elsewhere{.x = 50, .y = 0, .z = 50};
    const std::uint32_t next = records.add(elsewhere, 3);
    CHECK(next == own);
    CHECK(records.live(next));
    CHECK(records[next].pos == elsewhere);
    CHECK(records[next].state == ChunkState::queued);
    CHECK(records[next].load_id == 3);
    CHECK(records.end_index() == 27);
}

TEST_CASE("neighbors past the world's edge count as planned and ready") {
    ChunkRecords records;
    // On one face of the world, along an edge, and in a corner: 9, 15 and
    // 19 of the 26 neighbors lie outside.
    const std::uint32_t face = records.add({.x = vx::highest_chunk, .y = 0, .z = 0}, 1);
    const std::uint32_t edge =
        records.add({.x = vx::highest_chunk, .y = 0, .z = vx::lowest_chunk}, 2);
    const std::uint32_t corner =
        records.add({.x = vx::lowest_chunk, .y = vx::highest_chunk, .z = vx::lowest_chunk}, 3);
    CHECK(records[face].planned_neighbors == 9);
    CHECK(records[face].ready_neighbors == 9);
    CHECK(records[edge].planned_neighbors == 15);
    CHECK(records[edge].ready_neighbors == 15);
    CHECK(records[corner].planned_neighbors == 19);
    CHECK(records[corner].ready_neighbors == 19);
    // A ready record next to the face moves the count past the edge's.
    const std::uint32_t inside = records.add({.x = vx::highest_chunk - 1, .y = 0, .z = 0}, 4);
    records.set_state(inside, ChunkState::ready, [](std::uint32_t) {});
    CHECK(records[face].planned_neighbors == 10);
    CHECK(records[face].ready_neighbors == 10);
}

TEST_CASE("under random adds, state changes and removes, every count matches its neighbors") {
    std::mt19937 random(0xC0047);
    std::uniform_int_distribution<int> coordinate(0, 4);
    std::uniform_int_distribution<int> state(0, 7);
    std::uniform_int_distribution<int> action(0, 9);
    ChunkRecords records;
    std::size_t changes = 0;
    for (int round = 0; round < 3'000; ++round) {
        const ChunkPos pos{
            .x = coordinate(random), .y = coordinate(random), .z = coordinate(random)};
        const std::optional<std::uint32_t> found = records.find(pos);
        const int roll = action(random);
        if (!found) {
            records.add(pos, static_cast<std::uint32_t>(round));
        } else if (roll < 2) {
            records.remove(*found, [](std::uint32_t) {});
        } else {
            // The callback names exactly the neighbors whose counts moved.
            const std::vector<std::array<std::uint8_t, 2>> before = neighbor_counts(records, pos);
            std::size_t reported = 0;
            records.set_state(*found, static_cast<ChunkState>(state(random)),
                              [&](std::uint32_t) { ++reported; });
            const std::vector<std::array<std::uint8_t, 2>> after = neighbor_counts(records, pos);
            REQUIRE(after.size() == before.size());
            std::size_t moved = 0;
            for (std::size_t at = 0; at < before.size(); ++at) {
                if (after[at] != before[at]) {
                    ++moved;
                }
            }
            CHECK(reported == moved);
            changes += moved;
        }
        if (round % 50 == 0) {
            check_counts(records);
        }
    }
    check_counts(records);
    CHECK(changes > 1'000);
}
