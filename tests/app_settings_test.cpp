#include "app/settings.hpp"
#include "app/app_controller.hpp"
#include "app/language_catalog.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    assert(dictscribe::app::kLanguageOptions.size() == 33);
    assert(dictscribe::app::CanonicalLanguageCode("DE_de") == "de-DE");
    assert(dictscribe::app::CanonicalLanguageCode("pt-br") == "pt-BR");
    assert(dictscribe::app::CanonicalLanguageCode("el-GR").empty());

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dictscribe-settings-test-" + suffix + ".json");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);

    dictscribe::app::AppSettings expected;
    expected.language = "de-DE";
    expected.cleanup_mode = dictscribe::app::CleanupMode::Ai;
    expected.asr_device = dictscribe::app::ComputeDevice::Gpu;
    expected.rewrite_device = dictscribe::app::ComputeDevice::Cpu;
    expected.overlay_appearance = dictscribe::app::OverlayAppearance::Solid;
    expected.color_theme = dictscribe::app::ColorTheme::Light;
    expected.overlay_position = dictscribe::app::ScreenPosition{321, 654};
    expected.overlay_size = dictscribe::app::ScreenSize{880, 440};
    std::string error;
    assert(dictscribe::app::SaveSettings(path, expected, error));

    const auto loaded = dictscribe::app::LoadSettings(path);
    assert(loaded.language == "de-DE");
    assert(loaded.cleanup_mode == dictscribe::app::CleanupMode::Ai);
    assert(loaded.asr_device == dictscribe::app::ComputeDevice::Gpu);
    assert(loaded.rewrite_device == dictscribe::app::ComputeDevice::Cpu);
    assert(loaded.overlay_appearance == dictscribe::app::OverlayAppearance::Solid);
    assert(loaded.color_theme == dictscribe::app::ColorTheme::Light);
    assert(loaded.overlay_position.has_value());
    assert(loaded.overlay_position->x == 321);
    assert(loaded.overlay_position->y == 654);
    assert(loaded.overlay_size.has_value());
    assert(loaded.overlay_size->width == 880);
    assert(loaded.overlay_size->height == 440);

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << R"({"language":"unsupported","asrDevice":"other","rewriteDevice":4,"overlayPosition":{"x":"bad","y":2},"overlaySize":{"width":100,"height":99999}})";
    }
    const auto defaults = dictscribe::app::LoadSettings(path);
    assert(defaults.language == "auto");
    assert(defaults.cleanup_mode == dictscribe::app::CleanupMode::Off);
    assert(defaults.asr_device == dictscribe::app::ComputeDevice::Cpu);
    assert(defaults.rewrite_device == dictscribe::app::ComputeDevice::Cpu);
    assert(defaults.overlay_appearance == dictscribe::app::OverlayAppearance::Glass);
    assert(defaults.color_theme == dictscribe::app::ColorTheme::Dark);
    assert(!defaults.overlay_position.has_value());
    assert(!defaults.overlay_size.has_value());

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << R"({"language":"en"})";
    }
    assert(dictscribe::app::LoadSettings(path).language == "en-US");

    dictscribe::app::AppSettings reconciled;
    dictscribe::app::PendingDeviceSettings pending;
    pending.asr_device = dictscribe::app::ComputeDevice::Gpu;
    dictscribe::app::AppSnapshot snapshot;
    snapshot.asr_use_gpu = true;
    snapshot.asr_ready = false;
    snapshot.mode = dictscribe::app::DictationMode::Starting;
    std::string notice;
    assert(!dictscribe::app::ReconcilePendingDeviceSettings(
        snapshot, pending, reconciled, notice));
    assert(reconciled.asr_device == dictscribe::app::ComputeDevice::Cpu);
    assert(pending.asr_device.has_value());

    snapshot.mode = dictscribe::app::DictationMode::Error;
    assert(!dictscribe::app::ReconcilePendingDeviceSettings(
        snapshot, pending, reconciled, notice));
    assert(reconciled.asr_device == dictscribe::app::ComputeDevice::Cpu);
    assert(!pending.asr_device.has_value());
    assert(!notice.empty());

    pending.asr_device = dictscribe::app::ComputeDevice::Gpu;
    snapshot.mode = dictscribe::app::DictationMode::Ready;
    snapshot.asr_ready = true;
    assert(dictscribe::app::ReconcilePendingDeviceSettings(
        snapshot, pending, reconciled, notice));
    assert(reconciled.asr_device == dictscribe::app::ComputeDevice::Gpu);
    assert(!pending.asr_device.has_value());
    assert(notice.empty());

    std::filesystem::remove(path, ignored);
    std::cout << "Application settings tests passed\n";
    return 0;
}
