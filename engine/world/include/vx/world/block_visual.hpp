#pragma once

#include <array>
#include <cstdint>

namespace vx {

// How a block's faces cover what is behind them (RFC 0003): air has none,
// an opaque block hides its neighbors' faces, and a cutout block, such as
// leaves or glass, draws faces with holes in them and hides nothing.
enum class Opacity : std::uint8_t { air, opaque, cutout };

// The alpha test cutout faces are drawn with (RFC 0004): a texel whose alpha
// is 128 or more, 0.5 or more in the shader, is drawn; any other is
// discarded.
inline constexpr std::uint8_t alpha_test_passes = 128;

// What the mesher needs to know about a block state (RFC 0003), one per
// state, built by the block registry (RFC 0004). Two cutout blocks with the
// same nonzero cull group hide each other's shared faces; 0 is none. A
// texture is a layer of the block texture array, indexed by Face; layer 0
// is a checker, so a zeroed entry is visibly wrong.
struct BlockVisual {
    Opacity opacity = Opacity::air;
    std::uint8_t cull_group = 0;
    std::array<std::uint16_t, 6> texture{};

    friend constexpr bool operator==(const BlockVisual&, const BlockVisual&) = default;
};

// The mesher reads one per block it meshes: 14 bytes, under 14 KiB for a
// thousand states.
static_assert(sizeof(BlockVisual) == 14);

} // namespace vx
