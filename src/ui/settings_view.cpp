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

constexpr std::size_t kVisibleLanguageRows = 8;

struct SettingsPalette {
    SkColor background;
    SkColor surface;
    SkColor control;
    SkColor control_hover;
    SkColor selected;
    SkColor selected_hover;
    SkColor border;
    SkColor text;
    SkColor muted;
    SkColor disabled;
    SkColor warning;
};

constexpr SettingsPalette kDarkPalette{
    SkColorSetRGB(14, 17, 23),
    SkColorSetRGB(22, 26, 35),
    SkColorSetRGB(31, 36, 47),
    SkColorSetRGB(38, 44, 57),
    SkColorSetRGB(66, 57, 112),
    SkColorSetRGB(36, 42, 55),
    SkColorSetRGB(61, 69, 84),
    SkColorSetRGB(244, 246, 250),
    SkColorSetRGB(158, 167, 184),
    SkColorSetRGB(89, 96, 110),
    SkColorSetRGB(255, 186, 85),
};

constexpr SettingsPalette kLightPalette{
    SkColorSetRGB(246, 248, 252),
    SkColorSetRGB(255, 255, 255),
    SkColorSetRGB(237, 241, 247),
    SkColorSetRGB(225, 231, 240),
    SkColorSetRGB(224, 219, 252),
    SkColorSetRGB(232, 235, 252),
    SkColorSetRGB(190, 200, 215),
    SkColorSetRGB(27, 34, 47),
    SkColorSetRGB(84, 96, 116),
    SkColorSetRGB(145, 154, 170),
    SkColorSetRGB(174, 103, 0),
};

const SettingsPalette& PaletteFor(app::ColorTheme theme) {
    return theme == app::ColorTheme::Light ? kLightPalette : kDarkPalette;
}

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

