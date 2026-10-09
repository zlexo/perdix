#include <vx/world/block_data_pool.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/versions.hpp>

#include <doctest/doctest.h>

#include <cstddef>

// RFC 0005's versions: an edit copies what jobs read, and a version neither
// current nor pinned goes at once.

namespace {

using vx::BlockState;
using vx::ChunkStorage;
using vx::Version;
using vx::Versions;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr BlockState dirt = static_cast<BlockState>(2);

// Stone with dirt at block 0: a chunk with block data.
ChunkStorage dirt_on_stone() {
    ChunkStorage chunk = ChunkStorage::uniform(stone);
    chunk.set(0, dirt);
    return chunk;
}

// The block data packed arrays hold, over every class.
std::size_t block_data_in_use() {
    std::size_t total = 0;
    for (const vx::BlockDataClass& sizes : vx::block_data::stats()) {
        total += sizes.in_use;
    }
    return total;
}

} // namespace

TEST_CASE("writing a pinned chunk copies it, and the pinned version stays as it was") {
    Versions<ChunkStorage> chunks;
    Version<ChunkStorage>* current = &chunks.make(dirt_on_stone());
    Version<ChunkStorage>* const pinned = current;
    chunks.pin(*pinned);
    chunks.write(current).set(0, stone);
    CHECK(current != pinned);
    CHECK(current->current());
    CHECK_FALSE(pinned->current());
    CHECK(pinned->value().get(0) == dirt);
    CHECK(current->value().get(0) == stone);
    // The copy keeps the rest of the chunk.
    CHECK(current->value().get(100) == stone);
    CHECK(chunks.copies() == 1);
    CHECK(chunks.live() == 2);
    CHECK(chunks.frozen() == 1);
    // Nobody reads the copy: the next write is in place.
    Version<ChunkStorage>* const copy = current;
    chunks.write(current).set(1, dirt);
    CHECK(current == copy);
    CHECK(chunks.copies() == 1);
    // The frozen version goes with its last pin.
    chunks.unpin(*pinned);
    CHECK(chunks.live() == 1);
    CHECK(chunks.frozen() == 0);
    CHECK(chunks.pins() == 0);
    chunks.retire(*current);
    CHECK(chunks.live() == 0);
}

TEST_CASE("a version neither current nor pinned is freed at once, and its place reused") {
    const std::size_t data_before = block_data_in_use();
    Versions<ChunkStorage> chunks;
    Version<ChunkStorage>& first = chunks.make(dirt_on_stone());
    const Version<ChunkStorage>* const place = &first;
    CHECK(block_data_in_use() > data_before);
    chunks.retire(first);
    CHECK(chunks.live() == 0);
    CHECK(block_data_in_use() == data_before);
    const Version<ChunkStorage>& next = chunks.make(ChunkStorage::uniform(dirt));
    CHECK(&next == place);
    CHECK(next.current());
    CHECK(next.value().get(5) == dirt);
}
