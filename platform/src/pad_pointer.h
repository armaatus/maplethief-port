// The gamepad pointer (#185, ADR 0107): a fallback for whatever still needs a finger (the help
// pages' swipe, the main menu's UIKit buttons other than Play, a screen the focus misses).
//
//   the right stick   (pad_pointer = stick | both) past a radial dead zone shows a pointer and
//                     moves it, faster the further it is pushed and the longer it stays
//                     pushed (pointer_velocity), clamped to the picture (PointerBounds: 480 x
//                     320 screen points, or widescreen's wider view). The port's action
//                     pointer_tap (actions.h: the touchpad's click, or R3 held 0.3 s while
//                     the pointer is shown, gamepad.h's holds) touches the screen there while
//                     held: a tap, or a drag when the stick moves meanwhile. The pointer hides
//                     after kIdleSeconds without a push or a tap, and on any use of the focus
//                     (a d-pad, OK or back press on the screens the focus drives).
//   a touchpad        (pad_pointer = touchpad | both; DualShock 4, DualSense, a Steam Deck's
//                     or Steam Controller's trackpads) places the same pointer: a finger on it
//                     shows the pointer at the same place of the picture (touchpad_point,
//                     absolute) and moves it while it stays down. The pad's click touches the
//                     screen there while held, as pointer_tap does: a tap, or a drag when the
//                     finger moves meanwhile. The click is SDL's touchpad button (the only pad
//                     of a DualShock or DualSense, the left one of a Steam Deck or Steam
//                     Controller) and, on a pad with two touchpads, misc2 (SDL's mapping for
//                     the right trackpad's click: is_right_pad_click).
//
// The touches go the one way every touch goes (ADR 0047): Input::touch, source kPointer (aimed
// as a finger is, so the minimap's compass redirect applies; a gamepad in use for the key pad's
// auto-hide). The focus (ADR 0051): a pointer tap drops the focus highlight
// (highlight_dropped()); the focus's next use brings it back and hides the pointer.
//
// Drawn by the frontend in the window only (draw_pad_pointer), after the read-back and the
// captures: never in a shot, a capture or a golden. The port's overlays release it.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_PAD_POINTER_H
#define MAPLETHIEF_PLATFORM_PAD_POINTER_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "actions.h"
#include "input.h"
#include "scaling.h"
#include "settings.h"

namespace maplethief::frontend {

struct PointerConfig {
    float deadzone = 0.20f;    // the stick moves the pointer past this fraction (radial)
    float exponent = 2.0f;     // the response curve: speed ~ (deflection past the zone)^exponent
    float speed = 240.0f;      // screen points a second at full deflection, at first
    float boost = 2.0f;        // and this many times that once pushed for ramp_seconds
    float ramp_seconds = 0.6f; // (linear in between)
};

// Where the pointer may go, in screen points: x in [x0, x1), y in [0, 320).
struct PointerBounds {
    float x0 = 0, x1 = static_cast<float>(kScreenWidth);
};
// The picture of a view `view_width` points wide (maplethief/widescreen.h's view: 480, or
// wider in widescreen's field), centred on the 480 screen points as widescreen draws it.
PointerBounds pointer_bounds(std::uint32_t view_width);

struct PointerVelocity {
    float x = 0, y = 0; // screen points a second (y down)
};
// The pointer's velocity for the stick at (x, y) (each -1..1, y down as SDL's), pushed past
// the dead zone for `pushed` seconds so far: none inside the dead zone; past it the stick's
// direction at speed * curve * ramp (curve: (r - deadzone) / (1 - deadzone), r capped at 1,
// to the exponent; ramp: 1 to boost over ramp_seconds).
PointerVelocity pointer_velocity(float x, float y, double pushed, const PointerConfig &config);
// (x, y) moved by v for `dt` seconds, clamped into the bounds (just below x1 and 320).
void pointer_step(float *x, float *y, PointerVelocity v, double dt, const PointerBounds &b);
// A touchpad point (SDL's 0..1 from its top-left) to the picture's point (absolute).
void touchpad_point(float nx, float ny, const PointerBounds &b, float *x, float *y);
// Whether `button` on a pad with `touchpads` touchpads is its right trackpad's click: SDL maps
// a Steam Deck's and a Steam Controller's (two touchpads) to misc2, and their left one's to
// the touchpad button.
bool is_right_pad_click(int button, int touchpads);

class PadPointer {
  public:
    static constexpr double kIdleSeconds = 3.0;
    // The virtual finger (an Input::touch id) of the pointer's touch.
    static constexpr std::uint32_t kPointerFinger = 0x50;