void Outline(
    SkCanvas& canvas,
    const SkRect& rect,
    float radius,
    const SettingsPalette& palette) {
    SkPaint paint = Fill(palette.border);
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
    const SkFont& font,
    const SettingsPalette& palette) {
    canvas.drawRoundRect(
        rect, 8.0F, 8.0F, Fill(selected ? palette.selected : palette.control));
    Outline(canvas, rect, 8.0F, palette);
    Text(
        canvas,
        label,
        rect.centerX() - TextWidth(font, label) * 0.5F,
        rect.centerY() + 4.5F,
        font,
        enabled ? (selected ? palette.text : palette.muted) : palette.disabled);
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
    const SettingsPalette& palette = PaletteFor(model.settings.color_theme);
#ifdef _WIN32
    constexpr float kAppearanceRowOffset = 82.0F;
#else
    constexpr float kAppearanceRowOffset = 0.0F;
#endif

    canvas.clear(palette.background);
    Text(canvas, "Settings", 32.0F, 48.0F, title_font, palette.text);
    Text(canvas, "Local preferences for DictScribe", 32.0F, 70.0F, body_font, palette.muted);
    layout.close = SkRect::MakeXYWH(width - 106.0F, 28.0F, 74.0F, 34.0F);
    SegmentedButton(canvas, layout.close, "Done", false, true, control_font, palette);

    Text(canvas, "DICTATION LANGUAGE", 32.0F, 114.0F, section_font, palette.muted);
    layout.language_select = SkRect::MakeXYWH(32.0F, 128.0F, width - 64.0F, 40.0F);
    canvas.drawRoundRect(
        layout.language_select,
        8.0F,
        8.0F,
        Fill(model.language_select_hovered ? palette.control_hover : palette.control));
    Outline(canvas, layout.language_select, 8.0F, palette);
    Text(
        canvas, app::LanguageDisplayName(model.settings.language),
        46.0F, 153.5F, body_font, palette.text);
    Text(canvas, model.language_menu_open ? "^" : "v", width - 55.0F, 153.5F, control_font, palette.muted);

    Text(canvas, "TRANSCRIPT CLEANUP", 32.0F, 202.0F, section_font, palette.muted);
#ifdef _WIN32
    layout.cleanup_off = SkRect::MakeXYWH(32.0F, 216.0F, 76.0F, 38.0F);
    layout.cleanup_ai = SkRect::MakeXYWH(116.0F, 216.0F, 126.0F, 38.0F);
#else
    layout.cleanup_off = SkRect::MakeXYWH(32.0F, 216.0F, 128.0F, 38.0F);
    layout.cleanup_ai = SkRect::MakeXYWH(168.0F, 216.0F, 168.0F, 38.0F);
#endif
    SegmentedButton(
        canvas, layout.cleanup_off, "Off", model.settings.cleanup_mode == app::CleanupMode::Off,
        model.device_controls_enabled, control_font, palette);
    SegmentedButton(
        canvas, layout.cleanup_ai, "AI cleanup", model.settings.cleanup_mode == app::CleanupMode::Ai,
        model.device_controls_enabled, control_font, palette);

#ifdef _WIN32
    Text(canvas, "OVERLAY DESIGN", 330.0F, 202.0F, section_font, palette.muted);
    layout.overlay_glass = SkRect::MakeXYWH(330.0F, 216.0F, 112.0F, 38.0F);
    layout.overlay_solid = SkRect::MakeXYWH(450.0F, 216.0F, 112.0F, 38.0F);
    SegmentedButton(
        canvas, layout.overlay_glass, "Glass",
        model.settings.overlay_appearance == app::OverlayAppearance::Glass,
        true, control_font, palette);
    SegmentedButton(
        canvas, layout.overlay_solid, "Solid",
        model.settings.overlay_appearance == app::OverlayAppearance::Solid,
        true, control_font, palette);

    Text(canvas, "COLOR THEME", 32.0F, 284.0F, section_font, palette.muted);
    layout.theme_dark = SkRect::MakeXYWH(32.0F, 298.0F, 112.0F, 38.0F);
    layout.theme_light = SkRect::MakeXYWH(152.0F, 298.0F, 112.0F, 38.0F);
    SegmentedButton(
        canvas, layout.theme_dark, "Dark",
        model.settings.color_theme == app::ColorTheme::Dark,
        true, control_font, palette);
    SegmentedButton(
        canvas, layout.theme_light, "Light",
        model.settings.color_theme == app::ColorTheme::Light,
        true, control_font, palette);
#endif

    Text(canvas, "COMPUTE DEVICE", 32.0F, 296.0F + kAppearanceRowOffset, section_font, palette.muted);
    Text(canvas, "Speech recognition", 32.0F, 328.0F + kAppearanceRowOffset, section_font, palette.text);
    Text(canvas, "Nemotron ASR worker", 32.0F, 348.0F + kAppearanceRowOffset, detail_font, palette.muted);
    layout.asr_cpu = SkRect::MakeXYWH(width - 216.0F, 309.0F + kAppearanceRowOffset, 84.0F, 38.0F);
    layout.asr_gpu = SkRect::MakeXYWH(width - 124.0F, 309.0F + kAppearanceRowOffset, 92.0F, 38.0F);
    SegmentedButton(
        canvas, layout.asr_cpu, "CPU", model.settings.asr_device == app::ComputeDevice::Cpu,
        model.device_controls_enabled, control_font, palette);
    SegmentedButton(
        canvas, layout.asr_gpu, "GPU", model.settings.asr_device == app::ComputeDevice::Gpu,
        model.device_controls_enabled, control_font, palette);

    Text(canvas, "AI cleanup", 32.0F, 390.0F + kAppearanceRowOffset, section_font, palette.text);
    Text(canvas, "llama.cpp rewrite worker", 32.0F, 410.0F + kAppearanceRowOffset, detail_font, palette.muted);
    layout.rewrite_cpu = SkRect::MakeXYWH(width - 216.0F, 371.0F + kAppearanceRowOffset, 84.0F, 38.0F);
    layout.rewrite_gpu = SkRect::MakeXYWH(width - 124.0F, 371.0F + kAppearanceRowOffset, 92.0F, 38.0F);
    SegmentedButton(
        canvas, layout.rewrite_cpu, "CPU", model.settings.rewrite_device == app::ComputeDevice::Cpu,
        model.device_controls_enabled, control_font, palette);
    SegmentedButton(
        canvas, layout.rewrite_gpu, "GPU", model.settings.rewrite_device == app::ComputeDevice::Gpu,
        model.device_controls_enabled, control_font, palette);

    const std::string device_note = model.device_controls_enabled
        ? "Changing a device immediately restarts only that local worker."
        : "Finish or cancel the current dictation before changing a device.";
    Text(
        canvas, device_note, 32.0F, 446.0F + kAppearanceRowOffset, body_font,
        model.device_controls_enabled ? palette.muted : palette.warning);

    Text(canvas, "LOCAL MODELS", 32.0F, 499.0F + kAppearanceRowOffset, section_font, palette.muted);
    canvas.drawRoundRect(
        SkRect::MakeXYWH(32.0F, 514.0F + kAppearanceRowOffset, width - 64.0F, 104.0F), 10.0F, 10.0F, Fill(palette.surface));
    Outline(canvas, SkRect::MakeXYWH(32.0F, 514.0F + kAppearanceRowOffset, width - 64.0F, 104.0F), 10.0F, palette);
    Text(canvas, "Speech recognition", 48.0F, 543.0F + kAppearanceRowOffset, detail_font, palette.muted);
    Text(canvas, ShortModelName(model.asr_model_name), 48.0F, 562.0F + kAppearanceRowOffset, body_font, palette.text);
    Text(canvas, "AI cleanup", 48.0F, 591.0F + kAppearanceRowOffset, detail_font, palette.muted);
    Text(canvas, ShortModelName(model.rewrite_model_name), 48.0F, 610.0F + kAppearanceRowOffset, body_font, palette.text);

    if (!model.notice.empty()) {
        Text(
            canvas, model.notice, 32.0F,
            std::min(height - 14.0F, 646.0F + kAppearanceRowOffset),
            detail_font, palette.warning);
    }

    if (model.language_menu_open) {
        const int maximum_scroll = std::max(
            0, static_cast<int>(app::kLanguageOptions.size() - kVisibleLanguageRows));
        const int scroll = std::clamp(model.language_menu_scroll, 0, maximum_scroll);
        layout.language_menu = SkRect::MakeXYWH(
            32.0F, 174.0F, width - 64.0F,
            12.0F + static_cast<float>(kVisibleLanguageRows) * 32.0F);
        canvas.drawRoundRect(layout.language_menu, 9.0F, 9.0F, Fill(palette.surface));
        Outline(canvas, layout.language_menu, 9.0F, palette);
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
                canvas.drawRoundRect(option, 6.0F, 6.0F, Fill(palette.selected_hover));
            } else if (app::kLanguageOptions[index].code ==
                       app::CanonicalLanguageCode(model.settings.language)) {
                canvas.drawRoundRect(option, 6.0F, 6.0F, Fill(palette.selected));
            }
            Text(
                canvas, app::kLanguageOptions[index].label,
                option.left() + 10.0F, option.centerY() + 4.5F, body_font, palette.text);
        }
        const float track_height = layout.language_menu.height() - 20.0F;
        const float thumb_height = track_height * static_cast<float>(kVisibleLanguageRows) /
            static_cast<float>(app::kLanguageOptions.size());
        const float thumb_y = layout.language_menu.top() + 10.0F +
            (track_height - thumb_height) * static_cast<float>(scroll) /
            static_cast<float>(std::max(maximum_scroll, 1));
        canvas.drawRoundRect(
            SkRect::MakeXYWH(layout.language_menu.right() - 9.0F, thumb_y, 3.0F, thumb_height),
            1.5F, 1.5F, Fill(palette.muted));
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
    if (layout.overlay_glass.contains(x, y)) return SettingsAction::OverlayGlass;
    if (layout.overlay_solid.contains(x, y)) return SettingsAction::OverlaySolid;
    if (layout.theme_dark.contains(x, y)) return SettingsAction::ThemeDark;
    if (layout.theme_light.contains(x, y)) return SettingsAction::ThemeLight;
    if (!device_controls_enabled) return SettingsAction::NoAction;
    if (layout.asr_cpu.contains(x, y)) return SettingsAction::AsrCpu;
    if (layout.asr_gpu.contains(x, y)) return SettingsAction::AsrGpu;
    if (layout.rewrite_cpu.contains(x, y)) return SettingsAction::RewriteCpu;
    if (layout.rewrite_gpu.contains(x, y)) return SettingsAction::RewriteGpu;
    return SettingsAction::NoAction;
}

} // namespace dictscribe::ui
