#pragma once

namespace dictscribe::app {
class AppController;
struct AppSettings;
}

namespace dictscribe::ui {

int RunDictationWindow(
    app::AppController& controller,
    app::AppSettings& settings);

} // namespace dictscribe::ui
