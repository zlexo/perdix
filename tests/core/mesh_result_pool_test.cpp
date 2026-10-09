#include <vx/core/mesh_result_pool.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

// RFC 0005's mesh result pool: buffers sized by class, reused once freed,
// allocated on workers and freed on another thread.

using vx::MeshBuffer;
using vx::MeshResultPool;

TEST_CASE("a buffer comes from the smallest class that holds the bytes asked for") {
    MeshResultPool pool;
    CHECK(pool.allocate(0).capacity() == 4'096);
    CHECK(pool.allocate(4'096).capacity() == 4'096);
    CHECK(pool.allocate(4'097).capacity() == 8'192);
    // RFC 0003's worst mesh: 196,608 quads of 8 bytes.
    CHECK(pool.allocate(std::size_t{196'608} * 8).capacity() == std::size_t{2} << 20U);
    CHECK(pool.allocate(MeshResultPool::largest).capacity() == MeshResultPool::largest);
}

TEST_CASE("a freed buffer is handed out again, and the counts follow it") {
    MeshResultPool pool;
    const std::byte* first = nullptr;
    {
        const MeshBuffer buffer = pool.allocate(10'000);
        first = buffer.data();
        CHECK(pool.stats()[2].in_use == 1);
        CHECK(pool.stats()[2].cached == 0);
    }
    CHECK(pool.stats()[2].in_use == 0);
    CHECK(pool.stats()[2].cached == 1);
    const MeshBuffer again = pool.allocate(16'000);
    CHECK(again.data() == first);
    CHECK(pool.stats()[2].cached == 0);
    CHECK(pool.stats()[2].buffer_bytes == 16'384);
    // Another class has its own list.
    const MeshBuffer other = pool.allocate(100);
    CHECK(other.data() != first);
}

TEST_CASE("moving a buffer moves the memory, and only the last holder frees it") {
    MeshResultPool pool;
    MeshBuffer buffer = pool.allocate(1);
    const std::byte* const memory = buffer.data();
    MeshBuffer moved = std::move(buffer);
    CHECK(buffer.empty()); // NOLINT(bugprone-use-after-move): what a move leaves
    CHECK(buffer.capacity() == 0);
    CHECK(moved.data() == memory);
    MeshBuffer assigned = pool.allocate(1);
    assigned = std::move(moved);
    CHECK(assigned.data() == memory);
    // The buffer assigned over went back to the pool.
    CHECK(pool.stats()[0].in_use == 1);
    CHECK(pool.stats()[0].cached == 1);
}

TEST_CASE("buffers filled on four threads come back intact and free on another") {
    constexpr int threads_count = 4;
    constexpr int each = 500;
    MeshResultPool pool;
    std::mutex lock;
    std::vector<std::pair<std::uint8_t, MeshBuffer>> filled;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < threads_count; ++thread) {
        threads.emplace_back([&, thread] {
            std::mt19937 random(static_cast<std::uint32_t>(thread) + 1);
            // Up to 32 KiB, four classes, so the 2,000 buffers held at once
            // take tens of MB, not hundreds.
            std::uniform_int_distribution<std::size_t> size(1, std::size_t{32} << 10U);
            for (int i = 0; i < each; ++i) {
                MeshBuffer buffer = pool.allocate(size(random));
                const auto mark = static_cast<std::uint8_t>((thread * each) + i);
                for (std::size_t at = 0; at < buffer.capacity(); at += 512) {
                    buffer.data()[at] = std::byte{mark};
                }
                const std::scoped_lock guard(lock);
                filled.emplace_back(mark, std::move(buffer));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::size_t damaged = 0;
    for (const auto& [mark, buffer] : filled) {
        for (std::size_t at = 0; at < buffer.capacity(); at += 512) {
            if (buffer.data()[at] != std::byte{mark}) {
                ++damaged;
            }
        }
    }
    CHECK(damaged == 0);
    filled.clear(); // freed here, not where they were allocated
    std::size_t in_use = 0;
    std::size_t cached = 0;
    for (const vx::MeshPoolClass& counts : pool.stats()) {
        in_use += counts.in_use;
        cached += counts.cached;
    }
    CHECK(in_use == 0);
    CHECK(cached > 0);
    CHECK(cached <= std::size_t{threads_count} * each);
}
