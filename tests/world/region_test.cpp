#include <vx/world/coordinates.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

// RFC 0006's region: the terrain disc, the near cylinder, the margin, the
// plan ring and hysteresis, as pure functions.

namespace {

using vx::ColumnPos;
using vx::Region;

} // namespace

TEST_CASE("at 32 chunks the region draws 3,209 columns and loads 3,473, and keeps 432 more") {
    // RFC 0006's counts: the disc, the disc grown by one column every way,
    // diagonals included, and that at 34 chunks.
    const Region region({.x = 5, .y = 2, .z = -9}, {});
    const Region kept = region.grown(region.settings().hysteresis);
    std::size_t drawn_columns = 0;
    std::size_t loaded_columns = 0;
    std::size_t kept_columns = 0;
    std::size_t planned_columns = 0;
    for (std::int32_t z = -9 - 40; z <= -9 + 40; ++z) {
        for (std::int32_t x = 5 - 40; x <= 5 + 40; ++x) {
            const ColumnPos column{.x = x, .z = z};
            if (region.column_drawn(column)) {
                ++drawn_columns;
            }
            if (region.column_loaded(column)) {
                ++loaded_columns;
            }
            if (region.column_planned(column)) {
                ++planned_columns;
            }
            if (kept.column_loaded(column)) {
                ++kept_columns;
            }
        }
    }
    CHECK(drawn_columns == 3'209);
    CHECK(loaded_columns == 3'473);
    CHECK(kept_columns == 3'473 + 432);
    // The plan ring: within two columns of a drawn one, diagonals included.
    CHECK(planned_columns == 3'745);
}
