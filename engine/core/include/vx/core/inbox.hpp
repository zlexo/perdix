#pragma once

#include <vx/core/wake_event.hpp>

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace vx {

// Messages to one thread from any others (RFC 0005): a lock and two arrays
// swapped on drain, so a producer never waits for the owner to finish with
// what it took. Messages from one producer arrive in the order it pushed
// them, with no order across producers, and what a producer wrote before a
// push is visible to the owner once the drain that delivers it starts. A push
// wakes the owner only when the inbox was empty, so a busy owner drains many
// messages a wake-up, and after each wait the owner drains every inbox that
// wakes it: a push into one that still holds messages wakes nobody.
template <class T> class Inbox {
public:
    // Room for `capacity` messages before an array grows, once, when a burst
    // outruns it, rather than dropping or blocking. A job's slot is free as
    // soon as it returns, before its result is drained, so the results inbox
    // reserves twice the job slots: a long tick can leave more results than
    // slots.
    Inbox(std::size_t capacity, WakeEvent& owner) : owner_(&owner) {
        incoming_.reserve(capacity);
        draining_.reserve(capacity);
    }

    // Any thread. Waits for the lock, never for the owner. The inbox and its
    // event outlive the call, which signals after letting go of the lock.
    void push(T&& message) {
        bool was_empty = false;
        {
            const std::scoped_lock guard(lock_);
            was_empty = incoming_.empty();
            incoming_.push_back(std::move(message));
        }
        if (was_empty) {
            owner_->signal();
        }
    }

    // The owner only: calls `handle(T&&)` for every message pushed before it
    // took them.
    template <class F> void drain(F&& handle) {
        {
            const std::scoped_lock guard(lock_);
            std::swap(incoming_, draining_);
        }
        for (T& message : draining_) {
            handle(std::move(message));
        }
        draining_.clear();
    }

    // Messages waiting: for plots and tests.
    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock guard(lock_);
        return incoming_.size();
    }
    // Messages the incoming array holds before it grows.
    [[nodiscard]] std::size_t capacity() const {
        const std::scoped_lock guard(lock_);
        return incoming_.capacity();
    }

private:
    mutable std::mutex lock_;
    std::vector<T> incoming_;
    std::vector<T> draining_; // the owner's, outside drain too
    WakeEvent* owner_;
};

} // namespace vx
