#include <vx/world/chunk_storage.hpp>

#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace vx {
namespace {

// A palette with room for every entry `bits` wide: 2, 4, 16 or 256 states.
std::unique_ptr<BlockState[]> make_palette(int bits) { // NOLINT(modernize-avoid-c-arrays)
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    return std::make_unique<BlockState[]>(std::size_t{1} << static_cast<unsigned>(bits));
}

} // namespace

ChunkStorage ChunkStorage::uniform(BlockState state) noexcept {
    ChunkStorage chunk;
    chunk.uniform_ = state;
    return chunk;
}

ChunkStorage::ChunkStorage(ChunkStorage&& other) noexcept
    : indices_(std::move(other.indices_)), palette_(std::move(other.palette_)),
      palette_size_(std::exchange(other.palette_size_, std::uint16_t{0})),
      uniform_(std::exchange(other.uniform_, BlockState::air)) {}

ChunkStorage& ChunkStorage::operator=(ChunkStorage&& other) noexcept {
    indices_ = std::move(other.indices_);
    palette_ = std::move(other.palette_);
    palette_size_ = std::exchange(other.palette_size_, std::uint16_t{0});
    uniform_ = std::exchange(other.uniform_, BlockState::air);
    return *this;
}

ChunkStorage ChunkStorage::clone() const {
    ChunkStorage copy;
    copy.indices_ = indices_.clone();
    if (palette_ != nullptr) {
        copy.palette_ = make_palette(indices_.bits());
        std::copy_n(palette_.get(), palette_size_, copy.palette_.get());
    }
    copy.palette_size_ = palette_size_;
    copy.uniform_ = uniform_;
    return copy;
}

void ChunkStorage::set(std::uint16_t index, BlockState state) {
    if (is_uniform()) {
        if (state == uniform_) {
            return;
        }
        // The first block that differs: 1 bit, the chunk's state at 0.
        indices_ = PackedArray(1);
        palette_ = make_palette(1);
        palette_[0] = uniform_;
        palette_size_ = 1;
    }
    if (palette_ == nullptr) {
        indices_.set(index, static_cast<std::uint32_t>(state));
        return;
    }
    // A linear scan: the palette is small where most edits happen, and
    // generators never pay it (RFC 0002).
    for (std::uint32_t entry = 0; entry < palette_size_; ++entry) {
        if (palette_[entry] == state) {
            indices_.set(index, entry);
            return;
        }
    }
    const std::uint32_t entry = add_to_palette(index, state);
    indices_.set(index, entry);
}

std::uint32_t ChunkStorage::add_to_palette(std::uint16_t index, BlockState state) {
    if (palette_size_ < palette_capacity()) {
        palette_[palette_size_] = state;
        return palette_size_++;
    }
    // A full palette: count each entry's blocks, the one at `index` as
    // overwritten already, and reuse an entry left with none. One pass,
    // only here, so that overwritten states cannot force growth.
    std::array<std::uint16_t, 256> uses{};
    for (int at = 0; at < chunk_volume; ++at) {
        ++uses[indices_.get(static_cast<std::uint16_t>(at))];
    }
    --uses[indices_.get(index)];
    for (std::uint32_t entry = 0; entry < palette_size_; ++entry) {
        if (uses[entry] == 0) {
            palette_[entry] = state;
            return entry;
        }
    }
    grow();
    if (palette_ == nullptr) {
        return static_cast<std::uint32_t>(state);
    }
    palette_[palette_size_] = state;
    return palette_size_++;
}

void ChunkStorage::grow() {
    const int bits = indices_.bits();
    if (bits < 8) {
        indices_ = indices_.repacked(bits * 2);
        auto palette = make_palette(bits * 2);
        std::copy_n(palette_.get(), palette_size_, palette.get());
        palette_ = std::move(palette);
        return;
    }
    // A 257th state: every block holds its state, with no palette.
    PackedArray states(16);
    for (int at = 0; at < chunk_volume; ++at) {
        const auto block = static_cast<std::uint16_t>(at);
        states.set(block, static_cast<std::uint32_t>(palette_[indices_.get(block)]));
    }
    indices_ = std::move(states);
    palette_.reset();
    palette_size_ = 0;
}

std::uint32_t ChunkStorage::palette_capacity() const noexcept {
    return std::uint32_t{1} << static_cast<unsigned>(indices_.bits());
}

std::size_t ChunkStorage::memory_bytes() const noexcept {
    const std::size_t palette = palette_ != nullptr ? palette_capacity() * sizeof(BlockState) : 0;
    return indices_.memory_bytes() + palette;
}

} // namespace vx
