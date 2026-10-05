#pragma once

#include <vx/world/coordinates.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vx {

// One entry for each block of a chunk, each 1, 2, 4, 8 or 16 bits wide,
// packed into 64-bit words (RFC 0002): a chunk's palette indices, or at 16
// bits its block states themselves. The widths are powers of two, so a word
// holds a whole number of entries and none straddles two words: reading one
// is a shift and a mask, with no branch.
class PackedArray {
public:
    // Every entry 0, `bits` wide: 1, 2, 4, 8 or 16.
    explicit PackedArray(int bits);

    PackedArray(PackedArray&& other) noexcept = default;
    PackedArray& operator=(PackedArray&& other) noexcept = default;
    // Copies are explicit, through clone(): copying a chunk's blocks is
    // copy on write (RFC 0005), never a side effect.
    PackedArray(const PackedArray&) = delete;
    PackedArray& operator=(const PackedArray&) = delete;
    ~PackedArray() = default;

    [[nodiscard]] PackedArray clone() const;

    [[nodiscard]] std::uint32_t get(std::uint16_t index) const noexcept {
        const std::uint32_t bit = std::uint32_t{index} << log2_bits_;
        return static_cast<std::uint32_t>(words_[bit >> 6U] >> (bit & 63U)) & mask_;
    }

    // `value` must fit the width.
    void set(std::uint16_t index, std::uint32_t value) noexcept {
        assert(value <= mask_);
        const std::uint32_t bit = std::uint32_t{index} << log2_bits_;
        const std::uint32_t shift = bit & 63U;
        std::uint64_t& word = words_[bit >> 6U];
        word = (word & ~(std::uint64_t{mask_} << shift)) | (std::uint64_t{value} << shift);
    }

    // The same entries at another width: wider to make room for a bigger
    // palette, or narrower once every entry fits.
    [[nodiscard]] PackedArray repacked(int bits) const;

    [[nodiscard]] int bits() const noexcept { return 1 << log2_bits_; }
    [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
        return {words_.get(), word_count()};
    }
    // The words' size: chunk_volume entries at the width, 4 KiB at 1 bit to
    // 64 KiB at 16.
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return word_count() * sizeof(std::uint64_t);
    }

private:
    [[nodiscard]] std::size_t word_count() const noexcept {
        return (std::size_t{chunk_volume} << log2_bits_) / 64;
    }

    // A pointer and two small fields, so the array adds 16 bytes to a chunk's
    // header, where RFC 0002 budgets 32 bytes for a whole uniform chunk: a
    // std::vector would take 24 for the words alone.
    std::unique_ptr<std::uint64_t[]> words_; // NOLINT(modernize-avoid-c-arrays)
    std::uint32_t mask_ = 0;
    std::uint8_t log2_bits_ = 0;
};

} // namespace vx
