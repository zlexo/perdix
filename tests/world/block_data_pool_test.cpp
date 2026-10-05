#include <vx/world/block_data_pool.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

// VX_ADDRESS_SANITIZER: AddressSanitizer instruments this file. GCC says so
// with __SANITIZE_ADDRESS__, Clang through __has_feature.
#ifdef __SANITIZE_ADDRESS__
#define VX_ADDRESS_SANITIZER
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VX_ADDRESS_SANITIZER
#endif
#endif
#ifdef VX_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

namespace {

std::size_t words_of(int log2_bits) {
    return (std::size_t{vx::chunk_volume} << static_cast<unsigned>(log2_bits)) / 64;
}

} // namespace

TEST_CASE("each class's blocks are as big as a packed array of its width needs") {
    const std::array<vx::BlockDataClass, vx::block_data::classes> counts = vx::block_data::stats();
    CHECK(counts[0].block_bytes == 4'096);
    CHECK(counts[1].block_bytes == 8'192);
    CHECK(counts[2].block_bytes == 16'384);
    CHECK(counts[3].block_bytes == 32'768);
    CHECK(counts[4].block_bytes == 65'536);
}

TEST_CASE("a block comes zeroed, and a freed one is handed out again") {
    // The pool is the process's, so the counts are compared with before.
    const std::array<vx::BlockDataClass, vx::block_data::classes> before = vx::block_data::stats();
    std::uint64_t* const first = vx::block_data::allocate(2);
    std::fill_n(first, words_of(2), ~std::uint64_t{0});
    vx::block_data::deallocate(first, 2);
    std::uint64_t* const second = vx::block_data::allocate(2);
    CHECK(second == first);
    CHECK(std::all_of(second, second + words_of(2), [](std::uint64_t word) { return word == 0; }));
    CHECK(vx::block_data::stats()[2].in_use == before[2].in_use + 1);
    vx::block_data::deallocate(second, 2);
    const std::array<vx::BlockDataClass, vx::block_data::classes> after = vx::block_data::stats();
    CHECK(after[2].in_use == before[2].in_use);
    CHECK(after[2].cached == std::max<std::size_t>(before[2].cached, 1));
}

#ifdef VX_ADDRESS_SANITIZER
// Only where AddressSanitizer runs (Linux CI): it hangs at startup on macOS.
TEST_CASE("under AddressSanitizer a cached block is poisoned until it is handed out again") {
    std::uint64_t* const words = vx::block_data::allocate(0);
    std::uint64_t* const last = words + words_of(0) - 1;
    CHECK(__asan_address_is_poisoned(words + 1) == 0);
    CHECK(__asan_address_is_poisoned(last) == 0);
    vx::block_data::deallocate(words, 0);
    // Reading or writing past the first word would now report. The first
    // word, the free-list link, stays readable for LeakSanitizer.
    CHECK(__asan_address_is_poisoned(words + 1) == 1);
    CHECK(__asan_address_is_poisoned(last) == 1);
    CHECK(__asan_address_is_poisoned(words) == 0);
    std::uint64_t* const again = vx::block_data::allocate(0);
    REQUIRE(again == words);
    CHECK(__asan_address_is_poisoned(again + 1) == 0);
    CHECK(__asan_address_is_poisoned(last) == 0);
    vx::block_data::deallocate(again, 0);
}
#endif

TEST_CASE("threads allocating and freeing at once never share a block, and every count returns") {
    // Each thread marks its blocks as its own before freeing them: a block
    // handed to two threads at once would lose one thread's mark.
    constexpr int threads = 4;
    constexpr int rounds = 2'000;
    const std::array<vx::BlockDataClass, vx::block_data::classes> before = vx::block_data::stats();
    std::vector<int> errors(threads, 0);
    std::vector<std::thread> workers;
    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([thread, &errors] {
            std::mt19937 generator(static_cast<std::uint32_t>(thread));
            std::uniform_int_distribution<int> pick(0, vx::block_data::classes - 1);
            const auto mark = static_cast<std::uint64_t>(thread) + 1;
            for (int round = 0; round < rounds; ++round) {
                const int log2_bits = pick(generator);
                std::uint64_t* const words = vx::block_data::allocate(log2_bits);
                const std::size_t count = words_of(log2_bits);
                if (words[0] != 0 || words[count - 1] != 0) {
                    ++errors[static_cast<std::size_t>(thread)];
                }
                std::fill_n(words, count, mark);
                std::this_thread::yield();
                if (!std::all_of(words, words + count,
                                 [mark](std::uint64_t word) { return word == mark; })) {
                    ++errors[static_cast<std::size_t>(thread)];
                }
                vx::block_data::deallocate(words, log2_bits);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(std::count(errors.begin(), errors.end(), 0) == threads);
    const std::array<vx::BlockDataClass, vx::block_data::classes> after = vx::block_data::stats();
    for (std::size_t size_class = 0; size_class < after.size(); ++size_class) {
        INFO("class ", size_class);
        CHECK(after[size_class].in_use == before[size_class].in_use);
    }
}
