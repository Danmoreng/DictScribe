#include "platform/win/win_text_injector.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kTargetWindowClass[] = L"DictScribeTestTextTarget";

LRESULT CALLBACK TargetWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE: {
        HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            12,
            12,
            440,
            120,
            window,
            reinterpret_cast<HMENU>(1),
            GetModuleHandleW(nullptr),
            nullptr);
        SetFocus(edit);
        return edit ? 0 : -1;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(window, 1));
        return 0;
    case WM_APP + 1:
        SetForegroundWindow(window);
        SetFocus(GetDlgItem(window, 1));
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

int RunTargetProcess() {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = TargetWindowProc;
    window_class.lpszClassName = kTargetWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    if (!RegisterClassExW(&window_class)) return 2;

    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kTargetWindowClass,
        L"DictScribe text insertion test target",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80,
        80,
        490,
        200,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!window) return 2;

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(GetDlgItem(window, 1));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct WindowSearch {
    DWORD process_id = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindTargetWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == search->process_id) {
        wchar_t class_name[128]{};
        GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
        if (std::wstring_view(class_name) == kTargetWindowClass) {
            search->window = window;
            return FALSE;
        }
    }
    return TRUE;
}

HWND WaitForTargetWindow(DWORD process_id) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        WindowSearch search{process_id, nullptr};
        EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) return search.window;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return nullptr;
}

bool ForceForegroundWindow(HWND window) {
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
    const bool attached_foreground = foreground_thread != 0 && foreground_thread != current_thread &&
        AttachThreadInput(current_thread, foreground_thread, TRUE);
    const bool attached_target = target_thread != 0 && target_thread != current_thread &&
        target_thread != foreground_thread && AttachThreadInput(current_thread, target_thread, TRUE);

    BringWindowToTop(window);
    const bool activated = SetForegroundWindow(window) != FALSE;
    SetFocus(GetDlgItem(window, 1));

    if (attached_target) AttachThreadInput(current_thread, target_thread, FALSE);
    if (attached_foreground) AttachThreadInput(current_thread, foreground_thread, FALSE);
    return activated && GetForegroundWindow() == window;
}

std::wstring ReadEditText(HWND edit) {
    const auto length = static_cast<std::size_t>(SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0));
    std::vector<wchar_t> buffer(length + 1, L'\0');
    SendMessageW(
        edit,
        WM_GETTEXT,
        static_cast<WPARAM>(buffer.size()),
        reinterpret_cast<LPARAM>(buffer.data()));
    return std::wstring(buffer.data());
}

std::wstring ReadClipboardText() {
    if (!OpenClipboard(nullptr)) return {};
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const auto* text = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    const std::wstring result = text ? text : L"";
    if (text) GlobalUnlock(data);
    CloseClipboard();
    return result;
}

void CloseTarget(PROCESS_INFORMATION& process, HWND window) {
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    if (process.hProcess) {
        if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 2);
            WaitForSingleObject(process.hProcess, INFINITE);
        }
        CloseHandle(process.hProcess);
    }
    if (process.hThread) CloseHandle(process.hThread);
    process = {};
}

int RunParentTest() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) {
        std::cerr << "Could not resolve test executable path.\n";
        return 1;
    }
    std::wstring command_line = L"\"" + std::wstring(executable) + L"\" --target";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable,
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        std::cerr << "Could not start text target process.\n";
        return 1;
    }

    HWND target_window = WaitForTargetWindow(process.dwProcessId);
    if (!target_window) {
        CloseTarget(process, nullptr);
        std::cerr << "Text target window did not appear.\n";
        return 1;
    }

    ForceForegroundWindow(target_window);

    const auto captured = dictscribe::win::CaptureTargetContext(nullptr, nullptr);
    if (captured.window != target_window) {
        CloseTarget(process, target_window);
        std::cerr << "External foreground target was not captured (expected="
                  << target_window << ", foreground=" << GetForegroundWindow()
                  << ", captured=" << captured.window << ").\n";
        return 1;
    }
    if (dictscribe::win::CaptureTargetContext(target_window, nullptr).window) {
        CloseTarget(process, target_window);
        std::cerr << "Excluded DictScribe window was captured as a target.\n";
        return 1;
    }

    std::string clipboard_error;
    constexpr std::string_view clipboard_sentinel = "clipboard sentinel";
    if (!dictscribe::win::PutTextOnClipboard(clipboard_sentinel, clipboard_error)) {
        CloseTarget(process, target_window);
        std::cerr << clipboard_error << '\n';
        return 1;
    }

    constexpr std::string_view dictated = "Unicode input: äöü ß ✓";
    std::string insertion_error;
    if (!dictscribe::win::InsertText(captured, dictated, insertion_error)) {
        CloseTarget(process, target_window);
        std::cerr << insertion_error << '\n';
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::wstring inserted = ReadEditText(GetDlgItem(target_window, 1));
    const std::wstring clipboard = ReadClipboardText();
    HWND own_window = CreateWindowExW(
        0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    dictscribe::win::TargetContext own_target;
    own_target.window = own_window;
    std::string own_error;
    const bool accepted_own_window = dictscribe::win::InsertText(own_target, "blocked", own_error);
    DestroyWindow(own_window);
    CloseTarget(process, target_window);

    if (inserted != L"Unicode input: äöü ß ✓") {
        std::wcerr << L"Unexpected inserted text: " << inserted << L'\n';
        return 1;
    }
    if (clipboard != L"clipboard sentinel") {
        std::wcerr << L"Direct insertion changed the clipboard.\n";
        return 1;
    }
    if (accepted_own_window) {
        std::cerr << "DictScribe accepted its own window as an insertion target.\n";
        return 1;
    }

    std::cout << "Windows target capture and Unicode insertion tests passed.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--target") {
        return RunTargetProcess();
    }
    return RunParentTest();
}
