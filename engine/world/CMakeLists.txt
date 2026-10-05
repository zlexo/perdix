# The world layer (RFC 0002): blocks, chunks and their coordinates. It has no
# GPU or window code (ADR 0003).
add_library(vx_world STATIC
    src/block_data_pool.cpp
    src/chunk_storage.cpp
    src/packed_array.cpp)
add_library(vx::world ALIAS vx_world)

# The block data pool's locks.
find_package(Threads REQUIRED)
target_link_libraries(vx_world PUBLIC Threads::Threads)

target_include_directories(vx_world PUBLIC include)
target_compile_features(vx_world PUBLIC cxx_std_20)
vx_target_warnings(vx_world)
vx_target_engine_options(vx_world)
