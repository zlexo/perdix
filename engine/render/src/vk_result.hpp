#pragma once

#include <volk.h>

#include <string>

namespace vx::render {

// A VkResult's name for error messages, such as "VK_ERROR_DEVICE_LOST", or its
// number for codes the engine does not expect.
[[nodiscard]] std::string vk_result_name(VkResult result);

} // namespace vx::render
