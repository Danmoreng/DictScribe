#pragma once

namespace dictscribe::app {

enum class CleanupMode {
    Off,
    Ai,
};

[[nodiscard]] inline const char* CleanupModeName(CleanupMode mode) {
    return mode == CleanupMode::Ai ? "ai" : "off";
}

} // namespace dictscribe::app
