#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

// The fuzz target (tests/fuzz/version_description_fuzz.cpp), built into these
// tests too, so it keeps compiling and every build replays the inputs below
// through it. The sanitizer builds check our code; they build yyjson's C
// without their flags, which their presets give C++ alone.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

TEST_CASE("the description fuzz target takes every seed and every input fuzzing saved") {
    // The descriptions and release times the fuzzer starts from, and inputs
    // that once stopped it, each fixed since, in
    // tests/fuzz/inputs/version_description once there are any.
    std::size_t inputs = 0;
    for (const char* folder :
         {VX_DESCRIPTIONS_DIR, VX_RELEASE_TIMES_DIR, VX_DESCRIPTION_FUZZ_INPUTS_DIR}) {
        std::error_code missing;
        for (const std::filesystem::directory_entry& file :
             std::filesystem::directory_iterator(folder, missing)) {
            if (!file.is_regular_file()) {
                continue;
            }
            CAPTURE(file.path().string());
            std::ifstream stream(file.path(), std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                          std::istreambuf_iterator<char>());
            CHECK(LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                         bytes.size()) == 0);
            ++inputs;
        }
    }
    CHECK(inputs >= 8);
}
