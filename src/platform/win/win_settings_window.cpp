#include "platform/win/win_settings_window.hpp"
#include "platform/win/resource.h"

#include "app/language_catalog.hpp"

#include <algorithm>
#include <cstdlib>
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

    static constexpr int kVisibleLanguageRows = 8;

    int maximum_language_scroll() const {
        return std::max(
            0, static_cast<int>(app::kLanguageOptions.size()) - kVisibleLanguageRows);
    }

    void ensure_language_visible(int option) {
        if (option < model.language_menu_scroll) {
            model.language_menu_scroll = option;
        } else if (option >= model.language_menu_scroll + kVisibleLanguageRows) {
            model.language_menu_scroll = option - kVisibleLanguageRows + 1;
        }
        model.language_menu_scroll = std::clamp(
            model.language_menu_scroll, 0, maximum_language_scroll());
    }

    void set_language_menu_open(bool open) {
        model.language_menu_open = open;
        if (open) {
            model.language_menu_highlight = static_cast<int>(
                app::LanguageOptionIndex(model.settings.language));
            model.language_menu_scroll = std::clamp(
                model.language_menu_highlight - 3, 0, maximum_language_scroll());
            SetFocus(hwnd);
        } else {
            model.language_menu_highlight = -1;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    int language_option_at(float logical_x, float logical_y) const {
        if (!model.language_menu_open) return -1;
        for (std::size_t row = 0; row < layout.language_option_count; ++row) {
            if (layout.language_options[row].contains(logical_x, logical_y)) {
                return static_cast<int>(layout.language_option_indices[row]);
            }
        }
        return -1;
    }

    void select_language_option(int option) {
        if (option < 0 || option >= static_cast<int>(app::kLanguageOptions.size())) return;
        set_language_menu_open(false);
        if (action_handler) action_handler(ui::LanguageSelectionAction(
            static_cast<std::size_t>(option)));
    }

    void move_language_highlight(int delta) {
        int option = model.language_menu_highlight;
        if (option < 0) option = static_cast<int>(
            app::LanguageOptionIndex(model.settings.language));
        option = std::clamp(
            option + delta, 0, static_cast<int>(app::kLanguageOptions.size()) - 1);
        model.language_menu_highlight = option;
        ensure_language_visible(option);
        InvalidateRect(hwnd, nullptr, FALSE);
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
            if (action == ui::SettingsAction::ToggleLanguageMenu) {
                self->set_language_menu_open(!self->model.language_menu_open);
            } else if (ui::IsLanguageSelection(action)) {
                self->select_language_option(static_cast<int>(
                    ui::LanguageSelectionIndex(action)));
            } else if (action == ui::SettingsAction::Close) {
                self->set_language_menu_open(false);
                ShowWindow(hwnd, SW_HIDE);
            } else if (action != ui::SettingsAction::NoAction && self->action_handler) {
                self->action_handler(action);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (self->model.language_menu_open) {
                const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
                const int steps = std::abs(delta) >= WHEEL_DELTA
                    ? delta / WHEEL_DELTA : (delta > 0 ? 1 : -1);
                self->model.language_menu_scroll = std::clamp(
                    self->model.language_menu_scroll - steps * 3,
                    0,
                    self->maximum_language_scroll());
                self->model.language_menu_highlight = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const float scale = static_cast<float>(self->dpi) / 96.0F;
            const float x = static_cast<float>(GET_X_LPARAM(l_param)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(l_param)) / scale;
            const bool select_hovered = self->layout.language_select.contains(x, y);
            const int option = self->language_option_at(x, y);
            const int highlight = self->model.language_menu_open
                ? option : self->model.language_menu_highlight;
            if (select_hovered != self->model.language_select_hovered ||
                highlight != self->model.language_menu_highlight) {
                self->model.language_select_hovered = select_hovered;
                self->model.language_menu_highlight = highlight;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = hwnd;
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE:
            self->model.language_select_hovered = false;
            if (self->model.language_menu_open) self->model.language_menu_highlight = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (self->model.language_menu_open) {
                if (w_param == VK_UP) self->move_language_highlight(-1);
                else if (w_param == VK_DOWN) self->move_language_highlight(1);
                else if (w_param == VK_PRIOR) self->move_language_highlight(-kVisibleLanguageRows);
                else if (w_param == VK_NEXT) self->move_language_highlight(kVisibleLanguageRows);
                else if (w_param == VK_HOME) {
                    self->model.language_menu_highlight = 0;
                    self->ensure_language_visible(0);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (w_param == VK_END) {
                    self->model.language_menu_highlight =
                        static_cast<int>(app::kLanguageOptions.size()) - 1;
                    self->ensure_language_visible(self->model.language_menu_highlight);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (w_param == VK_RETURN) {
                    self->select_language_option(self->model.language_menu_highlight);
                } else if (w_param == VK_ESCAPE) {
                    self->set_language_menu_open(false);
                } else {
                    break;
                }
                return 0;
            }
            break;
        case WM_SETCURSOR: {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            const float scale = static_cast<float>(self->dpi) / 96.0F;
            if (self->layout.language_select.contains(
                    static_cast<float>(cursor.x) / scale,
                    static_cast<float>(cursor.y) / scale)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            self->set_language_menu_open(false);
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
        return DefWindowProcW(hwnd, message, w_param, l_param);
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
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
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
    model.language_menu_open = impl_->model.language_menu_open;
    model.language_menu_scroll = impl_->model.language_menu_scroll;
    model.language_menu_highlight = impl_->model.language_menu_highlight;
    model.language_select_hovered = impl_->model.language_select_hovered;
    impl_->model = std::move(model);
    if (visible()) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinSettingsWindow::show() {
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, IsIconic(impl_->hwnd) ? SW_RESTORE : SW_SHOW);
    SetWindowPos(
        impl_->hwnd,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(impl_->hwnd);
    SetForegroundWindow(impl_->hwnd);
    SetActiveWindow(impl_->hwnd);
    SetFocus(impl_->hwnd);
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void WinSettingsWindow::hide() {
    if (impl_->hwnd) {
        impl_->set_language_menu_open(false);
        ShowWindow(impl_->hwnd, SW_HIDE);
    }
}

HWND WinSettingsWindow::window() const { return impl_->hwnd; }

bool WinSettingsWindow::visible() const {
    return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

bool WinSettingsWindow::language_menu_open() const {
    return impl_->model.language_menu_open;
}

} // namespace dictscribe::win
