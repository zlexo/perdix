#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// The fuzz target (tests/fuzz/archive_fuzz.cpp), built into these tests too,
// so it keeps compiling and every build, sanitizers included, replays the
// inputs below through it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

TEST_CASE("the fuzz target takes every test archive and every input fuzzing saved") {
    // Inputs that once stopped the fuzzer, each fixed since, in
    // tests/fuzz/inputs/archive, with the test archives the fuzzer starts from.
    std::size_t inputs = 0;
    for (const char* folder : {VX_ARCHIVES_DIR, VX_FUZZ_INPUTS_DIR}) {
        for (const std::filesystem::directory_entry& file :
             std::filesystem::directory_iterator(folder)) {
            if (!file.is_regular_file() || file.path().extension() == ".py") {
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
    CHECK(inputs >= 5);
}
