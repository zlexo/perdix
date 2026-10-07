#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vx {

// The CRC-32 that zip archives and PNG files store with their data
// (polynomial 0xEDB88320, reflected, started and finished with all bits
// set), so a damaged entry is caught before it is used. Of the nine bytes
// "123456789" it is 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

} // namespace vx
