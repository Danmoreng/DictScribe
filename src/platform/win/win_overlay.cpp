#include "platform/win/win_overlay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <string_view>
#include <vector>

#include <dwmapi.h>
#include <windowsx.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/ports/SkTypeface_win.h"
#pragma warning(pop)

namespace dictscribe::win {

namespace {

constexpr wchar_t kOverlayClass[] = L"DictScribeOverlayWindow";
constexpr int kLogicalWidth = 680;
constexpr int kLogicalHeight = 228;

constexpr SkColor kBackground = SkColorSetRGB(18, 21, 28);
constexpr SkColor kSurface = SkColorSetRGB(25, 29, 38);
constexpr SkColor kBorder = SkColorSetRGB(55, 62, 76);
constexpr SkColor kText = SkColorSetRGB(244, 246, 250);
constexpr SkColor kMuted = SkColorSetRGB(150, 159, 176);
constexpr SkColor kSubtle = SkColorSetRGB(101, 111, 130);
constexpr SkColor kAccent = SkColorSetRGB(139, 124, 255);
constexpr SkColor kRecording = SkColorSetRGB(255, 83, 112);
constexpr SkColor kSuccess = SkColorSetRGB(73, 214, 158);
constexpr SkColor kWarning = SkColorSetRGB(255, 186, 85);

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
    float max_width,
    std::size_t max_lines) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word;
    std::string current;
    while (words >> word) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && TextWidth(font, candidate) > max_width) {
            lines.push_back(current);
            current = word;
            if (lines.size() == max_lines) break;
        } else {
            current = candidate;
        }
    }
    if (lines.size() < max_lines && !current.empty()) lines.push_back(current);
    if (lines.size() == max_lines && words.good()) {
        auto& last = lines.back();
        while (!last.empty() && TextWidth(font, last + "…") > max_width) last.pop_back();
        last += "…";
    }
    return lines;
}

const char* StatusLabel(app::DictationMode mode) {
    switch (mode) {
    case app::DictationMode::Starting: return "Loading local models";
    case app::DictationMode::Ready: return "Ready";
    case app::DictationMode::StartingRecording: return "Opening microphone";
    case app::DictationMode::Recording: return "Listening";
    case app::DictationMode::Finalizing: return "Finalizing speech";
    case app::DictationMode::Rewriting: return "Polishing text locally";
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
    UINT dpi = 96;
    sk_sp<SkSurface> surface;
    sk_sp<SkFontMgr> font_manager;
    sk_sp<SkTypeface> regular;
    sk_sp<SkTypeface> semibold;
    app::AppSnapshot snapshot;
    std::string notice;
    std::array<float, 16> level_history{};

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
            return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ScreenToClient(hwnd, &point);
            const int header_height = MulDiv(56, static_cast<int>(self->dpi), 96);
            return point.y >= 0 && point.y < header_height ? HTCAPTION : HTTRANSPARENT;
        }
        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCAPTION) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, w_param, l_param);
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
            self->surface.reset();
            return 0;
        }
        case WM_DESTROY:
            self->surface.reset();
            return 0;
        default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
    }

    bool ensure_surface() {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return false;
        if (surface && surface->width() == width && surface->height() == height) return true;
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        return surface != nullptr;
    }

    void present() {
        SkPixmap pixmap;
        if (!surface || !surface->peekPixels(&pixmap)) return;
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = pixmap.width();
        bitmap.bmiHeader.biHeight = -pixmap.height();
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(
            dc,
            0,
            0,
            pixmap.width(),
            pixmap.height(),
            0,
            0,
            pixmap.width(),
            pixmap.height(),
            pixmap.addr(),
            &bitmap,
            DIB_RGB_COLORS,
            SRCCOPY);
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
        canvas->clear(kBackground);
        canvas->save();
        canvas->scale(scale, scale);

        const float width = static_cast<float>(surface->width()) / scale;
        const float height = static_cast<float>(surface->height()) / scale;
        const SkFont status_font(semibold, 13.0F);
        const SkFont body_font(regular, 20.0F);
        const SkFont small_font(regular, 12.0F);
        const SkFont hint_font(regular, 11.5F);

        canvas->drawRect(SkRect::MakeWH(width, height), Fill(kBackground));
        SkPaint border = Fill(kBorder);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(1.0F);
        canvas->drawRoundRect(
            SkRect::MakeXYWH(0.5F, 0.5F, width - 1.0F, height - 1.0F),
            20.0F,
            20.0F,
            border);

        const SkColor status_color = StatusColor(snapshot.mode);
        canvas->drawCircle(28.0F, 29.0F, 5.0F, Fill(status_color));
        DrawText(canvas[0], StatusLabel(snapshot.mode), 43.0F, 34.0F, status_font, kText);

        if (snapshot.mode == app::DictationMode::Recording) {
            const float start_x = width - 142.0F;
            for (std::size_t index = 0; index < level_history.size(); ++index) {
                const float bar_height = 2.0F + level_history[index] * 22.0F;
                const float x = start_x + static_cast<float>(index) * 7.0F;
                canvas->drawRoundRect(
                    SkRect::MakeXYWH(x, 29.0F - bar_height * 0.5F, 3.0F, bar_height),
                    1.5F,
                    1.5F,
                    Fill(level_history[index] > 0.08F ? status_color : kSubtle));
            }
        }

        canvas->drawLine(24.0F, 55.0F, width - 24.0F, 55.0F, border);
        std::string display;
        if (snapshot.mode == app::DictationMode::Recording) {
            display = snapshot.live_text;
        } else if (snapshot.mode == app::DictationMode::Rewriting ||
                   snapshot.mode == app::DictationMode::Finalizing) {
            display = !snapshot.raw_final_text.empty() ? snapshot.raw_final_text : snapshot.live_text;
        } else {
            display = !snapshot.rewritten_text.empty() ? snapshot.rewritten_text : snapshot.live_text;
        }
        std::string placeholder = "Speak naturally. Your words will appear here.";
        if (snapshot.mode == app::DictationMode::Starting) {
            placeholder = "Preparing the local speech and rewrite models…";
        } else if (snapshot.mode == app::DictationMode::Ready ||
                   snapshot.mode == app::DictationMode::Complete) {
            placeholder = "Press Ctrl+Alt+Space in any text field to start dictation.";
        } else if (snapshot.mode == app::DictationMode::Error && !snapshot.error.empty()) {
            placeholder = snapshot.error;
        }
        const std::string& body = display.empty() ? placeholder : display;
        float baseline = 88.0F;
        for (const auto& line : WrapText(body, body_font, width - 56.0F, 4)) {
            DrawText(
                canvas[0],
                line,
                28.0F,
                baseline,
                body_font,
                snapshot.mode == app::DictationMode::Error ? kWarning :
                    (display.empty() ? kMuted : kText));
            baseline += 27.0F;
        }

        canvas->drawRoundRect(
            SkRect::MakeXYWH(20.0F, height - 37.0F, width - 40.0F, 1.0F),
            0.5F,
            0.5F,
            Fill(kSurface));
        const char* hints = "Ctrl+Alt+Space  Start dictation     Drag header to move";
        if (snapshot.mode == app::DictationMode::Recording) {
            hints = "Enter or Ctrl+Alt+Space  Finish     Esc  Cancel     Drag header to move";
        } else if (snapshot.mode == app::DictationMode::Finalizing ||
                   snapshot.mode == app::DictationMode::Rewriting) {
            hints = "Finishing locally…";
        }
        DrawText(canvas[0], hints, 28.0F, height - 15.0F, hint_font, kSubtle);

        canvas->restore();
        present();
    }
};

