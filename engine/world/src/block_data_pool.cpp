#include <vx/world/block_data_pool.hpp>

#include <vx/world/coordinates.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

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

namespace vx::block_data {
namespace {

// A freed block's first word holds the next freed block's address, so the
// free list lives in the blocks themselves and freeing never allocates.
static_assert(sizeof(std::uint64_t*) == sizeof(std::uint64_t));

struct SizeClass {
    std::mutex lock;
    std::uint64_t* cached = nullptr; // the last freed block, the head of the list
    std::size_t cached_count = 0;
    std::size_t in_use = 0;
};

// Never destroyed, so a chunk freed during static destruction still finds
// its class.
std::array<SizeClass, classes>& pool() {
    static auto* const instance = new std::array<SizeClass, classes>();
    return *instance;
}

SizeClass& class_of(int log2_bits) {
    assert(log2_bits >= 0 && log2_bits < classes);
    return pool()[static_cast<std::size_t>(log2_bits)];
}

// chunk_volume entries, 2^log2_bits bits each.
std::size_t bytes_of(int log2_bits) {
    return (std::size_t{chunk_volume} << static_cast<unsigned>(log2_bits)) / 8;
}

// Cached blocks never go back to the heap, so AddressSanitizer would see a
// use after free of block data as an ordinary read. Under it, a cached block
// is poisoned until allocate hands it out again, all but its first word: the
// free-list link, which LeakSanitizer must read to reach the blocks behind
// it, since it does not follow pointers kept in poisoned memory.
void poison([[maybe_unused]] std::uint64_t* words, [[maybe_unused]] std::size_t bytes) noexcept {
#ifdef VX_ADDRESS_SANITIZER
    __asan_poison_memory_region(words + 1, bytes - sizeof(std::uint64_t));
#endif
}

void unpoison([[maybe_unused]] std::uint64_t* words, [[maybe_unused]] std::size_t bytes) noexcept {
#ifdef VX_ADDRESS_SANITIZER
    __asan_unpoison_memory_region(words + 1, bytes - sizeof(std::uint64_t));
#endif
}

// Under AddressSanitizer, whether a block is cached already: freeing it again
// would put it on the free list twice.
[[maybe_unused]] bool is_cached([[maybe_unused]] const std::uint64_t* words) noexcept {
#ifdef VX_ADDRESS_SANITIZER
    return __asan_address_is_poisoned(words + 1) != 0;
#else
    return false;
#endif
}

} // namespace

std::uint64_t* allocate(int log2_bits) {
    SizeClass& size_class = class_of(log2_bits);
    const std::size_t bytes = bytes_of(log2_bits);
    std::uint64_t* words = nullptr;
    {
        const std::scoped_lock guard(size_class.lock);
        if (size_class.cached != nullptr) {
            words = size_class.cached;
            unpoison(words, bytes);
            size_class.cached = std::bit_cast<std::uint64_t*>(words[0]);
            --size_class.cached_count;
        }
        ++size_class.in_use;
    }
    if (words == nullptr) {
        words = static_cast<std::uint64_t*>(::operator new(bytes));
    }
    std::memset(words, 0, bytes);
    return words;
}

void deallocate(std::uint64_t* words, int log2_bits) noexcept {
    SizeClass& size_class = class_of(log2_bits);
    const std::scoped_lock guard(size_class.lock);
    assert(!is_cached(words)); // freed twice
    words[0] = std::bit_cast<std::uint64_t>(size_class.cached);
    poison(words, bytes_of(log2_bits));
    size_class.cached = words;
    ++size_class.cached_count;
    --size_class.in_use;
}

std::array<BlockDataClass, classes> stats() {
    std::array<BlockDataClass, classes> counts{};
    for (int log2_bits = 0; log2_bits < classes; ++log2_bits) {
        SizeClass& size_class = class_of(log2_bits);
        const std::scoped_lock guard(size_class.lock);
        counts[static_cast<std::size_t>(log2_bits)] = {.block_bytes = bytes_of(log2_bits),
                                                       .in_use = size_class.in_use,
                                                       .cached = size_class.cached_count};
    }
    return counts;
}

} // namespace vx::block_data
