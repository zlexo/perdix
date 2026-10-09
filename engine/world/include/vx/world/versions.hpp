#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace vx {

template <class T> class Versions;

// One version of a chunk's blocks, a plan or a parameter snapshot (RFC 0005):
// the current one, which only the world thread writes, or a frozen one that
// jobs still read. Jobs reach versions through their records; only Versions
// makes and frees them.
template <class T> class Version {
public:
    Version() = default;
    // Jobs hold its address.
    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;
    Version(Version&&) = delete;
    Version& operator=(Version&&) = delete;
    ~Version() = default;

    [[nodiscard]] const T& value() const noexcept { return value_; }
    // The jobs reading this version that have not reported back.
    [[nodiscard]] std::uint32_t pins() const noexcept { return pins_; }
    // False once a copy replaced it or what it belongs to left the world.
    [[nodiscard]] bool current() const noexcept { return current_; }

private:
    friend class Versions<T>;

    T value_{};
    std::uint32_t pins_ = 0;
    bool current_ = false;
    Version* next_free_ = nullptr;
};

// The versions of one kind of thing: chunks' blocks, plans or parameter
// snapshots (RFC 0005). World thread only: pins are plain integers, raised
// when the world thread submits a job and lowered when the job's completion
// arrives or its cancel succeeds. A version that is neither current nor
// pinned is freed at once, so what a job reads lives exactly until it reports
// back. Versions sit at stable addresses, in blocks of 256 kept for reuse.
template <class T> class Versions {
public:
    Versions() = default;
    Versions(const Versions&) = delete;
    Versions& operator=(const Versions&) = delete;
    Versions(Versions&&) = delete;
    Versions& operator=(Versions&&) = delete;
    // Every job has reported back by then: the world goes after its jobs.
    ~Versions() { assert(pins_ == 0 && "a job still reads a version"); }

    // A new current version holding `value`.
    [[nodiscard]] Version<T>& make(T value) {
        if (free_ == nullptr) {
            grow();
        }
        Version<T>& version = *free_;
        free_ = version.next_free_;
        version.next_free_ = nullptr;
        version.value_ = std::move(value);
        version.current_ = true;
        ++live_;
        return version;
    }

    // For a job about to read `version`, a current one: it stays as it is
    // until the job reports back.
    void pin(Version<T>& version) noexcept {
        assert(version.current_ && "jobs are handed current versions");
        ++version.pins_;
        ++pins_;
    }

    // A job reading `version` reported back, or its cancel succeeded: the
    // version goes if that was its last pin and it is no longer current.
    void unpin(Version<T>& version) noexcept {
        assert(version.pins_ > 0 && "unpinned more often than pinned");
        --version.pins_;
        --pins_;
        if (version.pins_ == 0 && !version.current_) {
            --frozen_;
            recycle(version);
        }
    }

    // `version` stops being current: its chunk left the world, or its plan
    // was dropped. It goes at once unless jobs read it, else with their last
    // report.
    void retire(Version<T>& version) noexcept {
        assert(version.current_ && "retired twice");
        version.current_ = false;
        if (version.pins_ == 0) {
            recycle(version);
        } else {
            ++frozen_;
        }
    }

    // The value to write for `current`, a current version: its own when no
    // job reads it. Otherwise a copy becomes current, and `current` then
    // names it, while the pinned version stays frozen for its jobs.
    [[nodiscard]] T& write(Version<T>*& current) {
        assert(current != nullptr && current->current_);
        if (current->pins_ == 0) {
            return current->value_;
        }
        Version<T>& copy = make(copy_of(current->value_));
        ++copies_;
        retire(*current);
        current = &copy;
        return copy.value_;
    }

    // Current versions, and frozen ones that jobs still read.
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    // Versions kept only for the jobs reading them.
    [[nodiscard]] std::size_t frozen() const noexcept { return frozen_; }
    // Pins held, over every version.
    [[nodiscard]] std::uint64_t pins() const noexcept { return pins_; }
    // Copies made because a pinned version was written.
    [[nodiscard]] std::uint64_t copies() const noexcept { return copies_; }

private:
    static constexpr std::size_t block_size = 256;
    using Block = std::array<Version<T>, block_size>;

    // Chunks copy through clone() (RFC 0002); plans and snapshots by value.
    static T copy_of(const T& value) {
        if constexpr (requires {
                          { value.clone() } -> std::same_as<T>;
                      }) {
            return value.clone();
        } else {
            return value;
        }
    }

    void grow() {
        blocks_.push_back(std::make_unique<Block>());
        // Linked last to first, so versions are handed out in address order.
        Block& block = *blocks_.back();
        for (std::size_t at = block_size; at > 0; --at) {
            block[at - 1].next_free_ = free_;
            free_ = &block[at - 1];
        }
    }

    // Frees the value at once (a chunk's block data goes back to its pool)
    // and keeps the version for the next make.
    void recycle(Version<T>& version) noexcept {
        version.value_ = T{};
        version.next_free_ = free_;
        free_ = &version;
        --live_;
    }

    std::vector<std::unique_ptr<Block>> blocks_;
    Version<T>* free_ = nullptr;
    std::size_t live_ = 0;
    std::size_t frozen_ = 0;
    std::uint64_t pins_ = 0;
    std::uint64_t copies_ = 0;
};

} // namespace vx
