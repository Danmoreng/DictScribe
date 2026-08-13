#include "ui/settings_view.hpp"

#include "app/language_catalog.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"

namespace dictscribe::ui {

namespace {

constexpr SkColor kBackground = SkColorSetRGB(14, 17, 23);
constexpr SkColor kSurface = SkColorSetRGB(22, 26, 35);
constexpr SkColor kControl = SkColorSetRGB(31, 36, 47);
constexpr SkColor kSelected = SkColorSetRGB(66, 57, 112);
constexpr SkColor kBorder = SkColorSetRGB(61, 69, 84);
constexpr SkColor kText = SkColorSetRGB(244, 246, 250);
constexpr SkColor kMuted = SkColorSetRGB(158, 167, 184);
constexpr SkColor kDisabled = SkColorSetRGB(89, 96, 110);
constexpr SkColor kAccent = SkColorSetRGB(139, 124, 255);
constexpr SkColor kWarning = SkColorSetRGB(255, 186, 85);
constexpr std::size_t kVisibleLanguageRows = 8;

SkPaint Fill(SkColor color) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    return paint;
}

void Text(
    SkCanvas& canvas,
    std::string_view text,
    float x,
    float baseline,
    const SkFont& font,
    SkColor color) {
    canvas.drawSimpleText(
        text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, Fill(color));
}

float TextWidth(const SkFont& font, std::string_view text) {
    return font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
}

void Outline(SkCanvas& canvas, const SkRect& rect, float radius) {
    SkPaint paint = Fill(kBorder);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1.0F);
    canvas.drawRoundRect(rect, radius, radius, paint);
}

void SegmentedButton(
    SkCanvas& canvas,
    const SkRect& rect,
    std::string_view label,
    bool selected,
    bool enabled,
    const SkFont& font) {
    canvas.drawRoundRect(rect, 8.0F, 8.0F, Fill(selected ? kSelected : kControl));
    Outline(canvas, rect, 8.0F);
    Text(
        canvas,
        label,
        rect.centerX() - TextWidth(font, label) * 0.5F,
        rect.centerY() + 4.5F,
        font,
        enabled ? (selected ? kText : kMuted) : kDisabled);
}

std::string ShortModelName(const std::string& value) {
    constexpr std::size_t kMaximum = 62;
    if (value.size() <= kMaximum) return value;
    return value.substr(0, kMaximum - 3) + "...";
}

} // namespace

