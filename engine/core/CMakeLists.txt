configure_file(src/version_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.hpp"
    @ONLY)

add_library(vx_core STATIC
    src/version.cpp)
add_library(vx::core ALIAS vx_core)

# Public headers live under include/vx/core and are only visible to targets
# that link vx::core. The generated config header stays private.
target_include_directories(vx_core
    PUBLIC include
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(vx_core PUBLIC cxx_std_20)
vx_target_warnings(vx_core)
