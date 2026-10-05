#include <vx/core/diagnostic.hpp>

#include <algorithm>
#include <span>
#include <string>

namespace vx {

std::string format(const Diagnostic& diagnostic) {
    std::string text = diagnostic.file;
    if (diagnostic.line != 0) {
        text += ':' + std::to_string(diagnostic.line);
        if (diagnostic.column != 0) {
            text += ':' + std::to_string(diagnostic.column);
        }
    }
    text += diagnostic.severity == Severity::error ? ": error: " : ": warning: ";
    text += diagnostic.message;
    return text;
}

bool has_errors(std::span<const Diagnostic> diagnostics) noexcept {
    return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
}

} // namespace vx
