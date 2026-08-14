#include "platform/win/win_pipeline_debug_window.hpp"
#include "platform/win/resource.h"

#include <dwmapi.h>
#include <richedit.h>

#include <array>
#include <cstring>
#include <utility>

namespace dictscribe::win {

namespace {

constexpr wchar_t kWindowClass[] = L"DictScribePipelineDebugWindow";
constexpr wchar_t kRichEditClass[] = L"RICHEDIT50W";
constexpr int kCopyButton = 1001;

COLORREF WindowColor(app::ColorTheme theme) {
    return theme == app::ColorTheme::Light ? RGB(246, 248, 252) : RGB(14, 17, 23);
}

COLORREF PaneColor(app::ColorTheme theme) {
    return theme == app::ColorTheme::Light ? RGB(255, 255, 255) : RGB(19, 24, 33);
}

COLORREF TextColor(app::ColorTheme theme) {
    return theme == app::ColorTheme::Light ? RGB(27, 34, 47) : RGB(229, 233, 240);
}

COLORREF MutedColor(app::ColorTheme theme) {
    return theme == app::ColorTheme::Light ? RGB(84, 96, 116) : RGB(154, 164, 181);
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"<invalid UTF-8>";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}

bool CopyText(std::wstring_view text) {
    if (!OpenClipboard(nullptr)) return false;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

} // namespace

struct WinPipelineDebugWindow::Impl {
    HWND hwnd = nullptr;
    HWND status = nullptr;
    HWND copy_button = nullptr;
    std::array<HWND, 4> labels{};
    std::array<HWND, 4> edits{};
    std::array<std::wstring, 4> displayed{};
    HFONT ui_font = nullptr;
    HFONT mono_font = nullptr;
    HBRUSH window_brush = nullptr;
    HMODULE rich_edit = nullptr;
    app::PipelineDebugSnapshot snapshot;
    app::ColorTheme theme = app::ColorTheme::Dark;

    void apply_theme() {
        if (!hwnd) return;
        if (window_brush) DeleteObject(window_brush);
        window_brush = CreateSolidBrush(WindowColor(theme));
        const BOOL dark = theme == app::ColorTheme::Dark;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        for (HWND edit : edits) {
            if (!edit) continue;
            SendMessageW(edit, EM_SETBKGNDCOLOR, 0, PaneColor(theme));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR;
            format.crTextColor = TextColor(theme);
            SendMessageW(
                edit, EM_SETCHARFORMAT, SCF_ALL,
                reinterpret_cast<LPARAM>(&format));
        }
        RedrawWindow(
            hwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    void layout() const {
        if (!hwnd) return;
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        constexpr int margin = 16;
        constexpr int gap = 12;
        constexpr int top = 54;
        constexpr int label_height = 24;
        const int column_width = (width - margin * 2 - gap) / 2;
        const int row_height = (height - top - margin - gap) / 2;

        MoveWindow(status, margin, 14, width - 210, 28, TRUE);
        MoveWindow(copy_button, width - 174, 10, 158, 32, TRUE);
        for (int index = 0; index < 4; ++index) {
            const int column = index % 2;
            const int row = index / 2;
            const int x = margin + column * (column_width + gap);
            const int y = top + row * (row_height + gap);
            MoveWindow(labels[index], x, y, column_width, label_height, TRUE);
            MoveWindow(
                edits[index], x, y + label_height, column_width,
                row_height - label_height, TRUE);
        }
    }

    void set_edit_text(int index, std::wstring text, bool scroll_to_end) {
        if (displayed[index] == text) return;
        displayed[index] = std::move(text);
        SetWindowTextW(edits[index], displayed[index].c_str());
        SendMessageW(
            edits[index], EM_SETSEL,
            scroll_to_end ? static_cast<WPARAM>(-1) : 0,
            scroll_to_end ? static_cast<LPARAM>(-1) : 0);
        if (scroll_to_end) SendMessageW(edits[index], EM_SCROLLCARET, 0, 0);
    }

    std::wstring complete_snapshot() const {
        return L"NEMOTRON / ASR\r\n" + displayed[0] +
            L"\r\n\r\nREWRITE REQUEST\r\n" + displayed[1] +
            L"\r\n\r\nREWRITE RESPONSE\r\n" + displayed[2] +
            L"\r\n\r\nEFFECTIVE COMPOSED TEXT\r\n" + displayed[3];
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
        case WM_SIZE:
            self->layout();
            return 0;
        case WM_COMMAND:
            if (LOWORD(w_param) == kCopyButton) {
                CopyText(self->complete_snapshot());
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, reinterpret_cast<HWND>(l_param) == self->status
                ? MutedColor(self->theme) : TextColor(self->theme));
            return reinterpret_cast<LRESULT>(self->window_brush);
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(w_param), &client, self->window_brush);
            return 1;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
            info->ptMinTrackSize = {900, 620};
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            self->hwnd = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, w_param, l_param);
    }
};

WinPipelineDebugWindow::WinPipelineDebugWindow() : impl_(std::make_unique<Impl>()) {}

WinPipelineDebugWindow::~WinPipelineDebugWindow() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->ui_font) DeleteObject(impl_->ui_font);
    if (impl_->mono_font) DeleteObject(impl_->mono_font);
    if (impl_->window_brush) DeleteObject(impl_->window_brush);
    if (impl_->rich_edit) FreeLibrary(impl_->rich_edit);
}

