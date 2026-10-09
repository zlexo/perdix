#include "../benchmarks.hpp"

#include <vx/core/frustum.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/draw_lists.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <nanobench.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace vx::bench {

// RFC 0007's draw lists for 20,000 meshes around a camera looking north
// with a 70 degree vertical field of view on a 16:10 screen (budget 0.5 ms):
// meshes from a disc of columns 3 chunks deep, each group a mix of sizes and
// a third of them empty, as terrain gives.
void draw_lists(ankerl::nanobench::Bench& bench) {
    std::vector<render::ChunkMeshRecord> records;
    records.reserve(20'000);
    std::mt19937 random(0xD8A3);
    std::uniform_int_distribution<int> size(0, 400);
    std::uniform_int_distribution<int> empty(0, 2);
    std::uint32_t first = 0;
    for (std::int32_t radius = 0; records.size() < 20'000; ++radius) {
        for (std::int32_t z = -radius; z <= radius && records.size() < 20'000; ++z) {
            for (std::int32_t x = -radius; x <= radius && records.size() < 20'000; ++x) {
                if (std::max(std::abs(x), std::abs(z)) != radius) {
                    continue;
                }
                for (std::int32_t y = -1; y <= 1 && records.size() < 20'000; ++y) {
                    render::ChunkMeshRecord record{};
                    record.pos = {.x = x, .y = y, .z = z};
                    record.range.first_quad = first;
                    for (std::uint16_t& count : record.count) {
                        count = empty(random) == 0 ? 0 : static_cast<std::uint16_t>(size(random));
                        first += count;
                    }
                    records.push_back(record);
                }
            }
        }
    }
    const Frustum view({.x = 0.0, .y = 0.0, .z = -1.0}, {.x = 1.0, .y = 0.0, .z = 0.0},
                       {.x = 0.0, .y = 1.0, .z = 0.0}, std::tan(70.0 * 3.141592653589793 / 360.0),
                       1.6, 0.05);
    render::DrawLists lists;
    lists.origins.reserve(records.size());
    lists.opaque.reserve(records.size() * 3);
    lists.cutout.reserve(records.size() * 3);
    bench.run("draw lists: 20,000 meshes, culled, merged (budget 500 us)", [&] {
        render::build_draw_lists(records, glm::i64vec3(16, 16, 16), glm::vec3(0.5F), view, {},
                                 lists);
        ankerl::nanobench::doNotOptimizeAway(lists.opaque.size());
    });
}

} // namespace vx::bench
