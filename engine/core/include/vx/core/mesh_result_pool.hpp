#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace vx {

class MeshResultPool;

// A mesh job's result (RFC 0005): CPU memory from the pool, owned by
// whichever thread holds it, and back to the pool when destroyed. A worker
// copies its quads in and sends it on; the main thread drops it once RFC
// 0007 has copied the quads to the GPU.
class MeshBuffer {
public:
    MeshBuffer() noexcept = default;
    MeshBuffer(MeshBuffer&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), data_(std::exchange(other.data_, nullptr)),
          size_class_(std::exchange(other.size_class_, std::uint8_t{0})) {}
    MeshBuffer& operator=(MeshBuffer&& other) noexcept {
        if (this != &other) {
            release();
            pool_ = std::exchange(other.pool_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            size_class_ = std::exchange(other.size_class_, std::uint8_t{0});
        }
        return *this;
    }
    MeshBuffer(const MeshBuffer&) = delete;
    MeshBuffer& operator=(const MeshBuffer&) = delete;
    ~MeshBuffer() { release(); }

    [[nodiscard]] bool empty() const noexcept { return data_ == nullptr; }
    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    // Bytes it holds: its size class's, 0 when empty.
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    friend class MeshResultPool;
    MeshBuffer(MeshResultPool* pool, std::byte* data, std::uint8_t size_class) noexcept
        : pool_(pool), data_(data), size_class_(size_class) {}
    void release() noexcept;

    MeshResultPool* pool_ = nullptr;
    std::byte* data_ = nullptr;
    std::uint8_t size_class_ = 0;
};

// One size class, as stats() reports it.
struct MeshPoolClass {
    std::size_t buffer_bytes = 0;
    std::size_t in_use = 0; // buffers held
    std::size_t cached = 0; // buffers freed and kept for reuse
};

// Memory for mesh results (RFC 0005), like RFC 0002's block data: size
// classes of powers of two from 4 KiB to 2 MiB, each with a free list and a
// lock of its own, since workers allocate and the main thread frees. 2 MiB
// holds RFC 0003's worst mesh, 196,608 quads of 8 bytes. Freed buffers stay
// cached until the pool is destroyed, and freeing never allocates: a cached
// buffer's first bytes link it to the next.
class MeshResultPool {
public:
    static constexpr int classes = 10;
    static constexpr std::size_t smallest = std::size_t{4} << 10U;
    static constexpr std::size_t largest = smallest << (classes - 1);

    MeshResultPool() = default;
    // Asserts that every buffer has come back.
    ~MeshResultPool();
    MeshResultPool(const MeshResultPool&) = delete;
    MeshResultPool& operator=(const MeshResultPool&) = delete;
    MeshResultPool(MeshResultPool&&) = delete;
    MeshResultPool& operator=(MeshResultPool&&) = delete;

    // A buffer of at least `bytes`, at most `largest`, from the smallest class
    // that holds them: the class's last freed buffer when it has one.
    [[nodiscard]] MeshBuffer allocate(std::size_t bytes);
    // Each class's counts, each read under its class's lock.
    [[nodiscard]] std::array<MeshPoolClass, classes> stats() const;

private:
    friend class MeshBuffer;
    void free(std::byte* data, std::uint8_t size_class) noexcept;

    struct SizeClass {
        mutable std::mutex lock;
        std::byte* cached = nullptr; // the last freed buffer, the head of the list
        std::size_t cached_count = 0;
        std::size_t in_use = 0;
    };
    std::array<SizeClass, classes> classes_;
};

} // namespace vx
