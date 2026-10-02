#include <vx/core/version.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>

#include <iostream>

namespace {

// RFC 0001: validation in debug builds.
#ifdef NDEBUG
constexpr bool validation = false;
#else
constexpr bool validation = true;
#endif

} // namespace

int main() {
    std::cout << "Perdix " << vx::version_string() << '\n';

    vx::Result<vx::render::Context> context =
        vx::render::Context::create({.extensions = {}, .validation = validation});
    if (!context.has_value()) {
        std::cerr << "error: " << context.error().message << '\n';
        return 1;
    }
    std::cout << vx::render::format_device_report(context.value().device_info(),
                                                  context.value().portability_fallback());
    return 0;
}
