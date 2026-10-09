#pragma once

#include <cstddef>
#include <cstdint>

namespace vx {

// The cache line the reference machine reports (RFC 0005): 128 bytes on
// Apple silicon. std::hardware_destructive_interference_size says 64 under
// Apple Clang 17, changes with the compiler and its tuning, and draws a GCC
// warning in headers.
inline constexpr std::size_t cache_line = 128;

// A thread's quality of service on macOS (RFC 0005): the class the scheduler
// reads to decide how soon the thread runs and how strongly it prefers the
// performance cores. Other systems have no such thing.
enum class ThreadQos : std::uint8_t { user_interactive, user_initiated, utility, background };

// Names the calling thread, as debuggers and crash reports show it. Linux
// keeps the first 15 characters. Nothing outside macOS and Linux.
void set_thread_name(const char* name) noexcept;

// Puts the calling thread in `qos`. Nothing outside macOS.
void set_thread_qos(ThreadQos qos) noexcept;

} // namespace vx
