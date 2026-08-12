#include "ui/dictation_window.hpp"

#include "app/app_controller.hpp"
#include "ui/skia_surface.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "GLFW/glfw3.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

namespace dictscribe::ui {

namespace {

constexpr SkColor kBackground = SkColorSetRGB(13, 16, 23);
constexpr SkColor kPanel = SkColorSetRGB(22, 27, 37);
constexpr SkColor kPanelRaised = SkColorSetRGB(28, 34, 46);
constexpr SkColor kBorder = SkColorSetRGB(49, 58, 75);
constexpr SkColor kText = SkColorSetRGB(239, 243, 250);
constexpr SkColor kMuted = SkColorSetRGB(145, 156, 177);
constexpr SkColor kAccent = SkColorSetRGB(120, 103, 255);
constexpr SkColor kAccentBright = SkColorSetRGB(159, 146, 255);
constexpr SkColor kRecording = SkColorSetRGB(255, 82, 108);
constexpr SkColor kSuccess = SkColorSetRGB(61, 210, 151);
constexpr SkColor kWarning = SkColorSetRGB(255, 184, 77);

struct WindowState {
    app::AppController* controller = nullptr;
    SkRect primary_button = SkRect::MakeEmpty();
    SkRect cancel_button = SkRect::MakeEmpty();
    SkRect language_button = SkRect::MakeEmpty();
};

SkPaint Fill(SkColor color) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    paint.setStyle(SkPaint::kFill_Style);
    return paint;
}

float TextWidth(const SkFont& font, const std::string& text) {
    return font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
}

void DrawSimpleText(
    SkCanvas& canvas,
    const std::string& text,
    float x,
    float baseline,
    const SkFont& font,
    SkColor color) {
    SkPaint paint = Fill(color);
    canvas.drawSimpleText(
        text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, paint);
}

void DrawCenteredText(
    SkCanvas& canvas,
    const std::string& text,
    const SkRect& bounds,
    const SkFont& font,
    SkColor color,
    float baseline_adjustment = 5.0F) {
    const float x = bounds.centerX() - TextWidth(font, text) * 0.5F;
    const float y = bounds.centerY() + baseline_adjustment;
    DrawSimpleText(canvas, text, x, y, font, color);
}

std::vector<std::string> WrapText(
    const std::string& text,
    const SkFont& font,
    float max_width,
    std::size_t max_lines) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string word;
    std::string current;
    while (stream >> word) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && TextWidth(font, candidate) > max_width) {
            lines.push_back(current);
            current = word;
            if (lines.size() == max_lines) {
                break;
            }
        } else {
            current = candidate;
        }
    }
    if (lines.size() < max_lines && !current.empty()) {
        lines.push_back(current);
    }
    if (lines.size() == max_lines && stream.good()) {
        std::string& last = lines.back();
        while (!last.empty() && TextWidth(font, last + "...") > max_width) {
            last.pop_back();
        }
        last += "...";
    }
    return lines;
}

void DrawWrappedText(
    SkCanvas& canvas,
    const std::string& text,
    float x,
    float baseline,
    float max_width,
    float line_height,
    std::size_t max_lines,
    const SkFont& font,
    SkColor color) {
    for (const auto& line : WrapText(text, font, max_width, max_lines)) {
        DrawSimpleText(canvas, line, x, baseline, font, color);
        baseline += line_height;
    }
}

sk_sp<SkTypeface> FindTypeface(const sk_sp<SkFontMgr>& manager, SkFontStyle style) {
    for (const char* family : {"Inter", "Segoe UI", "Noto Sans", "DejaVu Sans", "sans-serif"}) {
        if (auto typeface = manager->matchFamilyStyle(family, style)) {
            return typeface;
        }
    }
    return manager->matchFamilyStyle(nullptr, style);
}

SkColor StatusColor(const app::AppSnapshot& snapshot) {
    if (snapshot.mode == app::DictationMode::Error) return kWarning;
    if (snapshot.mode == app::DictationMode::Recording) return kRecording;
    if (snapshot.mode == app::DictationMode::Ready ||
        snapshot.mode == app::DictationMode::Complete) return kSuccess;
    return kAccentBright;
}

const char* NextLanguage(const app::AppSnapshot& snapshot) {
    if (snapshot.language == "auto") return "de";
    if (snapshot.language == "de") return "en";
    return "auto";
}

void DrawStatusPill(
    SkCanvas& canvas,
    const std::string& label,
    bool ready,
    float right,
    float top,
    const SkFont& font) {
    const float width = TextWidth(font, label) + 36.0F;
    const SkRect pill = SkRect::MakeXYWH(right - width, top, width, 30.0F);
    canvas.drawRoundRect(pill, 15.0F, 15.0F, Fill(kPanelRaised));
    canvas.drawCircle(pill.left() + 15.0F, pill.centerY(), 4.0F, Fill(ready ? kSuccess : kWarning));
    DrawSimpleText(canvas, label, pill.left() + 25.0F, pill.top() + 20.0F, font, ready ? kText : kMuted);
}

