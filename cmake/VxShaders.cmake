# Slang shaders, compiled at build time (RFC 0001, pull request plan item 7).
#
# The compiler is Slang's own release for the build machine, pinned by version
# and SHA-256, so every machine compiles shaders with the same compiler and
# building needs no Vulkan SDK: the SDK supplies only what runs.

include(FetchContent)

set(VX_SLANG_VERSION 2026.18.3)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(vx_slang_package macos-aarch64)
    set(vx_slang_sha256 6c8bf066a4254e8d053f864a52ea18388702d409ed9f20b3d6da66e8d772c906)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    # Built against glibc 2.28, so it runs on any distribution since 2018.
    set(vx_slang_package linux-x86_64-glibc-2.28)
    set(vx_slang_sha256 40ee04ec13f8ba26444a53a001ec6926d04040b3fbad7036c9759d3db5756d5c)
else()
    message(FATAL_ERROR "No Slang compiler is pinned for ${CMAKE_HOST_SYSTEM_NAME} on "
        "${CMAKE_HOST_SYSTEM_PROCESSOR}: add its release package to cmake/VxShaders.cmake.")
endif()

# Like Vulkan's headers, SOURCE_SUBDIR points at a directory that does not
# exist: the release is unpacked and nothing is configured from it.
FetchContent_Declare(slang
    URL "https://github.com/shader-slang/slang/releases/download/v${VX_SLANG_VERSION}/slang-${VX_SLANG_VERSION}-${vx_slang_package}.tar.gz"
    URL_HASH SHA256=${vx_slang_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(slang)
find_program(VX_SLANGC slangc PATHS "${slang_SOURCE_DIR}/bin" NO_DEFAULT_PATH NO_CACHE REQUIRED)
