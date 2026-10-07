#include <vx/core/crc32.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

} // namespace

TEST_CASE("crc32 gives the check values zip and PNG files are written with") {
    // The check value every CRC-32 implementation is tested against.
    CHECK(vx::crc32(bytes_of("123456789")) == 0xCBF4'3926U);
    CHECK(vx::crc32({}) == 0U);
    // Python's zlib.crc32 gives these, an implementation apart from ours.
    CHECK(vx::crc32(bytes_of("a")) == 0xE8B7'BE43U);
    CHECK(vx::crc32(bytes_of("The quick brown fox jumps over the lazy dog")) == 0x414F'A339U);
}

TEST_CASE("crc32 changes when any one bit of the bytes does") {
    std::array<std::byte, 64> bytes{};
    const std::uint32_t zeros = vx::crc32(bytes);
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            bytes[at] = std::byte{static_cast<unsigned char>(1U << bit)};
            CHECK(vx::crc32(bytes) != zeros);
            bytes[at] = std::byte{0};
        }
    }
}