    PadPointer(Input *input, Actions *actions);

    void set_mode(PadPointerMode mode);
    PadPointerMode mode() const { return mode_; }
    void set_config(const PointerConfig &config) { config_ = config; }
    // The picture now (widescreen's view changes from screen to screen).
    void set_bounds(const PointerBounds &b) { bounds_ = b; }

    // An SDL event: the right stick's axes, a touchpad's fingers, a right trackpad's click
    // (is_right_pad_click), a pad removed. Returns true if it used it (a touchpad's finger, a
    // right trackpad's click).
    bool handle(const SDL_Event &e);
    // Once per run-loop turn at `now` (seconds, the guest's clock), after the focus's turn:
    // `focus_uses` is Focus::Stats::uses. Moves the pointer, makes or ends its touch, hides it.
    void turn(double now, std::uint64_t focus_uses);
    // Lifts every touch it makes and forgets the stick (a port overlay opened over the game).
    void release_all();

    // Whether the pointer is shown (and drawn), and where (screen points).
    bool shown() const { return shown_; }
    float x() const { return x_; }
    float y() const { return y_; }
    // Whether its touch is down.
    bool pressing() const { return pressing_; }
    // A pointer tap dropped the focus highlight (until the focus's next use).
    bool highlight_dropped() const { return dropped_; }
    // Gamepads' hold gate (gamepad.h): holds apply while the pointer is shown.
    static bool hold_gate(void *self);

    struct Stats {
        std::uint64_t taps = 0;        // the pointer's touches begun
        std::uint64_t pad_touches = 0; // touchpad fingers down (each places the pointer)
        std::uint64_t shows = 0, hides = 0;
    };
    const Stats &stats() const { return stats_; }

  private:
    struct PadFinger {
        SDL_JoystickID pad = 0;
        std::int32_t touchpad = 0, finger = 0;
    };
    bool stick_on() const {
        return mode_ == PadPointerMode::kStick || mode_ == PadPointerMode::kBoth;
    }
    bool touchpad_on() const {
        return mode_ == PadPointerMode::kTouchpad || mode_ == PadPointerMode::kBoth;
    }
    bool touchpad(const SDL_Event &e);
    bool right_pad_click(const SDL_Event &e);
    void show(const char *why);
    void hide(const char *why);
    void end_press();

    Input *input_;
    Actions *actions_;
    PadPointerMode mode_ = PadPointerMode::kOff;
    PointerConfig config_;
    PointerBounds bounds_;
    float sx_ = 0, sy_ = 0; // the right stick (-1..1)
    double pushed_ = 0;     // seconds pushed past the dead zone
    float x_ = kScreenWidth / 2.0f, y_ = kScreenHeight / 2.0f;
    float sent_x_ = 0, sent_y_ = 0; // where its touch was last sent
    bool shown_ = false;
    bool pressing_ = false;
    bool dropped_ = false;
    double last_ = -1;       // the last turn's time
    double active_ = 0;      // the last push or touch
    std::uint64_t uses_ = 0; // the focus's uses seen
    bool uses_known_ = false;
    std::vector<PadFinger> fingers_;
    std::vector<SDL_JoystickID> right_clicks_; // pads whose right trackpad is clicked
    Stats stats_;
};

// Draws the pointer at screen point (x, y) over the window through the placement (an arrow,
// its tip at the point, 12 screen points tall at the picture's scale; pressed: filled amber).
void draw_pad_pointer(SDL_Renderer *renderer, const Placement &placement, float x, float y,
                      bool pressed);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PAD_POINTER_H
