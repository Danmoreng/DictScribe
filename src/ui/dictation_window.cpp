#include "ui/dictation_window.hpp"

#include "app/app_controller.hpp"
#include "app/settings.hpp"
#include "platform/linux/linux_x11.hpp"
#include "ui/settings_view.hpp"
#include "ui/skia_surface.hpp"
#include "ui/text_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "GLFW/glfw3.h"
#define GLFW_EXPOSE_NATIVE_X11
#include "GLFW/glfw3native.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

namespace dictscribe::ui {

namespace {

constexpr int kLogicalWidth = 720;
constexpr int kMinimumLogicalHeight = 244;
constexpr int kMaximumLogicalHeight = 460;
constexpr float kHeaderHeight = 52.0F;
constexpr float kFooterHeight = 54.0F;
constexpr float kBodyTop = 75.0F;
constexpr float kBodyLineHeight = 23.0F;

constexpr SkColor kBackground = SkColorSetARGB(242, 14, 17, 23);
constexpr SkColor kSurface = SkColorSetARGB(250, 20, 24, 32);
constexpr SkColor kElevated = SkColorSetRGB(27, 32, 42);
constexpr SkColor kBorder = SkColorSetRGB(61, 69, 84);
constexpr SkColor kText = SkColorSetRGB(244, 246, 250);
constexpr SkColor kMuted = SkColorSetRGB(158, 167, 184);
constexpr SkColor kSubtle = SkColorSetRGB(107, 117, 136);
constexpr SkColor kAccent = SkColorSetRGB(139, 124, 255);
constexpr SkColor kRecording = SkColorSetRGB(255, 83, 112);
constexpr SkColor kSuccess = SkColorSetRGB(73, 214, 158);
constexpr SkColor kWarning = SkColorSetRGB(255, 186, 85);

struct WindowState {
    app::AppController* controller = nullptr;
    GLFWwindow* window = nullptr;
    GLFWwindow* settings_window = nullptr;
    app::AppSettings* settings = nullptr;
    linux_x11::X11Desktop desktop;
    linux_x11::TargetContext target;
    app::AppSnapshot snapshot;
    std::array<float, 16> level_history{};
    SkRect language_badge = SkRect::MakeEmpty();
    SkRect settings_button = SkRect::MakeEmpty();
    SettingsViewLayout settings_layout;
    SettingsViewModel settings_model;
    app::PendingDeviceSettings pending_devices;
    SkRect language_menu = SkRect::MakeEmpty();
    bool language_menu_open = false;
    bool session_active = false;
    bool dragging = false;
    int drag_window_x = 0;
    int drag_window_y = 0;
    int drag_pointer_x = 0;
    int drag_pointer_y = 0;
    int scroll_line = 0;
    int max_scroll_line = 0;
    int visible_line_count = 1;
    int rendered_height = kMinimumLogicalHeight;
};

SkPaint Fill(SkColor color) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    paint.setStyle(SkPaint::kFill_Style);
    return paint;
}

float TextWidth(const SkFont& font, std::string_view text) {
    return font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
}

void DrawText(
    SkCanvas& canvas,
    std::string_view text,
    float x,
    float baseline,
    const SkFont& font,
    SkColor color) {
    canvas.drawSimpleText(
        text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, Fill(color));
}

std::vector<std::string> WrapText(
    const std::string& text,
    const SkFont& font,
    float max_width) {
    std::vector<std::string> lines;
    for (const std::string& paragraph : SplitExplicitLines(text)) {
        if (paragraph.empty()) {
            lines.emplace_back();
            continue;
        }
        std::istringstream words(paragraph);
        std::string word;
        std::string current;
        while (words >> word) {
            const std::string candidate = current.empty() ? word : current + " " + word;
            if (!current.empty() && TextWidth(font, candidate) > max_width) {
                lines.push_back(current);
                current = word;
            } else {
                current = candidate;
            }
        }
        if (!current.empty()) lines.push_back(current);
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string DisplayText(const app::AppSnapshot& snapshot) {
    if (snapshot.mode == app::DictationMode::StartingRecording ||
        snapshot.mode == app::DictationMode::Recording ||
        snapshot.mode == app::DictationMode::Finalizing) {
        return snapshot.live_text;
    }
    return !snapshot.rewritten_text.empty() ? snapshot.rewritten_text : snapshot.live_text;
}

std::string PlaceholderText(const app::AppSnapshot& snapshot) {
    if (snapshot.mode == app::DictationMode::Starting) {
        return "Preparing the local speech and rewrite models...";
    }
    if (snapshot.mode == app::DictationMode::Ready ||
        snapshot.mode == app::DictationMode::Complete) {
        return "Press Ctrl+Alt+Space in any text field to start dictation.";
    }
    if (snapshot.mode == app::DictationMode::Error && !snapshot.error.empty()) {
        return snapshot.error;
    }
    return "Speak naturally. Your words will appear here.";
}

const char* LanguageBadge(const app::AppSnapshot& snapshot) {
    if (snapshot.language == "de") return "DE";
    if (snapshot.language == "en") return "EN";
    return "AUTO";
}

const char* StatusLabel(const app::AppSnapshot& snapshot) {
    if ((snapshot.mode == app::DictationMode::StartingRecording ||
         snapshot.mode == app::DictationMode::Finalizing) &&
        snapshot.status.find("Switching") != std::string::npos) {
        return "Switching language";
    }
    switch (snapshot.mode) {
    case app::DictationMode::Starting: return "Loading local models";
    case app::DictationMode::Ready: return "Ready";
    case app::DictationMode::StartingRecording: return "Opening microphone";
    case app::DictationMode::Recording: return "Listening";
    case app::DictationMode::Finalizing: return "Finalizing speech";
    case app::DictationMode::Complete: return "Complete";
    case app::DictationMode::Cancelling: return "Cancelling";
    case app::DictationMode::Error: return "Needs attention";
    }
    return "DictScribe";
}

SkColor StatusColor(app::DictationMode mode) {
    if (mode == app::DictationMode::Recording || mode == app::DictationMode::StartingRecording) {
        return kRecording;
    }
    if (mode == app::DictationMode::Ready || mode == app::DictationMode::Complete) {
        return kSuccess;
    }
    if (mode == app::DictationMode::Error) return kWarning;
    return kAccent;
}

sk_sp<SkTypeface> FindTypeface(const sk_sp<SkFontMgr>& manager, SkFontStyle style) {
    for (const char* family : {"Inter", "Noto Sans", "DejaVu Sans", "sans-serif"}) {
        if (auto typeface = manager->matchFamilyStyle(family, style)) return typeface;
    }
    return manager->matchFamilyStyle(nullptr, style);
}

std::vector<std::string> WrappedBody(
    const app::AppSnapshot& snapshot,
    const sk_sp<SkTypeface>& regular) {
    const SkFont body_font(regular, 16.5F);
    const std::string display = DisplayText(snapshot);
    const std::string body = display.empty() ? PlaceholderText(snapshot) : display;
    return WrapText(body, body_font, static_cast<float>(kLogicalWidth) - 78.0F);
}

int DesiredHeight(const app::AppSnapshot& snapshot, const sk_sp<SkTypeface>& regular) {
    const int lines = std::clamp(
        static_cast<int>(WrappedBody(snapshot, regular).size()), 3, 13);
    const int desired = static_cast<int>(
        kBodyTop + lines * kBodyLineHeight + 16.0F + kFooterHeight);
    return std::clamp(desired, kMinimumLogicalHeight, kMaximumLogicalHeight);
}

void ConfigureOverlayWindow(GLFWwindow* window) {
    Display* display = glfwGetX11Display();
    const Window xwindow = glfwGetX11Window(window);
    if (!display || xwindow == None) return;

    XWMHints hints{};
    hints.flags = InputHint;
    hints.input = False;
    XSetWMHints(display, xwindow, &hints);

    const Atom window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom notification = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(
        display,
        xwindow,
        window_type,
        XA_ATOM,
        32,
        PropModeReplace,
        reinterpret_cast<const unsigned char*>(&notification),
        1);

    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const std::array<Atom, 3> states = {
        XInternAtom(display, "_NET_WM_STATE_ABOVE", False),
        XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False),
    };
    XChangeProperty(
        display,
        xwindow,
        state,
        XA_ATOM,
        32,
        PropModeReplace,
        reinterpret_cast<const unsigned char*>(states.data()),
        static_cast<int>(states.size()));
    XFlush(display);
}

SkRect LanguageBadgeRect(const app::AppSnapshot& snapshot) {
    const float width = std::string_view(LanguageBadge(snapshot)) == "AUTO" ? 62.0F : 52.0F;
    return SkRect::MakeXYWH(static_cast<float>(kLogicalWidth) - 150.0F - width, 11.0F, width, 30.0F);
}

SkRect LanguageMenuRect(const app::AppSnapshot& snapshot) {
    const SkRect badge = LanguageBadgeRect(snapshot);
    return SkRect::MakeXYWH(badge.right() - 184.0F, badge.bottom() + 6.0F, 184.0F, 114.0F);
}

SkRect LanguageOptionRect(const app::AppSnapshot& snapshot, int option) {
    const SkRect menu = LanguageMenuRect(snapshot);
    return SkRect::MakeXYWH(
        menu.left() + 6.0F,
        menu.top() + 6.0F + static_cast<float>(option) * 34.0F,
        menu.width() - 12.0F,
        30.0F);
}

void SelectLanguage(WindowState& state, int option) {
    const char* language = option == 1 ? "de" : (option == 2 ? "en" : "auto");
    state.controller->set_language(language);
    state.settings->language = state.controller->snapshot().language;
    std::string error;
    if (!app::SaveSettings(*state.settings, error)) {
        state.settings_model.notice = std::move(error);
    } else {
        state.settings_model.notice.clear();
    }
}

void ShowSettings(WindowState& state) {
    if (!state.settings_window) return;
    glfwShowWindow(state.settings_window);
    glfwFocusWindow(state.settings_window);
}

void ApplySettingsAction(WindowState& state, SettingsAction action) {
    if (action == SettingsAction::Close) {
        glfwHideWindow(state.settings_window);
        return;
    }
    if (action == SettingsAction::LanguageAuto ||
        action == SettingsAction::LanguageGerman ||
        action == SettingsAction::LanguageEnglish) {
        SelectLanguage(
            state,
            action == SettingsAction::LanguageGerman ? 1 :
                (action == SettingsAction::LanguageEnglish ? 2 : 0));
        return;
    }
    if (action == SettingsAction::CleanupOff || action == SettingsAction::CleanupAi) {
        const app::CleanupMode mode = action == SettingsAction::CleanupAi
            ? app::CleanupMode::Ai : app::CleanupMode::Off;
        if (state.controller->set_cleanup_mode(mode)) {
            state.settings->cleanup_mode = mode;
            std::string error;
            if (!app::SaveSettings(*state.settings, error)) {
                state.settings_model.notice = std::move(error);
            } else {
                state.settings_model.notice.clear();
            }
        }
        return;
    }

    bool changed = false;
    if (action == SettingsAction::AsrCpu || action == SettingsAction::AsrGpu) {
        const bool gpu = action == SettingsAction::AsrGpu;
        changed = state.controller->set_asr_device(gpu);
        if (changed) {
            state.pending_devices.asr_device = gpu
                ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        }
    } else if (action == SettingsAction::RewriteCpu || action == SettingsAction::RewriteGpu) {
        const bool gpu = action == SettingsAction::RewriteGpu;
        changed = state.controller->set_rewrite_device(gpu);
        if (changed) {
            state.pending_devices.rewrite_device = gpu
                ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        }
    }
}

void ShowNearTarget(WindowState& state, const sk_sp<SkTypeface>& regular) {
    const int height = DesiredHeight(state.controller->snapshot(), regular);
    int x = state.settings->overlay_position
        ? state.settings->overlay_position->x
        : state.target.anchor_x - kLogicalWidth / 2;
    int y = state.settings->overlay_position
        ? state.settings->overlay_position->y
        : state.target.anchor_y - height - 20;

    int monitor_count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);
    GLFWmonitor* chosen = glfwGetPrimaryMonitor();
    const int placement_anchor_x = state.settings->overlay_position
        ? x + kLogicalWidth / 2
        : state.target.anchor_x;
    const int placement_anchor_y = state.settings->overlay_position
        ? y + height / 2
        : state.target.anchor_y;
    for (int index = 0; index < monitor_count; ++index) {
        int mx = 0;
        int my = 0;
        int mw = 0;
        int mh = 0;
        glfwGetMonitorWorkarea(monitors[index], &mx, &my, &mw, &mh);
        if (placement_anchor_x >= mx && placement_anchor_x < mx + mw &&
            placement_anchor_y >= my && placement_anchor_y < my + mh) {
            chosen = monitors[index];
            break;
        }
    }

    int mx = 0;
    int my = 0;
    int mw = kLogicalWidth;
    int mh = height;
    if (chosen) glfwGetMonitorWorkarea(chosen, &mx, &my, &mw, &mh);
    if (!state.settings->overlay_position && y < my + 8) y = state.target.anchor_y + 20;
    x = std::clamp(x, mx + 8, std::max(mx + 8, mx + mw - kLogicalWidth - 8));
    y = std::clamp(y, my + 8, std::max(my + 8, my + mh - height - 8));

    state.rendered_height = height;
    glfwSetWindowSize(state.window, kLogicalWidth, height);
    glfwSetWindowPos(state.window, x, y);
    glfwShowWindow(state.window);
}

void HideOverlay(WindowState& state) {
    state.language_menu_open = false;
    state.dragging = false;
    glfwHideWindow(state.window);
}

void ToggleDictation(WindowState& state, const sk_sp<SkTypeface>& regular) {
    const app::AppSnapshot snapshot = state.controller->snapshot();
    if (snapshot.mode == app::DictationMode::Ready ||
        snapshot.mode == app::DictationMode::Complete) {
        const auto candidate = state.desktop.capture_target();
        if (candidate.valid()) state.target = candidate;
        state.session_active = true;
        state.scroll_line = 0;
        state.desktop.set_session_hotkeys(true);
        ShowNearTarget(state, regular);
        state.controller->toggle_recording();
    } else if (snapshot.mode == app::DictationMode::Recording) {
        state.controller->toggle_recording();
    } else if (snapshot.mode == app::DictationMode::Starting ||
               snapshot.mode == app::DictationMode::Error) {
        const auto candidate = state.desktop.capture_target();
        if (candidate.valid()) state.target = candidate;
        ShowNearTarget(state, regular);
    }
}

void CancelDictation(WindowState& state) {
    if (!app::CanCancel(state.controller->snapshot())) return;
    state.controller->cancel_recording();
    state.session_active = false;
    state.desktop.set_session_hotkeys(false);
    HideOverlay(state);
}

void UpdateLevelHistory(WindowState& state) {
    std::move(
        state.level_history.begin() + 1,
        state.level_history.end(),
        state.level_history.begin());
    if (state.snapshot.mode != app::DictationMode::Recording) {
        state.level_history.back() = 0.0F;
        return;
    }
    const float source = std::max(
        state.snapshot.audio_rms, state.snapshot.audio_peak * 0.35F);
    const float decibels = 20.0F * std::log10(std::max(source, 0.00001F));
    state.level_history.back() = std::clamp((decibels + 55.0F) / 43.0F, 0.0F, 1.0F);
}

void Render(
    SkiaSurface& surface,
    WindowState& state,
    const sk_sp<SkTypeface>& regular,
    const sk_sp<SkTypeface>& semibold) {
    if (!surface.ensure_size(state.window) || !surface.surface()) return;

    int width = 0;
    int height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetWindowSize(state.window, &width, &height);
    glfwGetFramebufferSize(state.window, &framebuffer_width, &framebuffer_height);

    SkCanvas* canvas = surface.surface()->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->save();
    canvas->scale(
        static_cast<float>(framebuffer_width) / std::max(width, 1),
        static_cast<float>(framebuffer_height) / std::max(height, 1));

    const float logical_width = static_cast<float>(width);
    const float logical_height = static_cast<float>(height);
    canvas->clipRRect(
        SkRRect::MakeRectXY(SkRect::MakeWH(logical_width, logical_height), 8.0F, 8.0F),
        true);
    canvas->drawRect(SkRect::MakeWH(logical_width, logical_height), Fill(kBackground));

    const SkFont status_font(semibold, 13.0F);
    const SkFont body_font(regular, 16.5F);
    const SkFont badge_font(semibold, 11.5F);
    const SkFont hint_font(regular, 12.0F);
    const SkFont key_font(semibold, 11.5F);
    const SkFont menu_font(regular, 12.5F);
    SkPaint border = Fill(kBorder);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(1.0F);

    const SkColor status_color = StatusColor(state.snapshot.mode);
    canvas->drawCircle(28.0F, 26.0F, 5.0F, Fill(status_color));
    DrawText(*canvas, StatusLabel(state.snapshot), 43.0F, 31.0F, status_font, kText);

    state.language_badge = LanguageBadgeRect(state.snapshot);
    canvas->drawRoundRect(state.language_badge, 7.0F, 7.0F, Fill(kElevated));
    canvas->drawRoundRect(
        SkRect::MakeXYWH(
            state.language_badge.left() + 0.5F,
            state.language_badge.top() + 0.5F,
            state.language_badge.width() - 1.0F,
            state.language_badge.height() - 1.0F),
        7.0F,
        7.0F,
        border);
    DrawText(
        *canvas,
        LanguageBadge(state.snapshot),
        state.language_badge.left() + 9.0F,
        30.5F,
        badge_font,
        kMuted);
    const float chevron_x = state.language_badge.right() - 12.0F;
    SkPaint chevron = Fill(kMuted);
    chevron.setStyle(SkPaint::kStroke_Style);
    chevron.setStrokeWidth(1.4F);
    if (state.language_menu_open) {
        canvas->drawLine(chevron_x - 3.0F, 27.0F, chevron_x, 24.0F, chevron);
        canvas->drawLine(chevron_x, 24.0F, chevron_x + 3.0F, 27.0F, chevron);
    } else {
        canvas->drawLine(chevron_x - 3.0F, 24.0F, chevron_x, 27.0F, chevron);
        canvas->drawLine(chevron_x, 27.0F, chevron_x + 3.0F, 24.0F, chevron);
    }

    state.settings_button = SkRect::MakeXYWH(410.0F, 11.0F, 88.0F, 30.0F);
    canvas->drawRoundRect(state.settings_button, 7.0F, 7.0F, Fill(kElevated));
    canvas->drawRoundRect(state.settings_button, 7.0F, 7.0F, border);
    DrawText(*canvas, "Settings", 426.0F, 30.5F, hint_font, kMuted);

    const float meter_start_x = logical_width - 132.0F;
    for (std::size_t index = 0; index < state.level_history.size(); ++index) {
        const float level = state.level_history[index];
        const float bar_height = 3.0F + level * 23.0F;
        const float x = meter_start_x + static_cast<float>(index) * 6.6F;
        canvas->drawRoundRect(
            SkRect::MakeXYWH(x, 26.0F - bar_height * 0.5F, 3.0F, bar_height),
            1.5F,
            1.5F,
            Fill(level > 0.08F ? status_color : kSubtle));
    }
    canvas->drawLine(24.0F, kHeaderHeight - 1.0F, logical_width - 24.0F, kHeaderHeight - 1.0F, border);

    const std::string display = DisplayText(state.snapshot);
    const std::string body = display.empty() ? PlaceholderText(state.snapshot) : display;
    const auto lines = WrapText(body, body_font, logical_width - 78.0F);
    const float body_bottom = logical_height - kFooterHeight - 16.0F;
    state.visible_line_count = std::max(
        1, static_cast<int>(std::floor((body_bottom - kBodyTop) / kBodyLineHeight)));
    state.max_scroll_line = std::max(0, static_cast<int>(lines.size()) - state.visible_line_count);
    state.scroll_line = std::clamp(state.scroll_line, 0, state.max_scroll_line);
    if (state.snapshot.mode == app::DictationMode::Recording) {
        state.scroll_line = state.max_scroll_line;
    }

    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(24.0F, kBodyTop - 16.0F, logical_width - 30.0F, body_bottom));
    float baseline = kBodyTop;
    const int last_line = std::min(
        static_cast<int>(lines.size()), state.scroll_line + state.visible_line_count);
    for (int index = state.scroll_line; index < last_line; ++index) {
        DrawText(
            *canvas,
            lines[static_cast<std::size_t>(index)],
            28.0F,
            baseline,
            body_font,
            state.snapshot.mode == app::DictationMode::Error ? kWarning :
                (display.empty() ? kMuted : kText));
        baseline += kBodyLineHeight;
    }
    canvas->restore();

