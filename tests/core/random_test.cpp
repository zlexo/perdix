#include <vx/core/random.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <set>

// The known answers come from a Python model written apart from this code:
// Vigna's splitmix64.c and FNV-1a's published values for the building
// blocks, and Python's own arithmetic for the rest, its floats rounded to
// single precision after every operation, which rounds each one correctly.

TEST_CASE("mix64 and Rng give Vigna's splitmix64.c numbers") {
    vx::Rng zero{0};
    CHECK(zero.next() == 0xE220'A839'7B1D'CDAFULL);
    CHECK(zero.next() == 0x6E78'9E6A'A1B9'65F4ULL);
    CHECK(zero.next() == 0x06C4'5D18'8009'454FULL);
    CHECK(zero.next() == 0xF88B'B8A8'724C'81ECULL);
    vx::Rng other{1'234'567};
    CHECK(other.next() == 6'457'827'717'110'365'317ULL);
    CHECK(other.next() == 3'203'168'211'198'807'973ULL);
    CHECK(other.next() == 9'817'491'932'198'370'423ULL);
    // A bijection: zero only from zero.
    CHECK(vx::mix64(0) == 0);
}

TEST_CASE("salt is FNV-1a over the name") {
    static_assert(vx::salt("") == 0xCBF2'9CE4'8422'2325ULL);
    CHECK(vx::salt("a") == 0xAF63'DC4C'8601'EC8CULL);
    CHECK(vx::salt("foobar") == 0x8594'4171'F739'67E8ULL);
    CHECK(vx::salt("feature.oak") == 0xDEAB'10FC'3073'BE46ULL);
    CHECK(vx::salt("feature.oak") != vx::salt("feature.spruce"));
}

TEST_CASE("a stream's seed depends on the world, the name and every key in order") {
    const std::uint64_t oak = vx::salt("feature.oak");
    CHECK(vx::stream_seed(0, oak, 0, 0, 0) == 0x0A00'545E'DF87'90B1ULL);
    CHECK(vx::stream_seed(42, oak, -1, 2, -3) == 0xB2F2'5F92'3B89'4528ULL);
    // Swapping two keys is another place.
    CHECK(vx::stream_seed(42, oak, 2, -1, -3) == 0x8755'BB6C'2CF8'2AAEULL);
    // A key's type does not matter, only its value.
    CHECK(vx::stream_seed(42, oak, std::int32_t{-1}, std::int64_t{2}, std::int8_t{-3}) ==
          vx::stream_seed(42, oak, -1, 2, -3));
    static_assert(vx::stream_seed(1, 2, 3) != vx::stream_seed(1, 2, 4));
    static_assert(vx::stream_seed(1, 2, 3) != vx::stream_seed(2, 2, 3));
    static_assert(vx::stream_seed(1, 2, 3) != vx::stream_seed(1, 3, 3));
    static_assert(vx::stream_seed(1, 2, 0) != vx::stream_seed(1, 2, 0, 0));

    // Neighboring places get unrelated streams: no two seeds of a 16 by 16
    // by 16 block of chunk positions meet.
    std::set<std::uint64_t> seeds;
    for (std::int32_t y = -8; y < 8; ++y) {
        for (std::int32_t z = -8; z < 8; ++z) {
            for (std::int32_t x = -8; x < 8; ++x) {
                seeds.insert(vx::stream_seed(7, oak, x, y, z));
            }
        }
    }
    CHECK(seeds.size() == 16U * 16U * 16U);
}

TEST_CASE("Rng's floats and integers are the model's") {
    vx::Rng rng{vx::stream_seed(7, vx::salt("test.rng"))};
    CHECK(std::bit_cast<std::uint32_t>(rng.next_float()) == 1'053'135'722U);
    CHECK(std::bit_cast<std::uint32_t>(rng.next_float()) == 1'048'851'184U);
    CHECK(std::bit_cast<std::uint32_t>(rng.next_float()) == 1'056'976'601U);
    const std::array<std::uint32_t, 5> expected{1, 4, 5, 1, 9};
    for (const std::uint32_t value : expected) {
        CHECK(rng.below(10) == value);
    }
}

TEST_CASE("Rng's floats are at least 0 and under 1, its integers under n") {
    vx::Rng rng{3};
    for (int at = 0; at < 10'000; ++at) {
        const float value = rng.next_float();
        CHECK(value >= 0.0F);
        CHECK(value < 1.0F);
        CHECK(rng.below(7) < 7U);
    }
    // The ends are reachable: 0 from a zero top, the float under 1 from an
    // all-ones one.
    CHECK(static_cast<float>(0xFFFF'FFFF'FFFF'FFFFULL >> 40U) * 0x1.0p-24F < 1.0F);
    CHECK(vx::Rng{0}.below(1) == 0U);
}

// RFC 0008's control: a build that fuses the multiply and add changes these
// bits. Measured on the reference machine with the core library rebuilt:
// Clang's default -ffp-contract=on turns this red at any optimization level
// on arm64, and on x86-64 with FMA (-march=haswell); -ffp-contract=fast
// from -O1, since unoptimized builds never fuse across statements.
TEST_CASE("the core library is built with RFC 0008's float rules") {
    CHECK(vx::interpolation_probe(vx::stream_seed(0, vx::salt("test.interpolation")), 100'000) ==
          0x5FEA'22D2'1FB2'70C7ULL);
}
