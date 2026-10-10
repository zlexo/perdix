#include <vx/world/memory_budget.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

// RFC 0006's memory budget: over it, the hysteresis goes first, then the view
// distance a chunk at a time; after five calm seconds they come back.

namespace {

using vx::LimitedBy;
using vx::MemoryBudget;
using vx::MemoryUse;
using vx::RegionLimits;
using Time = std::chrono::steady_clock::time_point;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::size_t budget = 1'000'000;
constexpr MemoryUse over{.chunk_bytes = budget + 1, .arena_full = false, .arena_share = 0.0};
constexpr MemoryUse calm{.chunk_bytes = budget / 10 * 9, .arena_share = 0.9};
constexpr MemoryUse uneasy{.chunk_bytes = budget, .arena_full = false, .arena_share = 0.0};

constexpr RegionLimits limits(std::int32_t view_distance, std::int32_t hysteresis) {
    return {.view_distance = view_distance, .hysteresis = hysteresis};
}

} // namespace

TEST_CASE("over budget, the hysteresis goes first, a ring at a time, then the view distance, a "
          "chunk at a time, down to 4") {
    MemoryBudget memory(budget, limits(8, 2));
    Time now{};
    CHECK_FALSE(memory.check(uneasy, now)); // at the budget is not over it
    CHECK(memory.limits() == limits(8, 2));
    CHECK(memory.limited_by() == LimitedBy::nothing);
    CHECK(memory.check(over, now));
    CHECK(memory.limits() == limits(8, 1));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
    CHECK(memory.check(over, now));
    CHECK(memory.limits() == limits(8, 0));
    for (std::int32_t distance = 7; distance >= 4; --distance) {
        now += milliseconds(250);
        CHECK(memory.check(over, now));
        CHECK(memory.limits() == limits(distance, 0));
    }
    CHECK_FALSE(memory.check(over, now));
    CHECK(memory.limits() == limits(4, 0));
}

TEST_CASE("an arena that cannot grow lowers the limits the same way, and says so") {
    MemoryBudget memory(budget, limits(32, 2));
    const MemoryUse full{.chunk_bytes = 0, .arena_full = true, .arena_share = 1.0};
    CHECK(memory.check(full, Time{}));
    CHECK(memory.limits() == limits(32, 1));
    CHECK(memory.limited_by() == LimitedBy::mesh_memory);
    // Chunk data over its budget too: that is what it says.
    const MemoryUse both{.chunk_bytes = budget * 2, .arena_full = true, .arena_share = 1.0};
    CHECK(memory.check(both, Time{}));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
}

