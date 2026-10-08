# The mesh layer (RFC 0003): a chunk's blocks into the quads the renderer
# draws. It reads the world layer and has no GPU or window code (ADR 0003).
add_library(vx_mesh STATIC
    src/padded_chunk.cpp)
add_library(vx::mesh ALIAS vx_mesh)

target_include_directories(vx_mesh PUBLIC include)
target_link_libraries(vx_mesh PUBLIC vx::world)
target_compile_features(vx_mesh PUBLIC cxx_std_20)
vx_target_warnings(vx_mesh)
vx_target_engine_options(vx_mesh)
