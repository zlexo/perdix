#include <vx/core/version.hpp>

#include "version_config.hpp"

namespace vx {

Version version() noexcept {
    return {config::version_major, config::version_minor, config::version_patch};
}

std::string_view version_string() noexcept {
    return config::version_string;
}

} // namespace vx