bool WinPipelineDebugWindow::create(HINSTANCE instance, std::string& error) {
    impl_->rich_edit = LoadLibraryW(L"Msftedit.dll");
    if (!impl_->rich_edit) {
        error = "Could not load the Windows rich edit control for pipeline debugging.";
        return false;
    }
    impl_->window_brush = CreateSolidBrush(WindowColor(impl_->theme));
    impl_->ui_font = CreateFontW(
        -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    impl_->mono_font = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH, L"Cascadia Mono");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = Impl::WindowProc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = impl_->window_brush;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_DICTSCRIBE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Could not register the DictScribe pipeline debugger window.";
        return false;
    }

    impl_->hwnd = CreateWindowExW(
        0, kWindowClass, L"DictScribe Pipeline Debugger", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1240, 780, nullptr, nullptr, instance, impl_.get());
    if (!impl_->hwnd) {
        error = "Could not create the DictScribe pipeline debugger window.";
        return false;
    }
    impl_->apply_theme();

    impl_->status = CreateWindowExW(
        0, L"STATIC", L"Live, in memory only · cleared when a new dictation starts",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, impl_->hwnd, nullptr, instance, nullptr);
    impl_->copy_button = CreateWindowExW(
        0, L"BUTTON", L"Copy complete snapshot", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyButton)), instance, nullptr);
    constexpr std::array<const wchar_t*, 4> titles = {
        L"1 · Nemotron / ASR", L"2 · Exact rewrite request JSON",
        L"3 · Worker response and decision", L"4 · Effective composed app text",
    };
    for (int index = 0; index < 4; ++index) {
        impl_->labels[index] = CreateWindowExW(
            0, L"STATIC", titles[index], WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, impl_->hwnd, nullptr, instance, nullptr);
        impl_->edits[index] = CreateWindowExW(
            WS_EX_CLIENTEDGE, kRichEditClass, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL,
            0, 0, 0, 0, impl_->hwnd, nullptr, instance, nullptr);
        if (!impl_->labels[index] || !impl_->edits[index]) {
            error = "Could not create all controls for the pipeline debugger.";
            return false;
        }
        SendMessageW(impl_->labels[index], WM_SETFONT,
            reinterpret_cast<WPARAM>(impl_->ui_font), TRUE);
        SendMessageW(impl_->edits[index], WM_SETFONT,
            reinterpret_cast<WPARAM>(impl_->mono_font), TRUE);
        SendMessageW(impl_->edits[index], EM_SETBKGNDCOLOR, 0, PaneColor(impl_->theme));
        SendMessageW(impl_->edits[index], EM_SETREADONLY, TRUE, 0);
        SendMessageW(impl_->edits[index], EM_SETLIMITTEXT, 1024 * 1024, 0);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR;
        format.crTextColor = TextColor(impl_->theme);
        SendMessageW(impl_->edits[index], EM_SETCHARFORMAT, SCF_ALL,
            reinterpret_cast<LPARAM>(&format));
    }
    SendMessageW(impl_->status, WM_SETFONT, reinterpret_cast<WPARAM>(impl_->ui_font), TRUE);
    SendMessageW(impl_->copy_button, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), TRUE);
    impl_->layout();
    return true;
}

void WinPipelineDebugWindow::set_theme(app::ColorTheme theme) {
    impl_->theme = theme;
    impl_->apply_theme();
}

void WinPipelineDebugWindow::update(const app::PipelineDebugSnapshot& snapshot) {
    if (!impl_->hwnd) return;
    impl_->snapshot = snapshot;
    std::wstring asr = Utf8ToWide(snapshot.asr_stage) + L"\r\nEvent " +
        std::to_wstring(snapshot.asr_event_count) + L"\r\n\r\n" +
        Utf8ToWide(snapshot.nemotron_text);
    std::wstring request = L"Request: " + Utf8ToWide(snapshot.rewrite_request_id) +
        L"\r\nStatus: " + Utf8ToWide(snapshot.rewrite_request_status) + L"\r\n\r\n" +
        (snapshot.rewrite_request_json.empty()
            ? L"<no rewrite request has been dispatched yet>"
            : Utf8ToWide(snapshot.rewrite_request_json));
    std::wstring response = L"Response for: " +
        Utf8ToWide(snapshot.rewrite_response_request_id) +
        L"\r\nDecision: " + Utf8ToWide(snapshot.rewrite_decision) + L"\r\n\r\n" +
        (snapshot.rewrite_response_json.empty()
            ? L"<no worker response yet>"
            : Utf8ToWide(snapshot.rewrite_response_json));
    std::wstring composed = snapshot.composed_text.empty()
        ? L"No composed text yet."
        : Utf8ToWide(snapshot.composed_text);
    impl_->set_edit_text(0, std::move(asr), true);
    impl_->set_edit_text(1, std::move(request), false);
    impl_->set_edit_text(2, std::move(response), false);
    impl_->set_edit_text(3, std::move(composed), true);
}

void WinPipelineDebugWindow::show() {
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(impl_->hwnd);
}

void WinPipelineDebugWindow::hide() {
    if (impl_->hwnd) ShowWindow(impl_->hwnd, SW_HIDE);
}

bool WinPipelineDebugWindow::visible() const {
    return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

} // namespace dictscribe::win
