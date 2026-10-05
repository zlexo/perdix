include(FetchContent)

# nanobench (RFC 0002), at its latest release: one header, its implementation
# compiled once below. Only the sources are fetched; the target is ours.
FetchContent_Declare(nanobench
    GIT_REPOSITORY https://github.com/martinus/nanobench.git
    GIT_TAG v4.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(nanobench)

add_library(vx_nanobench STATIC nanobench.cpp)
target_include_directories(vx_nanobench SYSTEM PUBLIC "${nanobench_SOURCE_DIR}/src/include")
target_compile_features(vx_nanobench PUBLIC cxx_std_20)

# Every benchmark, in one program. Run a release build for numbers; CI runs
# every build's with --quick, which checks they still run and times nothing.
add_executable(vx_bench
    main.cpp
    render/block_textures_bench.cpp
    world/block_registry_bench.cpp
    world/chunk_storage_bench.cpp
    world/packed_array_bench.cpp)
target_link_libraries(vx_bench PRIVATE vx::world vx::render vx_nanobench)
# The leaves of the texture tests, copied 1,000 times into a pack.
target_compile_definitions(vx_bench PRIVATE VX_BENCH_LEAVES_PNG="${PROJECT_SOURCE_DIR}/tests/render/block_textures/test/textures/blocks/leaves.png")
vx_target_warnings(vx_bench)

if(VX_BUILD_TESTS)
    add_test(NAME "the benchmarks run" COMMAND vx_bench --quick)
endif()
