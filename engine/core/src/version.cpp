#include <vx/core/version.hpp>

#include "version_config.hpp"

namespace vx {

EngineVersion version() noexcept {
    return {
        .major = config::version_major,
        .minor = config::version_minor,
        .patch = config::version_patch,
    };
}

std::string_view version_string() noexcept {
    return config::version_string;
}

} // namespace vx
