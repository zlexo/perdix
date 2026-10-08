include(FetchContent)

# Tracy, the profiler (RFC 0001). Only its sources are fetched; vx_tracy below
# is ours. The client and the viewer must speak the same protocol, and 0.13.1
# is the viewer Homebrew ships: bump both together.
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.13.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
# stb_image: PNG decoding for block textures (RFC 0004), and the raw deflate
# decoder inside it for zip archives (RFC 0009). stb publishes no releases,
# so its one header is fetched at a commit (the last to change it, version
# 2.30) and checked against its hash: no clone of the whole repository.
FetchContent_Declare(stb_image
    URL https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(tracy stb_image)
# Upstream fixes to Tracy's Vulkan context, backported at every configure
# rather than once when the sources arrive, so an existing build directory
# picks up a changed backport; editing the script configures again.
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/cmake/VxPatchTracy.cmake"
    WORKING_DIRECTORY "${tracy_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/cmake/VxPatchTracy.cmake")

# Without VX_PROFILE the client is little more than thread naming and Tracy's
# macros expand to nothing, so engine code is instrumented the same way in
# every build. Built without our warnings: it is not our code.
find_package(Threads REQUIRED)
add_library(vx_tracy STATIC "${tracy_SOURCE_DIR}/public/TracyClient.cpp")
target_include_directories(vx_tracy SYSTEM PUBLIC "${tracy_SOURCE_DIR}/public")
target_link_libraries(vx_tracy PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# Tracy formats names and numbers with sprintf, which Apple's SDK marks
# deprecated: a warning a dozen times over, in code that is not ours.
target_compile_options(vx_tracy PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wno-deprecated-declarations>)
if(VX_PROFILE)
    target_compile_definitions(vx_tracy PUBLIC TRACY_ENABLE)
    # A laptop joins networks it does not trust: the client listens on
    # localhost only, where the viewer runs, and does not announce itself.
    target_compile_definitions(vx_tracy PRIVATE TRACY_ONLY_LOCALHOST TRACY_NO_BROADCAST)
    message(STATUS "Tracy profiler: compiled in")
else()
    message(STATUS "Tracy profiler: off, without VX_PROFILE")
endif()

# stb_image, PNG only (its deflate decoder comes with PNG), built without
# our warnings: it is not our code. The defines that change its header are
# public, so every includer sees the same declarations.
add_library(vx_stb_image STATIC src/stb_image.cpp)
target_include_directories(vx_stb_image SYSTEM PUBLIC "${stb_image_SOURCE_DIR}")
target_compile_definitions(vx_stb_image PUBLIC STBI_NO_STDIO STBI_NO_LINEAR STBI_NO_HDR)
target_compile_definitions(vx_stb_image PRIVATE STB_IMAGE_IMPLEMENTATION STBI_ONLY_PNG)

configure_file(src/version_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.hpp"
    @ONLY)

add_library(vx_core STATIC
    src/archive.cpp
    src/crc32.cpp
    src/diagnostic.cpp
    src/file.cpp
    src/version.cpp)
add_library(vx::core ALIAS vx_core)

# Public headers live under include/vx/core and are only visible to targets
# that link vx::core. The generated config header stays private.
target_include_directories(vx_core
    PUBLIC include
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(vx_core PUBLIC cxx_std_20)
target_link_libraries(vx_core PUBLIC vx_tracy PRIVATE vx_stb_image)
vx_target_warnings(vx_core)
vx_target_engine_options(vx_core)
