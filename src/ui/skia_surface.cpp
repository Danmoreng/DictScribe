#include "ui/skia_surface.hpp"

#include <iostream>

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurfaceProps.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

namespace dictscribe::ui {

bool SkiaSurface::initialize(GLFWwindow* window) {
    glfwMakeContextCurrent(window);
    auto interface = GrGLMakeNativeInterface();
    if (!interface) {
        std::cerr << "Could not create the Skia OpenGL interface.\n";
        return false;
    }
    context_ = GrDirectContexts::MakeGL(interface);
    if (!context_) {
        std::cerr << "Could not create the Skia GPU context.\n";
        return false;
    }
    return ensure_size(window);
}

bool SkiaSurface::ensure_size(GLFWwindow* window) {
    if (!context_) {
        return false;
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        surface_.reset();
        return false;
    }
    if (surface_ && surface_->width() == width && surface_->height() == height) {
        return true;
    }

    GrGLFramebufferInfo framebuffer{};
    framebuffer.fFBOID = 0;
    framebuffer.fFormat = 0x8058; // GL_RGBA8
    const GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(
        width,
        height,
        0,
        8,
        framebuffer);
    SkSurfaceProps properties;
    surface_ = SkSurfaces::WrapBackendRenderTarget(
        context_.get(),
        target,
        kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        nullptr,
        &properties);
    return surface_ != nullptr;
}

void SkiaSurface::present(GLFWwindow* window) {
    if (context_) {
        context_->flushAndSubmit();
    }
    glfwSwapBuffers(window);
}

void SkiaSurface::shutdown() {
    surface_.reset();
    if (context_) {
        context_->abandonContext();
        context_.reset();
    }
}

} // namespace dictscribe::ui
