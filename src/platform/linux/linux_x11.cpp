#include "platform/linux/linux_x11.hpp"

#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <array>
#include <cstdlib>

namespace dictscribe::linux_x11 {

namespace {

constexpr unsigned int kToggleModifiers = ControlMask | Mod1Mask;

std::string DisplayError(const char* message) {
    const char* display = std::getenv("DISPLAY");
    return std::string(message) + (display ? std::string(" (DISPLAY=") + display + ")" : "");
}

} // namespace

X11Desktop::~X11Desktop() {
    shutdown();
}

bool X11Desktop::initialize(Window overlay_window, std::string& error) {
    if (display_) return true;
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        error = DisplayError("Could not open the X11 display");
        return false;
    }

    root_window_ = DefaultRootWindow(display_);
    overlay_window_ = overlay_window;
    control_window_ = XCreateSimpleWindow(display_, root_window_, -10, -10, 1, 1, 0, 0, 0);
    clipboard_atom_ = XInternAtom(display_, "CLIPBOARD", False);
    targets_atom_ = XInternAtom(display_, "TARGETS", False);
    utf8_atom_ = XInternAtom(display_, "UTF8_STRING", False);
    text_atom_ = XInternAtom(display_, "TEXT", False);
    active_window_atom_ = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    plain_text_atom_ = XInternAtom(display_, "text/plain;charset=utf-8", False);

    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    xtest_available_ = XTestQueryExtension(
        display_, &event_base, &error_base, &major, &minor) == True;

    toggle_key_ = XKeysymToKeycode(display_, XK_space);
    quit_key_ = XKeysymToKeycode(display_, XK_q);
    accept_key_ = XKeysymToKeycode(display_, XK_Return);
    keypad_accept_key_ = XKeysymToKeycode(display_, XK_KP_Enter);
    cancel_key_ = XKeysymToKeycode(display_, XK_Escape);

    const KeyCode num_lock_key = XKeysymToKeycode(display_, XK_Num_Lock);
    if (XModifierKeymap* modifiers = XGetModifierMapping(display_)) {
        for (int modifier = 0; modifier < 8; ++modifier) {
            for (int index = 0; index < modifiers->max_keypermod; ++index) {
                if (modifiers->modifiermap[modifier * modifiers->max_keypermod + index] ==
                    num_lock_key) {
                    num_lock_mask_ = 1U << modifier;
                }
            }
        }
        XFreeModifiermap(modifiers);
    }

    grab_key(toggle_key_, kToggleModifiers, true);
    grab_key(quit_key_, kToggleModifiers, true);
    XSync(display_, False);
    return true;
}

void X11Desktop::shutdown() {
    if (!display_) return;
    set_session_hotkeys(false);
    grab_key(toggle_key_, kToggleModifiers, false);
    grab_key(quit_key_, kToggleModifiers, false);
    if (control_window_ != None) XDestroyWindow(display_, control_window_);
    XCloseDisplay(display_);
    display_ = nullptr;
    root_window_ = None;
    control_window_ = None;
    overlay_window_ = None;
    clipboard_text_.clear();
}

void X11Desktop::grab_key(KeyCode key, unsigned int modifiers, bool grab) {
    if (!display_ || key == 0) return;
    const std::array<unsigned int, 4> lock_variants = {
        0U,
        LockMask,
        num_lock_mask_,
        LockMask | num_lock_mask_,
    };
    for (const unsigned int locks : lock_variants) {
        if (grab) {
            XGrabKey(
                display_, key, modifiers | locks, root_window_, False, GrabModeAsync, GrabModeAsync);
        } else {
            XUngrabKey(display_, key, modifiers | locks, root_window_);
        }
    }
}

void X11Desktop::set_session_hotkeys(bool enabled) {
    if (!display_ || session_hotkeys_ == enabled) return;
    grab_key(accept_key_, 0, enabled);
    if (keypad_accept_key_ != accept_key_) grab_key(keypad_accept_key_, 0, enabled);
    grab_key(cancel_key_, 0, enabled);
    session_hotkeys_ = enabled;
    XSync(display_, False);
}

bool X11Desktop::pointer_position(int& root_x, int& root_y) const {
    if (!display_) return false;
    Window root = None;
    Window child = None;
    int window_x = 0;
    int window_y = 0;
    unsigned int mask = 0;
    return XQueryPointer(
        display_,
        root_window_,
        &root,
        &child,
        &root_x,
        &root_y,
        &window_x,
        &window_y,
        &mask) == True;
}

unsigned int X11Desktop::normalized_modifiers(unsigned int state) const {
    return state & ~(LockMask | num_lock_mask_);
}

std::vector<DesktopCommand> X11Desktop::poll_commands() {
    std::vector<DesktopCommand> commands;
    if (!display_) return commands;
    while (XPending(display_) > 0) {
        XEvent event{};
        XNextEvent(display_, &event);
        if (event.type == SelectionRequest) {
            handle_selection_request(event.xselectionrequest);
            continue;
        }
        if (event.type == SelectionClear) {
            clipboard_text_.clear();
            continue;
        }
        if (event.type != KeyPress) continue;

        const KeyCode key = event.xkey.keycode;
        const unsigned int modifiers = normalized_modifiers(event.xkey.state);
        if (key == toggle_key_ && modifiers == kToggleModifiers) {
            commands.push_back(DesktopCommand::Toggle);
        } else if (key == quit_key_ && modifiers == kToggleModifiers) {
            commands.push_back(DesktopCommand::Quit);
        } else if (session_hotkeys_ && modifiers == 0 &&
                   (key == accept_key_ || key == keypad_accept_key_)) {
            commands.push_back(DesktopCommand::Accept);
        } else if (session_hotkeys_ && modifiers == 0 && key == cancel_key_) {
            commands.push_back(DesktopCommand::Cancel);
        }
    }
    return commands;
}

