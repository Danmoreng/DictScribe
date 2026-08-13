#include "ui/settings_view.hpp"

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
    const float language_width = (width - 64.0F - 16.0F) / 3.0F;
    layout.language_auto = SkRect::MakeXYWH(32.0F, 128.0F, language_width, 38.0F);
    layout.language_german = SkRect::MakeXYWH(
        40.0F + language_width, 128.0F, language_width, 38.0F);
    layout.language_english = SkRect::MakeXYWH(
        48.0F + language_width * 2.0F, 128.0F, language_width, 38.0F);
    SegmentedButton(
        canvas, layout.language_auto, "Automatic", model.settings.language == "auto", true, control_font);
    SegmentedButton(
        canvas, layout.language_german, "Deutsch", model.settings.language == "de", true, control_font);
    SegmentedButton(
        canvas, layout.language_english, "English", model.settings.language == "en", true, control_font);

    Text(canvas, "COMPUTE DEVICE", 32.0F, 215.0F, section_font, kMuted);
    Text(canvas, "Speech recognition", 32.0F, 247.0F, section_font, kText);
    Text(canvas, "Nemotron ASR worker", 32.0F, 267.0F, detail_font, kMuted);
    layout.asr_cpu = SkRect::MakeXYWH(width - 216.0F, 228.0F, 84.0F, 38.0F);
    layout.asr_gpu = SkRect::MakeXYWH(width - 124.0F, 228.0F, 92.0F, 38.0F);
    SegmentedButton(
        canvas, layout.asr_cpu, "CPU", model.settings.asr_device == app::ComputeDevice::Cpu,
        model.device_controls_enabled, control_font);
    SegmentedButton(
        canvas, layout.asr_gpu, "GPU", model.settings.asr_device == app::ComputeDevice::Gpu,
        model.device_controls_enabled, control_font);

    Text(canvas, "AI cleanup", 32.0F, 309.0F, section_font, kText);
    Text(canvas, "llama.cpp rewrite worker", 32.0F, 329.0F, detail_font, kMuted);
    layout.rewrite_cpu = SkRect::MakeXYWH(width - 216.0F, 290.0F, 84.0F, 38.0F);
    layout.rewrite_gpu = SkRect::MakeXYWH(width - 124.0F, 290.0F, 92.0F, 38.0F);
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
        canvas, device_note, 32.0F, 365.0F, body_font,
        model.device_controls_enabled ? kMuted : kWarning);

    Text(canvas, "LOCAL MODELS", 32.0F, 418.0F, section_font, kMuted);
    canvas.drawRoundRect(
        SkRect::MakeXYWH(32.0F, 433.0F, width - 64.0F, 104.0F), 10.0F, 10.0F, Fill(kSurface));
    Outline(canvas, SkRect::MakeXYWH(32.0F, 433.0F, width - 64.0F, 104.0F), 10.0F);
    Text(canvas, "Speech recognition", 48.0F, 462.0F, detail_font, kMuted);
    Text(canvas, ShortModelName(model.asr_model_name), 48.0F, 481.0F, body_font, kText);
    Text(canvas, "AI cleanup", 48.0F, 510.0F, detail_font, kMuted);
    Text(canvas, ShortModelName(model.rewrite_model_name), 48.0F, 529.0F, body_font, kText);

    if (!model.notice.empty()) {
        Text(canvas, model.notice, 32.0F, std::min(height - 22.0F, 572.0F), detail_font, kWarning);
    }
    return layout;
}

SettingsAction HitTestSettingsView(
    const SettingsViewLayout& layout,
    float x,
    float y,
    bool device_controls_enabled) {
    if (layout.close.contains(x, y)) return SettingsAction::Close;
    if (layout.language_auto.contains(x, y)) return SettingsAction::LanguageAuto;
    if (layout.language_german.contains(x, y)) return SettingsAction::LanguageGerman;
    if (layout.language_english.contains(x, y)) return SettingsAction::LanguageEnglish;
    if (!device_controls_enabled) return SettingsAction::NoAction;
    if (layout.asr_cpu.contains(x, y)) return SettingsAction::AsrCpu;
    if (layout.asr_gpu.contains(x, y)) return SettingsAction::AsrGpu;
    if (layout.rewrite_cpu.contains(x, y)) return SettingsAction::RewriteCpu;
    if (layout.rewrite_gpu.contains(x, y)) return SettingsAction::RewriteGpu;
    return SettingsAction::NoAction;
}

} // namespace dictscribe::ui
