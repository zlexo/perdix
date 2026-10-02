#include "device_fixtures.hpp"

#include <vx/render/device_report.hpp>

#include <doctest/doctest.h>
#include <vulkan/vulkan_core.h>

#include <string>

using vx::test::full_floor;
using vx::test::m3_on_kosmickrisp;
using vx::test::m3_on_moltenvk;

TEST_CASE("the report names the device, its driver and what it does without") {
    const std::string report = vx::render::format_device_report(m3_on_kosmickrisp(), false);

    CHECK(report == "Device: Apple M3 (integrated GPU)\n"
                    "Driver: KosmicKrisp vulkan-sdk-1.4.363 (git-269a955c89), Vulkan 1.4.363\n"
                    "Portability fallback: not needed\n"
                    "Not offered: fillModeNonSolid, so there is no wireframe toggle\n");
}

TEST_CASE("the report says when the portability fallback was taken") {
    const std::string report = vx::render::format_device_report(m3_on_moltenvk(), true);

    CHECK(report ==
          "Device: Apple M3 (integrated GPU)\n"
          "Driver: MoltenVK 1.4.2, Vulkan 1.3.357\n"
          "Portability fallback: taken, no device on a conformant driver passed the floor\n");
}

TEST_CASE("a software driver without optional features reports both") {
    const vx::render::DeviceInfo lavapipe = {
        .name = "llvmpipe (LLVM 18.1.3, 256 bits)",
        .driver_name = "llvmpipe",
        .driver_info = "Mesa 24.0.9 (LLVM 18.1.3)",
        .type = VK_PHYSICAL_DEVICE_TYPE_CPU,
        .features = full_floor(VK_MAKE_API_VERSION(0, 1, 3, 274)),
        .optional = {.sampler_anisotropy = false, .fill_mode_non_solid = false},
        .portability_subset = false,
        .queue_family = 0U,
    };

    const std::string report = vx::render::format_device_report(lavapipe, false);

    CHECK(report == "Device: llvmpipe (LLVM 18.1.3, 256 bits) (CPU)\n"
                    "Driver: llvmpipe Mesa 24.0.9 (LLVM 18.1.3), Vulkan 1.3.274\n"
                    "Portability fallback: not needed\n"
                    "Not offered: samplerAnisotropy, so filtering stays trilinear\n"
                    "Not offered: fillModeNonSolid, so there is no wireframe toggle\n");
}

TEST_CASE("an empty driver info leaves no stray space") {
    vx::render::DeviceInfo device = m3_on_moltenvk();
    device.driver_info.clear();

    const std::string report = vx::render::format_device_report(device, true);

    CHECK(report.find("Driver: MoltenVK, Vulkan 1.3.357\n") != std::string::npos);
}
