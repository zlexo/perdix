// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest and
// skipped by the Linux presets until RFC 0001's lavapipe job (pull request
// plan item 6). On macOS, source the Vulkan SDK's setup-env.sh first.

#include <vx/render/context.hpp>

#include <doctest/doctest.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.extensions = {}, .validation = true});
}

} // namespace

TEST_CASE("a context runs on a conformant driver when one passes the floor") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Context& context = created.value();

    CHECK_FALSE(context.portability_fallback());
    CHECK_FALSE(context.device_info().portability_subset);
    CHECK(context.device().handle() != VK_NULL_HANDLE);
    CHECK(context.device().queue() != VK_NULL_HANDLE);
    CHECK(context.instance().validation_counts().errors == 0);
}

#ifdef __APPLE__

namespace {

// Sets an environment variable for one scope, then puts back what was there.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (old_.has_value()) {
            setenv(name_, old_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("with only MoltenVK installed, the context falls back to portability drivers") {
    // VK_DRIVER_FILES hides every other driver from the loader: the situation
    // on Intel Macs, and RFC 0001's way to rerun on MoltenVK.
    const char* sdk = std::getenv("VULKAN_SDK");
    REQUIRE_MESSAGE(sdk != nullptr, "source the Vulkan SDK's setup-env.sh first");
    const ScopedEnv drivers("VK_DRIVER_FILES",
                            std::string(sdk) + "/share/vulkan/icd.d/MoltenVK_icd.json");

    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Context& context = created.value();

    CHECK(context.portability_fallback());
    CHECK(context.device_info().driver_name == "MoltenVK");
    CHECK(context.device_info().portability_subset);
    CHECK(context.instance().validation_counts().errors == 0);
}

#endif
