#include "device_fixtures.hpp"

#include <vx/render/device_selection.hpp>

#include <doctest/doctest.h>
#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <string>
#include <vector>

using vx::test::full_floor;
using vx::test::m3_on_kosmickrisp;
using vx::test::m3_on_moltenvk;

namespace {

vx::render::DeviceInfo discrete_gpu() {
    vx::render::DeviceInfo device = m3_on_kosmickrisp();
    device.name = "Discrete GPU";
    device.type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    return device;
}

} // namespace

TEST_CASE("for one GPU on two drivers, the conformant driver wins") {
    // Once an instance opts in to portability drivers, the loader lists MoltenVK first.
    const std::vector devices = {m3_on_moltenvk(), m3_on_kosmickrisp()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 1);
}

TEST_CASE("a discrete GPU beats an integrated one") {
    const std::vector devices = {m3_on_kosmickrisp(), discrete_gpu()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 1);
}

TEST_CASE("a conformant driver outranks a discrete GPU") {
    vx::render::DeviceInfo portable = discrete_gpu();
    portable.portability_subset = true;
    const std::vector devices = {portable, m3_on_kosmickrisp()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 1);
}

TEST_CASE("equal devices keep the loader's order") {
    const std::vector devices = {m3_on_kosmickrisp(), m3_on_kosmickrisp()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 0);
}

TEST_CASE("a device below the floor is never chosen, however it ranks") {
    vx::render::DeviceInfo fast = discrete_gpu();
    fast.features.multi_draw_indirect = false;
    const std::vector devices = {fast, m3_on_kosmickrisp()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 1);
}

TEST_CASE("a device without a graphics and compute queue is never chosen") {
    vx::render::DeviceInfo no_queue = m3_on_kosmickrisp();
    no_queue.queue_family.reset();
    const std::vector devices = {no_queue, m3_on_moltenvk()};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE(chosen.has_value());
    CHECK(chosen.value() == 1);
}

TEST_CASE("when no device qualifies, the error names each one and what it lacks") {
    vx::render::DeviceInfo old_gpu = discrete_gpu();
    old_gpu.name = "Old GPU";
    old_gpu.driver_name = "OldDriver";
    old_gpu.features = full_floor(VK_API_VERSION_1_2);
    old_gpu.features.synchronization2 = false;
    vx::render::DeviceInfo no_queue = m3_on_kosmickrisp();
    no_queue.queue_family.reset();
    no_queue.features.multi_draw_indirect = false;
    const std::vector devices = {old_gpu, no_queue};

    const vx::Result<std::size_t> chosen = vx::render::choose_device(devices);

    REQUIRE_FALSE(chosen.has_value());
    CHECK(chosen.error().message ==
          "no Vulkan device meets the feature floor:\n"
          "  Old GPU (OldDriver) lacks Vulkan 1.3, synchronization2\n"
          "  Apple M3 (KosmicKrisp) lacks multiDrawIndirect, a queue with graphics and compute");
}

TEST_CASE("with no device at all, the error says so") {
    const vx::Result<std::size_t> chosen = vx::render::choose_device({});

    REQUIRE_FALSE(chosen.has_value());
    CHECK(chosen.error().message == "no Vulkan device found");
}
