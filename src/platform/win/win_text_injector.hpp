#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <string_view>

namespace dictscribe::win {

struct TargetContext {
    HWND window = nullptr;
    POINT anchor{};
    bool caret_anchor = false;
};

TargetContext CaptureTargetContext(HWND overlay_window);
bool InsertText(const TargetContext& target, std::string_view text, std::string& error);
bool PutTextOnClipboard(std::string_view text, std::string& error);

} // namespace dictscribe::win
