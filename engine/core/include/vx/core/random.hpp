#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

namespace vx {

// RFC 0008's random streams. Nothing draws from a shared generator: every
// decision takes its own stream, keyed by the world's seed, a salt naming
// what it decides and the place it decides it, so the result cannot depend
// on which thread asks or in what order. Everything here is unsigned 64-bit
// arithmetic, which wraps by definition, and is written out rather than
// taken from <random>, whose distributions differ between standard
// libraries.

// SplitMix64's increment, the golden ratio's fraction in 64 bits.
inline constexpr std::uint64_t golden_gamma = 0x9E37'79B9'7F4A'7C15ULL;

// SplitMix64's finalizer, constants from Vigna's public-domain splitmix64.c:
// a bijection that spreads every input bit over every output bit.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return z ^ (z >> 31U);
}

// The 64-bit FNV-1a hash of a name. Streams are keyed by names, not by
// declaration order, so adding a field or a feature leaves every other
// stream as it was.
[[nodiscard]] constexpr std::uint64_t salt(std::string_view name) noexcept {
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const char c : name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    return hash;
}

// The seed of the stream for one decision at one place: the world's seed,
// the decision's salt, then the place's integers in order (a chunk's x, y
// and z, a level, a tile). Negative keys count by their two's complement,
// which the conversion to unsigned defines.
template <std::integral... Keys>
[[nodiscard]] constexpr std::uint64_t stream_seed(std::uint64_t world, std::uint64_t name,
                                                  Keys... keys) noexcept {
    std::uint64_t seed = mix64(world + golden_gamma) ^ name;
    ((seed =
          mix64(seed + golden_gamma + static_cast<std::uint64_t>(static_cast<std::int64_t>(keys)))),
     ...);
    return seed;
}

// SplitMix64: a counter run through mix64. Small, fast, and the same
// numbers on every compiler and machine.
class Rng {
public:
    constexpr explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        state_ += golden_gamma;
        return mix64(state_);
    }

    // In [0, 1): the top 24 bits over 2^24, exact in a float.
    [[nodiscard]] constexpr float next_float() noexcept {
        return static_cast<float>(next() >> 40U) * 0x1.0p-24F;
    }

    // In [0, n): the top 32 bits times n, shifted down. No rejection, so
    // an n that is not a power of two leans by at most n / 2^32.
    [[nodiscard]] constexpr std::uint32_t below(std::uint32_t n) noexcept {
        return static_cast<std::uint32_t>(((next() >> 32U) * n) >> 32U);
    }

private:
    std::uint64_t state_;
};

// RFC 0008's control for the float rules: the hash of `count`
// interpolations a * (1 - t) + b * t, each input from next_float of a
// stream with `seed`, folded as hash = mix64(hash ^ bits). It is built into
// the core library with the engine's flags, so a test of its value checks
// that the library was: a fused multiply-add changes it.
[[nodiscard]] std::uint64_t interpolation_probe(std::uint64_t seed, std::uint32_t count) noexcept;

} // namespace vx
