// Touch input (#65, ADR 0047): SDL's mouse and touch events, and the frontend's own virtual
// touches, as the game's UITouches. One Input object per window; game.cpp hands it every SDL
// event, and it calls mt_uikit_touch (the runtime queues the touch and delivers it at the next
// UIKit turn, ADR 0037).
//
//   the left mouse button   one touch (finger kMouseFinger): button down is Began, motion while
//                           it is held Moved, button up Ended
//   the right mouse button  with right_touch set (settings.ini, --right-touch): a second touch
//                           (kRightMouseFinger) at that fixed screen point while it is held, so
//                           one hand can hold the d-pad with the left button and attack
//   SDL finger events       multi-touch on a touch screen: one finger id (kFirstScreenFinger +
//                           a slot) per SDL finger while it is down. Only touch screens
//                           (SDL_TOUCH_DEVICE_DIRECT, or a device SDL does not know, as for an
//                           event pushed by a test) count: a trackpad's fingers are the
//                           pointer's, not touches.
//   SDL's synthetic events  a mouse event made from a touch (which == SDL_TOUCH_MOUSEID) or a
//                           pen (SDL_PEN_MOUSEID) and a touch made from the mouse (touchID ==
//                           SDL_MOUSE_TOUCHID) are dropped: each press is one touch, never two.
//                           A pen is its touch (SDL_PEN_TOUCHID, a direct touch device). On iOS
//                           SDL makes both kinds by default (SDL_HINT_TOUCH_MOUSE_EVENTS,
//                           SDL_HINT_MOUSE_TOUCH_EVENTS), and real touches are DIRECT (#73).
//   Input::touch()          the shared entry point for touches that are not SDL's: the gamepad's
//                           (#67) virtual fingers and the scripted taps before the first frame.
//
// Coordinates: window points through the #59 placement (window_point_to_screen, ADR 0043) to the
// game's 480x320 screen points. A touch that begins over the bars around the image is dropped;
// a touch that leaves the image while down is clamped to its edge (and ends there).
//
// Interruptions end touches, they never cancel them: the window losing the keyboard focus,
// being hidden or minimised (on iOS: the app resigning active, SDL 3.4's UIKit backend), and the
// display turning or the safe area changing (a phone turned over, #73), lifts every live touch
// (Ended at its last point); the mouse leaving
// the window lifts the mouse's touches; the system cancelling a finger (SDL_EVENT_FINGER_CANCELED)
// lifts that finger; a button that is no longer held in a motion event (its up was lost) lifts
// the mouse's touch there; a second down of a finger that is down lifts the first touch. UIKit
// would send touchesCancelled:withEvent:, but the game's -[TouchKeyPadView touchesCancelled:
// withEvent:] (0xB6BA5) is empty: a cancelled d-pad or attack touch would stay held
// (docs/controls/keypad.md, ADR 0047).
//
// Activity (for #71's key pad auto-hide): the listener hears every touch that begins, with its
// source (is_touch_source: mouse, finger, scripted), and every key or gamepad button press
// (non-touch input), so a caller can tell "touch input happened" from "non-touch input
// happened".
//
// Single thread: the frontend's (the guest's main thread, between run-loop turns).
#ifndef MAPLETHIEF_PLATFORM_INPUT_H
#define MAPLETHIEF_PLATFORM_INPUT_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "scaling.h"
#include "settings.h"

struct SDL_Window;
union SDL_Event;

namespace maplethief::frontend {

// A touch redirect (Input::set_redirects), in screen points.
struct InputRedirect {
    Rect from, to;
};

// Where input came from.
enum class InputSource {
    kMouse,    // the mouse's buttons
    kFinger,   // a touch screen
    kScripted, // --tap
    kGamepad,  // a gamepad (#67)
    kKeyboard, // the keyboard
    kPointer,  // the gamepad pointer and a pad's touchpad (pad_pointer.h, #185)
};
// "mouse", "finger", "scripted", "gamepad", "keyboard", "pointer".
const char *input_source_name(InputSource source);
// Whether input from `source` is a touch to the player: mouse, finger and scripted taps. Not
// the gamepad pointer: a gamepad is in use (the key pad's auto-hide), though its touches are
// aimed as a finger's (redirects apply to them).
bool is_touch_source(InputSource source);

// The finger ids Input hands to mt_uikit_touch.
inline constexpr std::uint32_t kMouseFinger = 1;             // the left mouse button (and --tap)
inline constexpr std::uint32_t kRightMouseFinger = 2;        // the right mouse button
inline constexpr std::uint32_t kFirstScreenFinger = 0x100;   // SDL fingers: + a slot
inline constexpr std::uint32_t kMaxScreenFingers = 20;       // more at once are ignored
inline constexpr std::uint32_t kFirstVirtualFinger = 0x1000; // Input::touch(): + its id

// mt_uikit_touch's shape: queue a touch of `finger` at screen point (x, y).
using TouchSink = int (*)(void *user, std::uint32_t finger, std::uint32_t phase, float x, float y);
// The activity listener: input from `source` happened.
using ActivityListener = void (*)(void *user, InputSource source);

class Input {
  public:
    Input(); // the sink is mt_uikit_touch

