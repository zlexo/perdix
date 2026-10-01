include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(doctest)
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

# One test executable per layer. Every test case is registered with CTest on
# its own, so ctest can run, filter and report each case.
add_executable(vx_core_tests
    main.cpp
    core/version_test.cpp)
target_link_libraries(vx_core_tests PRIVATE vx::core doctest::doctest)
vx_target_warnings(vx_core_tests)
doctest_discover_tests(vx_core_tests)
