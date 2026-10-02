#include "device_fixtures.hpp"

#include <vx/render/feature_floor.hpp>

#include <doctest/doctest.h>
#include <vulkan/vulkan_core.h>

#include <string_view>
#include <vector>

using vx::test::full_floor;

TEST_CASE("a device with every floor feature passes with nothing missing") {
    const vx::render::FeatureReport report = vx::render::check_feature_floor(full_floor());

    CHECK(report.supported);
    CHECK(report.missing.empty());
}

TEST_CASE("each missing floor feature is named") {
    struct Case {
        bool vx::render::DeviceFeatures::* feature;
        std::string_view name;
    };
    const Case cases[] = {
        {&vx::render::DeviceFeatures::dynamic_rendering, "dynamicRendering"},
        {&vx::render::DeviceFeatures::synchronization2, "synchronization2"},
        {&vx::render::DeviceFeatures::timeline_semaphore, "timelineSemaphore"},
        {&vx::render::DeviceFeatures::buffer_device_address, "bufferDeviceAddress"},
        {&vx::render::DeviceFeatures::descriptor_indexing, "descriptorIndexing"},
        {&vx::render::DeviceFeatures::runtime_descriptor_array, "runtimeDescriptorArray"},
        {&vx::render::DeviceFeatures::descriptor_binding_partially_bound,
         "descriptorBindingPartiallyBound"},
        {&vx::render::DeviceFeatures::multi_draw_indirect, "multiDrawIndirect"},
        {&vx::render::DeviceFeatures::draw_indirect_first_instance, "drawIndirectFirstInstance"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        vx::render::DeviceFeatures features = full_floor();
        features.*c.feature = false;

        const vx::render::FeatureReport report = vx::render::check_feature_floor(features);

        CHECK_FALSE(report.supported);
        REQUIRE(report.missing.size() == 1);
        CHECK(report.missing[0] == c.name);
    }
}

TEST_CASE("a Vulkan 1.2 device misses the version") {
    vx::render::DeviceFeatures features = full_floor();
    features.api_version = VK_API_VERSION_1_2;

    const vx::render::FeatureReport report = vx::render::check_feature_floor(features);

    CHECK_FALSE(report.supported);
    REQUIRE(report.missing.size() == 1);
    CHECK(report.missing[0] == "Vulkan 1.3");
}

TEST_CASE("an empty report lists the whole floor, in order") {
    const vx::render::FeatureReport report = vx::render::check_feature_floor({});

    const std::vector<std::string_view> expected = {"Vulkan 1.3",
                                                    "dynamicRendering",
                                                    "synchronization2",
                                                    "timelineSemaphore",
                                                    "bufferDeviceAddress",
                                                    "descriptorIndexing",
                                                    "runtimeDescriptorArray",
                                                    "descriptorBindingPartiallyBound",
                                                    "multiDrawIndirect",
                                                    "drawIndirectFirstInstance"};
    CHECK_FALSE(report.supported);
    CHECK(report.missing == expected);
}
