#pragma once

#include <vx/world/block_physics.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <span>
#include <vector>

namespace vx {

// One chunk's blocks as movement sees them (RFC 0010): each state as
// `physics`, the block registry's, says, a full cube or nothing, and how
// slippery; without it, every block but air a full cube on default ground.
// Nothing lies outside the chunk until M3 brings a map of chunks. The chunk
// and the physics must outlive this.
class ChunkBoxes final : public BlockBoxes {
public:
    ChunkBoxes(const ChunkStorage& chunk, ChunkPos position,
               std::span<const BlockPhysics> physics = {}) noexcept;

    // A region with a coordinate that is not finite holds nothing.
    void collect(const Box& region, std::vector<Box>& out) const override;
    // Outside the chunk, as for air: default ground.
    [[nodiscard]] double slipperiness(BlockPos block) const override;

private:
    // A state the physics does not cover counts as a full cube.
    [[nodiscard]] BlockPhysics physics_of(BlockState state) const noexcept;

    const ChunkStorage* chunk_;
    BlockPos origin_; // the chunk's lowest block
    std::span<const BlockPhysics> physics_;
};

} // namespace vx
