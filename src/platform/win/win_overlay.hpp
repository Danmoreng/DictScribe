#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app/app_controller.hpp"
#include "app/settings.hpp"
#include "platform/win/win_text_injector.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dictscribe::win {

class WinOverlay {
public:
    WinOverlay();
    ~WinOverlay();

    WinOverlay(const WinOverlay&) = delete;
    WinOverlay& operator=(const WinOverlay&) = delete;

    bool create(HINSTANCE instance, std::string& error);
    void set_language_handler(std::function<void(std::string)> handler);
    void set_settings_handler(std::function<void()> handler);
    void set_geometry_handler(std::function<void(POINT, SIZE)> handler);
    void set_preferred_position(std::optional<POINT> position);
    void set_preferred_size(std::optional<SIZE> size);
    void set_appearance(app::OverlayAppearance appearance);
    void set_animation_refresh_rate(float refresh_rate);
    void animation_frame();
    void update(const app::AppSnapshot& snapshot, std::string notice = {});
    void show_near(const TargetContext& target);
    void hide();

    [[nodiscard]] HWND window() const;
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool language_menu_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dictscribe::win
