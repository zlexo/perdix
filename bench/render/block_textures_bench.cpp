#include "../benchmarks.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/world/block_registry.hpp>

#include <nanobench.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace vx::bench {

void block_textures(ankerl::nanobench::Bench& bench) {
    // RFC 0004's budget: decode, turn and mipmap 1,000 textures on one
    // thread in under 50 ms. A pack of 1,000 files, each a copy of the test
    // leaves (every other texel transparent, so filling and the coverage
    // step run on every one), in all four turns.
    const std::filesystem::path assets =
        std::filesystem::temp_directory_path() / "perdix-bench-block-textures";
    const std::filesystem::path blocks = assets / "bench" / "textures" / "blocks";
    std::error_code error;
    std::filesystem::remove_all(assets, error);
    std::filesystem::create_directories(blocks, error);
    std::vector<TextureRef> textures(1);
    for (int texture = 0; texture < 1'000; ++texture) {
        const std::string name = "t" + std::to_string(texture);
        std::filesystem::copy_file(VX_BENCH_LEAVES_PNG, blocks / (name + ".png"), error);
        textures.push_back({.name = "bench:" + name,
                            .quarter_turns = static_cast<std::uint8_t>(texture % 4),
                            .used_opaque = false,
                            .used_cutout = true,
                            .first_use = {}});
    }
    // A pack that failed to build would time the wrong thing.
    std::vector<Diagnostic> check;
    if (render::build_block_textures(textures, assets, check).size() !=
            textures.size() * render::block_texture_layer_bytes ||
        !check.empty()) {
        std::cerr << "the 1,000 block textures did not build cleanly\n";
        std::exit(1);
    }
    bench.batch(1).unit("pack").run("block textures: read, decode, turn, fill, mipmap 1,000", [&] {
        std::vector<Diagnostic> diagnostics;
        auto bytes = render::build_block_textures(textures, assets, diagnostics);
        ankerl::nanobench::doNotOptimizeAway(bytes);
    });
    std::filesystem::remove_all(assets, error);
}

} // namespace vx::bench
