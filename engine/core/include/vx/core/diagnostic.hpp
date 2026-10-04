#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace vx {

enum class Severity : std::uint8_t { warning, error };

// A problem in a file the engine loads, such as a block definition
// (RFC 0004), placed the way compilers place theirs, so that terminals and
// editors link to it. Loading collects every one before it stops.
struct Diagnostic {
    Severity severity = Severity::error;
    std::string file;
    std::uint32_t line = 0;   // 1-based; 0 when the problem is the whole file
    std::uint32_t column = 0; // 1-based; 0 when it is the whole line
    std::string message;
};

// "file:line:column: error: message", leaving out a line or column of 0.
[[nodiscard]] std::string format(const Diagnostic& diagnostic);
// Whether any is an error: one error stops loading.
[[nodiscard]] bool has_errors(std::span<const Diagnostic> diagnostics) noexcept;

} // namespace vx
