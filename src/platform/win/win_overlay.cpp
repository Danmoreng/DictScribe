#include "platform/win/win_overlay.hpp"

#include "app/language_catalog.hpp"
#include "ui/text_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/ports/SkTypeface_win.h"
#pragma warning(pop)

namespace dictscribe::win {

namespace {

constexpr wchar_t kOverlayClass[] = L"DictScribeOverlayWindow";
constexpr wchar_t kBackdropClass[] = L"DictScribeBackdropWindow";
constexpr int kLogicalWidth = 720;
constexpr int kMinimumLogicalHeight = 244;
constexpr int kMaximumLogicalHeight = 460;
constexpr float kCornerRadius = 8.0F;
constexpr float kHeaderHeight = 52.0F;
constexpr float kFooterHeight = 54.0F;
constexpr float kBodyTop = 75.0F;
constexpr float kBodyLineHeight = 23.0F;

constexpr SkColor kBackground = SkColorSetARGB(132, 14, 17, 23);
constexpr SkColor kSurface = SkColorSetARGB(168, 20, 24, 32);
constexpr SkColor kElevated = SkColorSetARGB(218, 27, 32, 42);
constexpr SkColor kBorder = SkColorSetRGB(61, 69, 84);
constexpr SkColor kText = SkColorSetRGB(244, 246, 250);
constexpr SkColor kMuted = SkColorSetRGB(158, 167, 184);
constexpr SkColor kSubtle = SkColorSetRGB(107, 117, 136);
constexpr SkColor kAccent = SkColorSetRGB(139, 124, 255);
constexpr SkColor kRecording = SkColorSetRGB(255, 83, 112);
constexpr SkColor kSuccess = SkColorSetRGB(73, 214, 158);
constexpr SkColor kWarning = SkColorSetRGB(255, 186, 85);

enum class AccentState : int {
    Disabled = 0,
    BlurBehind = 3,
    AcrylicBlurBehind = 4,
};

struct AccentPolicy {
    AccentState state = AccentState::Disabled;
    int flags = 0;
    DWORD gradient_color = 0;
    int animation_id = 0;
};

struct WindowCompositionAttributeData {
    int attribute = 0;
    void* data = nullptr;
    SIZE_T size = 0;
};

using SetWindowCompositionAttributeFunction = BOOL(WINAPI*)(
    HWND,
    WindowCompositionAttributeData*);

UINT EffectiveDpiForMonitor(HMONITOR monitor) {
    UINT horizontal = 96;
    UINT vertical = 96;
    if (monitor && SUCCEEDED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &horizontal, &vertical)) && horizontal != 0) {
        return horizontal;
    }
    const UINT system_dpi = GetDpiForSystem();
    return system_dpi == 0 ? 96 : system_dpi;
}

