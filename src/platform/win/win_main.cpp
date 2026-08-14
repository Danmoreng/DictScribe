#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "app/app_controller.hpp"
#include "app/language_catalog.hpp"
#include "app/model_discovery.hpp"
#include "app/settings.hpp"
#include "platform/win/win_overlay.hpp"
#include "platform/win/win_pipeline_debug_window.hpp"
#include "platform/win/resource.h"
#include "platform/win/win_settings_window.hpp"
#include "platform/win/win_text_injector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dictscribe::win {

namespace {

constexpr wchar_t kControlWindowClass[] = L"DictScribeControlWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\DictScribe.Desktop";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowSettingsMessage = WM_APP + 2;
constexpr UINT kAnimationFrameMessage = WM_APP + 3;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickIntervalMilliseconds = 16;
constexpr int kToggleHotkey = 1;
constexpr int kAcceptHotkey = 2;
constexpr int kCancelHotkey = 3;
constexpr UINT kCommandToggle = 100;
constexpr UINT kCommandSettings = 101;
constexpr UINT kCommandPipelineDebug = 102;
constexpr UINT kCommandLanguageBase = 110;
constexpr UINT kCommandQuit = 199;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x00000002;
#endif

UINT MonitorRefreshRate(HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency >= 30 && mode.dmDisplayFrequency <= 500) {
            return mode.dmDisplayFrequency;
        }
    }
    return 60;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

struct CommandLineArguments {
    std::vector<std::string> values;
    std::vector<char*> pointers;
};

CommandLineArguments ReadCommandLine() {
    CommandLineArguments result;
    int count = 0;
    LPWSTR* wide_arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide_arguments) return result;
    result.values.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        result.values.push_back(WideToUtf8(wide_arguments[index]));
    }
    LocalFree(wide_arguments);
    result.pointers.reserve(result.values.size());
    for (auto& value : result.values) result.pointers.push_back(value.data());
    return result;
}

const wchar_t* ToggleMenuLabel(app::DictationMode mode) {
    if (mode == app::DictationMode::Recording) return L"Finish dictation";
    return L"Start dictation";
}

class RefreshFrameScheduler {
public:
    ~RefreshFrameScheduler() { shutdown(); }

    bool initialize(HWND target) {
        target_ = target;
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!wake_event_ || !timer_) {
            if (wake_event_) CloseHandle(wake_event_);
            if (timer_) CloseHandle(timer_);
            wake_event_ = nullptr;
            timer_ = nullptr;
            return false;
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { run(); });
        return true;
    }

    void start(UINT refresh_rate) {
        refresh_rate_.store(std::clamp(refresh_rate, 30U, 500U), std::memory_order_release);
        active_.store(true, std::memory_order_release);
        if (wake_event_) SetEvent(wake_event_);
    }

    void pause() {
        active_.store(false, std::memory_order_release);
        if (wake_event_) SetEvent(wake_event_);
    }

    void frame_handled() {
        frame_pending_.store(false, std::memory_order_release);
    }

    void shutdown() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        active_.store(false, std::memory_order_release);
        if (wake_event_) SetEvent(wake_event_);
        if (thread_.joinable()) thread_.join();
        if (timer_) CloseHandle(timer_);
        if (wake_event_) CloseHandle(wake_event_);
        timer_ = nullptr;
        wake_event_ = nullptr;
        target_ = nullptr;
    }

private:
    void run() {
        HANDLE waits[] = {wake_event_, timer_};
        while (running_.load(std::memory_order_acquire)) {
            if (!active_.load(std::memory_order_acquire)) {
                WaitForSingleObject(wake_event_, INFINITE);
                continue;
            }

            const UINT refresh_rate = refresh_rate_.load(std::memory_order_acquire);
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(
                1, 10'000'000LL / static_cast<LONGLONG>(refresh_rate));
            SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (result != WAIT_OBJECT_0 + 1 ||
                !running_.load(std::memory_order_acquire) ||
                !active_.load(std::memory_order_acquire)) {
                continue;
            }

            bool expected = false;
            if (frame_pending_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel) &&
                !PostMessageW(target_, kAnimationFrameMessage, 0, 0)) {
                frame_pending_.store(false, std::memory_order_release);
            }
        }
        CancelWaitableTimer(timer_);
    }

    HWND target_ = nullptr;
    HANDLE wake_event_ = nullptr;
    HANDLE timer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> frame_pending_{false};
    std::atomic<UINT> refresh_rate_{60};
};

} // namespace

