#include <vx/core/thread.hpp>

#include <algorithm>
#include <array>
#include <string_view>

// MSVC has no pthread.h.
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace vx {

void set_thread_name(const char* name) noexcept {
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__linux__)
    // Linux refuses a name of more than 15 characters outright.
    std::array<char, 16> kept{};
    std::ranges::copy(std::string_view(name).substr(0, kept.size() - 1), kept.begin());
    pthread_setname_np(pthread_self(), kept.data());
#else
    static_cast<void>(name);
#endif
}

void set_thread_qos(ThreadQos qos) noexcept {
#ifdef __APPLE__
    qos_class_t qos_class = QOS_CLASS_UTILITY;
    switch (qos) {
    case ThreadQos::user_interactive:
        qos_class = QOS_CLASS_USER_INTERACTIVE;
        break;
    case ThreadQos::user_initiated:
        qos_class = QOS_CLASS_USER_INITIATED;
        break;
    case ThreadQos::utility:
        qos_class = QOS_CLASS_UTILITY;
        break;
    case ThreadQos::background:
        qos_class = QOS_CLASS_BACKGROUND;
        break;
    }
    pthread_set_qos_class_self_np(qos_class, 0);
#else
    static_cast<void>(qos);
#endif
}

} // namespace vx
