#include <vx/core/inbox.hpp>
#include <vx/core/wake_event.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// RFC 0005's wake events and inboxes: a signal is never lost, each
// producer's messages keep their order, and the owner is woken only when
// there is something new.

namespace {

using std::chrono::steady_clock;

// A deadline long enough that reaching it means the wake-up was lost.
steady_clock::time_point patience() {
    return steady_clock::now() + std::chrono::seconds(10);
}

} // namespace

TEST_CASE("a wake event keeps a signal sent while nobody waits, once") {
    vx::WakeEvent event;
    event.signal();
    event.signal();
    CHECK(event.wait_until(steady_clock::now()));
    // Cleared by the wait it ended: the second signal did not latch twice.
    CHECK_FALSE(event.wait_until(steady_clock::now() + std::chrono::milliseconds(1)));
    CHECK(event.signals() == 2);
}

TEST_CASE("a wake event wakes a waiter from another thread") {
    vx::WakeEvent event;
    std::thread signaler([&event] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        event.signal();
    });
    const steady_clock::time_point deadline = patience();
    CHECK(event.wait_until(deadline));
    // Woken by the signal: a wait that slept to its deadline and only then
    // found the flag set returns true as well.
    CHECK(steady_clock::now() < deadline);
    signaler.join();
}

TEST_CASE("a push between a drain and the owner's next wait is not lost") {
    vx::WakeEvent event;
    vx::Inbox<int> inbox(8, event);
    std::vector<int> got;
    inbox.drain([&got](int&& message) { got.push_back(message); });
    // A producer pushes after the drain, before the owner waits again.
    inbox.push(7);
    CHECK(event.wait_until(steady_clock::now()));
    inbox.drain([&got](int&& message) { got.push_back(message); });
    CHECK(got == std::vector<int>{7});
}

TEST_CASE("a push wakes the owner only when the inbox was empty") {
    vx::WakeEvent event;
    vx::Inbox<int> inbox(8, event);
    inbox.push(1);
    inbox.push(2);
    inbox.push(3);
    CHECK(event.signals() == 1);
    CHECK(inbox.size() == 3);
    std::vector<int> got;
    inbox.drain([&got](int&& message) { got.push_back(message); });
    CHECK(got == std::vector<int>{1, 2, 3});
    CHECK(inbox.size() == 0);
    inbox.push(4);
    CHECK(event.signals() == 2);
}

TEST_CASE("an inbox reserved for a burst never grows to hold it") {
    // The results inbox reserves one message for each job slot. 1,000, not a
    // power of two: an array that grew while the burst arrived, doubling from
    // nothing, would end at 1,024, not on the reservation.
    vx::WakeEvent event;
    vx::Inbox<std::uint64_t> inbox(1'000, event);
    const std::size_t reserved = inbox.capacity();
    REQUIRE(reserved >= 1'000);
    // Three rounds, so each of the two arrays takes a burst.
    for (int round = 0; round < 3; ++round) {
        for (std::uint64_t i = 0; i < 1'000; ++i) {
            inbox.push(std::uint64_t{i});
        }
        CHECK(inbox.capacity() == reserved);
        std::size_t drained = 0;
        inbox.drain([&drained](std::uint64_t&& /*message*/) { ++drained; });
        CHECK(drained == 1'000);
    }
}

namespace {

struct Message {
    int producer = 0;
    int sequence = 0;
};

} // namespace

TEST_CASE("messages from each of four producers arrive in the order each pushed them") {
    constexpr int producers = 4;
    constexpr int each = 20'000;
    vx::WakeEvent event;
    vx::Inbox<Message> inbox(256, event);
    std::vector<std::thread> threads;
    for (int producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&inbox, producer] {
            for (int sequence = 0; sequence < each; ++sequence) {
                inbox.push(Message{.producer = producer, .sequence = sequence});
            }
        });
    }
    std::array<int, producers> next{};
    int received = 0;
    int out_of_order = 0;
    while (received < producers * each) {
        // A wait that reaches its deadline lost a wake-up, even when it then
        // finds the flag set: the counts below say so.
        const steady_clock::time_point deadline = patience();
        if (!event.wait_until(deadline) || steady_clock::now() >= deadline) {
            break;
        }
        inbox.drain([&](Message&& message) {
            const auto at = static_cast<std::size_t>(message.producer);
            out_of_order += message.sequence == next[at] ? 0 : 1;
            next[at] = message.sequence + 1;
            ++received;
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(received == producers * each);
    CHECK(out_of_order == 0);
}