void DrawTranscriptCard(
    SkCanvas& canvas,
    const SkRect& bounds,
    const char* label,
    const std::string& text,
    const std::string& placeholder,
    const SkFont& label_font,
    const SkFont& body_font,
    SkColor body_color,
    bool active,
    double time) {
    SkPaint panel = Fill(active ? kPanelRaised : kPanel);
    canvas.drawRoundRect(bounds, 18.0F, 18.0F, panel);
    SkPaint border = Fill(active ? kAccent : kBorder);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(active ? 1.5F : 1.0F);
    canvas.drawRoundRect(bounds, 18.0F, 18.0F, border);

    DrawSimpleText(canvas, label, bounds.left() + 24.0F, bounds.top() + 31.0F, label_font, active ? kAccentBright : kMuted);
    if (active) {
        const float pulse = static_cast<float>((std::sin(time * 5.0) + 1.0) * 0.5);
        const SkColor color = SkColorSetARGB(
            150 + static_cast<int>(pulse * 105.0F), 159, 146, 255);
        canvas.drawCircle(bounds.right() - 27.0F, bounds.top() + 25.0F, 4.0F + pulse, Fill(color));
    }

    const std::string& display = text.empty() ? placeholder : text;
    DrawWrappedText(
        canvas,
        display,
        bounds.left() + 24.0F,
        bounds.top() + 73.0F,
        bounds.width() - 48.0F,
        31.0F,
        4,
        body_font,
        text.empty() ? kMuted : body_color);
}

void Render(
    GLFWwindow* window,
    SkiaSurface& surface,
    WindowState& window_state,
    const sk_sp<SkTypeface>& regular,
    const sk_sp<SkTypeface>& bold) {
    if (!surface.ensure_size(window) || !surface.surface()) {
        return;
    }
    const app::AppSnapshot snapshot = window_state.controller->snapshot();
    SkCanvas* canvas = surface.surface()->getCanvas();
    int width = 0;
    int height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

    canvas->clear(kBackground);
    canvas->save();
    canvas->scale(
        static_cast<float>(framebuffer_width) / std::max(width, 1),
        static_cast<float>(framebuffer_height) / std::max(height, 1));

    const SkFont title_font(bold, 25.0F);
    const SkFont subtitle_font(regular, 13.0F);
    const SkFont status_font(regular, 14.0F);
    const SkFont pill_font(regular, 11.5F);
    const SkFont label_font(bold, 11.5F);
    const SkFont body_font(regular, 21.0F);
    const SkFont button_font(bold, 15.0F);
    const float margin = 34.0F;

    canvas->drawRoundRect(SkRect::MakeXYWH(margin, 28.0F, 48.0F, 48.0F), 14.0F, 14.0F, Fill(kAccent));
    DrawCenteredText(*canvas, "DS", SkRect::MakeXYWH(margin, 28.0F, 48.0F, 48.0F), button_font, kText, 5.0F);
    DrawSimpleText(*canvas, "DictScribe", margin + 64.0F, 51.0F, title_font, kText);
    DrawSimpleText(*canvas, "Local dictation preview", margin + 65.0F, 72.0F, subtitle_font, kMuted);

    float pill_right = static_cast<float>(width) - margin;
    DrawStatusPill(*canvas, snapshot.rewrite_ready ? "Rewrite ready" : "Rewrite loading", snapshot.rewrite_ready, pill_right, 34.0F, pill_font);
    pill_right -= TextWidth(pill_font, snapshot.rewrite_ready ? "Rewrite ready" : "Rewrite loading") + 48.0F;
    DrawStatusPill(*canvas, snapshot.asr_ready ? "Speech ready" : "Speech loading", snapshot.asr_ready, pill_right, 34.0F, pill_font);

    canvas->drawCircle(margin + 5.0F, 112.0F, 5.0F, Fill(StatusColor(snapshot)));
    DrawSimpleText(*canvas, snapshot.status, margin + 18.0F, 117.0F, status_font, kMuted);

    const std::string language_label = LanguageLabel(snapshot);
    const float language_width = TextWidth(pill_font, language_label) + 34.0F;
    window_state.language_button = SkRect::MakeXYWH(
        static_cast<float>(width) - margin - language_width,
        96.0F,
        language_width,
        32.0F);
    canvas->drawRoundRect(
        window_state.language_button,
        12.0F,
        12.0F,
        Fill(CanSetLanguage(snapshot) ? kPanelRaised : kPanel));
    SkPaint language_border = Fill(CanSetLanguage(snapshot) ? kAccent : kBorder);
    language_border.setStyle(SkPaint::kStroke_Style);
    language_border.setStrokeWidth(1.0F);
    canvas->drawRoundRect(window_state.language_button, 12.0F, 12.0F, language_border);
    DrawCenteredText(
        *canvas,
        language_label,
        window_state.language_button,
        pill_font,
        CanSetLanguage(snapshot) ? kText : kMuted,
        4.0F);

    const float cards_width = static_cast<float>(width) - margin * 2.0F;
    const SkRect raw_card = SkRect::MakeXYWH(margin, 140.0F, cards_width, 188.0F);
    const SkRect clean_card = SkRect::MakeXYWH(margin, 348.0F, cards_width, std::max(150.0F, static_cast<float>(height) - 478.0F));
    DrawTranscriptCard(
        *canvas,
        raw_card,
        "LIVE TRANSCRIPT",
        snapshot.live_text,
        snapshot.mode == app::DictationMode::Recording
            ? "Listening for speech..."
            : "Your cumulative Nemotron transcript appears here while you speak.",
        label_font,
        body_font,
        kText,
        snapshot.mode == app::DictationMode::Recording || snapshot.mode == app::DictationMode::Finalizing,
        glfwGetTime());
    DrawTranscriptCard(
        *canvas,
        clean_card,
        "LIVE CLEANUP",
        snapshot.rewritten_text,
        snapshot.rewrite_in_progress
            ? "Cleaning the latest available transcript locally..."
            : "The debounced llama.cpp cleanup appears here while you continue speaking.",
        label_font,
        body_font,
        kText,
        snapshot.rewrite_in_progress,
        glfwGetTime());

    const float button_y = static_cast<float>(height) - 91.0F;
    window_state.primary_button = SkRect::MakeXYWH(margin, button_y, cards_width, 54.0F);
    const bool enabled = CanToggle(snapshot);
    const SkColor button_color = snapshot.mode == app::DictationMode::Recording
        ? kRecording
        : (enabled ? kAccent : kPanelRaised);
    canvas->drawRoundRect(window_state.primary_button, 15.0F, 15.0F, Fill(button_color));
    DrawCenteredText(
        *canvas,
        PrimaryButtonLabel(snapshot),
        window_state.primary_button,
        button_font,
        enabled ? kText : kMuted);

    if (CanCancel(snapshot)) {
        window_state.cancel_button = SkRect::MakeXYWH(
            window_state.primary_button.right() - 86.0F,
            window_state.primary_button.top() + 9.0F,
            74.0F,
            36.0F);
        canvas->drawRoundRect(window_state.cancel_button, 12.0F, 12.0F, Fill(kPanel));
        DrawCenteredText(*canvas, "Cancel", window_state.cancel_button, pill_font, kText, 4.0F);
    } else {
        window_state.cancel_button = SkRect::MakeEmpty();
    }

    if (!snapshot.error.empty()) {
        const SkRect error_bar = SkRect::MakeXYWH(margin, 94.0F, cards_width, 35.0F);
        canvas->drawRoundRect(error_bar, 10.0F, 10.0F, Fill(SkColorSetRGB(70, 43, 31)));
        DrawWrappedText(*canvas, snapshot.error, error_bar.left() + 12.0F, error_bar.top() + 22.0F,
            error_bar.width() - 24.0F, 18.0F, 1, pill_font, kWarning);
    }

    canvas->restore();
    surface.present(window);
}

void KeyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state || !state->controller) return;
    const auto snapshot = state->controller->snapshot();
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE) {
        if (CanToggle(snapshot)) state->controller->toggle_recording();
    } else if (key == GLFW_KEY_ESCAPE) {
        if (CanCancel(snapshot)) {
            state->controller->cancel_recording();
        } else {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } else if (key == GLFW_KEY_L && CanSetLanguage(snapshot)) {
        state->controller->set_language(NextLanguage(snapshot));
    }
}

void MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(window));
    if (!state || !state->controller) return;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    const auto snapshot = state->controller->snapshot();
    if (state->language_button.contains(static_cast<float>(x), static_cast<float>(y)) &&
        CanSetLanguage(snapshot)) {
        state->controller->set_language(NextLanguage(snapshot));
    } else if (state->cancel_button.contains(static_cast<float>(x), static_cast<float>(y))) {
        state->controller->cancel_recording();
    } else if (state->primary_button.contains(static_cast<float>(x), static_cast<float>(y)) &&
               CanToggle(snapshot)) {
        state->controller->toggle_recording();
    }
}

} // namespace

int RunDictationWindow(app::AppController& controller) {
    glfwSetErrorCallback([](int, const char* description) {
        std::cerr << "GLFW: " << description << '\n';
    });
    if (!glfwInit()) {
        std::cerr << "Could not initialize GLFW. Is a graphical session available?\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "dictscribe");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "dictscribe");
    GLFWwindow* window = glfwCreateWindow(900, 650, "DictScribe - Live Dictation", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 760, 580, GLFW_DONT_CARE, GLFW_DONT_CARE);

    SkiaSurface surface;
    if (!surface.initialize(window)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    glfwSwapInterval(1);

    const auto font_manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    const auto regular = font_manager ? FindTypeface(font_manager, SkFontStyle::Normal()) : nullptr;
    const auto bold = font_manager ? FindTypeface(font_manager, SkFontStyle::Bold()) : nullptr;
    if (!regular || !bold) {
        std::cerr << "Could not initialize a system typeface.\n";
        surface.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    WindowState window_state{.controller = &controller};
    glfwSetWindowUserPointer(window, &window_state);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);

    while (!glfwWindowShouldClose(window)) {
        controller.tick();
        Render(window, surface, window_state, regular, bold);
        glfwWaitEventsTimeout(1.0 / 30.0);
    }

    surface.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace dictscribe::ui
