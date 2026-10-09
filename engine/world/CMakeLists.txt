include(FetchContent)

# toml++, for block files (RFC 0004), at the release the RFC names. Only its
# sources are fetched: vx_tomlplusplus compiles its implementation once,
# without exceptions, as engine code builds.
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(tomlplusplus)
# Backports for malformed files that 3.4.0 cannot parse safely. Run at every
# configure, as Tracy's are, so an existing build directory picks up a
# changed backport; editing the script configures again.
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/cmake/VxPatchTomlplusplus.cmake"
    WORKING_DIRECTORY "${tomlplusplus_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/cmake/VxPatchTomlplusplus.cmake")

add_library(vx_tomlplusplus STATIC src/tomlplusplus.cpp)
target_include_directories(vx_tomlplusplus SYSTEM PUBLIC "${tomlplusplus_SOURCE_DIR}/include")
target_compile_definitions(vx_tomlplusplus PUBLIC TOML_HEADER_ONLY=0 TOML_EXCEPTIONS=0)
target_compile_features(vx_tomlplusplus PUBLIC cxx_std_20)
vx_target_engine_options(vx_tomlplusplus)

# The world layer (RFC 0002): blocks, chunks and their coordinates. It has no
# GPU or window code (ADR 0003).
add_library(vx_world STATIC
    src/block_data_pool.cpp
    src/block_file.cpp
    src/block_registry.cpp
    src/chunk_boxes.cpp
    src/chunk_map.cpp
    src/chunk_records.cpp
    src/chunk_storage.cpp
    src/collision.cpp
    src/job_record.cpp
    src/movement.cpp
    src/names.cpp
    src/packed_array.cpp
    src/placeholder_generator.cpp
    src/region.cpp
    src/streamed_boxes.cpp
    src/world_streamer.cpp)
add_library(vx::world ALIAS vx_world)
target_link_libraries(vx_world PUBLIC vx::core PRIVATE vx_tomlplusplus)

# The block data pool's locks.
find_package(Threads REQUIRED)
target_link_libraries(vx_world PUBLIC Threads::Threads)

target_include_directories(vx_world PUBLIC include)
target_compile_features(vx_world PUBLIC cxx_std_20)
vx_target_warnings(vx_world)
vx_target_engine_options(vx_world)
