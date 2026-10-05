#include <vx/world/coordinates.hpp>
#include <vx/world/packed_array.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

namespace {

// Values in [0, 2^bits), from a seeded generator, so a failure reproduces.
std::vector<std::uint32_t> random_values(int bits, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::uint32_t> value(0, (std::uint32_t{1} << bits) - 1);
    std::vector<std::uint32_t> values(vx::chunk_volume);
    for (std::uint32_t& entry : values) {
        entry = value(generator);
    }
    return values;
}

} // namespace

TEST_CASE("a packed array adds 16 bytes to a chunk's header") {
    // RFC 0002 budgets 32 bytes for a uniform chunk's whole header.
    CHECK(sizeof(vx::PackedArray) == 16);
}

TEST_CASE("an empty packed array has no entries and no memory, and a move leaves one") {
    const vx::PackedArray empty;
    CHECK(empty.empty());
    CHECK(empty.bits() == 0);
    CHECK(empty.memory_bytes() == 0);
    CHECK(empty.words().empty());
    CHECK(empty.clone().empty());

    // Reads what each move leaves behind, on purpose: not a stale width
    // over words it gave away.
    vx::PackedArray array(4);
    array.set(3, 9);
    vx::PackedArray moved = std::move(array);
    CHECK(moved.bits() == 4);
    CHECK(moved.get(3) == 9);
    CHECK(array.empty()); // NOLINT(bugprone-use-after-move)
    CHECK(array.bits() == 0);
    CHECK(array.memory_bytes() == 0);
    array = std::move(moved);
    CHECK(array.get(3) == 9);
    CHECK(moved.empty()); // NOLINT(bugprone-use-after-move)
}

TEST_CASE("a new packed array is all zeros, and its size follows its width") {
    for (const auto& [bits, bytes] :
         {std::pair{1, 4'096}, std::pair{2, 8'192}, std::pair{4, 16'384}, std::pair{8, 32'768},
          std::pair{16, 65'536}}) {
        INFO(bits, " bits");
        const vx::PackedArray array(bits);
        CHECK(array.bits() == bits);
        CHECK(array.memory_bytes() == static_cast<std::size_t>(bytes));
        bool all_zero = true;
        for (const std::uint64_t word : array.words()) {
            all_zero = all_zero && word == 0;
        }
        CHECK(all_zero);
    }
}

TEST_CASE("every entry reads back what was written, at every width") {
    for (const int bits : {1, 2, 4, 8, 16}) {
        INFO(bits, " bits");
        const std::vector<std::uint32_t> values =
            random_values(bits, 1234U + static_cast<std::uint32_t>(bits));
        vx::PackedArray array(bits);
        for (std::size_t index = 0; index < values.size(); ++index) {
            array.set(static_cast<std::uint16_t>(index), values[index]);
        }
        std::size_t wrong = 0;
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (array.get(static_cast<std::uint16_t>(index)) != values[index]) {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE("writing an entry leaves its neighbors in the same word alone") {
    for (const int bits : {1, 2, 4, 8, 16}) {
        INFO(bits, " bits");
        const std::uint32_t top = (std::uint32_t{1} << bits) - 1;
        vx::PackedArray array(bits);
        // Every entry of the first word set to its highest value, then the
        // middle one cleared.
        const int per_word = 64 / bits;
        for (int index = 0; index < per_word; ++index) {
            array.set(static_cast<std::uint16_t>(index), top);
        }
        array.set(static_cast<std::uint16_t>(per_word / 2), 0);
        for (int index = 0; index < per_word; ++index) {
            CHECK(array.get(static_cast<std::uint16_t>(index)) ==
                  (index == per_word / 2 ? 0U : top));
        }
        CHECK(array.get(static_cast<std::uint16_t>(per_word)) == 0); // the next word, untouched
    }
}

TEST_CASE("repacking keeps every entry, wider or narrower, and a clone is a separate copy") {
    const std::vector<std::uint32_t> values = random_values(1, 99);
    vx::PackedArray array(1);
    for (std::size_t index = 0; index < values.size(); ++index) {
        array.set(static_cast<std::uint16_t>(index), values[index]);
    }
    // Up through every width to 16 bits, then back down to 1.
    for (const int bits : {2, 4, 8, 16, 8, 4, 2, 1}) {
        INFO("to ", bits, " bits");
        array = array.repacked(bits);
        CHECK(array.bits() == bits);
        std::size_t wrong = 0;
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (array.get(static_cast<std::uint16_t>(index)) != values[index]) {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
    }
    vx::PackedArray copy = array.clone();
    copy.set(0, 1 - array.get(0));
    CHECK(copy.get(0) != array.get(0));
}