    if (state.max_scroll_line > 0) {
        const float track_top = kBodyTop - 14.0F;
        const float track_bottom = body_bottom - 2.0F;
        const float track_height = track_bottom - track_top;
        const float ratio = static_cast<float>(state.visible_line_count) /
            static_cast<float>(lines.size());
        const float thumb_height = std::max(34.0F, track_height * ratio);
        const float available = track_height - thumb_height;
        const float thumb_top = track_top + available *
            static_cast<float>(state.scroll_line) / static_cast<float>(state.max_scroll_line);
        canvas->drawRoundRect(
            SkRect::MakeXYWH(logical_width - 18.0F, track_top, 3.0F, track_height),
            1.5F,
            1.5F,
            Fill(kSurface));
        canvas->drawRoundRect(
            SkRect::MakeXYWH(logical_width - 19.0F, thumb_top, 5.0F, thumb_height),
            2.5F,
            2.5F,
            Fill(kMuted));
    }

    const float footer_top = logical_height - kFooterHeight;
    canvas->drawRect(
        SkRect::MakeXYWH(0.0F, footer_top, logical_width, kFooterHeight), Fill(kSurface));
    const auto draw_keycap = [&](float x, float key_width, std::string_view key) {
        const float y = footer_top + 12.0F;
        canvas->drawRoundRect(SkRect::MakeXYWH(x, y, key_width, 30.0F), 7.0F, 7.0F, Fill(kElevated));
        canvas->drawRoundRect(
            SkRect::MakeXYWH(x + 0.5F, y + 0.5F, key_width - 1.0F, 29.0F),
            7.0F,
            7.0F,
            border);
        DrawText(
            *canvas,
            key,
            x + (key_width - TextWidth(key_font, key)) * 0.5F,
            y + 20.0F,
            key_font,
            kText);
    };
    const float key_y = footer_top + 12.0F;
    if (state.snapshot.mode == app::DictationMode::Recording) {
        draw_keycap(24.0F, 58.0F, "Enter");
        DrawText(*canvas, "Finish & insert", 94.0F, key_y + 20.0F, hint_font, kMuted);
        draw_keycap(logical_width - 174.0F, 44.0F, "Esc");
        DrawText(*canvas, "Cancel", logical_width - 118.0F, key_y + 20.0F, hint_font, kMuted);
    } else if (state.snapshot.mode == app::DictationMode::Finalizing) {
        const char* progress = state.snapshot.status.find("Switching") != std::string::npos
            ? "Switching language..."
            : "Finalizing locally...";
        DrawText(*canvas, progress, 28.0F, key_y + 20.0F, hint_font, kMuted);
    } else {
        draw_keycap(24.0F, 124.0F, "Ctrl Alt Space");
        DrawText(*canvas, "Start dictation", 160.0F, key_y + 20.0F, hint_font, kMuted);
    }

