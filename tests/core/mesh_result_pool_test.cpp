#include <vx/core/inbox.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/wake_event.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
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

#ifdef VX_ADDRESS_SANITIZER
// Only where AddressSanitizer runs (Linux CI): it hangs at startup on macOS.
TEST_CASE("under AddressSanitizer a cached buffer is poisoned until it is handed out again") {
    MeshResultPool pool;
    std::byte* bytes = nullptr;
    {
        MeshBuffer buffer = pool.allocate(4'096);
        bytes = buffer.data();
    }
    std::byte* const past_link = bytes + sizeof(std::byte*);
    std::byte* const last = bytes + 4'095;
    // Reading or writing the quads of a dropped buffer would now report. The
    // first bytes, the link, stay readable.
    CHECK(__asan_address_is_poisoned(past_link) == 1);
    CHECK(__asan_address_is_poisoned(last) == 1);
    CHECK(__asan_address_is_poisoned(bytes) == 0);
    const MeshBuffer again = pool.allocate(4'096);
    REQUIRE(again.data() == bytes);
    CHECK(__asan_address_is_poisoned(past_link) == 0);
    CHECK(__asan_address_is_poisoned(last) == 0);
}
#endif

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

TEST_CASE("every buffer freed to a class is handed out again") {
    MeshResultPool pool;
    std::vector<MeshBuffer> buffers;
    std::vector<const std::byte*> freed;
    for (int i = 0; i < 3; ++i) {
        buffers.push_back(pool.allocate(5'000));
        freed.push_back(buffers.back().data());
    }
    buffers.clear();
    CHECK(pool.stats()[1].cached == 3);
    std::vector<const std::byte*> again;
    for (int i = 0; i < 3; ++i) {
        buffers.push_back(pool.allocate(5'000));
        again.push_back(buffers.back().data());
    }
    // In any order: which one the list holds first depends on the order
    // clear() destroyed them in.
    std::ranges::sort(freed);
    std::ranges::sort(again);
    CHECK(again == freed);
    CHECK(pool.stats()[1].cached == 0);
    CHECK(pool.stats()[1].in_use == 3);
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
    // Moving a buffer onto itself keeps it.
    MeshBuffer& same = assigned;
    assigned = std::move(same);
    CHECK(assigned.data() == memory);
    CHECK(pool.stats()[0].in_use == 1);
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

namespace {

// A buffer on its way to the thread that frees it, with the tag its
// producer wrote into its first and last bytes.
struct Tagged {
    std::uint64_t tag = 0;
    MeshBuffer buffer;
};

std::uint64_t tag_at(const std::byte* bytes) {
    std::uint64_t tag = 0;
    std::memcpy(&tag, bytes, sizeof(tag));
    return tag;
}

} // namespace

TEST_CASE("buffers allocated on four threads while another frees them come back whole") {
    // RFC 0005's pattern: workers allocate while the main thread frees, so
    // allocations and frees meet on the same free list.
    constexpr int threads_count = 4;
    constexpr int each = 2'000;
    MeshResultPool pool;
    vx::WakeEvent event;
    vx::Inbox<Tagged> inbox(256, event);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < threads_count; ++thread) {
        threads.emplace_back([&pool, &inbox, thread] {
            for (int i = 0; i < each; ++i) {
                MeshBuffer buffer = pool.allocate(4'096);
                const std::uint64_t tag =
                    (static_cast<std::uint64_t>(thread) << 32U) | static_cast<std::uint64_t>(i);
                // The first bytes are where a cached buffer keeps its link.
                std::memcpy(buffer.data(), &tag, sizeof(tag));
                std::memcpy(buffer.data() + buffer.capacity() - sizeof(tag), &tag, sizeof(tag));
                inbox.push(Tagged{.tag = tag, .buffer = std::move(buffer)});
            }
        });
    }
    std::size_t received = 0;
    std::size_t damaged = 0;
    std::vector<const std::byte*> seen;
    while (received < std::size_t{threads_count} * each) {
        if (!event.wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
            break; // a lost wake-up: the counts below say so
        }
        inbox.drain([&](Tagged&& tagged) {
            // Freed at the end of this call, while the producers allocate.
            const MeshBuffer buffer = std::move(tagged.buffer);
            const std::byte* const last = buffer.data() + buffer.capacity() - sizeof(std::uint64_t);
            if (tag_at(buffer.data()) != tagged.tag || tag_at(last) != tagged.tag) {
                ++damaged;
            }
            seen.push_back(buffer.data());
            ++received;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(received == std::size_t{threads_count} * each);
    CHECK(damaged == 0);
    // Every buffer the pool made is back in its list, once.
    std::ranges::sort(seen);
    const auto made = static_cast<std::size_t>(std::ranges::unique(seen).begin() - seen.begin());
    CHECK(pool.stats()[0].in_use == 0);
    CHECK(pool.stats()[0].cached == made);
}
