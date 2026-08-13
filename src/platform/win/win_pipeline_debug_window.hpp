#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "app/app_controller.hpp"

#include <memory>
#include <string>

namespace dictscribe::win {

class WinPipelineDebugWindow {
public:
    WinPipelineDebugWindow();
    ~WinPipelineDebugWindow();

    WinPipelineDebugWindow(const WinPipelineDebugWindow&) = delete;
    WinPipelineDebugWindow& operator=(const WinPipelineDebugWindow&) = delete;

    bool create(HINSTANCE instance, std::string& error);
    void update(const app::PipelineDebugSnapshot& snapshot);
    void show();
    void hide();

    [[nodiscard]] bool visible() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dictscribe::win
