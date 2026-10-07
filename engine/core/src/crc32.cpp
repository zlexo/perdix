#include <vx/core/crc32.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {
namespace {

// The remainder of each byte value, worked out once at compile time, so
// the loop below takes a byte at a time instead of a bit.
constexpr std::array<std::uint32_t, 256> remainders = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t value = 0; value < table.size(); ++value) {
        std::uint32_t remainder = value;
        for (int bit = 0; bit < 8; ++bit) {
            remainder = (remainder & 1U) != 0 ? 0xEDB8'8320U ^ (remainder >> 1U) : remainder >> 1U;
        }
        table[value] = remainder;
    }
    return table;
}();

} // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (const std::byte byte : bytes) {
        crc = remainders[(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFF'FFFFU;
}

} // namespace vx