    if (state.language_menu_open) {
        state.language_menu = LanguageMenuRect(state.snapshot);
        canvas->drawRoundRect(state.language_menu, 9.0F, 9.0F, Fill(kElevated));
        canvas->drawRoundRect(
            SkRect::MakeXYWH(
                state.language_menu.left() + 0.5F,
                state.language_menu.top() + 0.5F,
                state.language_menu.width() - 1.0F,
                state.language_menu.height() - 1.0F),
            9.0F,
            9.0F,
            border);
        const std::array<std::string_view, 3> labels = {
            "Automatic detection", "Deutsch", "English"};
        const int selected = state.snapshot.language == "de" ? 1 :
            (state.snapshot.language == "en" ? 2 : 0);
        for (int option = 0; option < 3; ++option) {
            const SkRect item = LanguageOptionRect(state.snapshot, option);
            if (option == selected) {
                canvas->drawRoundRect(item, 6.0F, 6.0F, Fill(SkColorSetRGB(42, 39, 66)));
            }
            canvas->drawCircle(
                item.left() + 14.0F,
                item.centerY(),
                option == selected ? 3.5F : 2.0F,
                Fill(option == selected ? kAccent : kSubtle));
            DrawText(
                *canvas,
                labels[static_cast<std::size_t>(option)],
                item.left() + 27.0F,
                item.centerY() + 4.5F,
                menu_font,
                option == selected ? kText : kMuted);
        }
    } else {
        state.language_menu = SkRect::MakeEmpty();
    }