TEST_CASE("after five seconds under 90% of both budgets the distance comes back a chunk at a "
          "time, then the hysteresis, and between 90% and the budget nothing moves") {
    MemoryBudget memory(budget, limits(10, 2));
    Time now{};
    for (int step = 0; step < 4; ++step) {
        REQUIRE(memory.check(over, now));
    }
    REQUIRE(memory.limits() == limits(8, 0));
    // Calm for under five seconds: nothing yet.
    CHECK_FALSE(memory.check(calm, now));
    now += milliseconds(4'900);
    CHECK_FALSE(memory.check(calm, now));
    // Uneasy, between 90% and the budget, starts the wait over.
    CHECK_FALSE(memory.check(uneasy, now));
    now += seconds(5);
    CHECK_FALSE(memory.check(calm, now));
    now += seconds(5);
    CHECK(memory.check(calm, now));
    CHECK(memory.limits() == limits(9, 0));
    // The wait starts again at each raise: a check soon after raises nothing.
    CHECK_FALSE(memory.check(calm, now + milliseconds(250)));
    CHECK(memory.limits() == limits(9, 0));
    // The arena's share counts as much as the chunk data.
    now += seconds(5);
    CHECK_FALSE(memory.check({.chunk_bytes = 0, .arena_full = false, .arena_share = 0.95}, now));
    now += seconds(5);
    CHECK_FALSE(memory.check(calm, now));
    now += seconds(5);
    CHECK(memory.check(calm, now));
    CHECK(memory.limits() == limits(10, 0));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
    now += seconds(5);
    CHECK(memory.check(calm, now));
    CHECK(memory.limits() == limits(10, 1));
    now += seconds(5);
    CHECK(memory.check(calm, now));
    CHECK(memory.limits() == limits(10, 2));
    CHECK(memory.limited_by() == LimitedBy::nothing);
    // Back at the player's: calm changes nothing.
    now += seconds(30);
    CHECK_FALSE(memory.check(calm, now));
}

TEST_CASE("the player's limits are followed at once while nothing is held back, cap the "
          "budget's, and are come back to step by step while it holds them back") {
    MemoryBudget memory(budget, limits(32, 2));
    memory.choose(limits(16, 2));
    CHECK(memory.limits() == limits(16, 2));
    memory.choose(limits(48, 2));
    CHECK(memory.limits() == limits(48, 2));
    // Held back: a nearer choice caps it at once, a farther one waits.
    Time now{};
    for (int step = 0; step < 5; ++step) {
        REQUIRE(memory.check(over, now));
    }
    REQUIRE(memory.limits() == limits(45, 0));
    memory.choose(limits(64, 2));
    CHECK(memory.limits() == limits(45, 0));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
    memory.choose(limits(20, 2));
    CHECK(memory.limits() == limits(20, 0));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
    // Choosing exactly what the budget left ends the hold.
    memory.choose(limits(20, 0));
    CHECK(memory.limits() == limits(20, 0));
    CHECK(memory.limited_by() == LimitedBy::nothing);
    now += seconds(30);
    CHECK_FALSE(memory.check(calm, now));
    CHECK_FALSE(memory.check(calm, now + seconds(10)));
}

TEST_CASE("a raise the budget goes back over doubles the next wait, up to 16 times, and one that "
          "holds until the next is due sets it back") {
    MemoryBudget memory(budget, limits(8, 0));
    Time now{};
    REQUIRE(memory.check(over, now));
    REQUIRE(memory.limits() == limits(7, 0));
    // Each time: calm long enough to raise, then over again 250 ms later,
    // as when the raised ring's chunks load past the budget.
    for (const int wait : {5, 10, 20, 40, 80, 80}) {
        INFO("wait ", wait);
        CHECK(memory.calm_needed() == seconds(wait));
        CHECK_FALSE(memory.check(calm, now));
        now += seconds(wait) - milliseconds(250);
        CHECK_FALSE(memory.check(calm, now));
        now += milliseconds(250);
        REQUIRE(memory.check(calm, now));
        REQUIRE(memory.limits() == limits(8, 0));
        now += milliseconds(250);
        REQUIRE(memory.check(over, now));
        REQUIRE(memory.limits() == limits(7, 0));
    }
    // The player's new limits start afresh.
    memory.choose(limits(10, 0));
    CHECK(memory.calm_needed() == seconds(5));

    // Two raises in a row: the first held until the second was due, so the
    // wait goes back to 5 seconds, though the first came after a failure.
    MemoryBudget again(budget, limits(10, 0));
    now = Time{};
    REQUIRE(again.check(over, now));
    REQUIRE(again.check(over, now));
    REQUIRE(again.limits() == limits(8, 0));
    REQUIRE_FALSE(again.check(calm, now));
    now += seconds(5);
    REQUIRE(again.check(calm, now)); // to 9
    now += milliseconds(250);
    REQUIRE(again.check(over, now)); // failed: back to 8, the wait doubles
    CHECK(again.calm_needed() == seconds(10));
    REQUIRE_FALSE(again.check(calm, now));
    now += seconds(10);
    REQUIRE(again.check(calm, now)); // to 9, and it holds
    now += seconds(10);
    REQUIRE(again.check(calm, now)); // to 10: the raise before held
    CHECK(again.limits() == limits(10, 0));
    CHECK(again.calm_needed() == seconds(5));
}

TEST_CASE("the view distance goes no nearer than the floor, and the reason follows what is over") {
    // The near cylinder's radius, 8: nearer would free nothing it keeps.
    MemoryBudget memory(budget, limits(10, 0), 8);
    Time now{};
    CHECK(memory.check(over, now));
    CHECK(memory.check(over, now));
    CHECK(memory.limits() == limits(8, 0));
    CHECK_FALSE(memory.check(over, now));
    CHECK(memory.limits() == limits(8, 0));
    CHECK(memory.limited_by() == LimitedBy::chunk_memory);
    // At the floor, the arena fills while chunk data fits: the reason says so.
    const MemoryUse full{.chunk_bytes = 0, .arena_full = true, .arena_share = 1.0};
    CHECK_FALSE(memory.check(full, now));
    CHECK(memory.limited_by() == LimitedBy::mesh_memory);
    // A floor under 4 is 4.
    MemoryBudget low(budget, limits(5, 0), 2);
    REQUIRE(low.check(over, now));
    CHECK_FALSE(low.check(over, now));
    CHECK(low.limits() == limits(4, 0));
}
