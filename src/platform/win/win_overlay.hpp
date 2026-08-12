#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app/app_controller.hpp"
#include "platform/win/win_text_injector.hpp"

#include <memory>
#include <string>

namespace dictscribe::win {

class WinOverlay {
public:
    WinOverlay();
    ~WinOverlay();

    WinOverlay(const WinOverlay&) = delete;
    WinOverlay& operator=(const WinOverlay&) = delete;

    bool create(HINSTANCE instance, std::string& error);
    void update(const app::AppSnapshot& snapshot, std::string notice = {});
    void show_near(const TargetContext& target);
    void hide();

    [[nodiscard]] HWND window() const;
    [[nodiscard]] bool visible() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dictscribe::win
