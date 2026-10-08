#include <vx/core/vec3d.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// RFC 0010's properties, over random worlds and random keys: after every
// tick the player's box is inside no block, every number is finite, and the
// same keys in the same world give the same positions, bit for bit. The
// worlds are a chunk of stone, ice, poppies (which collide with nothing)
// and air on a floor of stone and ice.

namespace {

// splitmix64: the same numbers on every machine and standard library.
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform in [0, 1).
    double unit() noexcept { return static_cast<double>(next() >> 11U) * 0x1.0p-53; }
    bool chance(double probability) noexcept { return unit() < probability; }

private:
    std::uint64_t state_;
};

vx::BlockRegistry test_pack() {
    vx::BlockLoad load = vx::BlockRegistry::load(std::vector<vx::SourceFile>{
        {.path = "assets/core/blocks/ice.toml",
         .text = "slipperiness = 0.98\n[textures]\nall = \"ice\"\n"},
        {.path = "assets/core/blocks/poppy.toml",
         .text = "collision = \"none\"\nopacity = \"cutout\"\n[textures]\nall = \"poppy\"\n"},
        {.path = "assets/core/blocks/stone.toml", .text = "[textures]\nall = \"stone\"\n"}});
    REQUIRE(load.registry.has_value());
    return *std::move(load.registry);
}

// A random world closed in, so the player stays in it: a floor of stone
// and ice at y = 0, a ceiling at y = 12 and walls around the chunk's edge,
// and in between, up to y = 6, a block in about one cell in five, the cells
// where the player starts left clear.
vx::ChunkStorage random_world(const vx::BlockRegistry& registry, Random& random) {
    const std::array<vx::BlockState, 3> blocks = {
        *registry.find("core:stone"), *registry.find("core:ice"), *registry.find("core:poppy")};
    constexpr int ceiling = 12;
    vx::ChunkStorage chunk;
    for (int x = 0; x < vx::chunk_size; ++x) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            const bool wall =
                x == 0 || z == 0 || x == vx::chunk_size - 1 || z == vx::chunk_size - 1;
            chunk.set(vx::local_index(x, 0, z), blocks[random.chance(0.3) ? 1 : 0]);
            chunk.set(vx::local_index(x, ceiling, z), blocks[0]);
            for (int y = 1; y < ceiling; ++y) {
                const bool start = x == 16 && z == 16 && y < 3;
                if (wall) {
                    chunk.set(vx::local_index(x, y, z), blocks[0]);
                } else if (y < 7 && !start && random.chance(0.2)) {
                    chunk.set(vx::local_index(x, y, z), blocks[random.next() % 3]);
                }
            }
        }
    }
    return chunk;
}

// Random keys: W more often than not, so the player gets around; the rest
// now and then, Space often enough to jump, and to fly where it may.
vx::TickInput random_input(Random& random, float& yaw) {
    if (random.chance(0.2)) {
        yaw = static_cast<float>(random.unit() * 6.283185307179586);
    }
    return {.held = {.forward = random.chance(0.6),
                     .back = random.chance(0.1),
                     .left = random.chance(0.15),
                     .right = random.chance(0.15),
                     .jump = random.chance(0.1),
                     .sneak = random.chance(0.15),
                     .sprint = random.chance(0.4)},
            .yaw = yaw};
}

