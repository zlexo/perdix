#include <vx/core/diagnostic.hpp>

#include <doctest/doctest.h>

#include <vector>

TEST_CASE("a diagnostic reads the way a compiler's does, with what it knows of the place") {
    const vx::Diagnostic located{.severity = vx::Severity::error,
                                 .file = "assets/core/blocks/oak_log.toml",
                                 .line = 12,
                                 .column = 12,
                                 .message = "rotate_x must be 0, 90, 180 or 270, not 45"};
    CHECK(
        vx::format(located) ==
        "assets/core/blocks/oak_log.toml:12:12: error: rotate_x must be 0, 90, 180 or 270, not 45");

    const vx::Diagnostic line_only{.severity = vx::Severity::warning,
                                   .file = "a.toml",
                                   .line = 3,
                                   .column = 0,
                                   .message = "m"};
    CHECK(vx::format(line_only) == "a.toml:3: warning: m");

    const vx::Diagnostic whole_file{.severity = vx::Severity::error,
                                    .file = "assets/core/textures/blocks/stone.png",
                                    .line = 0,
                                    .column = 0,
                                    .message = "not 16 by 16 pixels"};
    CHECK(vx::format(whole_file) ==
          "assets/core/textures/blocks/stone.png: error: not 16 by 16 pixels");
}

TEST_CASE("loading stops on an error, not on warnings") {
    const vx::Diagnostic warning{.severity = vx::Severity::warning, .file = "a", .message = "w"};
    const vx::Diagnostic error{.severity = vx::Severity::error, .file = "b", .message = "e"};
    CHECK_FALSE(vx::has_errors({}));
    CHECK_FALSE(vx::has_errors(std::vector{warning, warning}));
    CHECK(vx::has_errors(std::vector{warning, error}));
}
