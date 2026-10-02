#include <vx/render/instance.hpp>

#include "vk_result.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace vx::render {
namespace {

constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

// volk finds the loader by name. On macOS the SDK's loader is findable by name
// only through DYLD_LIBRARY_PATH, which the system strips from programs started
// through protected binaries, so the SDK path seen at configure time is tried next.
bool load_vulkan() {
    if (volkInitialize() == VK_SUCCESS) {
        return true;
    }
#if defined(__APPLE__) && defined(VX_VULKAN_LOADER_FALLBACK)
    // Like volk's own, the loader stays open for the life of the process.
    if (void* library = dlopen(VX_VULKAN_LOADER_FALLBACK, RTLD_NOW | RTLD_LOCAL)) {
        // POSIX guarantees that dlsym's result converts to a function pointer.
        const auto get_instance_proc_addr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        if (get_instance_proc_addr != nullptr) {
            volkInitializeCustom(get_instance_proc_addr);
            return true;
        }
    }
#endif
    return false;
}

bool layer_available(std::string_view name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(
        layers, [name](const VkLayerProperties& layer) { return name == layer.layerName; });
}

bool layer_has_extension(const char* layer, std::string_view extension) {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data());
    return std::ranges::any_of(extensions, [extension](const VkExtensionProperties& e) {
        return extension == e.extensionName;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* counts = static_cast<ValidationCounts*>(user);
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (error) {
        ++counts->errors;
    } else {
        ++counts->warnings;
    }
    std::fprintf(stderr, "[vulkan %s] %s: %s\n", error ? "error" : "warning",
                 data->pMessageIdName != nullptr ? data->pMessageIdName : "-", data->pMessage);
    return VK_FALSE;
}

constexpr VkDebugUtilsMessageTypeFlagsEXT validation_types =
    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

VkDebugUtilsMessengerCreateInfoEXT messenger_info(ValidationCounts* counts,
                                                  VkDebugUtilsMessageTypeFlagsEXT types) {
    return {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = types,
        .pfnUserCallback = on_validation_message,
        .pUserData = counts,
    };
}

} // namespace

Result<Instance> Instance::create(const InstanceDesc& desc) {
    if (!load_vulkan()) {
        return Error{"no Vulkan loader found: install the Vulkan SDK (RFC 0001) or a Vulkan "
                     "driver, and on macOS source the SDK's setup-env.sh"};
    }

    std::uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        vkEnumerateInstanceVersion(&loader_version);
    }
    if (loader_version < VK_API_VERSION_1_3) {
        return Error{"the Vulkan loader is older than 1.3: update the Vulkan SDK or driver"};
    }

    std::vector<const char*> extensions(desc.extensions.begin(), desc.extensions.end());
    std::vector<const char*> layers;
    VkInstanceCreateFlags flags = 0;
    if (desc.portability) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    Instance instance;
    instance.counts_ = std::make_unique<ValidationCounts>();
    instance.loader_version_ = loader_version;
    instance.portability_ = desc.portability;

    // The messenger chained into vkCreateInstance and vkDestroyInstance leaves
    // out the loader's general messages. With only portability drivers
    // installed, the loader reports RFC 0001's first, conformant attempt at
    // error severity, though failing there is how the fallback starts; the
    // device selection names what it found instead.
    const VkDebugUtilsMessengerCreateInfoEXT creation_messenger =
        messenger_info(instance.counts_.get(), validation_types);
    const VkDebugUtilsMessengerCreateInfoEXT messenger = messenger_info(
        instance.counts_.get(), validation_types | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT);
    const VkValidationFeatureEnableEXT synchronization_validation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext = &creation_messenger,
        .enabledValidationFeatureCount = 1,
        .pEnabledValidationFeatures = &synchronization_validation,
        .disabledValidationFeatureCount = 0,
        .pDisabledValidationFeatures = nullptr,
    };
    const void* chain = nullptr;
    if (desc.validation) {
        if (!layer_available(validation_layer)) {
            return Error{"validation requested but VK_LAYER_KHRONOS_validation was not found: "
                         "install the Vulkan SDK, or turn validation off"};
        }
        layers.push_back(validation_layer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        chain = &creation_messenger;
        if (layer_has_extension(validation_layer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            chain = &validation_features;
        }
    }

    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Perdix",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 0),
        .pEngineName = "Perdix",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = chain,
        .flags = flags,
        .pApplicationInfo = &application,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (const VkResult result = vkCreateInstance(&info, nullptr, &instance.instance_);
        result != VK_SUCCESS) {
        return Error{"vkCreateInstance failed: " + vk_result_name(result)};
    }
    volkLoadInstance(instance.instance_);

    if (desc.validation) {
        if (const VkResult result = vkCreateDebugUtilsMessengerEXT(instance.instance_, &messenger,
                                                                   nullptr, &instance.messenger_);
            result != VK_SUCCESS) {
            return Error{"vkCreateDebugUtilsMessengerEXT failed: " + vk_result_name(result)};
        }
    }
    return instance;
}

Instance::Instance(Instance&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE)),
      counts_(std::move(other.counts_)), loader_version_(other.loader_version_),
      portability_(other.portability_) {}

Instance& Instance::operator=(Instance&& other) noexcept {
    if (this != &other) {
        destroy();
        instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
        messenger_ = std::exchange(other.messenger_, VK_NULL_HANDLE);
        counts_ = std::move(other.counts_);
        loader_version_ = other.loader_version_;
        portability_ = other.portability_;
    }
    return *this;
}

Instance::~Instance() {
    destroy();
}

ValidationCounts Instance::validation_counts() const noexcept {
    return counts_ != nullptr ? *counts_ : ValidationCounts{};
}

void Instance::destroy() noexcept {
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
