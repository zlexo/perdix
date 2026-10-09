#pragma once

#include <cstdint>

namespace vx::app {

// The memory the process holds now, in bytes, as RFC 0006's flythrough
// reports it: the footprint on macOS (what Activity Monitor shows), the
// resident set on Linux; 0 where it cannot be read.
[[nodiscard]] std::uint64_t process_footprint() noexcept;

} // namespace vx::app
