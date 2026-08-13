#include "app/settings.hpp"

#include "app/app_controller.hpp"
#include "app/language_catalog.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

namespace dictscribe::app {

namespace {

ComputeDevice ParseDevice(const nlohmann::json& document, const char* field) {
    const auto value = document.find(field);
    if (value != document.end() && value->is_string() && value->get<std::string>() == "gpu") {
        return ComputeDevice::Gpu;
    }
    return ComputeDevice::Cpu;
}

std::filesystem::path EnvironmentPath(const char* name) {
    if (const char* value = std::getenv(name); value && value[0] != '\0') return value;
    return {};
}

} // namespace

bool IsSupportedLanguage(const std::string& language) {
    return IsSupportedLanguageCode(language);
}

CleanupMode ParseCleanupMode(const std::string& value) {
    return value == "ai" ? CleanupMode::Ai : CleanupMode::Off;
}

const char* ComputeDeviceName(ComputeDevice device) {
    return device == ComputeDevice::Gpu ? "gpu" : "cpu";
}

std::filesystem::path DefaultSettingsPath() {
#ifdef _WIN32
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value) / "DictScribe" / "settings.json";
#else
    if (const auto configured = EnvironmentPath("XDG_CONFIG_HOME"); !configured.empty()) {
        return configured / "dictscribe/settings.json";
    }
    if (const auto home = EnvironmentPath("HOME"); !home.empty()) {
        return home / ".config/dictscribe/settings.json";
    }
    return {};
#endif
}

AppSettings LoadSettings(const std::filesystem::path& path) {
    AppSettings settings;
    if (path.empty()) return settings;

    std::ifstream input(path, std::ios::binary);
    if (!input) return settings;
    const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
    if (!document.is_object()) return settings;

    const auto language = document.find("language");
    if (language != document.end() && language->is_string()) {
        const std::string value = CanonicalLanguageCode(language->get<std::string>());
        if (!value.empty()) settings.language = value;
    }
    const auto cleanup_mode = document.find("cleanupMode");
    if (cleanup_mode != document.end() && cleanup_mode->is_string()) {
        settings.cleanup_mode = ParseCleanupMode(cleanup_mode->get<std::string>());
    }
    settings.asr_device = ParseDevice(document, "asrDevice");
    settings.rewrite_device = ParseDevice(document, "rewriteDevice");

    const auto position = document.find("overlayPosition");
    if (position != document.end() && position->is_object()) {
        const auto x = position->find("x");
        const auto y = position->find("y");
        if (x != position->end() && x->is_number_integer() &&
            y != position->end() && y->is_number_integer()) {
            try {
                settings.overlay_position = ScreenPosition{x->get<int>(), y->get<int>()};
            } catch (const nlohmann::json::exception&) {
                settings.overlay_position.reset();
            }
        }
    }
    const auto size = document.find("overlaySize");
    if (size != document.end() && size->is_object()) {
        const auto width = size->find("width");
        const auto height = size->find("height");
        if (width != size->end() && width->is_number_integer() &&
            height != size->end() && height->is_number_integer()) {
            try {
                const ScreenSize value{width->get<int>(), height->get<int>()};
                if (value.width >= 480 && value.width <= 2000 &&
                    value.height >= 200 && value.height <= 1200) {
                    settings.overlay_size = value;
                }
            } catch (const nlohmann::json::exception&) {
                settings.overlay_size.reset();
            }
        }
    }
    return settings;
}

AppSettings LoadSettings() {
    return LoadSettings(DefaultSettingsPath());
}

bool SaveSettings(
    const std::filesystem::path& path,
    const AppSettings& settings,
    std::string& error) {
    if (path.empty()) {
        error = "The user configuration directory is unavailable.";
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Could not create the DictScribe settings directory: " +
            filesystem_error.message();
        return false;
    }

    nlohmann::json document = {
        {"version", 4},
        {"language", [&settings] {
            const std::string language = CanonicalLanguageCode(settings.language);
            return language.empty() ? std::string("auto") : language;
        }()},
        {"cleanupMode", CleanupModeName(settings.cleanup_mode)},
        {"asrDevice", ComputeDeviceName(settings.asr_device)},
        {"rewriteDevice", ComputeDeviceName(settings.rewrite_device)},
    };
    if (settings.overlay_position) {
        document["overlayPosition"] = {
            {"x", settings.overlay_position->x},
            {"y", settings.overlay_position->y},
        };
    }
    if (settings.overlay_size) {
        document["overlaySize"] = {
            {"width", settings.overlay_size->width},
            {"height", settings.overlay_size->height},
        };
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not open the temporary DictScribe settings file.";
            return false;
        }
        output << document.dump(2) << '\n';
        if (!output) {
            error = "Could not write the DictScribe settings file.";
            return false;
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, filesystem_error);
        error = "Could not replace the DictScribe settings file: " +
            std::system_category().message(static_cast<int>(code));
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        const std::string rename_error = filesystem_error.message();
        std::filesystem::remove(temporary, filesystem_error);
        error = "Could not replace the DictScribe settings file: " +
            rename_error;
        return false;
    }
#endif
    error.clear();
    return true;
}

bool SaveSettings(const AppSettings& settings, std::string& error) {
    return SaveSettings(DefaultSettingsPath(), settings, error);
}

bool ReconcilePendingDeviceSettings(
    const AppSnapshot& snapshot,
    PendingDeviceSettings& pending,
    AppSettings& settings,
    std::string& notice) {
    if (snapshot.mode == DictationMode::Error &&
        (pending.asr_device || pending.rewrite_device)) {
        pending.asr_device.reset();
        pending.rewrite_device.reset();
        notice = "The worker could not start. The previous saved device was kept.";
        return false;
    }

    bool changed = false;
    if (pending.asr_device && snapshot.asr_ready) {
        const ComputeDevice active = snapshot.asr_use_gpu
            ? ComputeDevice::Gpu : ComputeDevice::Cpu;
        if (active == *pending.asr_device) {
            settings.asr_device = active;
            pending.asr_device.reset();
            changed = true;
        }
    }
    if (pending.rewrite_device &&
        (snapshot.rewrite_ready || snapshot.cleanup_mode == CleanupMode::Off)) {
        const ComputeDevice active = snapshot.rewrite_use_gpu
            ? ComputeDevice::Gpu : ComputeDevice::Cpu;
        if (active == *pending.rewrite_device) {
            settings.rewrite_device = active;
            pending.rewrite_device.reset();
            changed = true;
        }
    }
    if (changed) notice.clear();
    return changed;
}

} // namespace dictscribe::app