bool finite(vx::Vec3d v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The player's positions through `ticks` ticks of random keys from `seed`,
// checking after each tick that its box is inside no block and that its
// numbers are finite.
std::vector<vx::Vec3d> walk(const vx::BlockBoxes& world, std::uint64_t seed, int ticks,
                            vx::Vec3d start = {.x = 16.5, .y = 1.0, .z = 16.5}) {
    Random random(seed);
    const vx::MovementNumbers numbers;
    std::vector<vx::Box> scratch;
    std::vector<vx::Box> around;
    vx::Player player;
    player.position = start;
    player.on_ground = true;
    player.may_fly = random.chance(0.5); // half the walks may fly
    float yaw = 0.0F;
    std::vector<vx::Vec3d> positions;
    for (int i = 0; i < ticks; ++i) {
        vx::tick(player, random_input(random, yaw), world, numbers, scratch);
        positions.push_back(player.position);
        INFO("tick ", i + 1);
        REQUIRE(finite(player.position));
        REQUIRE(finite(player.velocity));
        const vx::Box body = vx::body_box(player.position, player.pose);
        around.clear();
        world.collect(body, around);
        REQUIRE_FALSE(vx::overlaps_any(body, around));
    }
    return positions;
}

// A world of boxes that are not all full cubes, as RFC 0010's properties
// ask: in each cell of a 24 by 24 room, from the cell's bottom up to a
// height from an eighth of a block to a block and a half (slabs, beds,
// fences), so that step-up and the edge guard run between whole blocks.
class BoxRoom final : public vx::BlockBoxes {
public:
    static constexpr int size = 24;
    static constexpr int low = -1;  // the floor's layer
    static constexpr int high = 11; // above the ceiling's layer

    explicit BoxRoom(Random& random) : heights_(cells, 0.0), slipperiness_(cells, 0.6) {
        constexpr std::array<double, 8> heights = {0.125, 0.25, 0.5, 0.5625, 0.75, 0.875, 1.0, 1.5};
        for (int x = 0; x < size; ++x) {
            for (int z = 0; z < size; ++z) {
                const bool wall = x == 0 || z == 0 || x == size - 1 || z == size - 1;
                set(x, low, z, 1.0, random.chance(0.3) ? 0.98 : 0.6);
                set(x, high - 1, z, 1.0, 0.6);
                for (int y = 0; y < 5 || (wall && y < high - 1); ++y) {
                    const bool start = x == size / 2 && z == size / 2 && y < 3;
                    if (wall) {
                        set(x, y, z, 1.0, 0.6);
                    } else if (!start && height_at(x, y - 1, z) <= 1.0 && random.chance(0.18)) {
                        // A box 1.5 tall only on the floor, its top half in
                        // the cell above, which stays empty.
                        const double height = heights[random.next() % heights.size()];
                        if (height <= 1.0 || y == 0) {
                            set(x, y, z, height, random.chance(0.3) ? 0.98 : 0.6);
                            y += height > 1.0 ? 1 : 0;
                        }
                    }
                }
            }
        }
    }

    [[nodiscard]] double slipperiness(vx::BlockPos block) const override {
        const std::optional<std::size_t> cell = index(block.x, block.y, block.z);
        return cell ? slipperiness_[*cell] : 0.6;
    }

    void collect(const vx::Box& region, std::vector<vx::Box>& out) const override {
        for (const double coordinate :
             {region.min.x, region.min.y, region.min.z, region.max.x, region.max.y, region.max.z}) {
            if (!std::isfinite(coordinate)) {
                return;
            }
        }
        // Clamped as doubles, so a region far away never overflows an int.
        const auto first = [](double v, int least) {
            return static_cast<int>(
                std::clamp(std::floor(v) - 2.0, static_cast<double>(least), 64.0));
        };
        const auto last = [](double v, int most) {
            return static_cast<int>(std::clamp(std::floor(v), -64.0, static_cast<double>(most)));
        };
        for (int y = first(region.min.y, low); y <= last(region.max.y, high - 1); ++y) {
            for (int z = first(region.min.z, 0); z <= last(region.max.z, size - 1); ++z) {
                for (int x = first(region.min.x, 0); x <= last(region.max.x, size - 1); ++x) {
                    const double height = height_at(x, y, z);
                    const vx::Vec3d bottom{.x = static_cast<double>(x),
                                           .y = static_cast<double>(y),
                                           .z = static_cast<double>(z)};
                    const vx::Box box{.min = bottom,
                                      .max = bottom + vx::Vec3d{.x = 1.0, .y = height, .z = 1.0}};
                    if (height > 0.0 && box.max.x >= region.min.x && box.min.x <= region.max.x &&
                        box.max.y >= region.min.y && box.min.y <= region.max.y &&
                        box.max.z >= region.min.z && box.min.z <= region.max.z) {
                        out.push_back(box);
                    }
                }
            }
        }
    }

private:
    static constexpr std::size_t cells = std::size_t{size} * size * (high - low);

    static std::optional<std::size_t> index(int x, int y, int z) {
        if (x < 0 || x >= size || z < 0 || z >= size || y < low || y >= high) {
            return std::nullopt;
        }
        return (static_cast<std::size_t>(y - low) * size + static_cast<std::size_t>(z)) * size +
               static_cast<std::size_t>(x);
    }
    [[nodiscard]] double height_at(int x, int y, int z) const {
        const std::optional<std::size_t> cell = index(x, y, z);
        return cell ? heights_[*cell] : 0.0;
    }
    void set(int x, int y, int z, double height, double slipperiness) {
        const std::size_t cell = *index(x, y, z);
        heights_[cell] = height;
        slipperiness_[cell] = slipperiness;
    }

    std::vector<double> heights_;
    std::vector<double> slipperiness_;
};

} // namespace

TEST_CASE("in random worlds with random keys, the box is never inside a block, and every number "
          "stays finite") {
    const vx::BlockRegistry registry = test_pack();
    for (std::uint64_t world_seed = 1; world_seed <= 40; ++world_seed) {
        INFO("world ", world_seed);
        Random random(world_seed);
        const vx::ChunkStorage chunk = random_world(registry, random);
        const vx::ChunkBoxes world(chunk, {}, registry.physics());
        static_cast<void>(walk(world, world_seed * 1000, 400));
    }
}

TEST_CASE("the same keys in the same world replay to the same positions, bit for bit") {
    const vx::BlockRegistry registry = test_pack();
    for (std::uint64_t world_seed = 1; world_seed <= 8; ++world_seed) {
        INFO("world ", world_seed);
        Random random(world_seed);
        const vx::ChunkStorage chunk = random_world(registry, random);
        const vx::ChunkBoxes world(chunk, {}, registry.physics());
        const std::vector<vx::Vec3d> first = walk(world, world_seed * 1000, 400);
        const std::vector<vx::Vec3d> again = walk(world, world_seed * 1000, 400);
        CHECK(first == again);
        // Different keys go elsewhere, so the comparison above can fail.
        CHECK(walk(world, (world_seed * 1000) + 1, 400) != first);
    }
}

TEST_CASE("in random rooms of boxes of many heights, the box is never inside one, and every "
          "number stays finite") {
    // RFC 0010's worlds of boxes: slabs and fences put the feet between
    // whole blocks, where step-up and the edge guard work.
    for (std::uint64_t world_seed = 1; world_seed <= 40; ++world_seed) {
        INFO("world ", world_seed);
        Random random(world_seed);
        const BoxRoom room(random);
        static_cast<void>(walk(room, world_seed * 1000, 400, {.x = 12.5, .y = 0.0, .z = 12.5}));
    }
}
