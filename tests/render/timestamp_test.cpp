#include <vx/render/gpu_timer.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>

using std::chrono::nanoseconds;

TEST_CASE("a timestamp interval is in nanoseconds, whatever a tick's length") {
    CHECK(vx::render::timestamp_interval(100, 160, 64, 1.0) == nanoseconds(60));
    // KosmicKrisp's ticks, at 24 MHz.
    CHECK(vx::render::timestamp_interval(100, 160, 64, 41.666668) == nanoseconds(2500));
}

TEST_CASE("a timestamp interval spans the counter wrapping past zero") {
    // 36 valid bits, the fewest a queue that writes timestamps may have.
    constexpr std::uint64_t last_of_36 = (std::uint64_t{1} << 36U) - 1;
    CHECK(vx::render::timestamp_interval(last_of_36 - 9, 5, 36, 1.0) == nanoseconds(15));
    constexpr std::uint64_t last_of_64 = std::numeric_limits<std::uint64_t>::max();
    CHECK(vx::render::timestamp_interval(last_of_64 - 9, 5, 64, 1.0) == nanoseconds(15));
}
