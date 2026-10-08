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
