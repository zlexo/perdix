#include <vx/world/chunk_records.hpp>

#include <vx/world/coordinates.hpp>
#include <vx/world/region.hpp>

#include <cassert>
#include <cstdint>
#include <utility>

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

bool shown(const ChunkRecord& record) noexcept {
    return record.state == ChunkState::complete && record.drawn;
}

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

std::uint32_t ChunkRecords::broken() const {
    std::uint32_t broken = 0;
    for (std::uint32_t index = 0; index < end_index(); ++index) {
        if (!live(index)) {
            continue;
        }
        const ChunkRecord& record = records_[index];
        int planned = outside_neighbors(record.pos);
        int ready = planned;
        int drawn = counted(record.drawn); // itself included
        int loaded = counted(record.loaded);
        for_neighbors(record.pos, [&](std::uint32_t neighbor) {
            const ChunkRecord& other = records_[neighbor];
            planned += counted(counts_as_planned(other.state));
            ready += counted(counts_as_ready(other.state));
            drawn += counted(other.drawn);
            loaded += counted(other.loaded);
        });
        const bool counts = std::cmp_equal(record.planned_neighbors, planned) &&
                            std::cmp_equal(record.ready_neighbors, ready) &&
                            std::cmp_equal(record.drawn_near, drawn) &&
                            std::cmp_equal(record.loaded_near, loaded);
        bool state = !record.drawn || record.loaded;
        switch (record.state) {
        case ChunkState::build_queued:
            state = state && record.loaded && record.planned_neighbors == 26;
            break;
        case ChunkState::ready:
            state = state && record.ready_neighbors < 26;
            break;
        case ChunkState::complete:
            state = state && record.ready_neighbors == 26;
            break;
        default:
            break;
        }
        if (!counts || !state) {
            ++broken;
        }
    }
    return broken;
}

std::uint32_t ChunkRecords::outside(const Region& kept) const {
    std::uint32_t count = 0;
    for (std::uint32_t index = 0; index < end_index(); ++index) {
        const ChunkRecord& record = records_[index];
        if (live(index) && !record.edited && record.state != ChunkState::unloading &&
            !kept.column_planned({.x = record.pos.x, .z = record.pos.z})) {
            ++count;
        }
    }
    return count;
}

} // namespace vx
