#pragma once

#include <vx/world/coordinates.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace vx {

// One entry for each block of a chunk, each 1, 2, 4, 8 or 16 bits wide,
// packed into 64-bit words (RFC 0002): a chunk's palette indices, or at 16
// bits its block states themselves. The widths are powers of two, so a word
// holds a whole number of entries and none straddles two words: reading one
// is a shift and a mask, with no branch. The words come from the block data
// pool, a size class for each width.
class PackedArray {
public:
    // No entries and no memory: a uniform chunk's, and what a move leaves.
    PackedArray() noexcept = default;
    // Every entry 0, `bits` wide: 1, 2, 4, 8 or 16.
    explicit PackedArray(int bits);

    PackedArray(PackedArray&& other) noexcept
        : words_(std::exchange(other.words_, nullptr)), mask_(std::exchange(other.mask_, 0U)),
          log2_bits_(std::exchange(other.log2_bits_, std::uint8_t{0})) {}
    // Gives this array's words back to the pool first.
    PackedArray& operator=(PackedArray&& other) noexcept {
        if (this != &other) {
            release();
            words_ = std::exchange(other.words_, nullptr);
            mask_ = std::exchange(other.mask_, 0U);
            log2_bits_ = std::exchange(other.log2_bits_, std::uint8_t{0});
        }
        return *this;
    }
    // Copies are explicit, through clone(): copying a chunk's blocks is
    // copy on write (RFC 0005), never a side effect.
    PackedArray(const PackedArray&) = delete;
    PackedArray& operator=(const PackedArray&) = delete;
    ~PackedArray() { release(); }

    [[nodiscard]] PackedArray clone() const;

    [[nodiscard]] std::uint32_t get(std::uint16_t index) const noexcept {
        assert(!empty());
        const std::uint32_t bit = std::uint32_t{index} << log2_bits_;
        return static_cast<std::uint32_t>(words_[bit >> 6U] >> (bit & 63U)) & mask_;
    }

    // `value` must fit the width.
    void set(std::uint16_t index, std::uint32_t value) noexcept {
        assert(!empty() && value <= mask_);
        const std::uint32_t bit = std::uint32_t{index} << log2_bits_;
        const std::uint32_t shift = bit & 63U;
        std::uint64_t& word = words_[bit >> 6U];
        word = (word & ~(std::uint64_t{mask_} << shift)) | (std::uint64_t{value} << shift);
    }

    // The same entries at another width: wider to make room for a bigger
    // palette, or narrower once every entry fits.
    [[nodiscard]] PackedArray repacked(int bits) const;

    [[nodiscard]] bool empty() const noexcept { return words_ == nullptr; }
    // 0 when empty.
    [[nodiscard]] int bits() const noexcept { return empty() ? 0 : 1 << log2_bits_; }
    // The words themselves: entry i holds bits i * width up, lowest bits
    // first. Writable for bulk conversion, a word at a time.
    [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
        return {words_, word_count()};
    }
    [[nodiscard]] std::span<std::uint64_t> words() noexcept { return {words_, word_count()}; }
    // The words' size: chunk_volume entries at the width, 4 KiB at 1 bit to
    // 64 KiB at 16, and none when empty.
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return word_count() * sizeof(std::uint64_t);
    }

private:
    // The words that hold chunk_volume entries, 2^log2_bits bits each.
    [[nodiscard]] static constexpr std::size_t words_for(std::uint8_t log2_bits) noexcept {
        return (std::size_t{chunk_volume} << log2_bits) / 64;
    }
    [[nodiscard]] std::size_t word_count() const noexcept {
        return empty() ? 0 : words_for(log2_bits_);
    }

    // Back to the pool, leaving the array empty.
    void release() noexcept;

    // A pointer and two small fields, so the array adds 16 bytes to a chunk's
    // header, where RFC 0002 budgets 32 bytes for a whole uniform chunk: a
    // std::vector would take 24 for the words alone. The words belong to the
    // block data pool's class for the width, and go back to it.
    std::uint64_t* words_ = nullptr;
    std::uint32_t mask_ = 0;
    std::uint8_t log2_bits_ = 0;
};

} // namespace vx
