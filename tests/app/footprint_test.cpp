#include "footprint.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// The process's memory, as RFC 0006's flythrough reports it.

TEST_CASE("the process's footprint is read, and grows with memory written") {
#if defined(__APPLE__) || defined(__linux__)
    const std::uint64_t before = vx::app::process_footprint();
    CHECK(before > 0);
    // 64 MiB, every page written so that the system gives it.
    constexpr std::size_t size = std::size_t{64} * 1024 * 1024;
    std::vector<unsigned char> memory(size);
    for (std::size_t at = 0; at < size; at += 4'096) {
        memory[at] = static_cast<unsigned char>(at);
    }
    const std::uint64_t after = vx::app::process_footprint();
    CHECK(after >= before + (size / 2));
    CHECK(memory[4'096] == 0);
#else
    CHECK(vx::app::process_footprint() == 0);
#endif
}
