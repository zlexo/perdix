# The mesh layer (RFC 0003): a chunk's blocks into the quads the renderer
# draws. It reads the world layer and has no GPU or window code (ADR 0003),
# and so far no sources: its headers are all inline until the first source
# file comes, when it becomes a static library like the others.
add_library(vx_mesh INTERFACE)
add_library(vx::mesh ALIAS vx_mesh)

target_include_directories(vx_mesh INTERFACE include)
target_link_libraries(vx_mesh INTERFACE vx::world)
target_compile_features(vx_mesh INTERFACE cxx_std_20)
