// The desktop conveniences (#145, ADR 0077): fullscreen, the window's size and position, the
// mouse cursor, the mouse wheel in lists, screenshots, vsync. pause_unfocused is the
// lifecycle's (lifecycle.h) and language the boot's (game.cpp); both are settings rows too.
//
//   fullscreen   `fullscreen = off | desktop` (settings.h). Desktop is SDL's desktop
//                fullscreen: the window, borderless, over the whole display at the display's
//                own mode (no mode switch). F11, Alt+Enter and Cmd+Ctrl+F (macOS: F11 is the
//                system's Show Desktop there) toggle it; the toggle is the setting's change,
//                written to settings.ini. macOS's green button or the system's own shortcut
//                change the window too; the frontend follows them into the setting.
//                The mouse keeps mapping to the screen through the placement of each frame
//                (scaling.h, ADR 0043), which follows the window's new size.
//   window       <data dir>/window.ini (WindowState): the windowed size and position (never the
//                fullscreen one) and whether it was maximised, written when the game ends and
//                read before the window opens. Only in an interactive run (is_interactive_run)
//                without --window, and the position only if the window would land on a
//                connected display.
//   cursor       hidden on a key press or a gamepad press (or a stick or trigger pushed past
//                half), shown again on a real mouse motion, click or wheel (CursorAutoHide).
//   wheel        in a menu (any key pad panel but the field's, actions.h's Context::kMenu), one
//                notch up or down is the gamepad's Up or Down: lists, shops and the UIKit
//                screens' focus move one row (ADR 0051). Nothing on the field.
//   screenshots  F12, the gamepad's Share / Capture button (SDL's MISC1), or the right stick
//                held in (R3) and Back pressed: the composed screen as --shot writes it (the
//                game's image at the render scale, without the port's overlays) to
//                <data dir>/screenshots/MapleThief_YYYY-MM-DD_HH-MM-SS-mmm.png, local time.
//                Never into --capture's directory: scripted outputs never see it.
//   vsync        `vsync = on | off`: SDL_SetRenderVSync. Off, SDL_RenderPresent does not wait
//                and the run loop paces by the game's own timer (wall clock).
//
// The hotkeys and the screenshot buttons are taken before the key bindings and the gamepad
// (their releases too), so the game never sees them. Every setting here applies at once
// (Desktop::apply, which #89's menu calls after a change).
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_DESKTOP_H
#define MAPLETHIEF_PLATFORM_DESKTOP_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "settings.h"

namespace maplethief::frontend {

struct GameOptions;

// Whether the run is a player's: none of the scripted options (--frames, --seconds, --capture,
// --capture-at, --shot, --tap / --swipe, --fixed-step, --audio-capture, --minimise,
// --background, --quit) and a video driver other than SDL's "dummy" and "offscreen"
// (`video_driver` null: not known, counts as a player's). The first-launch picker and
// window.ini are only for such runs.
bool is_interactive_run(const GameOptions &options, const char *video_driver);

// --- Keys and buttons -------------------------------------------------------------------------

// F11, Alt+Enter (Return or the keypad's Enter) or Cmd+Ctrl+F (GUI+Ctrl+F); Shift, Num Lock and
// Caps Lock are ignored.
bool is_fullscreen_hotkey(SDL_Keycode key, SDL_Keymod mod);
// F12 with no modifier but Shift, Num Lock or Caps Lock.
bool is_screenshot_key(SDL_Keycode key, SDL_Keymod mod);
// A gamepad button pressed: MISC1 (Share, Capture) alone, or Back while the right stick is held
// in (`right_stick_held`).
bool is_screenshot_button(int button, bool right_stick_held);

// --- The cursor -------------------------------------------------------------------------------

// The cursor's auto-hide: what an event asks of it. Starts shown.
class CursorAutoHide {
  public:
    enum class Change { kNone, kHide, kShow };
    // A key press (not a repeat), a gamepad button press, an axis past half: hide. A mouse
    // motion, button press or wheel that is the mouse's own (not a touch's or a pen's): show.
    // kNone when the cursor is already that way or the event says nothing.
    Change on_event(const SDL_Event &e);
    bool hidden() const { return hidden_; }

  private:
    bool hidden_ = false;
};

// --- The mouse wheel --------------------------------------------------------------------------

// Whole notches from a wheel's (possibly fractional, a trackpad's) movements: +1 up, -1 down.
class WheelSteps {
  public:
    // `y`: SDL_MouseWheelEvent.y (positive: away from the player). The whole notches it
    // completes, signed; the remainder is kept for the next movement (and dropped when the
    // direction changes).
    int add(float y);

