#include <vx/world/packed_array.hpp>

#include <vx/world/block_data_pool.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>

namespace vx {
namespace {

// log2 of a width the array takes: 1, 2, 4, 8 or 16.
std::uint8_t log2_of_width(int bits) {
    assert(bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16);
    return static_cast<std::uint8_t>(std::countr_zero(static_cast<unsigned>(bits)));
}

} // namespace

PackedArray::PackedArray(int bits) : log2_bits_(log2_of_width(bits)) {
    mask_ = (std::uint32_t{1} << static_cast<unsigned>(bits)) - 1;
    // Zeroed, from the pool's class for the width.
    words_ = block_data::allocate(log2_bits_);
}

void PackedArray::release() noexcept {
    if (words_ != nullptr) {
        block_data::deallocate(words_, log2_bits_);
        words_ = nullptr;
    }
}

PackedArray PackedArray::clone() const {
    if (empty()) {
        return {};
    }
    PackedArray copy(bits());
    std::copy_n(words_, word_count(), copy.words_);
    return copy;
}

PackedArray PackedArray::repacked(int bits) const {
    assert(!empty());
    PackedArray result(bits);
    for (int index = 0; index < chunk_volume; ++index) {
        const auto at = static_cast<std::uint16_t>(index);
        result.set(at, get(at));
    }
    return result;
}

} // namespace vx
