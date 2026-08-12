#include "platform/win/win_text_injector.hpp"

#include <chrono>
#include <cstring>
#include <objidl.h>
#include <ole2.h>
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

} // namespace

TargetContext CaptureTargetContext(HWND overlay_window) {
    TargetContext result;
    result.window = GetForegroundWindow();
    if (result.window == overlay_window || !IsWindow(result.window)) {
        result.window = nullptr;
    }

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(0, &info) && info.hwndCaret && info.hwndCaret != overlay_window) {
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
    return SetClipboardText(wide, error);
}

bool InsertText(const TargetContext& target, std::string_view text, std::string& error) {
    if (!target.window || !IsWindow(target.window)) {
        error = "The text field that was active when dictation started is no longer available.";
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

    IDataObject* previous_clipboard = nullptr;
    OleGetClipboard(&previous_clipboard);

    if (!PutTextOnClipboard(text, error)) {
        if (previous_clipboard) previous_clipboard->Release();
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

    const UINT sent = SendInput(4, inputs, sizeof(INPUT));
    if (sent != 4) {
        error = WindowsError("Could not send the paste shortcut");
        if (previous_clipboard) previous_clipboard->Release();
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (previous_clipboard) {
        if (SUCCEEDED(OleSetClipboard(previous_clipboard))) {
            OleFlushClipboard();
        }
        previous_clipboard->Release();
    } else if (OpenClipboardWithRetry(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
    return true;
}

} // namespace dictscribe::win
