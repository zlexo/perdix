#include <vx/core/mesh_result_pool.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

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

namespace vx {
namespace {

constexpr std::size_t class_bytes(int size_class) noexcept {
    return MeshResultPool::smallest << static_cast<unsigned>(size_class);
}

// The smallest class that holds `bytes`.
int class_for(std::size_t bytes) noexcept {
    int size_class = 0;
    while (class_bytes(size_class) < bytes) {
        ++size_class;
    }
    return size_class;
}

// A cached buffer's link to the next one, kept in its first bytes.
std::byte* next_of(const std::byte* buffer) noexcept {
    std::byte* next = nullptr;
    std::memcpy(static_cast<void*>(&next), buffer, sizeof(next));
    return next;
}

void link(std::byte* buffer, std::byte* next) noexcept {
    std::memcpy(buffer, static_cast<const void*>(&next), sizeof(next));
}

// Cached buffers go back to the heap only with the pool, so AddressSanitizer
// would see a read of a dropped buffer's quads as an ordinary read, as with
// RFC 0002's block data. Under it, a cached buffer is poisoned until
// allocate hands it out again, all but its first bytes: the link.
void poison([[maybe_unused]] std::byte* buffer, [[maybe_unused]] std::size_t bytes) noexcept {
#ifdef VX_ADDRESS_SANITIZER
    __asan_poison_memory_region(buffer + sizeof(std::byte*), bytes - sizeof(std::byte*));
#endif
}

void unpoison([[maybe_unused]] std::byte* buffer, [[maybe_unused]] std::size_t bytes) noexcept {
#ifdef VX_ADDRESS_SANITIZER
    __asan_unpoison_memory_region(buffer + sizeof(std::byte*), bytes - sizeof(std::byte*));
#endif
}

} // namespace

std::size_t MeshBuffer::capacity() const noexcept {
    return empty() ? 0 : class_bytes(size_class_);
}

void MeshBuffer::release() noexcept {
    if (data_ != nullptr) {
        pool_->free(std::exchange(data_, nullptr), size_class_);
        pool_ = nullptr;
    }
}

MeshResultPool::~MeshResultPool() {
    for (int size_class = 0; size_class < classes; ++size_class) {
        const SizeClass& sizes = classes_[static_cast<std::size_t>(size_class)];
        assert(sizes.in_use == 0 && "mesh buffers still held");
        for (std::byte* buffer = sizes.cached; buffer != nullptr;) {
            std::byte* const next = next_of(buffer);
            ::operator delete(buffer);
            buffer = next;
        }
    }
}

MeshBuffer MeshResultPool::allocate(std::size_t bytes) {
    assert(bytes <= largest && "no mesh is that large");
    const int size_class = class_for(bytes);
    SizeClass& sizes = classes_[static_cast<std::size_t>(size_class)];
    std::byte* buffer = nullptr;
    {
        const std::scoped_lock guard(sizes.lock);
        if (sizes.cached != nullptr) {
            buffer = sizes.cached;
            unpoison(buffer, class_bytes(size_class));
            sizes.cached = next_of(buffer);
            --sizes.cached_count;
        }
        ++sizes.in_use;
    }
    if (buffer == nullptr) {
        // Plain new, as for block data: the heap's alignment suits 8-byte
        // quads, and profiling builds see it in Tracy's memory view.
        buffer = static_cast<std::byte*>(::operator new(class_bytes(size_class)));
    }
    return {this, buffer, static_cast<std::uint8_t>(size_class)};
}

void MeshResultPool::free(std::byte* data, std::uint8_t size_class) noexcept {
    SizeClass& sizes = classes_[size_class];
    const std::scoped_lock guard(sizes.lock);
    link(data, sizes.cached);
    poison(data, class_bytes(size_class));
    sizes.cached = data;
    ++sizes.cached_count;
    --sizes.in_use;
}

std::array<MeshPoolClass, MeshResultPool::classes> MeshResultPool::stats() const {
    std::array<MeshPoolClass, classes> counts{};
    for (int size_class = 0; size_class < classes; ++size_class) {
        const SizeClass& sizes = classes_[static_cast<std::size_t>(size_class)];
        const std::scoped_lock guard(sizes.lock);
        counts[static_cast<std::size_t>(size_class)] = {.buffer_bytes = class_bytes(size_class),
                                                        .in_use = sizes.in_use,
                                                        .cached = sizes.cached_count};
    }
    return counts;
}

} // namespace vx
