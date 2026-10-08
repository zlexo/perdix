#include <vx/core/vec3d.hpp>

#include <doctest/doctest.h>

TEST_CASE("Vec3d adds, subtracts and scales each component") {
    constexpr vx::Vec3d a{.x = 1.0, .y = -2.0, .z = 0.5};
    constexpr vx::Vec3d b{.x = 0.25, .y = 4.0, .z = -1.5};
    CHECK(a + b == vx::Vec3d{.x = 1.25, .y = 2.0, .z = -1.0});
    CHECK(a - b == vx::Vec3d{.x = 0.75, .y = -6.0, .z = 2.0});
    CHECK(a * 2.0 == vx::Vec3d{.x = 2.0, .y = -4.0, .z = 1.0});
    CHECK(vx::Vec3d{} == vx::Vec3d{.x = 0.0, .y = 0.0, .z = 0.0});
    CHECK_FALSE(a == b);
}
