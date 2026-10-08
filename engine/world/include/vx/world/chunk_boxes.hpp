#pragma once

#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>

#include <vector>

namespace vx {

// One chunk's blocks as movement sees them (RFC 0010): every block but air a
// full cube, and nothing outside the chunk. The chunk must outlive this.
// RFC 0010's item 6 reads a world of chunks and each block's own collision.
class ChunkBoxes final : public BlockBoxes {
public:
    ChunkBoxes(const ChunkStorage& chunk, ChunkPos position) noexcept;

    // A region with a coordinate that is not finite holds nothing.
    void collect(const Box& region, std::vector<Box>& out) const override;

private:
    const ChunkStorage* chunk_;
    BlockPos origin_; // the chunk's lowest block
};

} // namespace vx
