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

add_executable(vx_app
    src/main.cpp)
target_link_libraries(vx_app PRIVATE vx::core vx::render SDL3::SDL3-static)
set_target_properties(vx_app PROPERTIES OUTPUT_NAME perdix)
vx_target_warnings(vx_app)
vx_target_engine_options(vx_app)
