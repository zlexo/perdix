#include "debug_name.hpp"

#include <string>

namespace vx::render {

void set_debug_name(VkDevice device, VkObjectType type, std::uint64_t handle,
                    std::string_view name) {
    if (vkSetDebugUtilsObjectNameEXT == nullptr || handle == 0) {
        return;
    }
    const std::string terminated(name);
    const VkDebugUtilsObjectNameInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .pNext = nullptr,
        .objectType = type,
        .objectHandle = handle,
        .pObjectName = terminated.c_str(),
    };
    // A name is a convenience: an object without one works the same.
    static_cast<void>(vkSetDebugUtilsObjectNameEXT(device, &info));
}

} // namespace vx::render
