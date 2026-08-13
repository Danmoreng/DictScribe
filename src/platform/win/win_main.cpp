#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "app/app_controller.hpp"
#include "app/model_discovery.hpp"
#include "app/settings.hpp"
#include "platform/win/win_overlay.hpp"
#include "platform/win/win_pipeline_debug_window.hpp"
#include "platform/win/resource.h"
#include "platform/win/win_settings_window.hpp"
#include "platform/win/win_text_injector.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dictscribe::win {

namespace {

constexpr wchar_t kControlWindowClass[] = L"DictScribeControlWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\DictScribe.Desktop";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTickTimer = 1;
constexpr int kToggleHotkey = 1;
constexpr int kAcceptHotkey = 2;
constexpr int kCancelHotkey = 3;
constexpr UINT kCommandToggle = 100;
constexpr UINT kCommandSettings = 101;
constexpr UINT kCommandPipelineDebug = 102;
constexpr UINT kCommandLanguageAuto = 110;
constexpr UINT kCommandLanguageGerman = 111;
constexpr UINT kCommandLanguageEnglish = 112;
constexpr UINT kCommandQuit = 199;

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
        overlay_.set_position_handler([this](POINT position) {
            settings_.overlay_position = app::ScreenPosition{position.x, position.y};
            persist_settings();
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
        SetTimer(control_window_, kTickTimer, 33, nullptr);
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
            overlay_.hide();
        }
    }

    void commit(const app::AppSnapshot& snapshot) {
        session_active_ = false;
        unregister_session_hotkeys();
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
        AppendMenuW(
            menu,
            MF_STRING | (snapshot.language == "auto" ? MF_CHECKED : 0),
            kCommandLanguageAuto,
            L"Language: Auto");
        AppendMenuW(
            menu,
            MF_STRING | (snapshot.language == "de" ? MF_CHECKED : 0),
            kCommandLanguageGerman,
            L"Language: Deutsch");
        AppendMenuW(
            menu,
            MF_STRING | (snapshot.language == "en" ? MF_CHECKED : 0),
            kCommandLanguageEnglish,
            L"Language: English");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandQuit, L"Quit DictScribe");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(control_window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, control_window_, nullptr);
        DestroyMenu(menu);
    }

    void handle_command(UINT command) {
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
        case kCommandLanguageAuto:
            select_language("auto");
            break;
        case kCommandLanguageGerman:
            select_language("de");
            break;
        case kCommandLanguageEnglish:
            select_language("en");
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
        settings_window_.update(settings_view_model(controller_.snapshot()));
        settings_window_.show();
    }

    void apply_settings_action(ui::SettingsAction action) {
        if (action == ui::SettingsAction::LanguageAuto) {
            select_language("auto");
            return;
        }
        if (action == ui::SettingsAction::LanguageGerman) {
            select_language("de");
            return;
        }
        if (action == ui::SettingsAction::LanguageEnglish) {
            select_language("en");
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
    int exit_code_ = 0;
    app::DictationMode previous_mode_ = app::DictationMode::Starting;
    TargetContext target_;
    std::string notice_;
    std::string settings_notice_;
    app::AppSettings settings_;
    app::PendingDeviceSettings pending_devices_;
    app::AppController controller_;
    WinOverlay overlay_;
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
