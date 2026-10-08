#include "../benchmarks.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>

#include <nanobench.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace vx::bench {
namespace {

// A chunk whose bottom layer is a floor: its top is y = 1.
ChunkStorage floor_chunk() {
    ChunkStorage chunk;
    for (int x = 0; x < chunk_size; ++x) {
        for (int z = 0; z < chunk_size; ++z) {
            chunk.set(local_index(x, 0, z), static_cast<BlockState>(std::uint16_t{1}));
        }
    }
    return chunk;
}

// One tick for the bench: the player turns a little each tick, so it goes
// round in a circle, about 9 blocks across walking and 14 sprint-jumping,
// and never leaves the chunk.
void timed_tick(Player& player, float& yaw, const MovementKeys& keys, const BlockBoxes& world,
                const MovementNumbers& numbers, std::vector<Box>& scratch) {
    constexpr float turn = 0.05F; // radians a tick
    yaw = std::fmod(yaw + turn, 2.0F * std::numbers::pi_v<float>);
    tick(player, {.held = keys, .yaw = yaw}, world, numbers, scratch);
    ankerl::nanobench::doNotOptimizeAway(player.position);
}

} // namespace

// RFC 0010's budget: under 2 microseconds a tick walking on flat ground. The
// scratch is kept from tick to tick, as the app keeps it.
void movement(ankerl::nanobench::Bench& bench) {
    const ChunkStorage chunk = floor_chunk();
    const ChunkBoxes world(chunk, {});
    const MovementNumbers numbers;
    std::vector<Box> scratch;
    scratch.reserve(256);
    const auto start = [] {
        Player player;
        player.position = {.x = 16.5, .y = 1.0, .z = 16.5};
        player.on_ground = true;
        return player;
    };

    Player walker = start();
    float walker_yaw = 0.0F;
    bench.batch(1).unit("tick").run("movement: one tick walking on flat ground", [&] {
        timed_tick(walker, walker_yaw, {.forward = true}, world, numbers, scratch);
    });

    // Every key that matters held: sprinting, jumping and landing in turn.
    Player jumper = start();
    float jumper_yaw = 0.0F;
    bench.batch(1).unit("tick").run("movement: one tick sprint-jumping on flat ground", [&] {
        timed_tick(jumper, jumper_yaw, {.forward = true, .jump = true, .sprint = true}, world,
                   numbers, scratch);
    });
}

} // namespace vx::bench
