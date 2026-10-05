#pragma once

#include <cstdint>

namespace vx {

// A block state: one block type plus its properties (for example "oak log,
// facing up"). 0 is always air, so zeroed memory is air. RFC 0004's block
// registry assigns the other ids, at run time, so air is the only name
// here; the 16 bits are for the 65,536 states RFC 0002 allows.
enum class BlockState : std::uint16_t { air = 0 }; // NOLINT(performance-enum-size)

} // namespace vx