bool EnableAcrylicBackdrop(HWND window) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    const auto set_attribute = reinterpret_cast<SetWindowCompositionAttributeFunction>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!set_attribute) return false;

    constexpr int kAccentPolicyAttribute = 19;
    AccentPolicy policy;
    policy.state = AccentState::AcrylicBlurBehind;
    policy.flags = 2;
    policy.gradient_color = 0x6617110EU;
    WindowCompositionAttributeData data{
        kAccentPolicyAttribute,
        &policy,
        sizeof(policy),
    };
    if (set_attribute(window, &data)) return true;

    policy.state = AccentState::BlurBehind;
    policy.flags = 0;
    policy.gradient_color = 0;
    return set_attribute(window, &data) != FALSE;
}

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
    for (const std::string& paragraph : dictscribe::ui::SplitExplicitLines(text)) {
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
        return "Preparing the local speech and rewrite models…";
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

std::string LanguageBadgeText(const app::AppSnapshot& snapshot) {
    return app::LanguageBadge(snapshot.language);
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

sk_sp<SkTypeface> FindTypeface(
    const sk_sp<SkFontMgr>& manager,
    SkFontStyle style) {
    for (const char* family : {"Segoe UI Variable Text", "Segoe UI", "Inter", "Arial"}) {
        if (auto typeface = manager->matchFamilyStyle(family, style)) return typeface;
    }
    return manager->matchFamilyStyle(nullptr, style);
}

} // namespace

struct WinOverlay::Impl {
    HWND hwnd = nullptr;
    HWND backdrop_hwnd = nullptr;
    UINT dpi = 96;
    sk_sp<SkSurface> surface;
    HDC surface_dc = nullptr;
    HBITMAP surface_bitmap = nullptr;
    HGDIOBJ previous_surface_bitmap = nullptr;
    sk_sp<SkFontMgr> font_manager;
    sk_sp<SkTypeface> regular;
    sk_sp<SkTypeface> semibold;
    app::AppSnapshot snapshot;
    std::string notice;
    std::function<void(std::string)> language_handler;
    std::function<void()> settings_handler;
    std::function<void(POINT)> position_handler;
    std::optional<POINT> preferred_position;
    std::array<float, 16> level_history{};
    int scroll_line = 0;
    int max_scroll_line = 0;
    int visible_line_count = 1;
    bool user_scrolled = false;
    bool dragging_scrollbar = false;
    bool language_pressed = false;
    bool settings_pressed = false;
    bool language_menu_open = false;
    int language_menu_scroll = 0;
    int pressed_language_option = -1;
    int hovered_language_option = -1;
    HWND language_menu_previous_foreground = nullptr;
    float scrollbar_track_top = 0.0F;
    float scrollbar_track_bottom = 0.0F;
    float scrollbar_thumb_top = 0.0F;
    float scrollbar_thumb_bottom = 0.0F;
    float scrollbar_drag_offset = 0.0F;

    std::vector<std::string> wrapped_body(float width = static_cast<float>(kLogicalWidth)) const {
        const SkFont body_font(regular, 16.5F);
        const std::string display = DisplayText(snapshot);
        const std::string body = display.empty() ? PlaceholderText(snapshot) : display;
        return WrapText(body, body_font, width - 78.0F);
    }

    int desired_logical_height() const {
        if (!regular) return kMinimumLogicalHeight;
        const int content_lines = std::clamp(
            static_cast<int>(wrapped_body().size()), 3, 13);
        const int desired = static_cast<int>(
            kBodyTop + content_lines * kBodyLineHeight + 16.0F + kFooterHeight);
        const int minimum = language_menu_open ? 326 : kMinimumLogicalHeight;
        return std::clamp(std::max(desired, minimum), kMinimumLogicalHeight, kMaximumLogicalHeight);
    }

    SkRect language_badge_rect(float width = static_cast<float>(kLogicalWidth)) const {
        const std::string language = LanguageBadgeText(snapshot);
        const float badge_width = language == "AUTO" ? 62.0F : 72.0F;
        return SkRect::MakeXYWH(width - 150.0F - badge_width, 11.0F, badge_width, 30.0F);
    }

    bool language_badge_contains(int client_x, int client_y) const {
        RECT client{};
        GetClientRect(hwnd, &client);
        const float scale = static_cast<float>(dpi) / 96.0F;
        const float width = static_cast<float>(client.right - client.left) / scale;
        return language_badge_rect(width).contains(
            static_cast<float>(client_x) / scale,
            static_cast<float>(client_y) / scale);
    }

    SkRect settings_button_rect() const {
        return SkRect::MakeXYWH(398.0F, 11.0F, 88.0F, 30.0F);
    }

    bool settings_button_contains(int client_x, int client_y) const {
        const float scale = static_cast<float>(dpi) / 96.0F;
        return settings_button_rect().contains(
            static_cast<float>(client_x) / scale,
            static_cast<float>(client_y) / scale);
    }

    SkRect language_menu_rect(float width = static_cast<float>(kLogicalWidth)) const {
        const SkRect badge = language_badge_rect(width);
        return SkRect::MakeXYWH(badge.right() - 270.0F, badge.bottom() + 6.0F, 270.0F, 268.0F);
    }

    SkRect language_option_rect(
        int row,
        float width = static_cast<float>(kLogicalWidth)) const {
        const SkRect menu = language_menu_rect(width);
        return SkRect::MakeXYWH(
            menu.left() + 6.0F,
            menu.top() + 6.0F + static_cast<float>(row) * 32.0F,
            menu.width() - 20.0F,
            30.0F);
    }

    int language_option_at(int client_x, int client_y) const {
        if (!language_menu_open) return -1;
        RECT client{};
        GetClientRect(hwnd, &client);
        const float scale = static_cast<float>(dpi) / 96.0F;
        const float width = static_cast<float>(client.right - client.left) / scale;
        const float logical_x = static_cast<float>(client_x) / scale;
        const float logical_y = static_cast<float>(client_y) / scale;
        for (int row = 0; row < 8; ++row) {
            if (language_option_rect(row, width).contains(logical_x, logical_y)) {
                const int option = language_menu_scroll + row;
                return option < static_cast<int>(app::kLanguageOptions.size()) ? option : -1;
            }
        }
        return -1;
    }

    void choose_language(int option) const {
        if (!language_handler || option < 0 ||
            option >= static_cast<int>(app::kLanguageOptions.size())) return;
        language_handler(std::string(app::kLanguageOptions[option].code));
    }

    int maximum_language_scroll() const {
        return std::max(0, static_cast<int>(app::kLanguageOptions.size()) - 8);
    }

    void ensure_language_visible(int option) {
        if (option < language_menu_scroll) {
            language_menu_scroll = option;
        } else if (option >= language_menu_scroll + 8) {
            language_menu_scroll = option - 7;
        }
        language_menu_scroll = std::clamp(
            language_menu_scroll, 0, maximum_language_scroll());
    }

    void set_language_menu_open(bool open) {
        if (language_menu_open == open) return;
        language_menu_open = open;
        pressed_language_option = -1;
        if (open) {
            hovered_language_option = static_cast<int>(
                app::LanguageOptionIndex(snapshot.language));
            language_menu_scroll = std::clamp(
                hovered_language_option - 3, 0, maximum_language_scroll());
            language_menu_previous_foreground = GetForegroundWindow();
            if (language_menu_previous_foreground == hwnd) {
                language_menu_previous_foreground = nullptr;
            }
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_NOACTIVATE);
            SetWindowPos(
                hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        } else {
            hovered_language_option = -1;
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
            SetWindowPos(
                hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            if (language_menu_previous_foreground &&
                IsWindow(language_menu_previous_foreground)) {
                SetForegroundWindow(language_menu_previous_foreground);
            }
            language_menu_previous_foreground = nullptr;
        }
        resize_to_content();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void move_language_highlight(int delta) {
        int option = hovered_language_option;
        if (option < 0) option = static_cast<int>(
            app::LanguageOptionIndex(snapshot.language));
        option = std::clamp(
            option + delta, 0, static_cast<int>(app::kLanguageOptions.size()) - 1);
        hovered_language_option = option;
        ensure_language_visible(option);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    static LRESULT CALLBACK BackdropWindowProc(
        HWND window,
        UINT message,
        WPARAM w_param,
        LPARAM l_param) {
        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, w_param, l_param);
        }
    }

    void sync_backdrop(bool show) {
        if (!hwnd || !backdrop_hwnd) return;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) return;
        SetWindowPos(
            backdrop_hwnd,
            hwnd,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
    }

    void resize_to_content() {
        if (!hwnd || !IsWindowVisible(hwnd)) return;
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        const int width = rect.right - rect.left;
        const int new_height = MulDiv(desired_logical_height(), static_cast<int>(dpi), 96);
        if (new_height == rect.bottom - rect.top) return;

        HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        int x = rect.left;
        int y = rect.top;
        x = std::clamp(
            x,
            static_cast<int>(info.rcWork.left) + 8,
            static_cast<int>(info.rcWork.right) - width - 8);
        y = std::clamp(
            y,
            static_cast<int>(info.rcWork.top) + 8,
            static_cast<int>(info.rcWork.bottom) - new_height - 8);
        release_surface();
        SetWindowPos(
            hwnd,
            nullptr,
            x,
            y,
            width,
            new_height,
            SWP_NOACTIVATE | SWP_NOZORDER);
        sync_backdrop(true);
        if (preferred_position &&
            (preferred_position->x != x || preferred_position->y != y)) {
            preferred_position = POINT{x, y};
            if (position_handler) position_handler(*preferred_position);
        }
    }

    void release_surface() {
        surface.reset();
        if (surface_dc && previous_surface_bitmap) {
            SelectObject(surface_dc, previous_surface_bitmap);
        }
        if (surface_bitmap) DeleteObject(surface_bitmap);
        if (surface_dc) DeleteDC(surface_dc);
        surface_dc = nullptr;
        surface_bitmap = nullptr;
        previous_surface_bitmap = nullptr;
    }

    void scroll_by(int lines) {
        scroll_line = std::clamp(scroll_line + lines, 0, max_scroll_line);
        user_scrolled = scroll_line < max_scroll_line;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void scroll_from_client_y(int client_y, bool preserve_offset) {
        if (max_scroll_line <= 0 || scrollbar_track_bottom <= scrollbar_track_top) return;
        const float logical_y = static_cast<float>(client_y) * 96.0F / static_cast<float>(dpi);
        const float thumb_height = scrollbar_thumb_bottom - scrollbar_thumb_top;
        const float offset = preserve_offset ? scrollbar_drag_offset : thumb_height * 0.5F;
        const float available = scrollbar_track_bottom - scrollbar_track_top - thumb_height;
        const float position = std::clamp(
            logical_y - offset - scrollbar_track_top, 0.0F, std::max(available, 0.0F));
        scroll_line = available > 0.0F
            ? static_cast<int>(std::lround(position / available * max_scroll_line))
            : 0;
        scroll_line = std::clamp(scroll_line, 0, max_scroll_line);
        user_scrolled = scroll_line < max_scroll_line;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<Impl*>(create->lpCreateParams);
            self->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w_param, l_param);

        switch (message) {
        case WM_PAINT:
            self->render();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return self->language_menu_open ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ScreenToClient(hwnd, &point);
            if (self->language_menu_open) return HTCLIENT;
            if (self->language_badge_contains(point.x, point.y)) return HTCLIENT;
            if (self->settings_button_contains(point.x, point.y)) return HTCLIENT;
            const int header_height = MulDiv(
                static_cast<int>(kHeaderHeight), static_cast<int>(self->dpi), 96);
            return point.y >= 0 && point.y < header_height ? HTCAPTION : HTCLIENT;
        }
        case WM_MOUSEWHEEL: {
            if (self->language_menu_open) {
                const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
                const int steps = std::abs(delta) >= WHEEL_DELTA
                    ? delta / WHEEL_DELTA : (delta > 0 ? 1 : -1);
                self->language_menu_scroll = std::clamp(
                    self->language_menu_scroll - steps,
                    0,
                    self->maximum_language_scroll());
                self->hovered_language_option = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int steps = GET_WHEEL_DELTA_WPARAM(w_param) / WHEEL_DELTA;
            self->scroll_by(-steps * 3);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (self->settings_button_contains(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param))) {
                self->settings_pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->language_badge_contains(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param))) {
                self->language_pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->language_menu_open) {
                const int option = self->language_option_at(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                if (option >= 0) {
                    self->pressed_language_option = option;
                    SetCapture(hwnd);
                } else {
                    self->set_language_menu_open(false);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const float logical_x = static_cast<float>(GET_X_LPARAM(l_param)) * 96.0F /
                static_cast<float>(self->dpi);
            const float logical_y = static_cast<float>(GET_Y_LPARAM(l_param)) * 96.0F /
                static_cast<float>(self->dpi);
            if (logical_x >= static_cast<float>(kLogicalWidth) - 28.0F &&
                logical_y >= self->scrollbar_track_top &&
                logical_y <= self->scrollbar_track_bottom && self->max_scroll_line > 0) {
                self->dragging_scrollbar = true;
                self->scrollbar_drag_offset =
                    logical_y >= self->scrollbar_thumb_top && logical_y <= self->scrollbar_thumb_bottom
                    ? logical_y - self->scrollbar_thumb_top
                    : (self->scrollbar_thumb_bottom - self->scrollbar_thumb_top) * 0.5F;
                SetCapture(hwnd);
                self->scroll_from_client_y(GET_Y_LPARAM(l_param), true);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (self->language_menu_open) {
                const int hovered = self->language_option_at(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                if (hovered != self->hovered_language_option) {
                    self->hovered_language_option = hovered;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = hwnd;
                TrackMouseEvent(&tracking);
            }
            if (self->dragging_scrollbar && (w_param & MK_LBUTTON)) {
                self->scroll_from_client_y(GET_Y_LPARAM(l_param), true);
            }
            return 0;
        case WM_KEYDOWN:
            if (self->language_menu_open) {
                if (w_param == VK_UP) self->move_language_highlight(-1);
                else if (w_param == VK_DOWN) self->move_language_highlight(1);
                else if (w_param == VK_PRIOR) self->move_language_highlight(-8);
                else if (w_param == VK_NEXT) self->move_language_highlight(8);
                else if (w_param == VK_HOME) {
                    self->hovered_language_option = 0;
                    self->ensure_language_visible(0);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (w_param == VK_END) {
                    self->hovered_language_option =
                        static_cast<int>(app::kLanguageOptions.size()) - 1;
                    self->ensure_language_visible(self->hovered_language_option);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (w_param == VK_RETURN) {
                    const int option = self->hovered_language_option;
                    self->set_language_menu_open(false);
                    self->choose_language(option);
                } else if (w_param == VK_ESCAPE) {
                    self->set_language_menu_open(false);
                } else {
                    break;
                }
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            if (self->hovered_language_option >= 0) {
                self->hovered_language_option = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->settings_pressed) {
                const bool activate = self->settings_button_contains(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                self->settings_pressed = false;
                ReleaseCapture();
                if (activate && self->settings_handler) {
                    self->set_language_menu_open(false);
                    self->settings_handler();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->language_pressed) {
                const bool toggle_menu = self->language_badge_contains(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                self->language_pressed = false;
                ReleaseCapture();
                if (toggle_menu) {
                    self->set_language_menu_open(!self->language_menu_open);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->pressed_language_option >= 0) {
                const int pressed = self->pressed_language_option;
                const int released = self->language_option_at(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
                self->pressed_language_option = -1;
                ReleaseCapture();
                if (pressed == released) {
                    self->set_language_menu_open(false);
                    self->choose_language(pressed);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (self->dragging_scrollbar) {
                self->dragging_scrollbar = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            self->dragging_scrollbar = false;
            self->language_pressed = false;
            self->settings_pressed = false;
            self->pressed_language_option = -1;
            return 0;
        case WM_EXITSIZEMOVE: {
            RECT rect{};
            if (GetWindowRect(hwnd, &rect)) {
                self->preferred_position = POINT{rect.left, rect.top};
                if (self->position_handler) {
                    self->position_handler(*self->preferred_position);
                }
            }
            return 0;
        }
        case WM_SETCURSOR: {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            if (self->language_badge_contains(cursor.x, cursor.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (self->settings_button_contains(cursor.x, cursor.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (self->language_menu_open) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (LOWORD(l_param) == HTCAPTION) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
        case WM_DPICHANGED: {
            self->dpi = HIWORD(w_param);
            const auto* suggested = reinterpret_cast<RECT*>(l_param);
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE);
            self->release_surface();
            return 0;
        }
        case WM_WINDOWPOSCHANGED: {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(l_param);
            const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
            if ((position->flags & (SWP_NOMOVE | SWP_NOSIZE)) !=
                (SWP_NOMOVE | SWP_NOSIZE)) {
                self->sync_backdrop(IsWindowVisible(hwnd) != FALSE);
            }
            return result;
        }
        case WM_SHOWWINDOW:
            if (w_param) self->sync_backdrop(true);
            else if (self->backdrop_hwnd) ShowWindow(self->backdrop_hwnd, SW_HIDE);
            return DefWindowProcW(hwnd, message, w_param, l_param);
        case WM_DESTROY:
            self->release_surface();
            return 0;
        default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
        return DefWindowProcW(hwnd, message, w_param, l_param);
    }

    bool ensure_surface() {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return false;
        if (surface && surface->width() == width && surface->height() == height) return true;
        release_surface();

        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width;
        bitmap.bmiHeader.biHeight = -height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;

        HDC screen_dc = GetDC(nullptr);
        surface_dc = CreateCompatibleDC(screen_dc);
        void* pixels = nullptr;
        surface_bitmap = CreateDIBSection(
            screen_dc,
            &bitmap,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0);
        ReleaseDC(nullptr, screen_dc);
        if (!surface_dc || !surface_bitmap || !pixels) {
            release_surface();
            return false;
        }

        previous_surface_bitmap = SelectObject(surface_dc, surface_bitmap);
        surface = SkSurfaces::WrapPixels(
            SkImageInfo::MakeN32Premul(width, height),
            pixels,
            static_cast<std::size_t>(width) * 4U);
        if (!surface) {
            release_surface();
            return false;
        }
        return true;
    }

    void present() {
        if (!surface || !surface_dc) return;
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);

        RECT window{};
        GetWindowRect(hwnd, &window);
        POINT destination{window.left, window.top};
        POINT source{0, 0};
        SIZE size{surface->width(), surface->height()};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        HDC screen_dc = GetDC(nullptr);
        UpdateLayeredWindow(
            hwnd,
            screen_dc,
            &destination,
            &size,
            surface_dc,
            &source,
            0,
            &blend,
            ULW_ALPHA);
        ReleaseDC(nullptr, screen_dc);
        EndPaint(hwnd, &paint);
    }

    void render() {
        if (!ensure_surface()) {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return;
        }
        SkCanvas* canvas = surface->getCanvas();
        const float scale = static_cast<float>(dpi) / 96.0F;
        canvas->clear(SK_ColorTRANSPARENT);
        canvas->save();
        canvas->scale(scale, scale);

        const float width = static_cast<float>(surface->width()) / scale;
        const float height = static_cast<float>(surface->height()) / scale;
        canvas->clipRRect(
            SkRRect::MakeRectXY(
                SkRect::MakeWH(width, height),
                kCornerRadius,
                kCornerRadius),
            true);
        const SkFont status_font(semibold, 13.0F);
        const SkFont body_font(regular, 16.5F);
        const SkFont badge_font(semibold, 11.5F);
        const SkFont hint_font(regular, 12.0F);
        const SkFont key_font(semibold, 11.5F);
        const SkFont menu_font(regular, 12.5F);

        canvas->drawRect(SkRect::MakeWH(width, height), Fill(kBackground));
        SkPaint border = Fill(kBorder);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(1.0F);

        const SkColor status_color = StatusColor(snapshot.mode);
        canvas->drawCircle(28.0F, 26.0F, 5.0F, Fill(status_color));
        DrawText(canvas[0], StatusLabel(snapshot), 43.0F, 31.0F, status_font, kText);

        const std::string language = LanguageBadgeText(snapshot);
        const SkRect badge_rect = language_badge_rect(width);
        const float badge_x = badge_rect.left();
        const SkColor badge_fill = language_pressed ? kBorder : kElevated;
        canvas->drawRoundRect(
            badge_rect,
            7.0F,
            7.0F,
            Fill(badge_fill));
        SkPaint badge_border = Fill(kBorder);
        badge_border.setStyle(SkPaint::kStroke_Style);
        badge_border.setStrokeWidth(1.0F);
        canvas->drawRoundRect(
            SkRect::MakeXYWH(
                badge_rect.left() + 0.5F,
                badge_rect.top() + 0.5F,
                badge_rect.width() - 1.0F,
                badge_rect.height() - 1.0F),
            7.0F,
            7.0F,
            badge_border);
        DrawText(
            canvas[0],
            language,
            badge_x + 9.0F,
            30.5F,
            badge_font,
            kMuted);
        SkPaint chevron = Fill(kMuted);
        chevron.setStyle(SkPaint::kStroke_Style);
        chevron.setStrokeWidth(1.4F);
        chevron.setStrokeCap(SkPaint::kRound_Cap);
        const float chevron_x = badge_rect.right() - 12.0F;
        if (language_menu_open) {
            canvas->drawLine(chevron_x - 3.0F, 27.0F, chevron_x, 24.0F, chevron);
            canvas->drawLine(chevron_x, 24.0F, chevron_x + 3.0F, 27.0F, chevron);
        } else {
            canvas->drawLine(chevron_x - 3.0F, 24.0F, chevron_x, 27.0F, chevron);
            canvas->drawLine(chevron_x, 27.0F, chevron_x + 3.0F, 24.0F, chevron);
        }

        const SkRect settings_rect = settings_button_rect();
        canvas->drawRoundRect(
            settings_rect,
            7.0F,
            7.0F,
            Fill(settings_pressed ? kBorder : kElevated));
        canvas->drawRoundRect(settings_rect, 7.0F, 7.0F, badge_border);
        DrawText(
            canvas[0],
            "Settings",
            settings_rect.centerX() - TextWidth(hint_font, "Settings") * 0.5F,
            30.5F,
            hint_font,
            kMuted);

        const float meter_start_x = width - 132.0F;
        for (std::size_t index = 0; index < level_history.size(); ++index) {
            const float level = snapshot.mode == app::DictationMode::Recording
                ? level_history[index]
                : 0.0F;
            const float bar_height = 3.0F + level * 23.0F;
            const float x = meter_start_x + static_cast<float>(index) * 6.6F;
            canvas->drawRoundRect(
                SkRect::MakeXYWH(x, 26.0F - bar_height * 0.5F, 3.0F, bar_height),
                1.5F,
                1.5F,
                Fill(level > 0.08F ? status_color : kSubtle));
        }

        canvas->drawLine(24.0F, kHeaderHeight - 1.0F, width - 24.0F, kHeaderHeight - 1.0F, border);

        const std::string display = DisplayText(snapshot);
        const auto lines = wrapped_body(width);
        const float body_bottom = height - kFooterHeight - 16.0F;
        visible_line_count = std::max(
            1, static_cast<int>(std::floor((body_bottom - kBodyTop) / kBodyLineHeight)));
        max_scroll_line = std::max(0, static_cast<int>(lines.size()) - visible_line_count);
        if (!user_scrolled) scroll_line = max_scroll_line;
        scroll_line = std::clamp(scroll_line, 0, max_scroll_line);

        canvas->save();
        canvas->clipRect(SkRect::MakeLTRB(24.0F, kBodyTop - 16.0F, width - 30.0F, body_bottom));
        float baseline = kBodyTop;
        const int last_line = std::min(
            static_cast<int>(lines.size()), scroll_line + visible_line_count);
        for (int index = scroll_line; index < last_line; ++index) {
            DrawText(
                canvas[0],
                lines[static_cast<std::size_t>(index)],
                28.0F,
                baseline,
                body_font,
                snapshot.mode == app::DictationMode::Error ? kWarning :
                    (display.empty() ? kMuted : kText));
            baseline += kBodyLineHeight;
        }
        canvas->restore();

        scrollbar_track_top = kBodyTop - 14.0F;
        scrollbar_track_bottom = body_bottom - 2.0F;
        if (max_scroll_line > 0) {
            const float track_height = scrollbar_track_bottom - scrollbar_track_top;
            const float ratio = static_cast<float>(visible_line_count) /
                static_cast<float>(lines.size());
            const float thumb_height = std::max(34.0F, track_height * ratio);
            const float available = track_height - thumb_height;
            scrollbar_thumb_top = scrollbar_track_top + available *
                static_cast<float>(scroll_line) / static_cast<float>(max_scroll_line);
            scrollbar_thumb_bottom = scrollbar_thumb_top + thumb_height;
            canvas->drawRoundRect(
                SkRect::MakeXYWH(width - 18.0F, scrollbar_track_top, 3.0F, track_height),
                1.5F,
                1.5F,
                Fill(kSurface));
            canvas->drawRoundRect(
                SkRect::MakeXYWH(
                    width - 19.0F,
                    scrollbar_thumb_top,
                    5.0F,
                    scrollbar_thumb_bottom - scrollbar_thumb_top),
                2.5F,
                2.5F,
                Fill(kMuted));
        } else {
            scrollbar_thumb_top = scrollbar_thumb_bottom = scrollbar_track_top;
        }

        const float footer_top = height - kFooterHeight;
        canvas->drawRect(
            SkRect::MakeXYWH(0.0F, footer_top, width, kFooterHeight),
            Fill(kSurface));

        const auto draw_keycap = [&](float x, float y, float key_width, std::string_view key) {
            canvas->drawRoundRect(
                SkRect::MakeXYWH(x, y, key_width, 30.0F),
                7.0F,
                7.0F,
                Fill(kElevated));
            canvas->drawRoundRect(
                SkRect::MakeXYWH(x + 0.5F, y + 0.5F, key_width - 1.0F, 29.0F),
                7.0F,
                7.0F,
                badge_border);
            DrawText(
                canvas[0],
                key,
                x + (key_width - TextWidth(key_font, key)) * 0.5F,
                y + 20.0F,
                key_font,
                kText);
        };

        const float key_y = footer_top + 12.0F;
        if (snapshot.mode == app::DictationMode::Recording) {
            draw_keycap(24.0F, key_y, 58.0F, "Enter");
            DrawText(canvas[0], "Finish & insert", 94.0F, key_y + 20.0F, hint_font, kMuted);
            const float cancel_x = width - 174.0F;
            draw_keycap(cancel_x, key_y, 44.0F, "Esc");
            DrawText(canvas[0], "Cancel", cancel_x + 56.0F, key_y + 20.0F, hint_font, kMuted);
        } else if (snapshot.mode == app::DictationMode::Finalizing) {
            const char* progress = snapshot.status.find("Switching") != std::string::npos
                ? "Switching language…"
                : "Finalizing locally…";
            DrawText(canvas[0], progress, 28.0F, key_y + 20.0F, hint_font, kMuted);
        } else {
            draw_keycap(24.0F, key_y, 124.0F, "Ctrl Alt Space");
            DrawText(canvas[0], "Start dictation", 160.0F, key_y + 20.0F, hint_font, kMuted);
        }

        if (language_menu_open) {
            const SkRect menu_rect = language_menu_rect(width);
            const SkRect shadow_rect = SkRect::MakeXYWH(
                menu_rect.left() - 3.0F,
                menu_rect.top() + 3.0F,
                menu_rect.width() + 6.0F,
                menu_rect.height() + 4.0F);
            canvas->drawRoundRect(
                shadow_rect,
                11.0F,
                11.0F,
                Fill(SkColorSetARGB(110, 0, 0, 0)));
            canvas->drawRoundRect(menu_rect, 9.0F, 9.0F, Fill(kElevated));

            SkPaint menu_border = Fill(kBorder);
            menu_border.setStyle(SkPaint::kStroke_Style);
            menu_border.setStrokeWidth(1.0F);
            canvas->drawRoundRect(
                SkRect::MakeXYWH(
                    menu_rect.left() + 0.5F,
                    menu_rect.top() + 0.5F,
                    menu_rect.width() - 1.0F,
                    menu_rect.height() - 1.0F),
                9.0F,
                9.0F,
                menu_border);

            const int selected = static_cast<int>(app::LanguageOptionIndex(snapshot.language));
            for (int row = 0; row < 8; ++row) {
                const int option = language_menu_scroll + row;
                if (option >= static_cast<int>(app::kLanguageOptions.size())) break;
                const SkRect item = language_option_rect(row, width);
                if (option == pressed_language_option) {
                    canvas->drawRoundRect(item, 6.0F, 6.0F, Fill(kBorder));
                } else if (option == hovered_language_option) {
                    canvas->drawRoundRect(
                        item,
                        6.0F,
                        6.0F,
                        Fill(SkColorSetRGB(36, 42, 55)));
                } else if (option == selected) {
                    canvas->drawRoundRect(
                        item,
                        6.0F,
                        6.0F,
                        Fill(SkColorSetRGB(42, 39, 66)));
                }

                const float center_y = item.centerY();
                if (option == selected) {
                    canvas->drawCircle(item.left() + 14.0F, center_y, 3.5F, Fill(kAccent));
                } else {
                    canvas->drawCircle(item.left() + 14.0F, center_y, 2.0F, Fill(kSubtle));
                }
                DrawText(
                    canvas[0],
                    app::kLanguageOptions[static_cast<std::size_t>(option)].label,
                    item.left() + 27.0F,
                    center_y + 4.5F,
                    menu_font,
                    option == selected ? kText : kMuted);
            }
            const float track_height = menu_rect.height() - 20.0F;
            const float thumb_height = track_height * 8.0F /
                static_cast<float>(app::kLanguageOptions.size());
            const float thumb_y = menu_rect.top() + 10.0F +
                (track_height - thumb_height) * static_cast<float>(language_menu_scroll) /
                static_cast<float>(std::max(maximum_language_scroll(), 1));
            canvas->drawRoundRect(
                SkRect::MakeXYWH(menu_rect.right() - 9.0F, menu_rect.top() + 10.0F, 3.0F, track_height),
                1.5F,
                1.5F,
                Fill(kSurface));
            canvas->drawRoundRect(
                SkRect::MakeXYWH(menu_rect.right() - 9.0F, thumb_y, 3.0F, thumb_height),
                1.5F,
                1.5F,
                Fill(kMuted));
        }

        canvas->drawRoundRect(
            SkRect::MakeXYWH(0.5F, 0.5F, width - 1.0F, height - 1.0F),
            kCornerRadius,
            kCornerRadius,
            border);

        canvas->restore();
        present();
    }
};

WinOverlay::WinOverlay() : impl_(std::make_unique<Impl>()) {}

WinOverlay::~WinOverlay() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->backdrop_hwnd) DestroyWindow(impl_->backdrop_hwnd);
}

bool WinOverlay::create(HINSTANCE instance, std::string& error) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = Impl::WindowProc;
    window_class.lpszClassName = kOverlayClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Could not register the DictScribe overlay window.";
        return false;
    }

    WNDCLASSEXW backdrop_class{};
    backdrop_class.cbSize = sizeof(backdrop_class);
    backdrop_class.hInstance = instance;
    backdrop_class.lpfnWndProc = Impl::BackdropWindowProc;
    backdrop_class.lpszClassName = kBackdropClass;
    backdrop_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&backdrop_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Could not register the DictScribe glass backdrop window.";
        return false;
    }

    impl_->backdrop_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kBackdropClass,
        L"DictScribe glass backdrop",
        WS_POPUP,
        0,
        0,
        kLogicalWidth,
        kMinimumLogicalHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!impl_->backdrop_hwnd) {
        error = "Could not create the DictScribe glass backdrop window.";
        return false;
    }

    impl_->hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kOverlayClass,
        L"DictScribe",
        WS_POPUP,
        0,
        0,
        kLogicalWidth,
        kMinimumLogicalHeight,
        nullptr,
        nullptr,
        instance,
        impl_.get());
    if (!impl_->hwnd) {
        error = "Could not create the DictScribe overlay window.";
        return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(impl_->hwnd, 20, &dark, sizeof(dark));
    const int disable_dwm_rounding = 1;
    DwmSetWindowAttribute(
        impl_->hwnd, 33, &disable_dwm_rounding, sizeof(disable_dwm_rounding));
    DwmSetWindowAttribute(impl_->backdrop_hwnd, 20, &dark, sizeof(dark));
    const int rounded_backdrop = 2;
    DwmSetWindowAttribute(
        impl_->backdrop_hwnd, 33, &rounded_backdrop, sizeof(rounded_backdrop));
    EnableAcrylicBackdrop(impl_->backdrop_hwnd);

    impl_->font_manager = SkFontMgr_New_DirectWrite();
    if (!impl_->font_manager) impl_->font_manager = SkFontMgr_New_GDI();
    if (impl_->font_manager) {
        impl_->regular = FindTypeface(impl_->font_manager, SkFontStyle::Normal());
        impl_->semibold = FindTypeface(
            impl_->font_manager,
            SkFontStyle(SkFontStyle::kSemiBold_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant));
    }
    if (!impl_->regular || !impl_->semibold) {
        error = "Could not initialize the Windows UI font.";
        return false;
    }
    return true;
}

void WinOverlay::set_language_handler(std::function<void(std::string)> handler) {
    impl_->language_handler = std::move(handler);
}

void WinOverlay::set_settings_handler(std::function<void()> handler) {
    impl_->settings_handler = std::move(handler);
}

void WinOverlay::set_position_handler(std::function<void(POINT)> handler) {
    impl_->position_handler = std::move(handler);
}

void WinOverlay::set_preferred_position(std::optional<POINT> position) {
    impl_->preferred_position = position;
}

void WinOverlay::update(const app::AppSnapshot& snapshot, std::string notice) {
    const app::DictationMode previous_mode = impl_->snapshot.mode;
    impl_->snapshot = snapshot;
    impl_->notice = std::move(notice);
    if (snapshot.mode == app::DictationMode::StartingRecording ||
        (snapshot.mode == app::DictationMode::Recording &&
         previous_mode != app::DictationMode::Recording &&
         previous_mode != app::DictationMode::StartingRecording)) {
        impl_->scroll_line = 0;
        impl_->user_scrolled = false;
    }
    if (snapshot.mode == app::DictationMode::Recording) {
        std::move(
            impl_->level_history.begin() + 1,
            impl_->level_history.end(),
            impl_->level_history.begin());
        const float source = std::max(snapshot.audio_rms, snapshot.audio_peak * 0.35F);
        const float decibels = 20.0F * std::log10(std::max(source, 0.00001F));
        impl_->level_history.back() = std::clamp((decibels + 55.0F) / 43.0F, 0.0F, 1.0F);
    } else {
        impl_->level_history.fill(0.0F);
    }
    if (visible()) {
        impl_->resize_to_content();
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
    }
}

void WinOverlay::show_near(const TargetContext& target) {
    const POINT placement_anchor = impl_->preferred_position.value_or(target.anchor);
    HMONITOR monitor = MonitorFromPoint(placement_anchor, MONITOR_DEFAULTTONEAREST);
    impl_->dpi = EffectiveDpiForMonitor(monitor);
    const int width = MulDiv(kLogicalWidth, static_cast<int>(impl_->dpi), 96);
    const int height = MulDiv(
        impl_->desired_logical_height(), static_cast<int>(impl_->dpi), 96);
    const int gap = MulDiv(target.caret_anchor ? 12 : 20, static_cast<int>(impl_->dpi), 96);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    int x = 0;
    int y = 0;
    if (impl_->preferred_position) {
        x = impl_->preferred_position->x;
        y = impl_->preferred_position->y;
    } else {
        x = target.anchor.x - width / 2;
        y = target.anchor.y - height - gap;
        if (y < info.rcWork.top) y = target.anchor.y + gap;
    }
    x = std::clamp(x, static_cast<int>(info.rcWork.left) + 8,
                   static_cast<int>(info.rcWork.right) - width - 8);
    y = std::clamp(y, static_cast<int>(info.rcWork.top) + 8,
                   static_cast<int>(info.rcWork.bottom) - height - 8);

    if (impl_->preferred_position &&
        (impl_->preferred_position->x != x || impl_->preferred_position->y != y)) {
        impl_->preferred_position = POINT{x, y};
        if (impl_->position_handler) impl_->position_handler(*impl_->preferred_position);
    }

    impl_->release_surface();
    SetWindowPos(
        impl_->hwnd,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    impl_->sync_backdrop(true);
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinOverlay::hide() {
    if (impl_->language_menu_open) impl_->set_language_menu_open(false);
    ShowWindow(impl_->hwnd, SW_HIDE);
    if (impl_->backdrop_hwnd) ShowWindow(impl_->backdrop_hwnd, SW_HIDE);
}

HWND WinOverlay::window() const { return impl_->hwnd; }

bool WinOverlay::visible() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }

bool WinOverlay::language_menu_open() const {
    return impl_->language_menu_open;
}

} // namespace dictscribe::win
