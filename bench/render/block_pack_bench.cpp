#include "../benchmarks.hpp"

#include <vx/render/block_pack.hpp>

#include <nanobench.h>

#include <cstdlib>
#include <iostream>

namespace vx::bench {

void block_pack(ankerl::nanobench::Bench& bench) {
    // RFC 0004's budget: the shipped M2 pack loaded and validated in under
    // 5 ms. Timed whole, as the app runs it at startup: the block files read
    // and checked, then the textures decoded, turned and mipmapped. It must
    // load cleanly first, or the time would be a failure's.
    const render::BlockPack check = render::load_block_pack(VX_SHIPPED_PACK_DIR);
    if (!check.diagnostics.empty() || !check.registry || check.textures.empty()) {
        std::cerr << "the shipped pack did not load cleanly\n";
        std::exit(1);
    }
    bench.batch(1).unit("pack").run("block pack: load the shipped M2 pack from disk", [&] {
        render::BlockPack pack = render::load_block_pack(VX_SHIPPED_PACK_DIR);
        ankerl::nanobench::doNotOptimizeAway(pack);
    });
}

} // namespace vx::bench
