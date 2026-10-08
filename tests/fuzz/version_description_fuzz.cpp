// libFuzzer's entry point for the launcher's version descriptions (RFC 0009):
// any bytes, read as a description and as a release time. scripts/fuzz.sh
// builds and runs it.

#include <vx/core/diagnostic.hpp>
#include <vx/minecraft/version_description.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    std::vector<vx::Diagnostic> diagnostics;
    static_cast<void>(vx::minecraft::read_version_description("fuzz.json", text, diagnostics));
    static_cast<void>(vx::minecraft::parse_release_time(text));
    return 0;
}
