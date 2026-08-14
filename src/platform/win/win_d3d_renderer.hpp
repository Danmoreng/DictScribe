#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <string>

class SkSurface;

namespace dictscribe::win {

class WinD3DRenderer {
public:
    WinD3DRenderer();
    ~WinD3DRenderer();

    WinD3DRenderer(const WinD3DRenderer&) = delete;
    WinD3DRenderer& operator=(const WinD3DRenderer&) = delete;

    bool initialize(HWND window, int width, int height, std::string& error);
    void shutdown();

    [[nodiscard]] bool valid() const;
    [[nodiscard]] SkSurface* begin_frame(int width, int height);
    bool present();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dictscribe::win
