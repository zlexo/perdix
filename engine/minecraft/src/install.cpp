#include <vx/minecraft/install.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace vx::minecraft {

std::vector<std::filesystem::path>
game_folders(System system, const std::filesystem::path& home,
             const std::optional<std::filesystem::path>& appdata) {
    std::vector<std::filesystem::path> folders;
    switch (system) {
    case System::macos:
        if (!home.empty()) {
            folders.push_back(home / "Library" / "Application Support" / "minecraft");
        }
        break;
    case System::windows:
        if (appdata && !appdata->empty()) {
            folders.push_back(*appdata / ".minecraft");
        } else if (!home.empty()) {
            folders.push_back(home / ".minecraft");
        }
        break;
    case System::linux_os:
        if (!home.empty()) {
            folders.push_back(home / ".minecraft");
            folders.push_back(home / ".var" / "app" / "com.mojang.Minecraft" / ".minecraft");
        }
        break;
    }
    return folders;
}

} // namespace vx::minecraft
