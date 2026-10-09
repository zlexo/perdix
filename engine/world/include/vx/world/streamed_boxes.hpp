#pragma once

#include <vx/world/block_physics.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/world_streamer.hpp>

#include <span>
#include <vector>

namespace vx {

// The streamed world's blocks for moves (RFC 0010 over RFC 0006), on the
// world thread: a ready chunk's blocks as ChunkBoxes reads them, and a chunk
// not ready yet as one solid cube, so nothing falls into the world before it
// loads, as Minecraft holds a player out of chunks it has not loaded.
class StreamedBoxes final : public BlockBoxes {
public:
    StreamedBoxes(const WorldStreamer& streamer, std::span<const BlockPhysics> physics) noexcept
        : streamer_(&streamer), physics_(physics) {}

    void collect(const Box& region, std::vector<Box>& out) const override;
    // Default ground in a chunk not ready.
    [[nodiscard]] double slipperiness(BlockPos block) const override;

private:
    const WorldStreamer* streamer_;
    std::span<const BlockPhysics> physics_;
};

} // namespace vx
