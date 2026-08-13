#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace dictscribe::linux_x11 {

enum class DesktopCommand {
    Toggle,
    Accept,
    Cancel,
    Quit,
};

struct TargetContext {
    Window active_window = None;
    Window focus_window = None;
    int anchor_x = 0;
    int anchor_y = 0;

    [[nodiscard]] bool valid() const {
        return active_window != None || focus_window != None;
    }
};

class X11Desktop {
public:
    X11Desktop() = default;
    ~X11Desktop();

    X11Desktop(const X11Desktop&) = delete;
    X11Desktop& operator=(const X11Desktop&) = delete;

    bool initialize(Window overlay_window, std::string& error);
    void set_settings_window(Window settings_window) { settings_window_ = settings_window; }
    void shutdown();

    [[nodiscard]] std::vector<DesktopCommand> poll_commands();
    [[nodiscard]] TargetContext capture_target() const;
    [[nodiscard]] bool pointer_position(int& root_x, int& root_y) const;
    void set_session_hotkeys(bool enabled);
    bool insert_text(const TargetContext& target, const std::string& text, std::string& error);

private:
    void grab_key(KeyCode key, unsigned int modifiers, bool grab);
    void handle_selection_request(const XSelectionRequestEvent& request);
    [[nodiscard]] Window active_window() const;
    [[nodiscard]] unsigned int normalized_modifiers(unsigned int state) const;

    Display* display_ = nullptr;
    Window root_window_ = None;
    Window control_window_ = None;
    Window overlay_window_ = None;
    Window settings_window_ = None;
    Atom clipboard_atom_ = None;
    Atom targets_atom_ = None;
    Atom utf8_atom_ = None;
    Atom text_atom_ = None;
    Atom active_window_atom_ = None;
    Atom plain_text_atom_ = None;
    KeyCode toggle_key_ = 0;
    KeyCode quit_key_ = 0;
    KeyCode accept_key_ = 0;
    KeyCode keypad_accept_key_ = 0;
    KeyCode cancel_key_ = 0;
    unsigned int num_lock_mask_ = 0;
    bool session_hotkeys_ = false;
    bool xtest_available_ = false;
    std::string clipboard_text_;
};

} // namespace dictscribe::linux_x11
