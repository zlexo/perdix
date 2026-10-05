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
set(VX_DOCTEST_PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 [|]")

add_executable(vx_core_tests
    main.cpp
    core/diagnostic_test.cpp
    core/result_test.cpp
    core/version_test.cpp)
target_link_libraries(vx_core_tests PRIVATE vx::core doctest::doctest)
vx_target_warnings(vx_core_tests)
doctest_discover_tests(vx_core_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

add_executable(vx_world_tests
    main.cpp
    world/block_data_pool_test.cpp
    world/block_file_test.cpp
    world/block_registry_test.cpp
    world/block_state_test.cpp
    world/chunk_conversion_test.cpp
    world/chunk_storage_test.cpp
    world/coordinates_test.cpp
    world/packed_array_test.cpp)
target_link_libraries(vx_world_tests PRIVATE vx::world doctest::doctest)
# RFC 0004's corpus of bad block files, read by the block file tests.
target_compile_definitions(vx_world_tests PRIVATE VX_BAD_BLOCKS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/world/bad_blocks")
vx_target_warnings(vx_world_tests)
doctest_discover_tests(vx_world_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

add_executable(vx_render_tests
    main.cpp
    render/camera_relative_test.cpp
    render/camera_test.cpp
    render/device_report_test.cpp
    render/device_selection_test.cpp
    render/feature_floor_test.cpp
    render/projection_test.cpp
    render/swapchain_choice_test.cpp
    render/timestamp_test.cpp)
target_link_libraries(vx_render_tests PRIVATE vx::render doctest::doctest)
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
    render/buffer_test.cpp
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
vx_target_warnings(vx_render_vulkan_tests)
doctest_discover_tests(vx_render_vulkan_tests PROPERTIES ${VX_VULKAN_DOCTEST_PROPERTIES})

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