    // Where touches go (tests record them); nullptr: mt_uikit_touch.
    void set_sink(TouchSink sink, void *user);
    void set_activity_listener(ActivityListener listener, void *user);
    // The window and its placement (Presenter::placement(), kept by pointer: it changes as the
    // window does). Without both, every window point is over the bars.
    void set_window(SDL_Window *window, const Placement *placement);
    void set_right_touch(const RightTouch &right) { right_ = right; }
    const RightTouch &right_touch() const { return right_; }

    // An SDL event. Returns true if it was input that Input used (a touch or an interruption).
    bool handle(const SDL_Event &e);

    // A touch that is not SDL's: virtual finger `id` of `source` (finger kFirstVirtualFinger +
    // id) at screen point (x, y), phase MT_UIKIT_TOUCH_*. The same rules as SDL's touches: a
    // Began outside the screen (and its side margin) is dropped, later phases are clamped to
    // it, a Moved, Ended or
    // Cancelled of a finger that is not down is ignored, a Began of one that is cancels it
    // first (Ended). Returns true if the touch went to the sink.
    bool touch(InputSource source, std::uint32_t id, std::uint32_t phase, float x, float y);
    // Non-touch input happened (a key, a gamepad button): tells the activity listener.
    void note(InputSource source);
    // Lifts every live touch: Ended at its last point.
    void release_all();
    // Widescreen with the key pad (#168): touches may also begin up to `margin` points left and
    // right of the screen (the wings beside it, where the game pad's moved keys are drawn), and
    // later phases are clamped to that wider band. 0 (the default): the screen only.
    void set_side_margin(float margin) { margin_ = margin > 0 ? margin : 0.0f; }
    float side_margin() const { return margin_; }
    // Touch redirects (the minimap's corner moves the compass, #176, minimap.h): a touch that
    // begins in a redirect's `from` (screen points, the first that holds it) is moved by
    // (to.x - from.x, to.y - from.y) and clamped into `to` for its whole life. Only the player's
    // touches (is_touch_source: mouse, finger, scripted; and the gamepad pointer's, aimed at
    // what the player sees); the gamepad's other virtual fingers go where the port aims them.
    // Touches already down keep theirs. Empty (the default): none.
    void set_redirects(std::vector<InputRedirect> redirects) { redirects_ = std::move(redirects); }
    const std::vector<InputRedirect> &redirects() const { return redirects_; }

    // The live touches (down and not yet ended).
    std::size_t live() const { return live_.size(); }
    // Whether finger `finger` is down.
    bool is_down(std::uint32_t finger) const;

  private:
    struct Live {
        std::uint32_t finger;
        InputSource source;
        float x, y;                  // the last screen point sent
        bool redirected = false;     // began in a redirect's `from`: moved into `to`
        InputRedirect redirect = {}; // that redirect
    };
    struct Slot {
        bool used = false;
        std::uint64_t touch_id = 0, finger_id = 0;
    };

    bool emit(InputSource source, std::uint32_t finger, std::uint32_t phase, ScreenPoint p);
    ScreenPoint map(float wx, float wy) const;
    bool mouse_button(const SDL_Event &e);
    bool mouse_motion(const SDL_Event &e);
    bool finger(const SDL_Event &e);
    void release_where(bool (*pred)(const Live &));
    Live *find(std::uint32_t finger);

    TouchSink sink_ = nullptr;
    void *sink_user_ = nullptr;
    ActivityListener listener_ = nullptr;
    void *listener_user_ = nullptr;
    SDL_Window *window_ = nullptr;
    const Placement *placement_ = nullptr;
    RightTouch right_;
    float margin_ = 0; // set_side_margin
    std::vector<InputRedirect> redirects_;
    std::vector<Live> live_;
    Slot slots_[kMaxScreenFingers];
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_INPUT_H
