cmake_minimum_required(VERSION 3.28)
# CMake 3.29 and later list each static library once where the linker does not
# need it repeated. Apple's linker warns about the repeats.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

project(perdix
    VERSION 0.0.0
    DESCRIPTION "A Minecraft-like voxel engine in C++20 and Vulkan"
    LANGUAGES C CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
# ADR 0001 rules out C++ modules, so skip the dependency scan CMake runs
# for them by default under C++20 (it doubled the build steps on Clang 18).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(VX_WARNINGS_AS_ERRORS "Treat compiler warnings in engine code as errors" ON)
option(VX_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(VX_PROFILE "Compile in the Tracy profiler: CPU and GPU zones, frames and allocations" OFF)
option(VX_BUILD_BENCHMARKS "Build the benchmarks" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VxWarnings)
include(VxEngineOptions)

add_subdirectory(engine/core)
add_subdirectory(engine/world)
add_subdirectory(engine/mesh)
add_subdirectory(engine/render)
add_subdirectory(engine/minecraft)
add_subdirectory(app)

if(VX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(VX_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
