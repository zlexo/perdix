// libFuzzer's entry point for the archive reader (RFC 0009): any bytes,
// opened as an archive from memory, then every entry it lists read, and a
// few names it may not list. scripts/fuzz.sh builds and runs it.

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::vector<std::byte> bytes(size);
    if (size != 0) {
        std::memcpy(bytes.data(), data, size);
    }
    const vx::Result<vx::Archive> archive = vx::Archive::from_bytes("fuzz", std::move(bytes));
    if (!archive.has_value()) {
        return 0;
    }
    for (const std::string_view name : archive.value().names()) {
        static_cast<void>(archive.value().contains(name));
        static_cast<void>(archive.value().read(name));
    }
    for (const std::string_view name : {"", "a.txt", "hello.txt"}) {
        static_cast<void>(archive.value().read(name));
    }
    return 0;
}
