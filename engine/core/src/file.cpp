#include <vx/core/file.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vx {
namespace {

struct Close {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};

} // namespace

std::optional<std::string> read_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::unique_ptr<std::FILE, Close> file(_wfopen(path.c_str(), L"rb"));
#else
    const std::unique_ptr<std::FILE, Close> file(std::fopen(path.c_str(), "rb"));
#endif
    if (file == nullptr) {
        return std::nullopt;
    }
    std::string bytes;
    std::array<char, 16'384> chunk{};
    for (;;) {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file.get());
        bytes.append(chunk.data(), got);
        // A short read is the end of the file or an error. A full one can
        // end there too, and reading on from the end or after an error is
        // what the stream checks flag: ask the stream first.
        if (got < chunk.size() || std::feof(file.get()) != 0 || std::ferror(file.get()) != 0) {
            break;
        }
    }
    if (std::ferror(file.get()) != 0) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace vx