WinOverlay::WinOverlay() : impl_(std::make_unique<Impl>()) {}

WinOverlay::~WinOverlay() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
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

    impl_->hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kOverlayClass,
        L"DictScribe",
        WS_POPUP,
        0,
        0,
        kLogicalWidth,
        kLogicalHeight,
        nullptr,
        nullptr,
        instance,
        impl_.get());
    if (!impl_->hwnd) {
        error = "Could not create the DictScribe overlay window.";
        return false;
    }
    SetLayeredWindowAttributes(impl_->hwnd, 0, 255, LWA_ALPHA);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(impl_->hwnd, 20, &dark, sizeof(dark));
    const int rounded = 2;
    DwmSetWindowAttribute(impl_->hwnd, 33, &rounded, sizeof(rounded));

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

void WinOverlay::update(const app::AppSnapshot& snapshot, std::string notice) {
    impl_->snapshot = snapshot;
    impl_->notice = std::move(notice);
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
    if (visible()) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinOverlay::show_near(const TargetContext& target) {
    impl_->dpi = target.window ? GetDpiForWindow(target.window) : 96;
    if (impl_->dpi == 0) impl_->dpi = 96;
    const int width = MulDiv(kLogicalWidth, static_cast<int>(impl_->dpi), 96);
    const int height = MulDiv(kLogicalHeight, static_cast<int>(impl_->dpi), 96);
    const int gap = MulDiv(target.caret_anchor ? 12 : 20, static_cast<int>(impl_->dpi), 96);

    HMONITOR monitor = MonitorFromPoint(target.anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    int x = target.anchor.x - width / 2;
    int y = target.anchor.y - height - gap;
    if (y < info.rcWork.top) y = target.anchor.y + gap;
    x = std::clamp(x, static_cast<int>(info.rcWork.left) + 8,
                   static_cast<int>(info.rcWork.right) - width - 8);
    y = std::clamp(y, static_cast<int>(info.rcWork.top) + 8,
                   static_cast<int>(info.rcWork.bottom) - height - 8);

    HRGN region = CreateRoundRectRgn(
        0,
        0,
        width + 1,
        height + 1,
        MulDiv(24, static_cast<int>(impl_->dpi), 96),
        MulDiv(24, static_cast<int>(impl_->dpi), 96));
    SetWindowRgn(impl_->hwnd, region, FALSE);
    impl_->surface.reset();
    SetWindowPos(
        impl_->hwnd,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinOverlay::hide() {
    ShowWindow(impl_->hwnd, SW_HIDE);
}

HWND WinOverlay::window() const { return impl_->hwnd; }

bool WinOverlay::visible() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }

} // namespace dictscribe::win
