#include <vx/world/chunk_map.hpp>

#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace vx {
namespace {

// The fewest slots a map that holds anything has.
constexpr std::size_t least_capacity = 16;

// The slots for `count` positions: a power of two, with `count` at most half
// of it. The doubling stops at the highest power of two, which no vector can
// hold, so too large a count fails to allocate instead of looping forever.
std::size_t capacity_for(std::size_t count) noexcept {
    std::size_t capacity = least_capacity;
    while (capacity / 2 < count && capacity <= std::numeric_limits<std::size_t>::max() / 2) {
        capacity *= 2;
    }
    return capacity;
}

} // namespace

bool ChunkMap::insert(ChunkPos pos, std::uint32_t record) {
    assert(record != no_record);
    if (size_ + 1 > slots_.size() / 2) {
        // A refused insert changes nothing, so it must not double the slots.
        if (find(pos).has_value()) {
            return false;
        }
        rehash(capacity_for(size_ + 1));
    }
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t at = chunk_hash(pos) & mask;; at = (at + 1) & mask) {
        Slot& slot = slots_[at];
        if (slot.record == no_record) {
            slot = {.pos = pos, .record = record};
            ++size_;
            return true;
        }
        if (slot.pos == pos) {
            return false;
        }
    }
}

bool ChunkMap::erase(ChunkPos pos) noexcept {
    if (slots_.empty()) {
        return false;
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t hole = chunk_hash(pos) & mask;
    for (;; hole = (hole + 1) & mask) {
        if (slots_[hole].record == no_record) {
            return false;
        }
        if (slots_[hole].pos == pos) {
            break;
        }
    }
    // The rest of the run shifts back over the hole: each later position
    // whose own slot is not between the hole and where it sits could have
    // been placed in the hole, so it moves there, and its old place is the
    // new hole. The run ends at an empty slot.
    for (std::size_t at = (hole + 1) & mask; slots_[at].record != no_record; at = (at + 1) & mask) {
        const std::size_t home = chunk_hash(slots_[at].pos) & mask;
        if (((at - home) & mask) >= ((at - hole) & mask)) {
            slots_[hole] = slots_[at];
            hole = at;
        }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
}

void ChunkMap::reserve(std::size_t count) {
    const std::size_t capacity = capacity_for(count);
    if (capacity > slots_.size()) {
        rehash(capacity);
    }
}

void ChunkMap::clear() noexcept {
    std::ranges::fill(slots_, Slot{});
    size_ = 0;
}

ProbeStats ChunkMap::probe_stats() const noexcept {
    ProbeStats stats;
    if (size_ == 0) {
        return stats;
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t total = 0;
    for (std::size_t at = 0; at < slots_.size(); ++at) {
        if (slots_[at].record == no_record) {
            continue;
        }
        const std::size_t distance = (at - (chunk_hash(slots_[at].pos) & mask)) & mask;
        stats.longest = std::max(stats.longest, distance);
        total += distance;
    }
    stats.mean = static_cast<double>(total) / static_cast<double>(size_);
    return stats;
}

void ChunkMap::rehash(std::size_t capacity) {
    const std::vector<Slot> old = std::exchange(slots_, std::vector<Slot>(capacity));
    const std::size_t mask = capacity - 1;
    for (const Slot& slot : old) {
        if (slot.record == no_record) {
            continue;
        }
        std::size_t at = chunk_hash(slot.pos) & mask;
        while (slots_[at].record != no_record) {
            at = (at + 1) & mask;
        }
        slots_[at] = slot;
    }
}

} // namespace vx