Window X11Desktop::active_window() const {
    if (!display_ || active_window_atom_ == None) return None;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* value = nullptr;
    const int status = XGetWindowProperty(
        display_,
        root_window_,
        active_window_atom_,
        0,
        1,
        False,
        XA_WINDOW,
        &actual_type,
        &actual_format,
        &count,
        &remaining,
        &value);
    Window result = None;
    if (status == Success && actual_type == XA_WINDOW && actual_format == 32 && count == 1 && value) {
        result = *reinterpret_cast<Window*>(value);
    }
    if (value) XFree(value);
    return result;
}

TargetContext X11Desktop::capture_target() const {
    TargetContext target;
    if (!display_) return target;

    target.active_window = active_window();
    int revert = RevertToNone;
    XGetInputFocus(display_, &target.focus_window, &revert);
    if (target.active_window == overlay_window_ || target.active_window == control_window_ ||
        target.active_window == root_window_) {
        target.active_window = None;
    }
    if (target.focus_window == overlay_window_ || target.focus_window == control_window_ ||
        target.focus_window == root_window_ || target.focus_window == PointerRoot) {
        target.focus_window = None;
    }

    Window child = None;
    int root_x = 0;
    int root_y = 0;
    int window_x = 0;
    int window_y = 0;
    unsigned int mask = 0;
    if (XQueryPointer(
            display_, root_window_, &child, &child, &root_x, &root_y, &window_x, &window_y, &mask)) {
        target.anchor_x = root_x;
        target.anchor_y = root_y;
    }
    return target;
}

bool X11Desktop::insert_text(
    const TargetContext& target,
    const std::string& text,
    std::string& error) {
    if (!display_ || control_window_ == None) {
        error = "The X11 desktop bridge is not available.";
        return false;
    }
    if (text.empty()) return true;

    clipboard_text_ = text;
    XSetSelectionOwner(display_, clipboard_atom_, control_window_, CurrentTime);
    XFlush(display_);
    if (XGetSelectionOwner(display_, clipboard_atom_) != control_window_) {
        error = "Could not claim the X11 clipboard.";
        return false;
    }
    if (!target.valid()) {
        error = "No external X11 target is available; the text remains on the clipboard.";
        return false;
    }
    if (!xtest_available_) {
        error = "The XTEST extension is unavailable; the text remains on the clipboard.";
        return false;
    }

    if (target.active_window != None) {
        XEvent activation{};
        activation.xclient.type = ClientMessage;
        activation.xclient.window = target.active_window;
        activation.xclient.message_type = active_window_atom_;
        activation.xclient.format = 32;
        activation.xclient.data.l[0] = 1;
        activation.xclient.data.l[1] = CurrentTime;
        XSendEvent(
            display_,
            root_window_,
            False,
            SubstructureRedirectMask | SubstructureNotifyMask,
            &activation);
    }
    if (target.focus_window != None) {
        XSetInputFocus(display_, target.focus_window, RevertToPointerRoot, CurrentTime);
    }
    XSync(display_, False);

    const KeyCode control = XKeysymToKeycode(display_, XK_Control_L);
    const KeyCode paste = XKeysymToKeycode(display_, XK_v);
    if (control == 0 || paste == 0) {
        error = "Could not map Ctrl+V; the text remains on the clipboard.";
        return false;
    }
    XTestFakeKeyEvent(display_, control, True, CurrentTime);
    XTestFakeKeyEvent(display_, paste, True, CurrentTime);
    XTestFakeKeyEvent(display_, paste, False, CurrentTime);
    XTestFakeKeyEvent(display_, control, False, CurrentTime);
    XFlush(display_);
    return true;
}

void X11Desktop::handle_selection_request(const XSelectionRequestEvent& request) {
    XEvent event{};
    XSelectionEvent& response = event.xselection;
    response.type = SelectionNotify;
    response.display = request.display;
    response.requestor = request.requestor;
    response.selection = request.selection;
    response.target = request.target;
    response.time = request.time;
    response.property = None;

    const Atom property = request.property == None ? request.target : request.property;
    if (request.target == targets_atom_) {
        const std::array<Atom, 5> targets = {
            targets_atom_, utf8_atom_, plain_text_atom_, text_atom_, XA_STRING};
        XChangeProperty(
            display_,
            request.requestor,
            property,
            XA_ATOM,
            32,
            PropModeReplace,
            reinterpret_cast<const unsigned char*>(targets.data()),
            static_cast<int>(targets.size()));
        response.property = property;
    } else if (request.target == utf8_atom_ || request.target == plain_text_atom_ ||
               request.target == text_atom_ || request.target == XA_STRING) {
        XChangeProperty(
            display_,
            request.requestor,
            property,
            request.target,
            8,
            PropModeReplace,
            reinterpret_cast<const unsigned char*>(clipboard_text_.data()),
            static_cast<int>(clipboard_text_.size()));
        response.property = property;
    }

    XSendEvent(display_, request.requestor, False, 0, &event);
    XFlush(display_);
}

} // namespace dictscribe::linux_x11
