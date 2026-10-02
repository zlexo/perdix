#include <vx/render/swapchain_choice.hpp>

#include <doctest/doctest.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace {

VkSurfaceFormatKHR srgb(VkFormat format) {
    return {.format = format, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}

// A surface that leaves the size to the swapchain, with room for every case.
vx::render::SurfaceSupport surface() {
    vx::render::SurfaceSupport support;
    support.capabilities.minImageCount = 2;
    support.capabilities.maxImageCount = 0;
    support.capabilities.currentExtent = {.width = 0xFFFFFFFF, .height = 0xFFFFFFFF};
    support.capabilities.minImageExtent = {.width = 1, .height = 1};
    support.capabilities.maxImageExtent = {.width = 16384, .height = 16384};
    support.capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    support.formats = {srgb(VK_FORMAT_B8G8R8A8_SRGB)};
    support.present_modes = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    return support;
}

constexpr vx::render::Extent framebuffer = {.width = 2560, .height = 1600};

vx::render::SwapchainChoice choose(const vx::render::SurfaceSupport& support,
                                   vx::render::PresentMode wanted = vx::render::PresentMode::fifo) {
    vx::Result<vx::render::SwapchainChoice> choice =
        vx::render::choose_swapchain(support, framebuffer, wanted);
    REQUIRE(choice.has_value());
    return choice.value();
}

} // namespace

TEST_CASE("B8G8R8A8_SRGB wins even after another sRGB format") {
    vx::render::SurfaceSupport support = surface();
    support.formats = {srgb(VK_FORMAT_R8G8B8A8_SRGB), srgb(VK_FORMAT_B8G8R8A8_SRGB)};

    CHECK(choose(support).format.format == VK_FORMAT_B8G8R8A8_SRGB);
}

TEST_CASE("without B8G8R8A8_SRGB, the first sRGB format offered") {
    vx::render::SurfaceSupport support = surface();
    support.formats = {srgb(VK_FORMAT_B8G8R8A8_UNORM), srgb(VK_FORMAT_R8G8B8A8_SRGB),
                       srgb(VK_FORMAT_A8B8G8R8_SRGB_PACK32)};

    CHECK(choose(support).format.format == VK_FORMAT_R8G8B8A8_SRGB);
}

TEST_CASE("an sRGB format in another color space does not count") {
    vx::render::SurfaceSupport support = surface();
    support.formats = {
        {.format = VK_FORMAT_B8G8R8A8_SRGB, .colorSpace = VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT},
        srgb(VK_FORMAT_R8G8B8A8_SRGB)};

    const vx::render::SwapchainChoice choice = choose(support);

    CHECK(choice.format.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(choice.format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

TEST_CASE("a surface without an sRGB format is an error") {
    vx::render::SurfaceSupport support = surface();
    support.formats = {srgb(VK_FORMAT_B8G8R8A8_UNORM)};

    const vx::Result<vx::render::SwapchainChoice> choice =
        vx::render::choose_swapchain(support, framebuffer, vx::render::PresentMode::fifo);

    REQUIRE_FALSE(choice.has_value());
    CHECK(choice.error().message == "the window's surface offers no sRGB format");
}

TEST_CASE("FIFO unless immediate is both wanted and offered") {
    vx::render::SurfaceSupport support = surface();
    CHECK(choose(support).present_mode == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(choose(support, vx::render::PresentMode::immediate).present_mode ==
          VK_PRESENT_MODE_IMMEDIATE_KHR);

    support.present_modes = {VK_PRESENT_MODE_FIFO_KHR};
    CHECK(choose(support, vx::render::PresentMode::immediate).present_mode ==
          VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("one image more than the minimum, within the maximum") {
    vx::render::SurfaceSupport support = surface();
    CHECK(choose(support).image_count == 3); // a maximum of 0 means no limit

    support.capabilities.maxImageCount = 3;
    CHECK(choose(support).image_count == 3);

    support.capabilities.minImageCount = 3;
    CHECK(choose(support).image_count == 3);
}

TEST_CASE("the surface's own size wins, or else the framebuffer's within limits") {
    vx::render::SurfaceSupport support = surface();
    const VkExtent2D sized = choose(support).extent;
    CHECK(sized.width == framebuffer.width);
    CHECK(sized.height == framebuffer.height);

    support.capabilities.maxImageExtent = {.width = 2048, .height = 2048};
    support.capabilities.minImageExtent = {.width = 1, .height = 1800};
    const VkExtent2D clamped = choose(support).extent;
    CHECK(clamped.width == 2048);
    CHECK(clamped.height == 1800);

    support.capabilities.currentExtent = {.width = 1280, .height = 800};
    const VkExtent2D fixed = choose(support).extent;
    CHECK(fixed.width == 1280);
    CHECK(fixed.height == 800);
}

TEST_CASE("opaque where the surface allows it, else the lowest alpha mode it supports") {
    vx::render::SurfaceSupport support = surface();
    support.capabilities.supportedCompositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(choose(support).composite_alpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);

    support.capabilities.supportedCompositeAlpha =
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(choose(support).composite_alpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR);
}

TEST_CASE("the image keeps the surface's current transform") {
    vx::render::SurfaceSupport support = surface();
    support.capabilities.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;

    CHECK(choose(support).transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);
}

TEST_CASE("the report gives the images, size, format and present mode") {
    vx::render::SwapchainChoice choice;
    choice.format = srgb(VK_FORMAT_B8G8R8A8_SRGB);
    choice.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    choice.image_count = 3;
    choice.extent = {.width = 2560, .height = 1600};

    CHECK(vx::render::format_swapchain_report(choice) ==
          "Swapchain: 3 images of 2560x1600, B8G8R8A8_SRGB, FIFO\n");

    choice.present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    CHECK(vx::render::format_swapchain_report(choice) ==
          "Swapchain: 3 images of 2560x1600, B8G8R8A8_SRGB, immediate\n");
}
