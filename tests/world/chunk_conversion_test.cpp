#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {

constexpr vx::BlockState state(int id) {
    return static_cast<vx::BlockState>(static_cast<std::uint16_t>(id));
}

std::span<const vx::BlockState, vx::chunk_volume> whole(const std::vector<vx::BlockState>& blocks) {
    return std::span<const vx::BlockState, vx::chunk_volume>(blocks.data(), vx::chunk_volume);
}

// A plain chunk of `states` distinct states, ids from `first_id` on: each at
// least once, the rest of the blocks random among them. Seeded.
std::vector<vx::BlockState> random_blocks(int states, int first_id, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> pick(0, states - 1);
    std::vector<vx::BlockState> blocks(vx::chunk_volume);
    for (vx::BlockState& block : blocks) {
        block = state(first_id + pick(generator));
    }
    for (int id = 0; id < states; ++id) {
        blocks[static_cast<std::size_t>(id) * 7] = state(first_id + id);
    }
    return blocks;
}

// Each storage mode, from one plain chunk: uniform, paletted and direct.
std::vector<std::vector<vx::BlockState>> one_of_each_mode() {
    return {random_blocks(1, 5, 1), random_blocks(10, 5, 2), random_blocks(300, 5, 3)};
}

} // namespace

TEST_CASE("a plain chunk survives from_dense then to_dense at every width, through one scratch") {
    // Every round draws on the same ids, so an entry that from_dense left in
    // the table would make a state look numbered already in the next round.
    // The last round follows two direct ones, which must leave it clean too.
    vx::PaletteScratch scratch;
    for (const auto& [states, bits] :
         {std::pair{1, 0}, std::pair{2, 1}, std::pair{3, 2}, std::pair{4, 2}, std::pair{16, 4},
          std::pair{17, 8}, std::pair{256, 8}, std::pair{257, 16}, std::pair{3'000, 16},
          std::pair{10, 4}}) {
        INFO(states, " states");
        const std::vector<vx::BlockState> blocks =
            random_blocks(states, 1, static_cast<std::uint32_t>(states));
        const vx::ChunkStorage chunk = vx::ChunkStorage::from_dense(whole(blocks), scratch);
        CHECK(chunk.bits_per_block() == bits);
        std::vector<vx::BlockState> out(vx::chunk_volume);
        chunk.to_dense(std::span<vx::BlockState, vx::chunk_volume>(out.data(), vx::chunk_volume));
        CHECK(out == blocks);
        std::size_t differing = 0;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            if (chunk.get(static_cast<std::uint16_t>(index)) != blocks[index]) {
                ++differing;
            }
        }
        CHECK(differing == 0);
    }
}

TEST_CASE("a chunk from from_dense owns what set-built storage would, and set grows it alike") {
    vx::PaletteScratch scratch;
    const vx::ChunkStorage single =
        vx::ChunkStorage::from_dense(whole(random_blocks(1, 9, 4)), scratch);
    CHECK(single.is_uniform());
    CHECK(single.memory_bytes() == 0);

    // Three states take 2 bits, with room for a fourth before growing.
    vx::ChunkStorage chunk = vx::ChunkStorage::from_dense(whole(random_blocks(3, 1, 5)), scratch);
    CHECK(chunk.memory_bytes() == 8'192 + 8);
    chunk.set(0, state(40));
    CHECK(chunk.bits_per_block() == 2);
    chunk.set(1, state(41));
    CHECK(chunk.bits_per_block() == 4);
    CHECK(chunk.get(0) == state(40));
    CHECK(chunk.get(1) == state(41));
}

TEST_CASE("to_dense with a layout fills the inside of a padded array and leaves its border alone") {
    // RFC 0003's mesher input: 34 blocks a side, the chunk at 1 to 32.
    constexpr std::size_t side = 34;
    const vx::BlockState border = state(9'999);
    vx::PaletteScratch scratch;
    for (const std::vector<vx::BlockState>& blocks : one_of_each_mode()) {
        const vx::ChunkStorage chunk = vx::ChunkStorage::from_dense(whole(blocks), scratch);
        INFO(chunk.bits_per_block(), " bits");
        std::vector<vx::BlockState> padded(side * side * side, border);
        chunk.to_dense(
            padded,
            {.offset = 1 + side + (side * side), .row_stride = side, .layer_stride = side * side});
        std::size_t wrong = 0;
        for (int y = 0; y < static_cast<int>(side); ++y) {
            for (int z = 0; z < static_cast<int>(side); ++z) {
                for (int x = 0; x < static_cast<int>(side); ++x) {
                    const bool inside = x >= 1 && x <= 32 && y >= 1 && y <= 32 && z >= 1 && z <= 32;
                    const vx::BlockState expected =
                        inside ? chunk.get(vx::local_index(x - 1, y - 1, z - 1)) : border;
                    const std::size_t at = static_cast<std::size_t>(x) +
                                           (static_cast<std::size_t>(z) * side) +
                                           (static_cast<std::size_t>(y) * side * side);
                    if (padded[at] != expected) {
                        ++wrong;
                    }
                }
            }
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE("copy_face gives a face's blocks in local_index order of the other two axes") {
    vx::PaletteScratch scratch;
    for (const std::vector<vx::BlockState>& blocks : one_of_each_mode()) {
        const vx::ChunkStorage chunk = vx::ChunkStorage::from_dense(whole(blocks), scratch);
        for (const vx::Face face : {vx::Face::pos_x, vx::Face::neg_x, vx::Face::pos_y,
                                    vx::Face::neg_y, vx::Face::pos_z, vx::Face::neg_z}) {
            INFO(chunk.bits_per_block(), " bits, face ", static_cast<int>(face));
            std::vector<vx::BlockState> out(vx::chunk_area);
            chunk.copy_face(face,
                            std::span<vx::BlockState, vx::chunk_area>(out.data(), vx::chunk_area));
            const bool positive =
                face == vx::Face::pos_x || face == vx::Face::pos_y || face == vx::Face::pos_z;
            const int fixed = positive ? vx::chunk_size - 1 : 0;
            std::size_t wrong = 0;
            for (int slow = 0; slow < vx::chunk_size; ++slow) {
                for (int fast = 0; fast < vx::chunk_size; ++fast) {
                    std::uint16_t index = 0;
                    if (face == vx::Face::pos_x || face == vx::Face::neg_x) {
                        index = vx::local_index(fixed, slow, fast); // out[z + 32 * y]
                    } else if (face == vx::Face::pos_y || face == vx::Face::neg_y) {
                        index = vx::local_index(fast, fixed, slow); // out[x + 32 * z]
                    } else {
                        index = vx::local_index(fast, slow, fixed); // out[x + 32 * y]
                    }
                    const auto at = static_cast<std::size_t>(fast + (slow * vx::chunk_size));
                    if (out[at] != chunk.get(index)) {
                        ++wrong;
                    }
                }
            }
            CHECK(wrong == 0);
        }
    }
}
