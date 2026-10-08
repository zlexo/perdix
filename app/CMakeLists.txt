include(FetchContent)

# SDL3 opens the window, delivers its events and creates the Vulkan surface
# (RFC 0001). Only its video subsystem is built: each other one turns on when a
# feature needs it, such as audio or gamepads, and until then its Linux
# development packages stay out of the build.
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
foreach(subsystem AUDIO GPU RENDER CAMERA JOYSTICK HAPTIC HIDAPI POWER SENSOR DIALOG TRAY)
    set(SDL_${subsystem}_DEFAULT OFF)
endforeach()
FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(sdl3)

# ImGui's SDL3 backend feeds the overlay the window's input. ImGui itself is
# fetched by the render library, which draws the overlay.
FetchContent_GetProperties(imgui)
add_library(vx_imgui_sdl3 STATIC "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
target_include_directories(vx_imgui_sdl3 SYSTEM PUBLIC "${imgui_SOURCE_DIR}/backends")
target_link_libraries(vx_imgui_sdl3 PUBLIC vx_imgui SDL3::SDL3-static)

# The debug overlay, a library of its own so that tests can drive it.
add_library(vx_app_overlay STATIC src/overlay.cpp)
target_include_directories(vx_app_overlay PUBLIC src)
target_link_libraries(vx_app_overlay PUBLIC vx::render vx_imgui_sdl3)
vx_target_warnings(vx_app_overlay)
vx_target_engine_options(vx_app_overlay)

# Loading the block pack at startup, drawn with the player's Minecraft (RFC
# 0009): a library of its own so that tests can load packs without a window.
add_library(vx_app_startup STATIC src/startup_pack.cpp)
target_include_directories(vx_app_startup PUBLIC src)
target_link_libraries(vx_app_startup PUBLIC vx::render vx::minecraft)
vx_target_warnings(vx_app_startup)
vx_target_engine_options(vx_app_startup)

add_executable(vx_app
    src/allocation_profiling.cpp
    src/demo_chunk.cpp
    src/fly_controls.cpp
    src/main.cpp
    src/pack.cpp
    src/window.cpp)
target_link_libraries(vx_app PRIVATE vx::core vx::world vx::mesh vx::render vx::minecraft
    vx_app_overlay vx_app_startup vx_imgui_sdl3 SDL3::SDL3-static)
set_target_properties(vx_app PROPERTIES OUTPUT_NAME perdix)
vx_target_warnings(vx_app)
vx_target_engine_options(vx_app)
