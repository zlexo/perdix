include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(doctest)
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

# One test executable per layer. Every test case is registered with CTest on
# its own, so ctest can run, filter and report each case. An entry whose run
# executes no test case fails: a name CMake splits at a semicolon, for one,
# registers halves that match nothing, run nothing and would pass.
# A case that cannot run here says why after "skipped: ", and CTest reports
# it skipped, not passed.
set(VX_DOCTEST_PROPERTIES
    FAIL_REGULAR_EXPRESSION "test cases: +0 [|]"
    SKIP_REGULAR_EXPRESSION "MESSAGE: skipped: ")

add_executable(vx_core_tests
    main.cpp
    core/archive_fuzz_test.cpp
    core/archive_test.cpp
    core/crc32_test.cpp
    core/diagnostic_test.cpp
    core/file_test.cpp
    core/inbox_test.cpp
    core/job_system_test.cpp
    core/job_workers_test.cpp
    core/mesh_result_pool_test.cpp
    core/result_test.cpp
    core/thread_test.cpp
    core/vec3d_test.cpp
    core/version_test.cpp
    fuzz/archive_fuzz.cpp)
target_link_libraries(vx_core_tests PRIVATE vx::core doctest::doctest)
# Archives from other zip writers, and inputs fuzzing saved, read by the
# archive tests (RFC 0009).
target_compile_definitions(vx_core_tests PRIVATE
    VX_ARCHIVES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/archives"
    VX_FUZZ_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/inputs/archive")
vx_target_warnings(vx_core_tests)
# The job workers' tests fail by hanging, as a lost wake-up never returns:
# five minutes, a thousand times the slowest core test here, fails the test
# instead of holding a CI job for GitHub's six hours.
doctest_discover_tests(vx_core_tests PROPERTIES ${VX_DOCTEST_PROPERTIES} TIMEOUT 300)

add_executable(vx_world_tests
    main.cpp
    world/block_data_pool_test.cpp
    world/block_pack_test.cpp
    world/block_file_test.cpp
    world/block_registry_test.cpp
    world/block_state_test.cpp
    world/body_test.cpp
    world/chunk_boxes_test.cpp
    world/chunk_conversion_test.cpp
    world/chunk_map_test.cpp
    world/chunk_storage_test.cpp
    world/collision_test.cpp
    world/coordinates_test.cpp
    world/crouch_test.cpp
    world/edge_guard_test.cpp
    world/flight_test.cpp
    world/job_record_test.cpp
    world/jump_test.cpp
    world/move_rules_test.cpp
    world/movement_property_test.cpp
    world/movement_test.cpp
    world/names_test.cpp
    world/packed_array_test.cpp
    world/placeholder_generator_test.cpp
    world/region_test.cpp
    world/slipperiness_test.cpp
    world/sprint_test.cpp
    world/step_up_test.cpp
    world/versions_test.cpp)
target_link_libraries(vx_world_tests PRIVATE vx::world doctest::doctest)
# RFC 0004's corpus of bad block files, read by the block file tests.
target_compile_definitions(vx_world_tests PRIVATE VX_BAD_BLOCKS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/world/bad_blocks")
vx_target_warnings(vx_world_tests)
doctest_discover_tests(vx_world_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

add_executable(vx_mesh_tests
    main.cpp
    mesh/mesh_chunk_test.cpp
    mesh/padded_chunk_test.cpp
    mesh/quad_test.cpp)
target_link_libraries(vx_mesh_tests PRIVATE vx::mesh doctest::doctest)
vx_target_warnings(vx_mesh_tests)
doctest_discover_tests(vx_mesh_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

add_executable(vx_render_tests
    main.cpp
    render/block_pack_test.cpp
    render/block_textures_test.cpp
    render/camera_relative_test.cpp
    render/camera_test.cpp
    render/device_report_test.cpp
    render/device_selection_test.cpp
    render/feature_floor_test.cpp
    render/projection_test.cpp
    render/swapchain_choice_test.cpp
    render/texture_table_capacity_test.cpp
    render/timestamp_test.cpp)
target_link_libraries(vx_render_tests PRIVATE vx::render doctest::doctest)
# Block texture files for RFC 0004's texture tests, laid out as a pack:
# <dir>/test/textures/blocks/<name>.png.
target_compile_definitions(vx_render_tests PRIVATE VX_BLOCK_TEXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/block_textures"
    # The pack this repository ships, for RFC 0004's shipped-pack test.
    VX_SHIPPED_PACK_DIR="${PROJECT_SOURCE_DIR}")
vx_target_warnings(vx_render_tests)
doctest_discover_tests(vx_render_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

# Tests that talk to a real Vulkan loader and driver. Labeled so presets
# without one can leave them out. Any validation message fails them as well,
# warnings included (RFC 0001), whatever their own checks say. One that the
# driver cannot run says why after "skipped: ", and CTest reports it skipped,
# not passed.
set(VX_VULKAN_DOCTEST_PROPERTIES
    LABELS vulkan
    FAIL_REGULAR_EXPRESSION "test cases: +0 [|]|\\[vulkan (warning|error)\\]"
    SKIP_REGULAR_EXPRESSION "MESSAGE: skipped: ")
add_executable(vx_render_vulkan_tests
    main.cpp
    render/block_texture_array_test.cpp
    render/buffer_test.cpp
    render/chunk_test.cpp
    render/context_test.cpp
    render/gpu_timer_test.cpp
    render/instance_test.cpp
    render/overlay_test.cpp
    render/pipeline_test.cpp
    render/renderer_test.cpp
    render/shader_test.cpp
    render/swapchain_test.cpp
    render/texture_table_test.cpp
    render/upload_test.cpp)
target_link_libraries(vx_render_vulkan_tests PRIVATE vx::render doctest::doctest)
# A shader that samples the block texture array through set 0, for its tests.
vx_add_shaders(vx_render_vulkan_tests render/shaders/block_texture_probe.slang)
# The block texture test files, for the array's readback test.
target_compile_definitions(vx_render_vulkan_tests PRIVATE VX_BLOCK_TEXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/block_textures")
vx_target_warnings(vx_render_vulkan_tests)
doctest_discover_tests(vx_render_vulkan_tests PROPERTIES ${VX_VULKAN_DOCTEST_PROPERTIES})

# The app's startup: the block pack loaded with the player's Minecraft (RFC
# 0009), from packs and jars the tests write; no window, no GPU.
add_executable(vx_app_tests
    main.cpp
    app/options_test.cpp
    app/startup_pack_test.cpp
    app/view_test.cpp
    app/walking_test.cpp)
target_link_libraries(vx_app_tests PRIVATE vx_app_play vx_app_startup vx_app_view doctest::doctest)
target_compile_definitions(vx_app_tests PRIVATE VX_SHIPPED_PACK_DIR="${PROJECT_SOURCE_DIR}")
vx_target_warnings(vx_app_tests)
doctest_discover_tests(vx_app_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

# The app's overlay, fed input events in a hidden window of SDL's dummy video
# driver and drawn by an offscreen renderer: Vulkan tests that need no display.
add_executable(vx_app_vulkan_tests
    main.cpp
    app/overlay_input_test.cpp)
target_link_libraries(vx_app_vulkan_tests PRIVATE vx_app_overlay doctest::doctest)
vx_target_warnings(vx_app_vulkan_tests)
doctest_discover_tests(vx_app_vulkan_tests PROPERTIES ${VX_VULKAN_DOCTEST_PROPERTIES})

# ThreadSanitizer's control (RFC 0005), in its builds only: a program with a
# planted data race, which passes only when the race is reported, so the TSan
# job cannot stay green by no longer finding races.
string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize=thread" vx_thread_sanitizer)
if(NOT vx_thread_sanitizer EQUAL -1)
    find_package(Threads REQUIRED)
    add_executable(vx_planted_race core/planted_race.cpp)
    target_link_libraries(vx_planted_race PRIVATE Threads::Threads)
    vx_target_warnings(vx_planted_race)
    add_test(NAME "ThreadSanitizer reports a planted race" COMMAND vx_planted_race)
    set_tests_properties("ThreadSanitizer reports a planted race" PROPERTIES
        PASS_REGULAR_EXPRESSION "WARNING: ThreadSanitizer: data race")
endif()

# Debug names reach validation messages (RFC 0001): a program that records
# one command breaking a rule about a named buffer passes when the layer's
# message about it carries the buffer's name.
add_executable(vx_render_naming_probe render/naming_probe.cpp)
target_link_libraries(vx_render_naming_probe PRIVATE vx::render)
vx_target_warnings(vx_render_naming_probe)
add_test(NAME "a validation message names the object it is about" COMMAND vx_render_naming_probe)
set_tests_properties("a validation message names the object it is about" PROPERTIES
    LABELS vulkan
    PASS_REGULAR_EXPRESSION "VUID-vkCmdFillBuffer-dstOffset-00025[^\n]*named probe buffer")

# A frame's heap allocations, counted by replacing the global operator new:
# an executable of its own, since the count would take in every other test.
add_executable(vx_render_allocation_tests
    main.cpp
    render/allocation_test.cpp)
target_link_libraries(vx_render_allocation_tests PRIVATE vx::render doctest::doctest)
vx_target_warnings(vx_render_allocation_tests)
doctest_discover_tests(vx_render_allocation_tests PROPERTIES ${VX_VULKAN_DOCTEST_PROPERTIES})

add_executable(vx_minecraft_tests
    main.cpp
    minecraft/install_test.cpp
    minecraft/made_textures_test.cpp
    minecraft/minecraft_pack_test.cpp
    minecraft/shipped_map_test.cpp
    minecraft/texture_map_test.cpp
    minecraft/texture_maps_test.cpp
    minecraft/version_description_fuzz_test.cpp
    minecraft/version_description_test.cpp
    fuzz/version_description_fuzz.cpp)
# render too, for the end-to-end test that loads the core pack with made
# textures, nothing in it touching a GPU; and stb_image, for the check against
# the player's own Minecraft, which decodes the jar's files apart.
target_link_libraries(vx_minecraft_tests PRIVATE vx::minecraft vx::render vx_stb_image
    doctest::doctest)
# RFC 0009's corpus of bad texture maps and the pack this repository ships,
# for the shipped map's test; version descriptions and release times, and
# inputs fuzzing saved, read by the description fuzz target's replay test.
target_compile_definitions(vx_minecraft_tests PRIVATE
    VX_BAD_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/minecraft/bad_maps"
    VX_SHIPPED_PACK_DIR="${PROJECT_SOURCE_DIR}"
    VX_DESCRIPTIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/minecraft/descriptions"
    VX_RELEASE_TIMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/minecraft/release_times"
    VX_DESCRIPTION_FUZZ_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/inputs/version_description")
vx_target_warnings(vx_minecraft_tests)
doctest_discover_tests(vx_minecraft_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})
