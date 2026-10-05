#include "../benchmarks.hpp"

#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>

#include <nanobench.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace vx::bench {
namespace {

// RFC 0004's synthetic pack: 1,000 blocks and 10,000 states, each block with
// properties of 2 and 5 values, textures for its sides, top and bottom, and
// a variant, read from memory.
std::vector<SourceFile> synthetic_pack() {
    std::vector<SourceFile> files;
    files.reserve(1'000);
    for (int block = 0; block < 1'000; ++block) {
        const std::string name = "block_" + std::to_string(block);
        files.push_back(
            {.path = "assets/bench/blocks/" + name + ".toml",
             .text = "[properties]\nlit = [false, true]\nlevel = [0, 1, 2, 3, 4]\n"
                     "[textures]\nside = \"" +
                     name + "\"\ntop = \"" + name + "_top\"\nbottom = \"" + name +
                     "_top\"\n[[variant]]\nwhen = { lit = true }\ntextures = { side = \"" + name +
                     "_lit\" }\n"});
    }
    return files;
}

} // namespace

void block_registry(ankerl::nanobench::Bench& bench) {
    // RFC 0004's budget: under 50 ms. A pack that failed to load would time
    // the wrong thing, so it must load, with every state and the faces of
    // each, before it is timed: three textures a block, and the checker.
    const std::vector<SourceFile> files = synthetic_pack();
    const BlockLoad check = BlockRegistry::load(files);
    if (!check.registry || check.registry->state_count() != 10'001 ||
        check.registry->visuals().size() != 10'001 || check.registry->textures().size() != 3'001) {
        std::cerr << "the synthetic pack did not load with 10,000 states and 3,000 textures\n";
        std::exit(1);
    }
    bench.batch(1).unit("pack").run("block registry: load 1,000 blocks, 10,000 states", [&] {
        BlockLoad load = BlockRegistry::load(files);
        ankerl::nanobench::doNotOptimizeAway(load);
    });
}

} // namespace vx::bench
