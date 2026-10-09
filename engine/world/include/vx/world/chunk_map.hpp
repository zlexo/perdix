#pragma once

#include <vx/world/coordinates.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// The chunk map's hash of a chunk's position (RFC 0006): the low 21 bits of
// each coordinate packed side by side, then MurmurHash3's 64-bit finalizer
// (Austin Appleby, public domain). Both multipliers are odd and each
// xor-shift can be undone, so the finalizer is a bijection: chunks less than
// 2^21 chunks apart on every axis never share a hash. The map compares all
// 12 bytes, so positions farther apart that do share one are still told
// apart.
[[nodiscard]] constexpr std::uint64_t chunk_hash(ChunkPos pos) noexcept {
    constexpr std::uint64_t low21 = 0x1F'FFFFU;
    std::uint64_t key = (static_cast<std::uint32_t>(pos.x) & low21) |
                        ((static_cast<std::uint32_t>(pos.y) & low21) << 21U) |
                        ((static_cast<std::uint32_t>(pos.z) & low21) << 42U);
    key = (key ^ (key >> 33U)) * 0xFF51'AFD7'ED55'8CCDULL;
    key = (key ^ (key >> 33U)) * 0xC4CE'B9FE'1A85'EC53ULL;
    return key ^ (key >> 33U);
}

// How far the map's positions sit from the slot their hash names: 0 for one
// in its own slot.
struct ProbeStats {
    std::size_t longest = 0;
    double mean = 0.0; // 0 for an empty map
};

// The world's index from a chunk's position to its record (RFC 0006): open
// addressing with linear probing over a power-of-two array of 16-byte slots,
// doubled before it is more than half full, so a hit takes at most 1.5 probes
// on average and a miss 2.5 (Knuth). Erasing shifts the rest of a probe run
// back rather than leaving a tombstone, so lookups after hours of streaming
// are as short as on the first chunk. Not thread-safe: one thread owns it,
// RFC 0005's world thread.
class ChunkMap {
public:
    // A record index the map never holds: it marks an empty slot.
    static constexpr std::uint32_t no_record = 0xFFFF'FFFFU;

    // The record of the chunk at `pos`, if the map holds one.
    [[nodiscard]] std::optional<std::uint32_t> find(ChunkPos pos) const noexcept {
        if (slots_.empty()) {
            return std::nullopt;
        }
        // Never more than half full, so a run always ends at an empty slot.
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t at = chunk_hash(pos) & mask;; at = (at + 1) & mask) {
            const Slot& slot = slots_[at];
            if (slot.record == no_record) {
                return std::nullopt;
            }
            if (slot.pos == pos) {
                return slot.record;
            }
        }
    }

    // Maps `pos` to `record`, which must not be no_record. False, changing
    // nothing, when `pos` is mapped already.
    bool insert(ChunkPos pos, std::uint32_t record);
    // False when `pos` is not mapped.
    bool erase(ChunkPos pos) noexcept;
    // Room for `count` positions without growing.
    void reserve(std::size_t count);
    // Every position gone; the slots stay.
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    // The slots: a power of two, at least 16, or none before the first
    // insert or reserve.
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return capacity() * sizeof(Slot); }
    // Walks every slot: for tests and diagnostics, not the hot path.
    [[nodiscard]] ProbeStats probe_stats() const noexcept;

private:
    struct Slot {
        ChunkPos pos;
        std::uint32_t record = no_record;
    };
    static_assert(sizeof(Slot) == 16, "RFC 0006: a slot is the position and a record index");

    // Moves every position into `capacity` slots, a power of two.
    void rehash(std::size_t capacity);

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

} // namespace vx
