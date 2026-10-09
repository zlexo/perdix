#include "benchmarks.hpp"

#include <nanobench.h>

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

int main(int argc, char* argv[]) {
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    // --quick runs each benchmark once, so CI can check they still run
    // without timing them: shared runners are too noisy for numbers.
    bool quick = false;
    for (const std::string_view arg : args.subspan(1)) {
        if (arg != "--quick") {
            std::cerr << "unknown option \"" << arg << "\": try --quick\n";
            return 2;
        }
        quick = true;
    }
#ifndef NDEBUG
    // nanobench warns about a debug build only when DEBUG is defined, which
    // CMake's Debug configuration does not define.
    if (!quick) {
        std::cerr << "warning: a debug build, whose timings say nothing about the engine's; "
                     "measure a release build (mac-release)\n";
    }
#endif
#ifdef __APPLE__
    // On a performance core: an efficiency core times everything slower.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    ankerl::nanobench::Bench bench;
    bench.title("Perdix").relative(false).warmup(quick ? 0 : 100);
    if (quick) {
        bench.epochs(1).epochIterations(1);
    }
    vx::bench::packed_array(bench);
    vx::bench::chunk_storage(bench);
    vx::bench::chunk_map(bench);
    vx::bench::block_registry(bench);
    vx::bench::block_textures(bench);
    vx::bench::block_pack(bench);
    vx::bench::padded_chunk(bench);
    vx::bench::mesh(bench);
    vx::bench::movement(bench);
    // No results means no benchmark ran, which --quick must not pass as a
    // run: the test "the benchmarks run" would stay green with none.
    return bench.results().empty() ? 1 : 0;
}
