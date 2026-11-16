#include <vx/core/random.hpp>

#include <bit>
#include <cstdint>

// RFC 0008's float rules: no fast math anywhere in the build. CMake refuses
// the flags at configure; this catches any that reach a compiler anyway.
#ifdef __FAST_MATH__
#error "Perdix is never built with fast math (RFC 0008): it breaks determinism"
#endif

namespace vx {

std::uint64_t interpolation_probe(std::uint64_t seed, std::uint32_t count) noexcept {
    Rng rng{seed};
    std::uint64_t hash = 0;
    for (std::uint32_t at = 0; at < count; ++at) {
        const float a = rng.next_float();
        const float b = rng.next_float();
        const float t = rng.next_float();
        const float value = (a * (1.0F - t)) + (b * t);
        hash = mix64(hash ^ std::bit_cast<std::uint32_t>(value));
    }
    return hash;
}

} // namespace vx
