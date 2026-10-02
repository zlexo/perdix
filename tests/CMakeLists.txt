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
    core/result_test.cpp
    core/version_test.cpp)
target_link_libraries(vx_core_tests PRIVATE vx::core doctest::doctest)
vx_target_warnings(vx_core_tests)
doctest_discover_tests(vx_core_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

add_executable(vx_render_tests
    main.cpp
    render/device_report_test.cpp
    render/device_selection_test.cpp
    render/feature_floor_test.cpp
    render/swapchain_choice_test.cpp)
target_link_libraries(vx_render_tests PRIVATE vx::render doctest::doctest)
vx_target_warnings(vx_render_tests)
doctest_discover_tests(vx_render_tests PROPERTIES ${VX_DOCTEST_PROPERTIES})

# Tests that talk to a real Vulkan loader and driver. Labeled so presets
# without one can leave them out.
add_executable(vx_render_vulkan_tests
    main.cpp
    render/context_test.cpp
    render/instance_test.cpp)
target_link_libraries(vx_render_vulkan_tests PRIVATE vx::render doctest::doctest)
vx_target_warnings(vx_render_vulkan_tests)
doctest_discover_tests(vx_render_vulkan_tests PROPERTIES LABELS vulkan ${VX_DOCTEST_PROPERTIES})
