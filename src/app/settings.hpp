#pragma once

#include "app/cleanup_mode.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace dictscribe::app {

struct AppSnapshot;

enum class ComputeDevice {
    Cpu,
    Gpu,
};

enum class OverlayAppearance {
    Glass,
    Solid,
};

struct ScreenPosition {
    int x = 0;
    int y = 0;
};

struct ScreenSize {
    int width = 0;
    int height = 0;
};

struct AppSettings {
    std::string language = "auto";
    CleanupMode cleanup_mode = CleanupMode::Off;
    ComputeDevice asr_device = ComputeDevice::Cpu;
    ComputeDevice rewrite_device = ComputeDevice::Cpu;
    OverlayAppearance overlay_appearance = OverlayAppearance::Glass;
    std::optional<ScreenPosition> overlay_position;
    std::optional<ScreenSize> overlay_size;
};

struct PendingDeviceSettings {
    std::optional<ComputeDevice> asr_device;
    std::optional<ComputeDevice> rewrite_device;
};

[[nodiscard]] bool IsSupportedLanguage(const std::string& language);
[[nodiscard]] CleanupMode ParseCleanupMode(const std::string& value);
[[nodiscard]] const char* ComputeDeviceName(ComputeDevice device);
[[nodiscard]] const char* OverlayAppearanceName(OverlayAppearance appearance);
[[nodiscard]] std::filesystem::path DefaultSettingsPath();
[[nodiscard]] AppSettings LoadSettings(const std::filesystem::path& path);
[[nodiscard]] AppSettings LoadSettings();
bool SaveSettings(
    const std::filesystem::path& path,
    const AppSettings& settings,
    std::string& error);
bool SaveSettings(const AppSettings& settings, std::string& error);
bool ReconcilePendingDeviceSettings(
    const AppSnapshot& snapshot,
    PendingDeviceSettings& pending,
    AppSettings& settings,
    std::string& notice);

} // namespace dictscribe::app
