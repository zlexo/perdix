#include <vx/core/file.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <system_error>

namespace {

// A folder of its own under the system's temporary folder, removed when
// the test ends.
class TempFolder {
public:
    TempFolder() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("vx-file-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    TempFolder(TempFolder&&) = delete;
    TempFolder& operator=(TempFolder&&) = delete;
    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a file is read whole, byte for byte, zero bytes and all") {
    const TempFolder folder;
    // Longer than one chunk of the read, with a zero byte in it.
    std::string bytes(40'000, 'x');
    bytes[20'000] = '\0';
    bytes.back() = '\n';
    std::ofstream(folder.path() / "file.bin", std::ios::binary) << bytes;
    CHECK(vx::read_file(folder.path() / "file.bin") == bytes);

    std::ofstream(folder.path() / "empty.bin", std::ios::binary).close();
    CHECK(vx::read_file(folder.path() / "empty.bin") == std::string());
}

TEST_CASE("a file that cannot be opened or read gives nothing") {
    const TempFolder folder;
    CHECK_FALSE(vx::read_file(folder.path() / "missing.bin").has_value());
    // A folder opens on both the Mac and Linux, then fails on the first read:
    // that must not pass for an empty file.
    CHECK_FALSE(vx::read_file(folder.path()).has_value());
}
