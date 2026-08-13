#pragma once

#include "app/settings.hpp"

#include <string>

#include "include/core/SkFont.h"
#include "include/core/SkRect.h"

class SkCanvas;

namespace dictscribe::ui {

enum class SettingsAction {
    NoAction,
    Close,
    LanguageAuto,
    LanguageGerman,
    LanguageEnglish,
    AsrCpu,
    AsrGpu,
    RewriteCpu,
    RewriteGpu,
};

struct SettingsViewModel {
    app::AppSettings settings;
    std::string asr_model_name;
    std::string rewrite_model_name;
    bool device_controls_enabled = true;
    std::string notice;
};

struct SettingsViewLayout {
    SkRect close;
    SkRect language_auto;
    SkRect language_german;
    SkRect language_english;
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
