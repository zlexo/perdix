#pragma once

#include <cstdint>

namespace vx {

// A block state: one block type plus its properties (for example "oak log,
// facing up"). 0 is always air, so zeroed memory is air. RFC 0004's block
// registry assigns the other ids.
enum class BlockState : std::uint16_t { air = 0 };

} // namespace vx
