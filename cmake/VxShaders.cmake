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

set(VX_EMBED_SPIRV "${CMAKE_CURRENT_LIST_DIR}/VxEmbedSpirv.cmake")

# vx_add_shaders(<target> <source.slang>...)
#
# Compiles each source into one SPIR-V module that holds all of its entry
# points under their own names, with warnings as errors, and embeds the module
# in a generated header: shader `triangle.slang` becomes the
# std::array<std::uint32_t, N> `vx::shaders::triangle` in
# <vx/shaders/triangle.hpp>. A shader rebuilds when its source or anything it
# imports changes.
function(vx_add_shaders target)
    set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/include")
    foreach(source IN LISTS ARGN)
        cmake_path(GET source STEM name)
        if(NOT name MATCHES "^[a-z][a-z0-9_]*$")
            message(FATAL_ERROR "Shader ${source}: its name becomes a C++ identifier, "
                "so it takes lowercase letters, digits and underscores.")
        endif()
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE source_path)
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
        set(header "${include_dir}/vx/shaders/${name}.hpp")

        add_custom_command(
            OUTPUT "${spirv}"
            COMMAND "${VX_SLANGC}" "${source_path}" -target spirv -fvk-use-entrypoint-name
                -warnings-as-errors all -depfile "${spirv}.d" -o "${spirv}"
            DEPENDS "${source_path}"
            DEPFILE "${spirv}.d"
            COMMENT "Compiling shader ${name}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" "-DSPIRV=${spirv}" "-DHEADER=${header}" "-DNAME=${name}"
                -P "${VX_EMBED_SPIRV}"
            DEPENDS "${spirv}" "${VX_EMBED_SPIRV}"
            COMMENT "Embedding shader ${name}"
            VERBATIM)
        target_sources(${target} PRIVATE "${header}")
    endforeach()
    target_include_directories(${target} PUBLIC "${include_dir}")
endfunction()
