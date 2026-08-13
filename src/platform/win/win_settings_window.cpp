#include "platform/win/win_settings_window.hpp"

#include <utility>

#include <dwmapi.h>
#include <windowsx.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/ports/SkTypeface_win.h"
#pragma warning(pop)

namespace dictscribe::win {

namespace {

constexpr wchar_t kSettingsClass[] = L"DictScribeSettingsWindow";
constexpr int kLogicalWidth = 640;
constexpr int kLogicalHeight = 660;

sk_sp<SkTypeface> FindTypeface(
    const sk_sp<SkFontMgr>& manager,
    SkFontStyle style) {
    for (const char* family : {"Segoe UI Variable Text", "Segoe UI", "Inter", "Arial"}) {
        if (auto typeface = manager->matchFamilyStyle(family, style)) return typeface;
    }
    return manager->matchFamilyStyle(nullptr, style);
}

} // namespace

struct WinSettingsWindow::Impl {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    sk_sp<SkSurface> surface;
    HDC surface_dc = nullptr;
    HBITMAP surface_bitmap = nullptr;
    HGDIOBJ previous_surface_bitmap = nullptr;
    sk_sp<SkFontMgr> font_manager;
    sk_sp<SkTypeface> regular;
    sk_sp<SkTypeface> semibold;
    ui::SettingsViewModel model;
    ui::SettingsViewLayout layout;
    std::function<void(ui::SettingsAction)> action_handler;

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
            screen_dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
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

    void render() {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(hwnd, &paint);
        if (!ensure_surface()) {
            EndPaint(hwnd, &paint);
            return;
        }
        const float scale = static_cast<float>(dpi) / 96.0F;
        SkCanvas* canvas = surface->getCanvas();
        canvas->save();
        canvas->scale(scale, scale);
        layout = ui::RenderSettingsView(
            *canvas,
            static_cast<float>(surface->width()) / scale,
            static_cast<float>(surface->height()) / scale,
            SkFont(regular, 13.0F),
            SkFont(semibold, 13.0F),
            model);
        canvas->restore();
        BitBlt(
            target,
            0,
            0,
            surface->width(),
            surface->height(),
            surface_dc,
            0,
            0,
            SRCCOPY);
        EndPaint(hwnd, &paint);
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
        case WM_LBUTTONUP: {
            const float scale = static_cast<float>(self->dpi) / 96.0F;
            const auto action = ui::HitTestSettingsView(
                self->layout,
                static_cast<float>(GET_X_LPARAM(l_param)) / scale,
                static_cast<float>(GET_Y_LPARAM(l_param)) / scale,
                self->model.device_controls_enabled);
            if (action == ui::SettingsAction::Close) {
                ShowWindow(hwnd, SW_HIDE);
            } else if (action != ui::SettingsAction::NoAction && self->action_handler) {
                self->action_handler(action);
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_SIZE:
            self->release_surface();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            self->dpi = HIWORD(w_param);
            const auto* suggested = reinterpret_cast<RECT*>(l_param);
            SetWindowPos(
                hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            self->release_surface();
            return 0;
        }
        case WM_DESTROY:
            self->release_surface();
            self->hwnd = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
    }
};

WinSettingsWindow::WinSettingsWindow() : impl_(std::make_unique<Impl>()) {}

WinSettingsWindow::~WinSettingsWindow() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

bool WinSettingsWindow::create(HINSTANCE instance, std::string& error) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = Impl::WindowProc;
    window_class.lpszClassName = kSettingsClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Could not register the DictScribe settings window.";
        return false;
    }

    impl_->dpi = GetDpiForSystem();
    RECT rect{0, 0, MulDiv(kLogicalWidth, impl_->dpi, 96), MulDiv(kLogicalHeight, impl_->dpi, 96)};
    AdjustWindowRectExForDpi(
        &rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, impl_->dpi);
    impl_->hwnd = CreateWindowExW(
        0,
        kSettingsClass,
        L"DictScribe Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        impl_.get());
    if (!impl_->hwnd) {
        error = "Could not create the DictScribe settings window.";
        return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(impl_->hwnd, 20, &dark, sizeof(dark));

    impl_->font_manager = SkFontMgr_New_DirectWrite();
    if (!impl_->font_manager) impl_->font_manager = SkFontMgr_New_GDI();
    if (impl_->font_manager) {
        impl_->regular = FindTypeface(impl_->font_manager, SkFontStyle::Normal());
        impl_->semibold = FindTypeface(
            impl_->font_manager,
            SkFontStyle(
                SkFontStyle::kSemiBold_Weight,
                SkFontStyle::kNormal_Width,
                SkFontStyle::kUpright_Slant));
    }
    if (!impl_->regular || !impl_->semibold) {
        error = "Could not initialize the Windows settings font.";
        return false;
    }
    return true;
}

void WinSettingsWindow::set_action_handler(
    std::function<void(ui::SettingsAction)> handler) {
    impl_->action_handler = std::move(handler);
}

void WinSettingsWindow::update(ui::SettingsViewModel model) {
    impl_->model = std::move(model);
    if (visible()) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinSettingsWindow::show() {
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(impl_->hwnd);
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinSettingsWindow::hide() {
    if (impl_->hwnd) ShowWindow(impl_->hwnd, SW_HIDE);
}

HWND WinSettingsWindow::window() const { return impl_->hwnd; }

bool WinSettingsWindow::visible() const {
    return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

} // namespace dictscribe::win