class WinApp {
public:
    bool initialize(HINSTANCE instance, app::DiscoveryResult discovery) {
        instance_ = instance;
        smoke_test_ = discovery.smoke_test;
        settings_ = app::LoadSettings();
        app::ApplyStoredSettings(discovery, settings_);
        std::string error;
        if (!overlay_.create(instance, error)) {
            MessageBoxA(nullptr, error.c_str(), "DictScribe", MB_ICONERROR | MB_OK);
            return false;
        }
        overlay_.set_language_handler(
            [this](std::string language) { select_language(std::move(language)); });
        overlay_.set_settings_handler([this]() { show_settings(); });
        overlay_.set_geometry_handler([this](POINT position, SIZE size) {
            settings_.overlay_position = app::ScreenPosition{position.x, position.y};
            settings_.overlay_size = app::ScreenSize{size.cx, size.cy};
            persist_settings();
            if (overlay_.visible()) configure_overlay_animation();
        });
        if (settings_.overlay_position) {
            overlay_.set_preferred_position(POINT{
                settings_.overlay_position->x,
                settings_.overlay_position->y,
            });
        }
        if (!settings_window_.create(instance, error)) {
            MessageBoxA(nullptr, error.c_str(), "DictScribe", MB_ICONERROR | MB_OK);
            return false;
        }
        settings_window_.set_action_handler(
            [this](ui::SettingsAction action) { apply_settings_action(action); });
        if (!pipeline_debug_window_.create(instance, error)) {
            MessageBoxA(nullptr, error.c_str(), "DictScribe", MB_ICONERROR | MB_OK);
            return false;
        }
        overlay_.set_appearance(settings_.overlay_appearance);
        if (settings_.overlay_size) {
            overlay_.set_preferred_size(SIZE{
                settings_.overlay_size->width,
                settings_.overlay_size->height,
            });
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = instance;
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = kControlWindowClass;
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE));
        window_class.hIconSm = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"Could not register the DictScribe control window.", L"DictScribe", MB_ICONERROR);
            return false;
        }
        control_window_ = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kControlWindowClass,
            L"DictScribe",
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            this);
        if (!control_window_) return false;
        animation_scheduler_available_ = animation_scheduler_.initialize(control_window_);

        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        add_tray_icon();
        if (!RegisterHotKey(
                control_window_,
                kToggleHotkey,
                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                VK_SPACE)) {
            show_notification(
                L"Shortcut unavailable",
                L"Ctrl+Alt+Space is already used by another application. You can still start from the tray icon.",
                NIIF_WARNING);
        }

        if (!discovery.error.empty()) {
            controller_.set_startup_error(discovery.error);
            show_notification(L"Models unavailable", L"Open DictScribe to see which local file is missing.", NIIF_WARNING);
        } else {
            controller_.start(discovery.config);
        }
        previous_mode_ = controller_.snapshot().mode;
        SetTimer(control_window_, kTickTimer, kTickIntervalMilliseconds, nullptr);
        return true;
    }

    int run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        WinApp* self = reinterpret_cast<WinApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<WinApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w_param, l_param);
        if (message == self->taskbar_created_message_) {
            self->add_tray_icon();
            return 0;
        }

        switch (message) {
        case WM_HOTKEY:
            self->handle_hotkey(static_cast<int>(w_param));
            return 0;
        case WM_TIMER:
            if (w_param == kTickTimer) self->tick();
            return 0;
        case WM_COMMAND:
            self->handle_command(LOWORD(w_param));
            return 0;
        case kShowSettingsMessage:
            self->show_settings_now();
            return 0;
        case kAnimationFrameMessage:
            if (self->overlay_.visible()) self->overlay_.animation_frame();
            // Keep the frame marked as pending until Skia has rendered and
            // presented it. Otherwise a high-refresh scheduler can keep one
            // posted message permanently queued and starve mouse/keyboard
            // input on the UI thread.
            self->animation_scheduler_.frame_handled();
            return 0;
        case kTrayMessage:
            if (LOWORD(l_param) == WM_LBUTTONDBLCLK) {
                self->toggle_dictation();
            } else if (LOWORD(l_param) == WM_RBUTTONUP || LOWORD(l_param) == WM_CONTEXTMENU) {
                self->show_tray_menu();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            self->animation_scheduler_.shutdown();
            self->remove_tray_icon();
            UnregisterHotKey(hwnd, kToggleHotkey);
            self->unregister_session_hotkeys();
            KillTimer(hwnd, kTickTimer);
            PostQuitMessage(self->exit_code_);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, w_param, l_param);
        }
    }

    void handle_hotkey(int id) {
        if (id == kAcceptHotkey || id == kCancelHotkey) {
            const WPARAM key = id == kAcceptHotkey ? VK_RETURN : VK_ESCAPE;
            if (settings_window_.language_menu_open()) {
                SendMessageW(settings_window_.window(), WM_KEYDOWN, key, 0);
                return;
            }
            if (overlay_.language_menu_open()) {
                SendMessageW(overlay_.window(), WM_KEYDOWN, key, 0);
                return;
            }
        }
        if (id == kCancelHotkey) {
            cancel_dictation();
        } else if (id == kToggleHotkey || id == kAcceptHotkey) {
            if (session_active_) refresh_target_context();
            toggle_dictation();
        }
    }

    void toggle_dictation() {
        const app::AppSnapshot snapshot = controller_.snapshot();
        if (snapshot.mode == app::DictationMode::Ready || snapshot.mode == app::DictationMode::Complete) {
            target_ = CaptureTargetContext(overlay_.window(), control_window_);
            session_active_ = true;
            notice_.clear();
            register_session_hotkeys();
            overlay_.update(snapshot);
            overlay_.show_near(target_);
            configure_overlay_animation();
            controller_.toggle_recording();
            return;
        }
        if (snapshot.mode == app::DictationMode::Recording) {
            controller_.toggle_recording();
            return;
        }
        if (snapshot.mode == app::DictationMode::Starting || snapshot.mode == app::DictationMode::Error) {
            target_ = CaptureTargetContext(overlay_.window(), control_window_);
            overlay_.update(snapshot);
            overlay_.show_near(target_);
        }
    }

    void cancel_dictation() {
        const auto snapshot = controller_.snapshot();
        if (!app::CanCancel(snapshot)) return;
        controller_.cancel_recording();
        session_active_ = false;
        unregister_session_hotkeys();
        animation_scheduler_.pause();
        overlay_.hide();
    }

    void tick() {
        if (session_active_) refresh_target_context();
        controller_.tick();
        const app::AppSnapshot snapshot = controller_.snapshot();
        if (app::ReconcilePendingDeviceSettings(
                snapshot, pending_devices_, settings_, settings_notice_)) {
            persist_settings();
        }
        overlay_.update(snapshot, notice_);
        if (!animation_scheduler_available_ && overlay_.visible()) {
            overlay_.animation_frame();
        }
        settings_window_.update(settings_view_model(snapshot));
        pipeline_debug_window_.update(snapshot.pipeline_debug);

        const bool smoke_ready = snapshot.mode == app::DictationMode::Ready &&
            (snapshot.cleanup_mode == app::CleanupMode::Off || snapshot.rewrite_ready);
        const bool smoke_failed = snapshot.mode == app::DictationMode::Error ||
            (snapshot.mode == app::DictationMode::Ready &&
             snapshot.cleanup_mode == app::CleanupMode::Ai &&
             !snapshot.rewrite_ready && !snapshot.error.empty());
        if (smoke_test_ && (smoke_ready || smoke_failed)) {
            exit_code_ = smoke_ready ? 0 : 1;
            DestroyWindow(control_window_);
            return;
        }

        if (previous_mode_ == app::DictationMode::Starting && snapshot.mode == app::DictationMode::Ready) {
            show_notification(L"DictScribe is ready", L"Press Ctrl+Alt+Space in any text field to dictate.", NIIF_INFO);
        }
        previous_mode_ = snapshot.mode;

        if (session_active_ && snapshot.mode == app::DictationMode::Complete) {
            commit(snapshot);
        } else if (session_active_ && snapshot.mode == app::DictationMode::Error) {
            session_active_ = false;
            unregister_session_hotkeys();
            animation_scheduler_.pause();
            overlay_.hide();
        }
    }

    void commit(const app::AppSnapshot& snapshot) {
        session_active_ = false;
        unregister_session_hotkeys();
        animation_scheduler_.pause();
        overlay_.hide();
        const std::string& text = !snapshot.rewritten_text.empty()
            ? snapshot.rewritten_text
            : (!snapshot.raw_final_text.empty() ? snapshot.raw_final_text : snapshot.live_text);
        if (text.empty()) return;

        std::string error;
        if (!InsertText(target_, text, error)) {
            std::string clipboard_error;
            if (PutTextOnClipboard(text, clipboard_error)) {
                show_notification(
                    L"Text copied to clipboard",
                    L"DictScribe could not safely type into the active field. Press Ctrl+V to insert it.",
                    NIIF_WARNING);
            } else {
                show_notification(
                    L"Text insertion failed",
                    L"DictScribe is still running, but Windows rejected both text input and clipboard access.",
                    NIIF_ERROR);
            }
        }
    }

    void refresh_target_context() {
        const TargetContext candidate = CaptureTargetContext(overlay_.window(), control_window_);
        if (!candidate.window) return;
        target_ = candidate;
    }

    void configure_overlay_animation() {
        const UINT refresh_rate = MonitorRefreshRate(overlay_.window());
        if (animation_scheduler_available_) {
            overlay_.set_animation_refresh_rate(static_cast<float>(refresh_rate));
            animation_scheduler_.start(refresh_rate);
        } else {
            overlay_.set_animation_refresh_rate(60.0F);
        }
    }

    void register_session_hotkeys() {
        RegisterHotKey(control_window_, kAcceptHotkey, MOD_NOREPEAT, VK_RETURN);
        RegisterHotKey(control_window_, kCancelHotkey, MOD_NOREPEAT, VK_ESCAPE);
    }

    void unregister_session_hotkeys() {
        if (!control_window_) return;
        UnregisterHotKey(control_window_, kAcceptHotkey);
        UnregisterHotKey(control_window_, kCancelHotkey);
    }

    void add_tray_icon() {
        if (!control_window_) return;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = control_window_;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = kTrayMessage;
        icon.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_DICTSCRIBE));
        wcscpy_s(icon.szTip, L"DictScribe — Ctrl+Alt+Space");
        Shell_NotifyIconW(NIM_ADD, &icon);
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
        tray_added_ = true;
    }

    void remove_tray_icon() {
        if (!tray_added_) return;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = control_window_;
        icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        tray_added_ = false;
    }

    void show_notification(const wchar_t* title, const wchar_t* message, DWORD flags) {
        if (!tray_added_) return;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = control_window_;
        icon.uID = 1;
        icon.uFlags = NIF_INFO;
        wcscpy_s(icon.szInfoTitle, title);
        wcscpy_s(icon.szInfo, message);
        icon.dwInfoFlags = flags;
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }

    void show_tray_menu() {
        const app::AppSnapshot snapshot = controller_.snapshot();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCommandToggle, ToggleMenuLabel(snapshot.mode));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandSettings, L"Settings...");
        AppendMenuW(menu, MF_STRING, kCommandPipelineDebug, L"Pipeline debugger...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU language_menu = CreatePopupMenu();
        for (std::size_t index = 0; index < app::kLanguageOptions.size(); ++index) {
            const auto& option = app::kLanguageOptions[index];
            const std::wstring label(option.label.begin(), option.label.end());
            AppendMenuW(
                language_menu,
                MF_STRING | (snapshot.language == option.code ? MF_CHECKED : 0),
                kCommandLanguageBase + static_cast<UINT>(index),
                label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language_menu), L"Dictation language");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandQuit, L"Quit DictScribe");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(control_window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, control_window_, nullptr);
        DestroyMenu(menu);
    }

    void handle_command(UINT command) {
        if (command >= kCommandLanguageBase &&
            command < kCommandLanguageBase + app::kLanguageOptions.size()) {
            select_language(std::string(
                app::kLanguageOptions[command - kCommandLanguageBase].code));
            return;
        }
        switch (command) {
        case kCommandToggle:
            toggle_dictation();
            break;
        case kCommandSettings:
            show_settings();
            break;
        case kCommandPipelineDebug:
            pipeline_debug_window_.update(controller_.snapshot().pipeline_debug);
            pipeline_debug_window_.show();
            break;
        case kCommandQuit:
            DestroyWindow(control_window_);
            break;
        default:
            break;
        }
    }

    void select_language(std::string language) {
        controller_.set_language(std::move(language));
        const std::string selected = controller_.snapshot().language;
        if (settings_.language == selected) return;
        settings_.language = selected;
        persist_settings();
    }

    ui::SettingsViewModel settings_view_model(const app::AppSnapshot& snapshot) const {
        ui::SettingsViewModel model;
        model.settings = settings_;
        model.settings.language = snapshot.language;
        model.settings.cleanup_mode = snapshot.cleanup_mode;
        model.settings.asr_device = snapshot.asr_use_gpu
            ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        model.settings.rewrite_device = snapshot.rewrite_use_gpu
            ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
        model.asr_model_name = snapshot.asr_model_name;
        model.rewrite_model_name = snapshot.rewrite_model_name;
        model.device_controls_enabled = app::CanSetComputeDevice(snapshot);
        model.notice = settings_notice_;
        return model;
    }

    void show_settings() {
        // Opening Settings directly from a tray command or the no-activate
        // overlay races the window that is currently finishing mouse/menu
        // activation. Defer the actual activation until that dispatch has
        // returned so Windows does not leave Settings behind another window.
        if (control_window_ &&
            PostMessageW(control_window_, kShowSettingsMessage, 0, 0)) {
            return;
        }
        show_settings_now();
    }

    void show_settings_now() {
        settings_window_.update(settings_view_model(controller_.snapshot()));
        settings_window_.show();
    }

    void apply_settings_action(ui::SettingsAction action) {
        if (ui::IsLanguageSelection(action)) {
            const std::size_t index = ui::LanguageSelectionIndex(action);
            if (index < app::kLanguageOptions.size()) {
                select_language(std::string(app::kLanguageOptions[index].code));
                settings_window_.update(settings_view_model(controller_.snapshot()));
            }
            return;
        }
        if (action == ui::SettingsAction::CleanupOff ||
            action == ui::SettingsAction::CleanupAi) {
            const app::CleanupMode mode = action == ui::SettingsAction::CleanupAi
                ? app::CleanupMode::Ai : app::CleanupMode::Off;
            if (controller_.set_cleanup_mode(mode)) {
                settings_.cleanup_mode = mode;
                persist_settings();
            }
            settings_window_.update(settings_view_model(controller_.snapshot()));
            return;
        }
        if (action == ui::SettingsAction::OverlayGlass ||
            action == ui::SettingsAction::OverlaySolid) {
            settings_.overlay_appearance = action == ui::SettingsAction::OverlaySolid
                ? app::OverlayAppearance::Solid
                : app::OverlayAppearance::Glass;
            overlay_.set_appearance(settings_.overlay_appearance);
            persist_settings();
            settings_window_.update(settings_view_model(controller_.snapshot()));
            return;
        }

        bool changed = false;
        if (action == ui::SettingsAction::AsrCpu || action == ui::SettingsAction::AsrGpu) {
            const bool gpu = action == ui::SettingsAction::AsrGpu;
            changed = controller_.set_asr_device(gpu);
            if (changed) {
                pending_devices_.asr_device = gpu
                    ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
            }
        } else if (
            action == ui::SettingsAction::RewriteCpu ||
            action == ui::SettingsAction::RewriteGpu) {
            const bool gpu = action == ui::SettingsAction::RewriteGpu;
            changed = controller_.set_rewrite_device(gpu);
            if (changed) {
                pending_devices_.rewrite_device = gpu
                    ? app::ComputeDevice::Gpu : app::ComputeDevice::Cpu;
            }
        }
        settings_window_.update(settings_view_model(controller_.snapshot()));
    }

    void persist_settings() {
        std::string error;
        if (!app::SaveSettings(settings_, error)) {
            settings_notice_ = error;
            OutputDebugStringA(("DictScribe settings: " + error + "\n").c_str());
        } else {
            settings_notice_.clear();
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND control_window_ = nullptr;
    UINT taskbar_created_message_ = 0;
    bool tray_added_ = false;
    bool session_active_ = false;
    bool smoke_test_ = false;
    bool animation_scheduler_available_ = false;
    int exit_code_ = 0;
    app::DictationMode previous_mode_ = app::DictationMode::Starting;
    TargetContext target_;
    std::string notice_;
    std::string settings_notice_;
    app::AppSettings settings_;
    app::PendingDeviceSettings pending_devices_;
    app::AppController controller_;
    WinOverlay overlay_;
    RefreshFrameScheduler animation_scheduler_;
    WinSettingsWindow settings_window_;
    WinPipelineDebugWindow pipeline_debug_window_;
};

} // namespace dictscribe::win

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize_com = SUCCEEDED(com_result);

    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, dictscribe::win::kSingleInstanceName);
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 0;
    }

    auto arguments = dictscribe::win::ReadCommandLine();
    auto discovery = dictscribe::app::DiscoverConfig(
        static_cast<int>(arguments.pointers.size()), arguments.pointers.data());
    if (discovery.show_version || discovery.show_help) {
        MessageBoxW(
            nullptr,
            discovery.show_version ? L"DictScribe 0.1.0" : L"Use Ctrl+Alt+Space to start or stop dictation.",
            L"DictScribe",
            MB_OK | MB_ICONINFORMATION);
        if (instance_mutex) CloseHandle(instance_mutex);
        if (uninitialize_com) CoUninitialize();
        return 0;
    }

    dictscribe::win::WinApp app;
    const int result = app.initialize(instance, std::move(discovery)) ? app.run() : 1;
    if (instance_mutex) CloseHandle(instance_mutex);
    if (uninitialize_com) CoUninitialize();
    return result;
}
