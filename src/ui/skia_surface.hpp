#pragma once

#include "GLFW/glfw3.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"

namespace dictscribe::ui {

class SkiaSurface {
public:
    bool initialize(GLFWwindow* window);
    bool ensure_size(GLFWwindow* window);
    void present(GLFWwindow* window);
    void shutdown();

    [[nodiscard]] SkSurface* surface() const { return surface_.get(); }

private:
    sk_sp<GrDirectContext> context_;
    sk_sp<SkSurface> surface_;
};

} // namespace dictscribe::ui
