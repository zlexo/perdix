#include <vx/world/chunk_storage.hpp>

#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace vx {
namespace {

// A palette with room for every entry `bits` wide: 2, 4, 16 or 256 states.
std::unique_ptr<BlockState[]> make_palette(int bits) { // NOLINT(modernize-avoid-c-arrays)
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    return std::make_unique<BlockState[]>(std::size_t{1} << static_cast<unsigned>(bits));
}

// The narrowest width with room for `states` palette entries, 2 to 256.
int width_for(std::uint32_t states) {
    int bits = 1;
    while ((std::uint32_t{1} << static_cast<unsigned>(bits)) < states) {
        bits *= 2;
    }
    return bits;
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
    std::array<std::uint16_t, 256> uses = count_uses();
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

void ChunkStorage::compact() {
    if (is_uniform()) {
        return;
    }
    if (palette_ == nullptr) {
        compact_direct();
        return;
    }
    const std::array<std::uint16_t, 256> uses = count_uses();
    std::uint32_t live = 0;
    std::uint32_t last_live = 0;
    for (std::uint32_t entry = 0; entry < palette_size_; ++entry) {
        if (uses[entry] > 0) {
            ++live;
            last_live = entry;
        }
    }
    if (live == 1) {
        *this = uniform(palette_[last_live]);
        return;
    }
    const int bits = width_for(live);
    if (bits == indices_.bits()) {
        return; // no narrower: unused entries stay, for set to reuse
    }
    // The live entries, renumbered in their order, in a narrower array.
    std::array<std::uint8_t, 256> renumbered{};
    auto palette = make_palette(bits);
    std::uint16_t size = 0;
    for (std::uint32_t entry = 0; entry < palette_size_; ++entry) {
        if (uses[entry] > 0) {
            renumbered[entry] = static_cast<std::uint8_t>(size);
            palette[size++] = palette_[entry];
        }
    }
    PackedArray narrower(bits);
    for (int at = 0; at < chunk_volume; ++at) {
        const auto block = static_cast<std::uint16_t>(at);
        narrower.set(block, renumbered[indices_.get(block)]);
    }
    indices_ = std::move(narrower);
    palette_ = std::move(palette);
    palette_size_ = size;
}

void ChunkStorage::compact_direct() {
    // Each state's palette entry, numbered as the states first appear;
    // `none` for a state the chunk does not hold. A table of 128 KiB, only
    // on this rare path.
    constexpr std::uint16_t none = 0xFFFF;
    std::vector<std::uint16_t> entry_of(std::size_t{1} << 16U, none);
    std::array<BlockState, 256> states{};
    std::uint16_t count = 0;
    for (int at = 0; at < chunk_volume; ++at) {
        const std::uint32_t value = indices_.get(static_cast<std::uint16_t>(at));
        if (entry_of[value] == none) {
            if (count == states.size()) {
                return; // more than 256 states: it stays direct
            }
            entry_of[value] = count;
            states[count++] = static_cast<BlockState>(value);
        }
    }
    if (count == 1) {
        *this = uniform(states[0]);
        return;
    }
    const int bits = width_for(count);
    auto palette = make_palette(bits);
    std::copy_n(states.begin(), count, palette.get());
    PackedArray narrower(bits);
    for (int at = 0; at < chunk_volume; ++at) {
        const auto block = static_cast<std::uint16_t>(at);
        narrower.set(block, entry_of[indices_.get(block)]);
    }
    indices_ = std::move(narrower);
    palette_ = std::move(palette);
    palette_size_ = count;
}

std::array<std::uint16_t, 256> ChunkStorage::count_uses() const noexcept {
    // Word by word, each word's entries into eight tallies in turn, summed
    // at the end: in a run of one state, a single tally's every count would
    // wait on the one before. Nearly four times faster on the reference
    // machine.
    constexpr std::size_t tallies = 8;
    std::array<std::array<std::uint16_t, 256>, tallies> counts{};
    const auto bits = static_cast<unsigned>(indices_.bits());
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    for (const std::uint64_t word : indices_.words()) {
        std::size_t tally = 0;
        for (unsigned shift = 0; shift < 64; shift += bits) {
            ++counts[tally][(word >> shift) & mask];
            tally = (tally + 1) % tallies;
        }
    }
    std::array<std::uint16_t, 256> uses{};
    for (std::size_t entry = 0; entry < uses.size(); ++entry) {
        unsigned sum = 0;
        for (const std::array<std::uint16_t, 256>& count : counts) {
            sum += count[entry];
        }
        uses[entry] = static_cast<std::uint16_t>(sum);
    }
    return uses;
}

std::uint32_t ChunkStorage::palette_capacity() const noexcept {
    return std::uint32_t{1} << static_cast<unsigned>(indices_.bits());
}

std::size_t ChunkStorage::memory_bytes() const noexcept {
    const std::size_t palette = palette_ != nullptr ? palette_capacity() * sizeof(BlockState) : 0;
    return indices_.memory_bytes() + palette;
}

} // namespace vx
