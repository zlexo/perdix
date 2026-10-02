include(FetchContent)
include(VxShaders)

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
# The SDK does not ship VMA, so it comes at its latest release.
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
# glm, for the camera's float math (RFC 0001). It is header-only.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
# Dear ImGui, for the overlay (RFC 0001), at its latest release. Only the
# sources are fetched; the targets below are ours.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(vulkan_headers volk vma glm imgui)

# volk loads the Vulkan loader at run time and every entry point from it, so
# nothing links the loader. Built without our warnings: it is not our code.
add_library(vx_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(vx_volk SYSTEM PUBLIC
    "${vulkan_headers_SOURCE_DIR}/include"
    "${volk_SOURCE_DIR}")
target_compile_definitions(vx_volk PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(vx_volk PRIVATE ${CMAKE_DL_LIBS})

# VMA is one header; its implementation compiles once, here, with VMA's own
# defaults like volk. Under VK_NO_PROTOTYPES it loads every Vulkan function it
# needs through the two entry points the device hands it.
add_library(vx_vma STATIC src/vma.cpp)
target_include_directories(vx_vma SYSTEM PUBLIC "${vma_SOURCE_DIR}/include")
target_compile_definitions(vx_vma PRIVATE VMA_IMPLEMENTATION)
target_link_libraries(vx_vma PUBLIC vx_volk)

# ImGui's core, which the app builds the overlay with and the renderer reads
# draw lists from, and its Vulkan backend, which calls Vulkan through volk as
# the renderer does. The SDL3 backend belongs to the app, beside the window.
# Built without our warnings: it is not our code.
add_library(vx_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
target_include_directories(vx_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(vx_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
add_library(vx_imgui_vulkan STATIC "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(vx_imgui_vulkan SYSTEM PUBLIC "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(vx_imgui_vulkan PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(vx_imgui_vulkan PUBLIC vx_imgui vx_volk)

# Vulkan's depth range runs from 0 to 1, not from -1 to 1 as OpenGL's does.
add_library(vx_glm INTERFACE)
target_include_directories(vx_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(vx_glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_library(vx_render STATIC
    src/buffer.cpp
    src/camera.cpp
    src/camera_relative.cpp
    src/context.cpp
    src/device.cpp
    src/device_report.cpp
    src/device_selection.cpp
    src/feature_floor.cpp
    src/image.cpp
    src/instance.cpp
    src/pipeline.cpp
    src/projection.cpp
    src/renderer.cpp
    src/surface.cpp
    src/swapchain.cpp
    src/swapchain_choice.cpp
    src/texture_table.cpp
    src/transition.cpp
    src/upload.cpp
    src/vk_result.cpp)
add_library(vx::render ALIAS vx_render)

target_include_directories(vx_render PUBLIC include)
target_link_libraries(vx_render PUBLIC vx::core vx_volk vx_vma vx_glm vx_imgui PRIVATE vx_imgui_vulkan)
target_compile_features(vx_render PUBLIC cxx_std_20)
vx_target_warnings(vx_render)
vx_target_engine_options(vx_render)
vx_add_shaders(vx_render shaders/cube.slang shaders/triangle.slang)

# macOS strips DYLD_LIBRARY_PATH from programs started through protected
# binaries, so a loader that the SDK's setup script made findable by name can
# vanish. The SDK seen at configure time becomes a fallback path.
if(APPLE AND DEFINED ENV{VULKAN_SDK})
    target_compile_definitions(vx_render PRIVATE
        VX_VULKAN_LOADER_FALLBACK="$ENV{VULKAN_SDK}/lib/libvulkan.1.dylib")
endif()
