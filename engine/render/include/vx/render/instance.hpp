#pragma once

#include <vx/core/result.hpp>

#include <volk.h>

#include <cstdint>
#include <memory>
#include <span>

namespace vx::render {

struct InstanceDesc {
    // The window library's surface extensions; empty when running headless.
    std::span<const char* const> extensions;
    // VK_LAYER_KHRONOS_validation with synchronization validation, and a debug
    // messenger that prints each message with its id and counts it.
    bool validation = false;
    // Also list portability drivers such as MoltenVK: RFC 0001's fallback when no
    // conformant driver passes the feature floor.
    bool portability = false;
};

// How many validation messages of each kind have arrived.
struct ValidationCounts {
    std::uint32_t warnings = 0;
    std::uint32_t errors = 0;
};

// Owns a VkInstance and, with validation on, its debug messenger. Creating the
// first instance also loads Vulkan through volk.
class Instance {
public:
    [[nodiscard]] static Result<Instance> create(const InstanceDesc& desc);

    Instance(Instance&& other) noexcept;
    Instance& operator=(Instance&& other) noexcept;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    ~Instance();

    [[nodiscard]] VkInstance handle() const noexcept { return instance_; }
    // The loader's instance version, from vkEnumerateInstanceVersion.
    [[nodiscard]] std::uint32_t loader_version() const noexcept { return loader_version_; }
    [[nodiscard]] bool lists_portability_drivers() const noexcept { return portability_; }
    [[nodiscard]] ValidationCounts validation_counts() const noexcept;

private:
    Instance() = default;
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::unique_ptr<ValidationCounts> counts_; // a stable address for the messenger's callback
    std::uint32_t loader_version_ = 0;
    bool portability_ = false;
};

} // namespace vx::render