SettingsViewLayout RenderSettingsView(
    SkCanvas& canvas,
    float width,
    float height,
    const SkFont& regular,
    const SkFont& semibold,
    const SettingsViewModel& model) {
    SettingsViewLayout layout;
    SkFont title_font = semibold;
    SkFont section_font = semibold;
    SkFont body_font = regular;
    SkFont control_font = semibold;
    SkFont detail_font = regular;
    title_font.setSize(24.0F);
    section_font.setSize(14.0F);
    body_font.setSize(13.0F);
    control_font.setSize(12.5F);
    detail_font.setSize(11.5F);

    canvas.clear(kBackground);
    Text(canvas, "Settings", 32.0F, 48.0F, title_font, kText);
    Text(canvas, "Local preferences for DictScribe", 32.0F, 70.0F, body_font, kMuted);
    layout.close = SkRect::MakeXYWH(width - 106.0F, 28.0F, 74.0F, 34.0F);
    SegmentedButton(canvas, layout.close, "Done", false, true, control_font);

    Text(canvas, "DICTATION LANGUAGE", 32.0F, 114.0F, section_font, kMuted);
    layout.language_select = SkRect::MakeXYWH(32.0F, 128.0F, width - 64.0F, 40.0F);
    canvas.drawRoundRect(
        layout.language_select,
        8.0F,
        8.0F,
        Fill(model.language_select_hovered ? SkColorSetRGB(38, 44, 57) : kControl));
    Outline(canvas, layout.language_select, 8.0F);
    Text(
        canvas, app::LanguageDisplayName(model.settings.language),
        46.0F, 153.5F, body_font, kText);
    Text(canvas, model.language_menu_open ? "^" : "v", width - 55.0F, 153.5F, control_font, kMuted);

    Text(canvas, "TRANSCRIPT CLEANUP", 32.0F, 202.0F, section_font, kMuted);
    layout.cleanup_off = SkRect::MakeXYWH(32.0F, 216.0F, 128.0F, 38.0F);
    layout.cleanup_ai = SkRect::MakeXYWH(168.0F, 216.0F, 168.0F, 38.0F);
    SegmentedButton(
        canvas, layout.cleanup_off, "Off", model.settings.cleanup_mode == app::CleanupMode::Off,
        model.device_controls_enabled, control_font);
    SegmentedButton(
        canvas, layout.cleanup_ai, "AI cleanup", model.settings.cleanup_mode == app::CleanupMode::Ai,
        model.device_controls_enabled, control_font);

    Text(canvas, "COMPUTE DEVICE", 32.0F, 296.0F, section_font, kMuted);
    Text(canvas, "Speech recognition", 32.0F, 328.0F, section_font, kText);
    Text(canvas, "Nemotron ASR worker", 32.0F, 348.0F, detail_font, kMuted);
    layout.asr_cpu = SkRect::MakeXYWH(width - 216.0F, 309.0F, 84.0F, 38.0F);
    layout.asr_gpu = SkRect::MakeXYWH(width - 124.0F, 309.0F, 92.0F, 38.0F);
    SegmentedButton(
        canvas, layout.asr_cpu, "CPU", model.settings.asr_device == app::ComputeDevice::Cpu,
        model.device_controls_enabled, control_font);
    SegmentedButton(
        canvas, layout.asr_gpu, "GPU", model.settings.asr_device == app::ComputeDevice::Gpu,
        model.device_controls_enabled, control_font);

    Text(canvas, "AI cleanup", 32.0F, 390.0F, section_font, kText);
    Text(canvas, "llama.cpp rewrite worker", 32.0F, 410.0F, detail_font, kMuted);
    layout.rewrite_cpu = SkRect::MakeXYWH(width - 216.0F, 371.0F, 84.0F, 38.0F);
    layout.rewrite_gpu = SkRect::MakeXYWH(width - 124.0F, 371.0F, 92.0F, 38.0F);
    SegmentedButton(
        canvas, layout.rewrite_cpu, "CPU", model.settings.rewrite_device == app::ComputeDevice::Cpu,
        model.device_controls_enabled, control_font);
    SegmentedButton(
        canvas, layout.rewrite_gpu, "GPU", model.settings.rewrite_device == app::ComputeDevice::Gpu,
        model.device_controls_enabled, control_font);

    const std::string device_note = model.device_controls_enabled
        ? "Changing a device immediately restarts only that local worker."
        : "Finish or cancel the current dictation before changing a device.";
    Text(
        canvas, device_note, 32.0F, 446.0F, body_font,
        model.device_controls_enabled ? kMuted : kWarning);

    Text(canvas, "LOCAL MODELS", 32.0F, 499.0F, section_font, kMuted);
    canvas.drawRoundRect(
        SkRect::MakeXYWH(32.0F, 514.0F, width - 64.0F, 104.0F), 10.0F, 10.0F, Fill(kSurface));
    Outline(canvas, SkRect::MakeXYWH(32.0F, 514.0F, width - 64.0F, 104.0F), 10.0F);
    Text(canvas, "Speech recognition", 48.0F, 543.0F, detail_font, kMuted);
    Text(canvas, ShortModelName(model.asr_model_name), 48.0F, 562.0F, body_font, kText);
    Text(canvas, "AI cleanup", 48.0F, 591.0F, detail_font, kMuted);
    Text(canvas, ShortModelName(model.rewrite_model_name), 48.0F, 610.0F, body_font, kText);

    if (!model.notice.empty()) {
        Text(canvas, model.notice, 32.0F, std::min(height - 14.0F, 646.0F), detail_font, kWarning);
    }

    if (model.language_menu_open) {
        const int maximum_scroll = std::max(
            0, static_cast<int>(app::kLanguageOptions.size() - kVisibleLanguageRows));
        const int scroll = std::clamp(model.language_menu_scroll, 0, maximum_scroll);
        layout.language_menu = SkRect::MakeXYWH(
            32.0F, 174.0F, width - 64.0F,
            12.0F + static_cast<float>(kVisibleLanguageRows) * 32.0F);
        canvas.drawRoundRect(layout.language_menu, 9.0F, 9.0F, Fill(kSurface));
        Outline(canvas, layout.language_menu, 9.0F);
        for (std::size_t row = 0; row < kVisibleLanguageRows; ++row) {
            const std::size_t index = static_cast<std::size_t>(scroll) + row;
            if (index >= app::kLanguageOptions.size()) break;
            const SkRect option = SkRect::MakeXYWH(
                layout.language_menu.left() + 6.0F,
                layout.language_menu.top() + 6.0F + static_cast<float>(row) * 32.0F,
                layout.language_menu.width() - 20.0F,
                32.0F);
            layout.language_options[layout.language_option_count] = option;
            layout.language_option_indices[layout.language_option_count] = index;
            ++layout.language_option_count;
            if (static_cast<int>(index) == model.language_menu_highlight) {
                canvas.drawRoundRect(option, 6.0F, 6.0F, Fill(SkColorSetRGB(36, 42, 55)));
            } else if (app::kLanguageOptions[index].code ==
                       app::CanonicalLanguageCode(model.settings.language)) {
                canvas.drawRoundRect(option, 6.0F, 6.0F, Fill(kSelected));
            }
            Text(
                canvas, app::kLanguageOptions[index].label,
                option.left() + 10.0F, option.centerY() + 4.5F, body_font, kText);
        }
        const float track_height = layout.language_menu.height() - 20.0F;
        const float thumb_height = track_height * static_cast<float>(kVisibleLanguageRows) /
            static_cast<float>(app::kLanguageOptions.size());
        const float thumb_y = layout.language_menu.top() + 10.0F +
            (track_height - thumb_height) * static_cast<float>(scroll) /
            static_cast<float>(std::max(maximum_scroll, 1));
        canvas.drawRoundRect(
            SkRect::MakeXYWH(layout.language_menu.right() - 9.0F, thumb_y, 3.0F, thumb_height),
            1.5F, 1.5F, Fill(kMuted));
    }
    return layout;
}

