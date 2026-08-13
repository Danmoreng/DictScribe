#include "platform/win/win_text_injector.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>
#include <vector>

namespace dictscribe::win {

namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            size) <= 0) {
        return {};
    }
    return result;
}

std::string WindowsError(const char* prefix, DWORD code = GetLastError()) {
    return std::string(prefix) + ": " +
        std::system_category().message(static_cast<int>(code));
}

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool SetClipboardText(std::wstring_view text, std::string& error) {
    if (!OpenClipboardWithRetry(nullptr)) {
        error = WindowsError("Could not open the clipboard");
        return false;
    }
    if (!EmptyClipboard()) {
        const DWORD code = GetLastError();
        CloseClipboard();
        error = WindowsError("Could not clear the clipboard", code);
        return false;
    }

    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        const DWORD code = GetLastError();
        CloseClipboard();
        error = WindowsError("Could not allocate clipboard memory", code);
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        const DWORD code = GetLastError();
        GlobalFree(memory);
        CloseClipboard();
        error = WindowsError("Could not lock clipboard memory", code);
        return false;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        const DWORD code = GetLastError();
        GlobalFree(memory);
        CloseClipboard();
        error = WindowsError("Could not place text on the clipboard", code);
        return false;
    }
    CloseClipboard();
    return true;
}

std::wstring NormalizeClipboardNewlines(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character == L'\r') {
            result.append(L"\r\n");
            if (index + 1 < text.size() && text[index + 1] == L'\n') ++index;
        } else if (character == L'\n') {
            result.append(L"\r\n");
        } else {
            result.push_back(character);
        }
    }
    return result;
}

bool ContainsLineBreak(std::string_view text) {
    return text.find_first_of("\r\n") != std::string_view::npos;
}

bool IsExternalTargetWindow(HWND window, HWND overlay_window, HWND control_window) {
    if (!window || !IsWindow(window) || window == overlay_window || window == control_window ||
        window == GetDesktopWindow() || window == GetShellWindow()) {
        return false;
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    return process_id != 0 && process_id != GetCurrentProcessId();
}

bool SendUnicodeText(std::wstring_view text, std::string& error) {
    if (text.size() > std::numeric_limits<UINT>::max() / 2U) {
        error = "The dictated text is too large to insert safely.";
        return false;
    }

    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2U);
    for (const wchar_t code_unit : text) {
        INPUT key_down{};
        key_down.type = INPUT_KEYBOARD;
        key_down.ki.wScan = code_unit;
        key_down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(key_down);

        INPUT key_up = key_down;
        key_up.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(key_up);
    }
    if (inputs.empty()) {
        return true;
    }

    const UINT requested = static_cast<UINT>(inputs.size());
    const UINT sent = SendInput(requested, inputs.data(), sizeof(INPUT));
    if (sent != requested) {
        const DWORD code = GetLastError();
        error = code == ERROR_SUCCESS
            ? "Windows did not accept the complete Unicode input."
            : WindowsError("Windows did not accept the complete Unicode input", code);
        return false;
    }
    return true;
}

bool WaitForPasteModifiersReleased() {
    constexpr int kModifierKeys[] = {
        VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
    };
    for (int attempt = 0; attempt < 30; ++attempt) {
        const bool released = std::all_of(
            std::begin(kModifierKeys),
            std::end(kModifierKeys),
            [](int key) { return (GetAsyncKeyState(key) & 0x8000) == 0; });
        if (released) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool SendClipboardPaste(std::string& error) {
    if (!WaitForPasteModifiersReleased()) {
        error = "A modifier key is still held. The text remains on the clipboard for manual paste.";
        return false;
    }

    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    const UINT sent = SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
    if (sent != static_cast<UINT>(std::size(inputs))) {
        const DWORD code = GetLastError();
        error = code == ERROR_SUCCESS
            ? "Windows did not accept the multiline paste shortcut."
            : WindowsError("Windows did not accept the multiline paste shortcut", code);
        return false;
    }
    return true;
}

} // namespace

TargetContext CaptureTargetContext(HWND overlay_window, HWND control_window) {
    TargetContext result;
    HWND foreground = GetForegroundWindow();
    if (foreground) {
        foreground = GetAncestor(foreground, GA_ROOT);
    }
    if (IsExternalTargetWindow(foreground, overlay_window, control_window)) {
        result.window = foreground;
    }

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    DWORD target_process_id = 0;
    DWORD caret_process_id = 0;
    if (result.window) {
        GetWindowThreadProcessId(result.window, &target_process_id);
    }
    if (GetGUIThreadInfo(0, &info) && info.hwndCaret) {
        GetWindowThreadProcessId(info.hwndCaret, &caret_process_id);
    }
    if (result.window && caret_process_id == target_process_id) {
        POINT caret{info.rcCaret.left, info.rcCaret.bottom};
        if (ClientToScreen(info.hwndCaret, &caret)) {
            result.anchor = caret;
            result.caret_anchor = true;
            return result;
        }
    }

    GetCursorPos(&result.anchor);
    return result;
}

bool PutTextOnClipboard(std::string_view text, std::string& error) {
    const std::wstring wide = Utf8ToWide(text);
    if (wide.empty() && !text.empty()) {
        error = "Could not convert the dictated text to UTF-16.";
        return false;
    }
    return SetClipboardText(NormalizeClipboardNewlines(wide), error);
}

bool InsertText(const TargetContext& target, std::string_view text, std::string& error) {
    if (!target.window || !IsWindow(target.window)) {
        error = "No usable external target window is available for the dictated text.";
        return false;
    }

    DWORD target_process_id = 0;
    GetWindowThreadProcessId(target.window, &target_process_id);
    if (target_process_id == 0 || target_process_id == GetCurrentProcessId()) {
        error = "DictScribe refused to insert text into one of its own windows.";
        return false;
    }

    if (GetForegroundWindow() != target.window) {
        SetForegroundWindow(target.window);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (GetForegroundWindow() != target.window) {
            error = "Windows did not allow DictScribe to restore the original application.";
            return false;
        }
    }

    const std::wstring wide = Utf8ToWide(text);
    if (wide.empty() && !text.empty()) {
        error = "Could not convert the dictated text to UTF-16.";
        return false;
    }
    if (!ContainsLineBreak(text)) return SendUnicodeText(wide, error);

    // A physical Enter can submit a form or send a chat message. Keep multiline
    // insertion application-neutral by putting the complete result on the
    // clipboard and issuing only Ctrl+V. The dictated text intentionally remains
    // on the clipboard, which also provides a recoverable manual-paste fallback.
    if (!SetClipboardText(NormalizeClipboardNewlines(wide), error)) return false;
    return SendClipboardPaste(error);
}

} // namespace dictscribe::win
