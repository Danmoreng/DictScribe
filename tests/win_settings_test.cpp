#include "platform/win/win_settings.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dictscribe-settings-test-" + std::to_string(GetCurrentProcessId()) + ".json");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.wstring() + L".tmp", ignored);

    dictscribe::win::WinSettings expected;
    expected.language = "de";
    expected.overlay_position = dictscribe::win::OverlayPosition{321, 654};
    std::string error;
    assert(dictscribe::win::SaveSettings(path, expected, error));

    const auto loaded = dictscribe::win::LoadSettings(path);
    assert(loaded.language == "de");
    assert(loaded.overlay_position.has_value());
    assert(loaded.overlay_position->x == 321);
    assert(loaded.overlay_position->y == 654);

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << R"({"language":"unsupported","overlayPosition":{"x":"bad","y":2}})";
    }
    const auto defaults = dictscribe::win::LoadSettings(path);
    assert(defaults.language == "auto");
    assert(!defaults.overlay_position.has_value());

    std::filesystem::remove(path, ignored);
    std::cout << "Windows settings tests passed\n";
    return 0;
}