  private:
    float rest_ = 0;
};

// --- window.ini -------------------------------------------------------------------------------

inline constexpr const char *kWindowStateFileName = "window.ini";

// The windowed window: its position (screen coordinates, points), size (points) and whether it
// was maximised.
struct WindowState {
    int x = 0, y = 0;
    int width = 0, height = 0;
    bool maximized = false;

    bool operator==(const WindowState &) const = default;
};

// window.ini's text: `key = value` lines (x, y, width, height, maximized), comments with '#'.
// Returns the state if x, y, width and height are all there and the size is within
// [min_side, max_side]; every other line, a bad value or a missing key is a problem (each one
// message), never fatal.
std::optional<WindowState> parse_window_state(std::string_view text, int min_side, int max_side,
                                              std::vector<std::string> *problems);
std::string serialise_window_state(const WindowState &state);
// Whether a window at `state` would show on one of `displays` (their usable bounds): its title
// bar's middle (the top 32 points, inset 32 from each side) lies on one. A position saved on a
// display that is gone is not restored.
bool window_state_on_display(const WindowState &state, std::span<const SDL_Rect> displays);

// <dir>/window.ini, if there is one and it is good (problems logged); std::nullopt otherwise.
std::optional<WindowState> load_window_state(const std::string &dir, int min_side, int max_side);
// Writes it (through a temporary file); false (logged) if it cannot.
bool save_window_state(const std::string &dir, const WindowState &state);

// --- Screenshots ------------------------------------------------------------------------------

inline constexpr const char *kScreenshotDirName = "screenshots";

// "MapleThief_2026-10-30_14-03-22-123.png" for that local time.
std::string screenshot_file_name(const SDL_DateTime &t);
// Writes `rgba` (width x height, top row first, RGBA8) to <data dir>/screenshots/ (made if
// needed) under screenshot_file_name(now, local time), with "_2", "_3" ... before ".png" if
// the name is taken. Returns the file's path, "" (logged) if it could not be written.
std::string write_screenshot(const std::string &data_dir, const std::uint8_t *rgba,
                             std::uint32_t width, std::uint32_t height);

// --- The window -------------------------------------------------------------------------------

// The desktop's window: applies fullscreen and vsync, takes the hotkeys and screenshot
// buttons, hides and shows the cursor, and keeps the windowed size and position for
// window.ini. attach() before use; without it every call does nothing.
class Desktop {
  public:
    // What an event asked for.
    struct Request {
        bool consumed = false;          // the event is the port's: the game never sees it
        bool toggle_fullscreen = false; // a fullscreen hotkey
        bool screenshot = false;        // a screenshot key or button
        // The window entered or left fullscreen (by the hotkey's request or by the system:
        // macOS's green button): the current state.
        std::optional<bool> fullscreen_changed;
    };

    // `windowed`: the windowed size and position to keep while the window starts fullscreen
    // (window.ini's, or none); a window that starts windowed is measured instead.
    void attach(SDL_Window *window, SDL_Renderer *renderer,
                std::optional<WindowState> windowed = std::nullopt);
    SDL_Window *window() const { return window_; }

    // Applies `settings`' fullscreen and vsync now (#89: after a change; the game at start).
    // Logs what changed. On iOS the window stays fullscreen whatever the setting.
    void apply(const Settings &settings);
    // Whether SDL_RenderPresent waits for the display (vsync on and the renderer took it).
    bool vsync() const { return vsync_; }
    // Whether the window is fullscreen now (SDL's flags).
    bool fullscreen() const;

    // An SDL event, before the game's input handlers.
    Request handle(const SDL_Event &e);

    // The windowed size and position seen last (for window.ini), and whether the window is
    // maximised now. std::nullopt before attach().
    std::optional<WindowState> state() const;

    std::uint64_t toggles() const { return toggles_; }
    std::uint64_t cursor_hides() const { return cursor_hides_; }

  private:
    void track();
    void set_cursor(CursorAutoHide::Change change);

    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    bool vsync_ = false;
    std::optional<bool> want_vsync_; // what apply() last asked (none: not yet)
    bool want_fullscreen_ = false;
    std::optional<WindowState> windowed_;
    CursorAutoHide cursor_;
    // The keys and buttons whose press was the port's: their releases (and repeats) are too.
    std::vector<SDL_Keycode> held_keys_;
    std::vector<std::pair<SDL_JoystickID, int>> held_buttons_;
    std::uint64_t toggles_ = 0;
    std::uint64_t cursor_hides_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DESKTOP_H
