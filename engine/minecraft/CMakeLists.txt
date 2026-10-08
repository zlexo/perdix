include(FetchContent)

# yyjson, for the launcher's version descriptions and a jar's version.json
# (RFC 0009), at the release the RFC names. Only its sources are fetched:
# vx_yyjson below is ours.
FetchContent_Declare(yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.13.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(yyjson)

# The reader alone: no writer, incremental reader, JSON Pointer or Patch, and
# no JSON beyond the standard. No file functions either: engine code reads
# files itself, and yyjson would open a path in the system's code page on
# Windows. Built without our warnings: it is not our code. The defines that
# change its header are public, so every includer sees the same declarations.
add_library(vx_yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
target_include_directories(vx_yyjson SYSTEM PUBLIC "${yyjson_SOURCE_DIR}/src")
target_compile_definitions(vx_yyjson PUBLIC
    YYJSON_DISABLE_WRITER=1
    YYJSON_DISABLE_INCR_READER=1
    YYJSON_DISABLE_UTILS=1
    YYJSON_DISABLE_NON_STANDARD=1
    YYJSON_DISABLE_FILE=1)

# The minecraft layer (RFC 0009): what Perdix knows about Minecraft's own
# files: where the game is, what a pack's textures are in it, and making
# them from its jar, decoded with core's stb_image. Like world, it has no
# GPU or window code (ADR 0003), so the web viewer can compile it to
# WebAssembly. Texture maps are TOML, read with world's toml++.
add_library(vx_minecraft STATIC
    src/install.cpp
    src/json.cpp
    src/made_textures.cpp
    src/texture_map.cpp
    src/version_description.cpp)
add_library(vx::minecraft ALIAS vx_minecraft)
target_link_libraries(vx_minecraft PUBLIC vx::core vx::world
    PRIVATE vx_stb_image vx_tomlplusplus vx_yyjson)
target_include_directories(vx_minecraft PUBLIC include)
target_compile_features(vx_minecraft PUBLIC cxx_std_20)
vx_target_warnings(vx_minecraft)
vx_target_engine_options(vx_minecraft)
