# The world layer (RFC 0002): blocks, chunks and their coordinates. It has no
# GPU or window code (ADR 0003), and so far no sources: its headers are all
# inline until the first source file comes, when it becomes a static library
# like the others.
add_library(vx_world INTERFACE)
add_library(vx::world ALIAS vx_world)

target_include_directories(vx_world INTERFACE include)
target_compile_features(vx_world INTERFACE cxx_std_20)
