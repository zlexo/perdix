cmake_minimum_required(VERSION 3.28)

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

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VxWarnings)
include(VxEngineOptions)

add_subdirectory(engine/core)
add_subdirectory(app)

if(VX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
