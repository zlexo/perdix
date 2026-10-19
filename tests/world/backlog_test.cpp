#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/world/backlog.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <vector>

// RFC 0006's backlogs: lowest score first, the score doubling outside the
// view, and rescoring in place when the camera moves.

namespace {

using vx::Backlog;
using vx::ChunkPos;

// Looking down -z from the middle of chunk (0, 0, 0).
vx::Frustum looking_north() {
    return {{.x = 0.0, .y = 0.0, .z = -1.0},
            {.x = 1.0, .y = 0.0, .z = 0.0},
            {.x = 0.0, .y = 1.0, .z = 0.0},
            std::tan(std::numbers::pi / 4.0),
            16.0 / 9.0,
            0.05};
}

constexpr vx::Vec3d eye{.x = 16.0, .y = 16.0, .z = 16.0};
constexpr ChunkPos origin{.x = 0, .y = 0, .z = 0};

} // namespace

TEST_CASE("the 27 chunks around the camera score 0, and the rest by squared distance") {
    const vx::Frustum view = looking_north();
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                CHECK(vx::chunk_score({.x = dx, .y = dy, .z = dz}, origin, eye, view) == 0);
            }
        }
    }
    // Ahead, in view: the square of the distance.
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = -5}, origin, eye, view) == 25);
    CHECK(vx::chunk_score({.x = 1, .y = 0, .z = -5}, origin, eye, view) == 26);
    // Behind: doubled, so four times the square, ranked with chunks in view
    // twice as far.
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = 5}, origin, eye, view) == 100);
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = 5}, origin, eye, view) ==
          vx::chunk_score({.x = 0, .y = 0, .z = -10}, origin, eye, view));
    // A chunk just outside the view whose grown box reaches into it still
    // ranks as in view: a visible chunk needs its neighbors.
    CHECK(vx::chunk_score({.x = 0, .y = 4, .z = -2}, origin, eye, view) == 20);
    // Columns by their horizontal distance alone.
    CHECK(vx::column_score(3, -4, origin) == 25);
    CHECK(vx::column_score(3, -4, {.x = 3, .y = 99, .z = -4}) == 0);
    // Far past any region, no overflow.
    CHECK(vx::chunk_score({.x = 60'000'000, .y = 0, .z = 0}, origin, eye, view) > 1'000'000'000U);
}

TEST_CASE("on a coarse level a chunk scores by that level's chunks, and its view test uses that "
          "level's box") {
    const vx::Frustum view = looking_north();
    // The eye in the middle of level-2 chunk (0, 0, 0), 128 blocks a side.
    constexpr vx::Vec3d middle{.x = 64.0, .y = 64.0, .z = 64.0};
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        CHECK(vx::chunk_score({.x = 1, .y = -1, .z = dz}, origin, middle, view, 2) == 0);
    }
    // Two level-2 chunks ahead: the square of the distance in them.
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = -2}, origin, middle, view, 2) == 4);
    // Two behind: the box grown by a level-2 chunk spans 64 to 448 blocks
    // behind the eye, so doubled. Taken as a level-0 box, the same chunk
    // would span 32 ahead to 64 behind and rank as in view.
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = 2}, origin, middle, view, 2) == 16);
    CHECK(vx::chunk_score({.x = 0, .y = 0, .z = 2}, origin, middle, view) == 4);
    // At the world's edge on the top level, no overflow.
    CHECK(vx::chunk_score({.x = -2'097'152, .y = 0, .z = 0}, origin, middle, view, 5) >
          1'000'000'000U);
}

TEST_CASE("a backlog gives its lowest key first, ties to the lower index") {
    Backlog backlog;
    std::mt19937 random(5);
    std::uniform_int_distribution<std::uint32_t> key(0, 50);
    std::vector<Backlog::Entry> pushed;
    for (std::uint32_t index = 0; index < 500; ++index) {
        const Backlog::Entry entry{.key = key(random), .index = index, .load_id = index + 1};
        pushed.push_back(entry);
        backlog.push(entry);
    }
    CHECK(backlog.size() == 500);
    std::uint32_t last_key = 0;
    std::uint32_t last_index = 0;
    std::size_t taken = 0;
    while (!backlog.empty()) {
        const Backlog::Entry entry = backlog.top();
        backlog.pop();
        if (taken > 0) {
            CHECK((entry.key > last_key || (entry.key == last_key && entry.index > last_index)));
        }
        CHECK(entry.load_id == entry.index + 1);
        last_key = entry.key;
        last_index = entry.index;
        ++taken;
    }
    CHECK(taken == 500);
}

TEST_CASE("rescoring keys every entry again and drops the ones that moved on") {
    Backlog backlog;
    for (std::uint32_t index = 0; index < 10; ++index) {
        backlog.push({.key = index, .index = index, .load_id = 7});
    }
    // The camera moved: the order flips, and odd entries are stale.
    backlog.rescore([](const Backlog::Entry& entry) -> std::optional<std::uint32_t> {
        if (entry.index % 2 == 1) {
            return std::nullopt;
        }
        return 100 - entry.index;
    });
    CHECK(backlog.size() == 5);
    std::vector<std::uint32_t> order;
    while (!backlog.empty()) {
        order.push_back(backlog.top().index);
        backlog.pop();
    }
    CHECK(order == std::vector<std::uint32_t>{8, 6, 4, 2, 0});
}