    canvas->drawRoundRect(
        SkRect::MakeXYWH(0.5F, 0.5F, logical_width - 1.0F, logical_height - 1.0F),
        8.0F,
        8.0F,
        border);
    canvas->restore();
    surface.present(state.window);
}

void MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state) return;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);

    if (action == GLFW_PRESS) {
        if (state->settings_button.contains(static_cast<float>(x), static_cast<float>(y))) {
            state->language_menu_open = false;
            ShowSettings(*state);
            return;
        }
        if (state->language_badge.contains(static_cast<float>(x), static_cast<float>(y))) {
            state->language_menu_open = !state->language_menu_open;
            return;
        }
        if (state->language_menu_open) {
            for (int option = 0; option < 3; ++option) {
                if (LanguageOptionRect(state->snapshot, option).contains(
                        static_cast<float>(x), static_cast<float>(y))) {
                    SelectLanguage(*state, option);
                    state->language_menu_open = false;
                    return;
                }
            }
            state->language_menu_open = false;
        }
        if (y < kHeaderHeight) {
            state->dragging = state->desktop.pointer_position(
                state->drag_pointer_x, state->drag_pointer_y);
            if (state->dragging) {
                glfwGetWindowPos(window, &state->drag_window_x, &state->drag_window_y);
            }
        }
    } else if (action == GLFW_RELEASE) {
        if (state->dragging) {
            int window_x = 0;
            int window_y = 0;
            glfwGetWindowPos(window, &window_x, &window_y);
            state->settings->overlay_position = app::ScreenPosition{window_x, window_y};
            std::string error;
            if (!app::SaveSettings(*state->settings, error)) {
                state->settings_model.notice = std::move(error);
            } else {
                state->settings_model.notice.clear();
            }
        }
        state->dragging = false;
    }
}

void SettingsMouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_RELEASE) return;
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state) return;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    ApplySettingsAction(
        *state,
        HitTestSettingsView(
            state->settings_layout,
            static_cast<float>(x),
            static_cast<float>(y),
            state->settings_model.device_controls_enabled));
}

void SettingsCloseCallback(GLFWwindow* window) {
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    glfwHideWindow(window);
}

void RenderSettings(
    SkiaSurface& surface,
    WindowState& state,
    const sk_sp<SkTypeface>& regular,
    const sk_sp<SkTypeface>& semibold) {
    if (!surface.ensure_size(state.settings_window) || !surface.surface()) return;
    int width = 0;
    int height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetWindowSize(state.settings_window, &width, &height);
    glfwGetFramebufferSize(state.settings_window, &framebuffer_width, &framebuffer_height);
    SkCanvas* canvas = surface.surface()->getCanvas();
    canvas->save();
    canvas->scale(
        static_cast<float>(framebuffer_width) / std::max(width, 1),
        static_cast<float>(framebuffer_height) / std::max(height, 1));
    state.settings_layout = RenderSettingsView(
        *canvas,
        static_cast<float>(width),
        static_cast<float>(height),
        SkFont(regular, 13.0F),
        SkFont(semibold, 13.0F),
        state.settings_model);
    canvas->restore();
    surface.present(state.settings_window);
}

