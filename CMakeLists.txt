cmake_minimum_required(VERSION 3.28)

project(voxel_engine
    VERSION 0.0.0
    DESCRIPTION "A Minecraft-like voxel engine in C++20 and Vulkan"
    LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

option(VX_WARNINGS_AS_ERRORS "Treat compiler warnings in engine code as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VxWarnings)

add_subdirectory(engine/core)
add_subdirectory(app)
