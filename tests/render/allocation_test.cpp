// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// An executable of its own, since it replaces the global operator new to count
// heap allocations, which would count every other test's too.

#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>

#include <dlfcn.h>
#include <doctest/doctest.h>
#include <execinfo.h>
#include <glm/ext/vector_int3_sized.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>

namespace {

// Allocations since the last reset, by who made them. The replaced operator
// new sees every library in the process, Vulkan's drivers included: Apple's
// Metal driver, under KosmicKrisp, allocates a few times a frame. Those are
// the driver's, not the frame's budget, so they are counted apart.
std::atomic<std::size_t> ours{0};
std::atomic<std::size_t> drivers{0};
thread_local bool inside = false; // walking the stack must not count itself

// Whether the allocation under way comes from this executable's code: the
// first frame up the stack that is neither this walk, operator new, nor the
// C++ runtime working for its caller decides.
[[gnu::noinline]] bool made_here() {
    std::array<void*, 32> frames{};
    const int depth = backtrace(frames.data(), static_cast<int>(frames.size()));
    Dl_info self{};
    dladdr(&ours, &self);
    for (int i = 2; i < depth; ++i) { // 0 is this function, 1 is operator new
        Dl_info info{};
        if (dladdr(frames.at(static_cast<std::size_t>(i)), &info) == 0 ||
            info.dli_fname == nullptr) {
            continue;
        }
        if (std::strstr(info.dli_fname, "libc++") != nullptr ||
            std::strstr(info.dli_fname, "libstdc++") != nullptr) {
            continue;
        }
        return info.dli_fbase == self.dli_fbase;
    }
    return false;
}

} // namespace

void* operator new(std::size_t size) {
    if (!inside) {
        inside = true;
        (made_here() ? ours : drivers).fetch_add(1, std::memory_order_relaxed);
        inside = false;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

TEST_CASE("after its first frames, a frame allocates nothing on the heap") {
    // The validation layers are C++ and allocate as they check every call, so
    // the renderer runs without them.
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = {.width = 64, .height = 32},
                                      .validation = false,
                                      .present_mode = vx::render::PresentMode::fifo});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    vx::render::Camera camera;
    camera.position = {0.5, 0.5, 3.0};
    const std::array cubes = {glm::i64vec3(0, 0, 0), glm::i64vec3(2, 0, -2)};
    const vx::render::FrameInputs inputs = {
        .sky = {.r = 0.2F, .g = 0.4F, .b = 1.0F, .a = 1.0F}, .camera = camera, .cubes = cubes};

    // RFC 0001's budget: no heap allocation in a frame once the first frames,
    // one for each slot and one more, are done. Nothing is checked until the
    // counts are taken, so the test itself allocates nothing in between.
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(renderer.render(inputs).has_value());
    }
    ours = 0;
    drivers = 0;
    bool drawn = true;
    for (int frame = 0; frame < 60 && drawn; ++frame) {
        drawn = renderer.render(inputs).has_value();
    }
    const std::size_t counted = ours;
    const std::size_t by_drivers = drivers;

    MESSAGE("60 frames: " << counted << " allocations by Perdix, " << by_drivers
                          << " by the driver");
    CHECK(drawn);
    CHECK(counted == 0);
}