void CursorPositionCallback(GLFWwindow* window, double, double) {
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state || !state->dragging) return;
    int pointer_x = 0;
    int pointer_y = 0;
    if (!state->desktop.pointer_position(pointer_x, pointer_y)) return;
    glfwSetWindowPos(
        window,
        state->drag_window_x + pointer_x - state->drag_pointer_x,
        state->drag_window_y + pointer_y - state->drag_pointer_y);
}

void ScrollCallback(GLFWwindow* window, double, double y_offset) {
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state || state->max_scroll_line <= 0) return;
    state->scroll_line = std::clamp(
        state->scroll_line - static_cast<int>(std::round(y_offset * 2.0)),
        0,
        state->max_scroll_line);
}

} // namespace

int RunDictationWindow(
    app::AppController& controller,
    app::AppSettings& settings) {
    glfwSetErrorCallback([](int, const char* description) {
        std::cerr << "GLFW: " << description << '\n';
    });
    if (!glfwInit()) {
        std::cerr << "Could not initialize GLFW. Is an X11 graphical session available?\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "dictscribe");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "dictscribe");
    GLFWwindow* window = glfwCreateWindow(
        kLogicalWidth, kMinimumLogicalHeight, "DictScribe", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    ConfigureOverlayWindow(window);

    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    GLFWwindow* settings_window = glfwCreateWindow(
        640, 660, "DictScribe Settings", nullptr, window);
    if (!settings_window) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    SkiaSurface surface;
    if (!surface.initialize(window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    glfwSwapInterval(1);
    SkiaSurface settings_surface;
    if (!settings_surface.initialize(settings_window)) {
        surface.shutdown();
        glfwDestroyWindow(settings_window);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    glfwSwapInterval(1);

    const auto font_manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    const auto regular = font_manager ? FindTypeface(font_manager, SkFontStyle::Normal()) : nullptr;
    const auto semibold = font_manager ? FindTypeface(font_manager, SkFontStyle::Bold()) : nullptr;
    if (!regular || !semibold) {
        std::cerr << "Could not initialize a system typeface.\n";
        surface.shutdown();
        settings_surface.shutdown();
        glfwDestroyWindow(settings_window);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    WindowState state;
    state.controller = &controller;
    state.window = window;
    state.settings_window = settings_window;
    state.settings = &settings;
    state.snapshot = controller.snapshot();
    std::string desktop_error;
    if (!state.desktop.initialize(glfwGetX11Window(window), desktop_error)) {
        std::cerr << desktop_error << '\n';
        surface.shutdown();
        settings_surface.shutdown();
        glfwDestroyWindow(settings_window);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    state.desktop.set_settings_window(glfwGetX11Window(settings_window));
    if (settings.overlay_position) {
        glfwSetWindowPos(window, settings.overlay_position->x, settings.overlay_position->y);
    }

    glfwSetWindowUserPointer(window, &state);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPositionCallback);
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetWindowUserPointer(settings_window, &state);
    glfwSetMouseButtonCallback(settings_window, SettingsMouseButtonCallback);
    glfwSetWindowCloseCallback(settings_window, SettingsCloseCallback);
    std::cerr << "DictScribe is ready in the background. Ctrl+Alt+Space toggles dictation; "
                 "Ctrl+Alt+Q quits.\n";

    while (!glfwWindowShouldClose(window)) {
        for (const auto command : state.desktop.poll_commands()) {
            switch (command) {
            case linux_x11::DesktopCommand::Toggle:
            case linux_x11::DesktopCommand::Accept:
                if (state.session_active) {
                    const auto candidate = state.desktop.capture_target();
                    if (candidate.valid()) state.target = candidate;
                }
                ToggleDictation(state, regular);
                break;
            case linux_x11::DesktopCommand::Cancel:
                CancelDictation(state);
                break;
            case linux_x11::DesktopCommand::Quit:
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                break;
            }
        }

        if (state.session_active) {
            const auto candidate = state.desktop.capture_target();
            if (candidate.valid()) state.target = candidate;
        }
        controller.tick();
        state.snapshot = controller.snapshot();
        if (app::ReconcilePendingDeviceSettings(
                state.snapshot,
                state.pending_devices,
                settings,
                state.settings_model.notice)) {
            std::string error;
            if (!app::SaveSettings(settings, error)) {
                state.settings_model.notice = std::move(error);
            }
        }
        state.settings_model.settings = settings;
        state.settings_model.settings.language = state.snapshot.language;
        state.settings_model.settings.cleanup_mode = state.snapshot.cleanup_mode;
        state.settings_model.settings.asr_device = state.snapshot.asr_use_gpu
            ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        state.settings_model.settings.rewrite_device = state.snapshot.rewrite_use_gpu
            ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        state.settings_model.asr_model_name = state.snapshot.asr_model_name;
        state.settings_model.rewrite_model_name = state.snapshot.rewrite_model_name;
        state.settings_model.device_controls_enabled = app::CanSetComputeDevice(state.snapshot);
        UpdateLevelHistory(state);

        if (state.session_active && state.snapshot.mode == app::DictationMode::Complete) {
            state.session_active = false;
            state.desktop.set_session_hotkeys(false);
            HideOverlay(state);
            const std::string& text = !state.snapshot.rewritten_text.empty()
                ? state.snapshot.rewritten_text
                : (!state.snapshot.raw_final_text.empty()
                    ? state.snapshot.raw_final_text
                    : state.snapshot.live_text);
            std::string error;
            if (!state.desktop.insert_text(state.target, text, error)) {
                std::cerr << "DictScribe insertion: " << error << '\n';
            }
        } else if (state.session_active && state.snapshot.mode == app::DictationMode::Error) {
            state.session_active = false;
            state.desktop.set_session_hotkeys(false);
            HideOverlay(state);
        }

        if (glfwGetWindowAttrib(window, GLFW_VISIBLE)) {
            const int desired_height = DesiredHeight(state.snapshot, regular);
            if (desired_height != state.rendered_height) {
                state.rendered_height = desired_height;
                glfwSetWindowSize(window, kLogicalWidth, desired_height);
            }
            Render(surface, state, regular, semibold);
        }
        if (glfwGetWindowAttrib(settings_window, GLFW_VISIBLE)) {
            RenderSettings(settings_surface, state, regular, semibold);
        }
        glfwWaitEventsTimeout(1.0 / 60.0);
    }

    state.desktop.shutdown();
    surface.shutdown();
    settings_surface.shutdown();
    glfwDestroyWindow(settings_window);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace dictscribe::ui
