#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr vx::BlockState state(int id) {
    return static_cast<vx::BlockState>(static_cast<std::uint16_t>(id));
}

constexpr vx::BlockState stone = state(1);
constexpr vx::BlockState dirt = state(2);
constexpr vx::BlockState grass = state(3);

// How many of the chunk's blocks differ from `expected`, one per block.
std::size_t blocks_differing(const vx::ChunkStorage& chunk,
                             const std::vector<vx::BlockState>& expected) {
    std::size_t differing = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (chunk.get(static_cast<std::uint16_t>(index)) != expected[index]) {
            ++differing;
        }
    }
    return differing;
}

} // namespace

TEST_CASE("a new chunk is uniform air, and a uniform chunk of any state owns no memory") {
    // RFC 0002 budgets 32 bytes for a uniform chunk, which is its header.
    CHECK(sizeof(vx::ChunkStorage) <= 32);
    const vx::ChunkStorage air;
    CHECK(air.is_uniform());
    CHECK(air.bits_per_block() == 0);
    CHECK(air.memory_bytes() == 0);
    CHECK(air.get(0) == vx::BlockState::air);
    CHECK(air.get(vx::chunk_volume - 1) == vx::BlockState::air);
    const vx::ChunkStorage rock = vx::ChunkStorage::uniform(stone);
    CHECK(rock.is_uniform());
    CHECK(rock.memory_bytes() == 0);
    CHECK(rock.get(12'345) == stone);
}

TEST_CASE("the first block that differs makes a chunk 1-bit, and its own state changes nothing") {
    vx::ChunkStorage chunk = vx::ChunkStorage::uniform(stone);
    chunk.set(100, stone);
    CHECK(chunk.is_uniform());
    chunk.set(100, dirt);
    CHECK_FALSE(chunk.is_uniform());
    CHECK(chunk.bits_per_block() == 1);
    CHECK(chunk.get(99) == stone);
    CHECK(chunk.get(100) == dirt);
    CHECK(chunk.get(101) == stone);
}

TEST_CASE("a chunk grows through every width to direct mode, its memory as RFC 0002 counts it") {
    // One new state at a time, each at a block of its own: the width doubles
    // at the 3rd, 5th, 17th and 257th state. The memory is RFC 0002's block
    // data plus the palette, 2 bytes an entry.
    const std::vector<std::tuple<int, int, std::size_t>> steps = {
        // states, bits, memory
        {2, 1, 4'096 + 4},    {3, 2, 8'192 + 8},     {4, 2, 8'192 + 8},      {5, 4, 16'384 + 32},
        {16, 4, 16'384 + 32}, {17, 8, 32'768 + 512}, {256, 8, 32'768 + 512}, {257, 16, 65'536}};
    vx::ChunkStorage chunk; // air is the first state
    std::vector<vx::BlockState> expected(vx::chunk_volume, vx::BlockState::air);
    int next = 1;
    for (const auto& [states, bits, memory] : steps) {
        for (; next < states; ++next) {
            const auto at = static_cast<std::uint16_t>(next * 7);
            chunk.set(at, state(next));
            expected[at] = state(next);
        }
        INFO(states, " states");
        CHECK(chunk.bits_per_block() == bits);
        CHECK(chunk.memory_bytes() == memory);
        CHECK(blocks_differing(chunk, expected) == 0);
    }
}

TEST_CASE("a full palette takes an entry that no block uses any more, instead of growing") {
    vx::ChunkStorage chunk = vx::ChunkStorage::uniform(stone);
    chunk.set(5, dirt);  // 1 bit, full: stone and dirt
    chunk.set(5, stone); // no dirt left, but it keeps its entry
    chunk.set(9, grass); // takes dirt's entry
    CHECK(chunk.bits_per_block() == 1);
    CHECK(chunk.get(5) == stone);
    CHECK(chunk.get(9) == grass);

    // The block being written frees its own entry: dirt's only block can
    // become grass without growing.
    vx::ChunkStorage single = vx::ChunkStorage::uniform(stone);
    single.set(5, dirt);
    single.set(5, grass);
    CHECK(single.bits_per_block() == 1);
    CHECK(single.get(5) == grass);
    // With every entry in use, the width doubles.
    single.set(6, dirt);
    CHECK(single.bits_per_block() == 2);
    CHECK(single.get(5) == grass);
    CHECK(single.get(6) == dirt);
}

TEST_CASE("a clone is an equal copy, and writes to either leave the other alone") {
    for (const int states : {1, 10, 300}) { // uniform, paletted, direct
        INFO(states, " states");
        vx::ChunkStorage chunk;
        std::vector<vx::BlockState> expected(vx::chunk_volume, vx::BlockState::air);
        for (int id = 1; id < states; ++id) {
            const auto at = static_cast<std::uint16_t>(id * 3);
            chunk.set(at, state(id));
            expected[at] = state(id);
        }
        vx::ChunkStorage copy = chunk.clone();
        CHECK(copy.bits_per_block() == chunk.bits_per_block());
        CHECK(copy.memory_bytes() == chunk.memory_bytes());
        CHECK(blocks_differing(copy, expected) == 0);
        copy.set(1'000, state(999));
        CHECK(blocks_differing(chunk, expected) == 0);
        chunk.set(2'000, state(998));
        CHECK(copy.get(2'000) == expected[2'000]);
    }
}

TEST_CASE("after thousands of random writes, every block reads what a plain array holds") {
    // Seeded, so a failure reproduces. Few states first, then more, so the
    // chunk passes through every width on the way to direct mode.
    std::mt19937 generator(2026);
    std::uniform_int_distribution<int> place(0, vx::chunk_volume - 1);
    vx::ChunkStorage chunk;
    std::vector<vx::BlockState> expected(vx::chunk_volume, vx::BlockState::air);
    for (const int states : {2, 3, 12, 200, 2'000}) {
        std::uniform_int_distribution<int> pick(0, states - 1);
        for (int write = 0; write < 5'000; ++write) {
            const auto at = static_cast<std::uint16_t>(place(generator));
            const vx::BlockState written = state(pick(generator));
            chunk.set(at, written);
            expected[at] = written;
        }
        INFO("up to ", states, " states, at ", chunk.bits_per_block(), " bits");
        CHECK(blocks_differing(chunk, expected) == 0);
    }
    CHECK(chunk.bits_per_block() == 16);
}

TEST_CASE("a moved-from chunk is uniform air") {
    // Reads what each move leaves behind, on purpose.
    vx::ChunkStorage chunk = vx::ChunkStorage::uniform(stone);
    chunk.set(7, dirt);
    vx::ChunkStorage moved = std::move(chunk);
    CHECK(moved.get(7) == dirt);
    CHECK(moved.get(8) == stone);
    CHECK(chunk.is_uniform()); // NOLINT(bugprone-use-after-move)
    CHECK(chunk.get(7) == vx::BlockState::air);
    CHECK(chunk.memory_bytes() == 0);
    chunk = std::move(moved);
    CHECK(chunk.get(7) == dirt);
    CHECK(moved.is_uniform()); // NOLINT(bugprone-use-after-move)
}