SettingsAction HitTestSettingsView(
    const SettingsViewLayout& layout,
    float x,
    float y,
    bool device_controls_enabled) {
    if (!layout.language_menu.isEmpty()) {
        for (std::size_t index = 0; index < layout.language_option_count; ++index) {
            if (layout.language_options[index].contains(x, y)) {
                return LanguageSelectionAction(layout.language_option_indices[index]);
            }
        }
        if (layout.language_select.contains(x, y) || !layout.language_menu.contains(x, y)) {
            return SettingsAction::ToggleLanguageMenu;
        }
        return SettingsAction::NoAction;
    }
    if (layout.close.contains(x, y)) return SettingsAction::Close;
    if (layout.language_select.contains(x, y)) return SettingsAction::ToggleLanguageMenu;
    if (layout.cleanup_off.contains(x, y)) return SettingsAction::CleanupOff;
    if (layout.cleanup_ai.contains(x, y)) return SettingsAction::CleanupAi;
    if (!device_controls_enabled) return SettingsAction::NoAction;
    if (layout.asr_cpu.contains(x, y)) return SettingsAction::AsrCpu;
    if (layout.asr_gpu.contains(x, y)) return SettingsAction::AsrGpu;
    if (layout.rewrite_cpu.contains(x, y)) return SettingsAction::RewriteCpu;
    if (layout.rewrite_gpu.contains(x, y)) return SettingsAction::RewriteGpu;
    return SettingsAction::NoAction;
}

} // namespace dictscribe::ui
