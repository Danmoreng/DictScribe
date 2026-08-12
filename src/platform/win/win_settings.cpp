#include "platform/win/win_settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace dictscribe::win {

namespace {

bool SupportedLanguage(const std::string& language) {
    return language == "auto" || language == "de" || language == "en";
}

std::string WindowsError(DWORD code) {
    return std::system_category().message(static_cast<int>(code));
}

} // namespace

std::filesystem::path DefaultSettingsPath() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value) / "DictScribe" / "settings.json";
}

WinSettings LoadSettings(const std::filesystem::path& path) {
    WinSettings settings;
    if (path.empty()) return settings;

    std::ifstream input(path, std::ios::binary);
    if (!input) return settings;
    const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
    if (!document.is_object()) return settings;

    const auto language = document.find("language");
    if (language != document.end() && language->is_string()) {
        const std::string value = language->get<std::string>();
        if (SupportedLanguage(value)) settings.language = value;
    }

    const auto position = document.find("overlayPosition");
    if (position != document.end() && position->is_object()) {
        const auto x = position->find("x");
        const auto y = position->find("y");
        if (x != position->end() && x->is_number_integer() &&
            y != position->end() && y->is_number_integer()) {
            try {
                settings.overlay_position = OverlayPosition{
                    x->get<int>(),
                    y->get<int>(),
                };
            } catch (const nlohmann::json::exception&) {
                settings.overlay_position.reset();
            }
        }
    }
    return settings;
}

WinSettings LoadSettings() {
    return LoadSettings(DefaultSettingsPath());
}

bool SaveSettings(
    const std::filesystem::path& path,
    const WinSettings& settings,
    std::string& error) {
    if (path.empty()) {
        error = "The Windows local application data directory is unavailable.";
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
        {"version", 1},
        {"language", SupportedLanguage(settings.language) ? settings.language : "auto"},
    };
    if (settings.overlay_position) {
        document["overlayPosition"] = {
            {"x", settings.overlay_position->x},
            {"y", settings.overlay_position->y},
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

    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, filesystem_error);
        error = "Could not replace the DictScribe settings file: " + WindowsError(code);
        return false;
    }
    error.clear();
    return true;
}

bool SaveSettings(const WinSettings& settings, std::string& error) {
    return SaveSettings(DefaultSettingsPath(), settings, error);
}

} // namespace dictscribe::win
