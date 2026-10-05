#include <vx/world/chunk_storage.hpp>

#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cassert>
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

// A packed array `bits` wide holding value(i) for every block i, filled a
// word at a time.
template <typename Value> PackedArray pack(int bits, Value value) {
    PackedArray array(bits);
    const auto width = static_cast<unsigned>(bits);
    const std::size_t per_word = 64 / width;
    std::size_t at = 0;
    for (std::uint64_t& word : array.words()) {
        std::uint64_t packed = 0;
        for (std::size_t entry = 0; entry < per_word; ++entry) {
            packed |= std::uint64_t{value(at++)} << (entry * width);
        }
        word = packed;
    }
    return array;
}

// The entries from `first` on, `bits` wide, each looked up and written to
// `out` until it is full: a word at a time, shifting entries out of it.
template <typename Lookup>
void decode(std::span<const std::uint64_t> words, unsigned bits, std::size_t first,
            std::span<BlockState> out, Lookup lookup) {
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    std::size_t bit = first * bits;
    std::size_t done = 0;
    while (done < out.size()) {
        std::uint64_t word = words[bit / 64] >> (bit % 64);
        const std::size_t run = std::min((64 - (bit % 64)) / bits, out.size() - done);
        for (std::size_t entry = 0; entry < run; ++entry) {
            out[done + entry] = lookup(word & mask);
            word >>= bits;
        }
        done += run;
        bit += run * bits;
    }
}

} // namespace

PaletteScratch::PaletteScratch()
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    : entry_of_(std::make_unique<std::uint16_t[]>(std::size_t{1} << 16U)),
      // NOLINTNEXTLINE(modernize-avoid-c-arrays)
      entries_(std::make_unique<std::uint8_t[]>(chunk_volume)) {
    std::fill_n(entry_of_.get(), std::size_t{1} << 16U, none);
}

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

ChunkStorage ChunkStorage::from_dense(std::span<const BlockState, chunk_volume> blocks,
                                      PaletteScratch& scratch) {
    // Each state's entry, numbered as the states first appear, until a
    // 257th means direct mode.
    std::uint16_t* const entry_of = scratch.entry_of_.get();
    std::uint8_t* const entries = scratch.entries_.get();
    std::array<BlockState, 256> states{};
    std::size_t count = 0;
    bool direct = false;
    for (std::size_t at = 0; at < blocks.size(); ++at) {
        const auto value = static_cast<std::uint16_t>(blocks[at]);
        if (entry_of[value] == PaletteScratch::none) {
            if (count == states.size()) {
                direct = true;
                break;
            }
            entry_of[value] = static_cast<std::uint16_t>(count);
            states[count++] = blocks[at];
        }
        entries[at] = static_cast<std::uint8_t>(entry_of[value]);
    }
    // The table goes back as it came, `none` throughout, touching only the
    // entries this call set.
    for (std::size_t entry = 0; entry < count; ++entry) {
        entry_of[static_cast<std::uint16_t>(states[entry])] = PaletteScratch::none;
    }

    ChunkStorage chunk;
    if (direct) {
        chunk.indices_ =
            pack(16, [&](std::size_t at) { return static_cast<std::uint16_t>(blocks[at]); });
        return chunk;
    }
    if (count == 1) {
        return uniform(states[0]);
    }
    const int bits = width_for(static_cast<std::uint32_t>(count));
    chunk.indices_ = pack(bits, [&](std::size_t at) { return entries[at]; });
    chunk.palette_ = make_palette(bits);
    std::copy_n(states.begin(), count, chunk.palette_.get());
    chunk.palette_size_ = static_cast<std::uint16_t>(count);
    return chunk;
}

void ChunkStorage::to_dense(std::span<BlockState, chunk_volume> out) const noexcept {
    decode_run(0, out);
}

void ChunkStorage::to_dense(std::span<BlockState> out, DenseLayout layout) const noexcept {
    // The last row must end inside `out`: subspan does not check. (Written
    // inside the assert, so a release build has no unused variable.)
    assert(layout.offset + ((std::size_t{chunk_size} - 1) * layout.row_stride) +
               ((std::size_t{chunk_size} - 1) * layout.layer_stride) + std::size_t{chunk_size} <=
           out.size());
    // Row by row: a row of 32 blocks along x is consecutive both here and in
    // `out`.
    for (int y = 0; y < chunk_size; ++y) {
        for (int z = 0; z < chunk_size; ++z) {
            const std::size_t row = layout.offset +
                                    (static_cast<std::size_t>(z) * layout.row_stride) +
                                    (static_cast<std::size_t>(y) * layout.layer_stride);
            decode_run(local_index(0, y, z), out.subspan(row, chunk_size));
        }
    }
}

void ChunkStorage::copy_face(Face face, std::span<BlockState, chunk_area> out) const noexcept {
    constexpr int last = chunk_size - 1;
    const auto row = [&](int at) {
        return std::span<BlockState>(out).subspan(static_cast<std::size_t>(at) * chunk_size,
                                                  chunk_size);
    };
    switch (face) {
    case Face::pos_y:
    case Face::neg_y: {
        const int y = face == Face::pos_y ? last : 0;
        for (int z = 0; z < chunk_size; ++z) {
            decode_run(local_index(0, y, z), row(z));
        }
        return;
    }
    case Face::pos_z:
    case Face::neg_z: {
        const int z = face == Face::pos_z ? last : 0;
        for (int y = 0; y < chunk_size; ++y) {
            decode_run(local_index(0, y, z), row(y));
        }
        return;
    }
    case Face::pos_x:
    case Face::neg_x: {
        // Neighbors along z are 32 entries apart, so block by block.
        const int x = face == Face::pos_x ? last : 0;
        for (int y = 0; y < chunk_size; ++y) {
            for (int z = 0; z < chunk_size; ++z) {
                row(y)[static_cast<std::size_t>(z)] = get(local_index(x, y, z));
            }
        }
        return;
    }
    }
}

void ChunkStorage::decode_run(std::uint16_t first, std::span<BlockState> out) const noexcept {
    if (is_uniform()) {
        std::ranges::fill(out, uniform_);
        return;
    }
    const auto bits = static_cast<unsigned>(indices_.bits());
    if (palette_ == nullptr) {
        decode(indices_.words(), bits, first, out,
               [](std::uint64_t state) { return static_cast<BlockState>(state); });
        return;
    }
    const BlockState* const palette = palette_.get();
    decode(indices_.words(), bits, first, out,
           [palette](std::uint64_t entry) { return palette[entry]; });
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
