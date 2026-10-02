#pragma once

#include <vx/render/device_selection.hpp>

#include <string>

namespace vx::render {

// The startup device report: the device and driver the engine runs on, whether
// it needed the portability fallback, and each optional feature the device does
// not offer. One fact per line, each line ending in a newline.
[[nodiscard]] std::string format_device_report(const DeviceInfo& device, bool portability_fallback);

} // namespace vx::render
