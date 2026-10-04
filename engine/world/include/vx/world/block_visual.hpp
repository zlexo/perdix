#pragma once

#include <cstdint>

namespace vx {

// How a block's faces cover what is behind them (RFC 0003): air has none,
// an opaque block hides its neighbors' faces, and a cutout block, such as
// leaves or glass, draws faces with holes in them and hides nothing.
enum class Opacity : std::uint8_t { air, opaque, cutout };

} // namespace vx
