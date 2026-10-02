include(FetchContent)

# Vulkan's headers and volk come at the Vulkan SDK's version, so building needs
# no SDK; at run time the SDK (or a system driver) provides the loader, the
# drivers and the validation layers. SOURCE_SUBDIR points at a directory that
# does not exist, so only the sources are fetched and the targets below are ours.
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.363
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.363.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(vulkan_headers volk)

# volk loads the Vulkan loader at run time and every entry point from it, so
# nothing links the loader. Built without our warnings: it is not our code.
add_library(vx_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(vx_volk SYSTEM PUBLIC
    "${vulkan_headers_SOURCE_DIR}/include"
    "${volk_SOURCE_DIR}")
target_compile_definitions(vx_volk PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(vx_volk PRIVATE ${CMAKE_DL_LIBS})

add_library(vx_render STATIC
    src/feature_floor.cpp)
add_library(vx::render ALIAS vx_render)

target_include_directories(vx_render PUBLIC include)
target_link_libraries(vx_render PUBLIC vx::core vx_volk)
target_compile_features(vx_render PUBLIC cxx_std_20)
vx_target_warnings(vx_render)
vx_target_engine_options(vx_render)
