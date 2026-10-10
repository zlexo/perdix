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
add_library(vx_app_overlay STATIC src/keys.cpp src/overlay.cpp)
target_include_directories(vx_app_overlay PUBLIC src)
target_link_libraries(vx_app_overlay PUBLIC vx::render vx_imgui_sdl3)
vx_target_warnings(vx_app_overlay)
vx_target_engine_options(vx_app_overlay)

# The command line, and loading the block pack at startup drawn with the
# player's Minecraft (RFC 0009): a library of its own so that tests can parse
# options and load packs without a window.
add_library(vx_app_startup STATIC src/options.cpp src/startup_pack.cpp)
target_include_directories(vx_app_startup PUBLIC src)
target_link_libraries(vx_app_startup PUBLIC vx::render vx::minecraft)
vx_target_warnings(vx_app_startup)
vx_target_engine_options(vx_app_startup)

# The player walking (RFC 0010): a library of its own, so that the world
# loop and the tests run it without a window.
add_library(vx_app_play STATIC src/walking.cpp)
target_include_directories(vx_app_play PUBLIC src)
target_link_libraries(vx_app_play PUBLIC vx::world)
vx_target_warnings(vx_app_play)
vx_target_engine_options(vx_app_play)

# RFC 0005's world loop: the streamer, the mesh half of each chunk's
# lifecycle and the messages to and from the main thread, without a window.
add_library(vx_app_world STATIC src/world_loop.cpp src/world_thread.cpp)
target_include_directories(vx_app_world PUBLIC src)
target_link_libraries(vx_app_world PUBLIC vx::core vx::world vx::mesh vx_app_play)
vx_target_warnings(vx_app_world)
vx_target_engine_options(vx_app_world)

# RFC 0006's benchmark flythrough: its camera path, its phases and its report,
# and the process's memory.
add_library(vx_app_flythrough STATIC src/flythrough.cpp src/flythrough_run.cpp src/footprint.cpp)
target_include_directories(vx_app_flythrough PUBLIC src)
target_link_libraries(vx_app_flythrough PUBLIC vx::core)
vx_target_warnings(vx_app_flythrough)
vx_target_engine_options(vx_app_flythrough)

# The world thread's meshes handed to RFC 0007's uploads, on the main thread.
add_library(vx_app_stream STATIC src/streamed_meshes.cpp)
target_include_directories(vx_app_stream PUBLIC src)
target_link_libraries(vx_app_stream PUBLIC vx_app_world vx::render)
vx_target_warnings(vx_app_stream)
vx_target_engine_options(vx_app_stream)

# The view the player sees: the camera with RFC 0010's view bobbing.
add_library(vx_app_view STATIC src/view.cpp)
target_include_directories(vx_app_view PUBLIC src)
target_link_libraries(vx_app_view PUBLIC vx::render vx_app_play)
vx_target_warnings(vx_app_view)
vx_target_engine_options(vx_app_view)

add_executable(vx_app
    src/allocation_profiling.cpp
    src/fly_controls.cpp
    src/main.cpp
    src/pack.cpp
    src/window.cpp)
target_link_libraries(vx_app PRIVATE vx::core vx::world vx::mesh vx::render vx::minecraft
    vx_app_flythrough vx_app_overlay vx_app_play vx_app_startup vx_app_stream vx_app_view
    vx_app_world vx_imgui_sdl3 SDL3::SDL3-static)
set_target_properties(vx_app PROPERTIES OUTPUT_NAME perdix)
# The commit the source tree is at, for the flythrough's report: the script
# runs at each build and rewrites the header only when the commit changed.
add_custom_target(vx_app_commit
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/commit.hpp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/commit.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/commit.hpp")
add_dependencies(vx_app vx_app_commit)
target_include_directories(vx_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
vx_target_warnings(vx_app)
vx_target_engine_options(vx_app)
