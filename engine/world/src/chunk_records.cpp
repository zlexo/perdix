#include <vx/world/chunk_records.hpp>

#include <vx/world/coordinates.hpp>
#include <vx/world/region.hpp>

#include <cassert>
#include <cstdint>

namespace vx {
namespace {

// How many of the 26 chunks around `pos` lie past the world's edge.
std::uint8_t outside_neighbors(ChunkPos pos) noexcept {
    // The coordinates within one of `at` that the world holds: 2 or 3.
    const auto inside = [](std::int32_t at) {
        return 1 + (at > lowest_chunk ? 1 : 0) + (at < highest_chunk ? 1 : 0);
    };
    return static_cast<std::uint8_t>(27 - (inside(pos.x) * inside(pos.y) * inside(pos.z)));
}

} // namespace

std::uint32_t ChunkRecords::add(ChunkPos pos, std::uint32_t load_id) {
    std::uint32_t index = 0;
    if (free_.empty()) {
        index = static_cast<std::uint32_t>(records_.size());
        records_.emplace_back();
    } else {
        index = free_.back();
        free_.pop_back();
    }
    [[maybe_unused]] const bool inserted = map_.insert(pos, index);
    assert(inserted && "a chunk has one record");
    ChunkRecord& record = records_[index];
    record.pos = pos;
    record.load_id = load_id;
    // Nothing past the world's edge is ever generated or waited for: those
    // neighbors count as planned and ready, as uniform air would.
    record.planned_neighbors = outside_neighbors(pos);
    record.ready_neighbors = record.planned_neighbors;
    for_neighbors(pos, [&](std::uint32_t neighbor) {
        const ChunkRecord& other = records_[neighbor];
        record.planned_neighbors = static_cast<std::uint8_t>(
            record.planned_neighbors + counted(counts_as_planned(other.state)));
        record.ready_neighbors = static_cast<std::uint8_t>(record.ready_neighbors +
                                                           counted(counts_as_ready(other.state)));
    });
    return index;
}

} // namespace vx
