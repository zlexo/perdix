#include <vx/core/thread.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <string>
#include <thread>

// MSVC has no pthread.h.
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

// RFC 0005's thread helpers: names for debuggers and crash reports, and
// quality of service on macOS.

namespace {

#if defined(__APPLE__) || defined(__linux__)
// The name a new thread reads back after naming itself `name`.
std::string named(const char* name) {
    std::array<char, 64> read{};
    std::thread thread([&] {
        vx::set_thread_name(name);
        pthread_getname_np(pthread_self(), read.data(), read.size());
    });
    thread.join();
    return read.data();
}
#endif

} // namespace

TEST_CASE("a thread names itself, and a long name keeps what the system keeps") {
#ifdef __APPLE__
    CHECK(named("Worker 3") == "Worker 3");
    CHECK(named("A name of twenty-eight bytes") == "A name of twenty-eight bytes");
#elif defined(__linux__)
    CHECK(named("Worker 3") == "Worker 3");
    CHECK(named("A name of twenty-eight bytes") == "A name of twent");
#else
    vx::set_thread_name("Worker 3");
    MESSAGE("skipped: names are read back on macOS and Linux");
#endif
}

TEST_CASE("a thread sets its own quality of service on macOS") {
#ifdef __APPLE__
    struct Case {
        vx::ThreadQos qos;
        qos_class_t expected;
    };
    for (const Case& setting : {Case{vx::ThreadQos::user_interactive, QOS_CLASS_USER_INTERACTIVE},
                                Case{vx::ThreadQos::user_initiated, QOS_CLASS_USER_INITIATED},
                                Case{vx::ThreadQos::utility, QOS_CLASS_UTILITY},
                                Case{vx::ThreadQos::background, QOS_CLASS_BACKGROUND}}) {
        qos_class_t read = QOS_CLASS_UNSPECIFIED;
        std::thread thread([&] {
            vx::set_thread_qos(setting.qos);
            int relative = 0;
            pthread_get_qos_class_np(pthread_self(), &read, &relative);
        });
        thread.join();
        CHECK(read == setting.expected);
    }
#else
    vx::set_thread_qos(vx::ThreadQos::utility);
    MESSAGE("skipped: quality of service is macOS's");
#endif
}

TEST_CASE("a cache line is the reference machine's 128 bytes") {
    static_assert(vx::cache_line == 128);
    CHECK(alignof(std::max_align_t) <= vx::cache_line);
}
