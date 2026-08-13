#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ui/settings_view.hpp"

#include <functional>
#include <memory>
#include <string>

namespace dictscribe::win {

class WinSettingsWindow {
public:
    WinSettingsWindow();
    ~WinSettingsWindow();

    WinSettingsWindow(const WinSettingsWindow&) = delete;
    WinSettingsWindow& operator=(const WinSettingsWindow&) = delete;

    bool create(HINSTANCE instance, std::string& error);
    void set_action_handler(std::function<void(ui::SettingsAction)> handler);
    void update(ui::SettingsViewModel model);
    void show();
    void hide();

    [[nodiscard]] HWND window() const;
    [[nodiscard]] bool visible() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dictscribe::win
