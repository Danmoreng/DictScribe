#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace dictscribe::win {

struct OverlayPosition {
    int x = 0;
    int y = 0;
};

struct WinSettings {
    std::string language = "auto";
    std::optional<OverlayPosition> overlay_position;
};

[[nodiscard]] std::filesystem::path DefaultSettingsPath();
[[nodiscard]] WinSettings LoadSettings(const std::filesystem::path& path);
[[nodiscard]] WinSettings LoadSettings();
bool SaveSettings(
    const std::filesystem::path& path,
    const WinSettings& settings,
    std::string& error);
bool SaveSettings(const WinSettings& settings, std::string& error);

} // namespace dictscribe::win
