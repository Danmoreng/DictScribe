#pragma once

#include "app/settings.hpp"

#include <array>
#include <cstddef>
#include <string>

#include "include/core/SkFont.h"
#include "include/core/SkRect.h"

class SkCanvas;

namespace dictscribe::ui {

enum class SettingsAction {
    NoAction,
    Close,
    ToggleLanguageMenu,
    CleanupOff,
    CleanupAi,
    OverlayGlass,
    OverlaySolid,
    AsrCpu,
    AsrGpu,
    RewriteCpu,
    RewriteGpu,
    LanguageOptionBase = 1000,
};

inline SettingsAction LanguageSelectionAction(std::size_t index) {
    return static_cast<SettingsAction>(
        static_cast<int>(SettingsAction::LanguageOptionBase) + static_cast<int>(index));
}

inline bool IsLanguageSelection(SettingsAction action) {
    const int value = static_cast<int>(action);
    return value >= static_cast<int>(SettingsAction::LanguageOptionBase);
}

inline std::size_t LanguageSelectionIndex(SettingsAction action) {
    return static_cast<std::size_t>(
        static_cast<int>(action) - static_cast<int>(SettingsAction::LanguageOptionBase));
}

struct SettingsViewModel {
    app::AppSettings settings;
    std::string asr_model_name;
    std::string rewrite_model_name;
    bool device_controls_enabled = true;
    bool language_menu_open = false;
    int language_menu_scroll = 0;
    int language_menu_highlight = -1;
    bool language_select_hovered = false;
    std::string notice;
};

struct SettingsViewLayout {
    SkRect close;
    SkRect language_select;
    SkRect language_menu;
    std::array<SkRect, 8> language_options{};
    std::array<std::size_t, 8> language_option_indices{};
    std::size_t language_option_count = 0;
    SkRect cleanup_off;
    SkRect cleanup_ai;
    SkRect overlay_glass;
    SkRect overlay_solid;
    SkRect asr_cpu;
    SkRect asr_gpu;
    SkRect rewrite_cpu;
    SkRect rewrite_gpu;
};

SettingsViewLayout RenderSettingsView(
    SkCanvas& canvas,
    float width,
    float height,
    const SkFont& regular,
    const SkFont& semibold,
    const SettingsViewModel& model);

[[nodiscard]] SettingsAction HitTestSettingsView(
    const SettingsViewLayout& layout,
    float x,
    float y,
    bool device_controls_enabled);

} // namespace dictscribe::ui
