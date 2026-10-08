#include "../benchmarks.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>

#include <nanobench.h>

#include <array>
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

// An endless slot 1.8 blocks tall between a floor and a ceiling of single
// blocks, each its own box: a synthetic world in which a player flying with
// its box against both gathers every block its move passes over and under,
// about 16 a tick at sprint-flying's speed, with nothing in its way.
class Slot final : public BlockBoxes {
public:
    void collect(const Box& region, std::vector<Box>& out) const override {
        // The floor's blocks fill y 0 to 1, the ceiling's 2.8 to 3.8; the
        // blocks p whose [p, p + 1] touches the region on x and z.
        constexpr std::array<double, 2> bottoms = {0.0, 1.0 + 1.8};
        const auto first = [](double low) { return static_cast<int>(std::ceil(low - 1.0)); };
        const auto last = [](double high) { return static_cast<int>(std::floor(high)); };
        for (const double bottom : bottoms) {
            if (region.max.y < bottom || region.min.y > bottom + 1.0) {
                continue;
            }
            for (int z = first(region.min.z); z <= last(region.max.z); ++z) {
                for (int x = first(region.min.x); x <= last(region.max.x); ++x) {
                    const Vec3d low{
                        .x = static_cast<double>(x), .y = bottom, .z = static_cast<double>(z)};
                    out.push_back({.min = low, .max = low + Vec3d{.x = 1.0, .y = 1.0, .z = 1.0}});
                }
            }
        }
    }
};

// One tick for the bench: the player turns `turn` radians each tick, so it
// goes round in a circle, about 9 blocks across walking and 14
// sprint-jumping, which never leaves its chunk, and 107 sprint-flying in
// the slot.
void timed_tick(Player& player, float& yaw, float turn, const MovementKeys& keys,
                const BlockBoxes& world, const MovementNumbers& numbers,
                std::vector<Box>& scratch) {
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
        timed_tick(walker, walker_yaw, 0.05F, {.forward = true}, world, numbers, scratch);
    });

    // Every key that matters held: sprinting, jumping and landing in turn.
    Player jumper = start();
    float jumper_yaw = 0.0F;
    bench.batch(1).unit("tick").run("movement: one tick sprint-jumping on flat ground", [&] {
        timed_tick(jumper, jumper_yaw, 0.05F, {.forward = true, .jump = true, .sprint = true},
                   world, numbers, scratch);
    });

    // RFC 0010's other budget, under 20 us: sprint-flying through the slot,
    // its box against the floor and the ceiling, turning slowly enough to
    // keep 98% of its top speed: 1.065 blocks a tick, sprinting every tick.
    const Slot slot;
    Player flyer;
    flyer.position = {.x = 0.5, .y = 1.0, .z = 0.5};
    flyer.flying = true;
    float flyer_yaw = 0.0F;
    bench.batch(1).unit("tick").run("movement: one tick sprint-flying between single blocks", [&] {
        timed_tick(flyer, flyer_yaw, 0.02F, {.forward = true, .sprint = true}, slot, numbers,
                   scratch);
    });
}

} // namespace vx::bench
