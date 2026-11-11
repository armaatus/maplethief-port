// Hiding the on-screen key pad (#71, ADR 0052): the game pad (TouchKeyPadView's panel 0: the
// d-pad, jump, attack, the quick slots, Map / Bag / Cash) fades out while the player uses a
// gamepad or the keyboard, and back in on a touch.
//
//   hide_keypad = auto     non-touch input (a gamepad, a key: Input's activity listener with
//                          is_touch_source false) hides it; a touch (the mouse, a finger, a
//                          scripted tap) shows it again
//                 always   hidden from the start, whatever the input
//                 never    always shown (the game as it was)
//
// On the Nintendo Switch (#239, ADR 0140) a controller is always attached (the Joy-Cons in
// handheld, a pad when docked), so `auto` starts hidden there (kKeypadAutoStartsHidden): the
// first touch shows the pad, as any touch does, and is delivered too.
//
// Hiding is the compositor's alone: the panel's host opacity (mt_uikit_set_host_opacity), never
// its alpha or hidden flag. The game shows and hides its panels by alpha and reads them back,
// and UIKit's hit testing skips views below alpha 0.01, so the guest sees the same view tree
// either way. A hidden pad still takes touches: a touch on it shows it again (auto) and is
// delivered too, so the first touch already moves or attacks (ADR 0052).
//
// A hidden pad never holds a finger's key: a hide waits until no touch is down (Input::live()),
// so a d-pad held with the mouse while a gamepad button is pressed keeps walking, visibly, and
// the pad fades out once the mouse lets go. The gamepad's own keys (#67: the BOOLs through their
// setters) are not touches and go on while the pad is hidden.
//
// The fade is linear over kKeypadFadeSeconds (a partial fade takes its share), on the run
// loop's clock: virtual under --fixed-step, so the scripted runs are deterministic.
//
// No SDL; single thread (the frontend's).
#ifndef MAPLETHIEF_PLATFORM_KEYPAD_HIDE_H
#define MAPLETHIEF_PLATFORM_KEYPAD_HIDE_H

#include <cstddef>
#include <cstdint>

#include "input.h"
#include "settings.h"

namespace maplethief::frontend {

// KeypadMode, keypad_mode_name and parse_keypad_mode are in settings.h (hide_keypad).

inline constexpr double kKeypadFadeSeconds = 0.2;
// Whether `auto` starts hidden: on the Switch, where a controller is always attached.
inline constexpr bool kKeypadAutoStartsHidden = kSettingsPlatform == SettingsPlatform::kSwitch;

class KeypadHider {
  public:
    // `always` starts hidden (opacity 0, no fade), `auto` too with `auto_hidden`, the others
    // shown.
    explicit KeypadHider(KeypadMode mode = KeypadMode::kAuto,
                         bool auto_hidden = kKeypadAutoStartsHidden);

    KeypadMode mode() const { return mode_; }
    // A new mode at run time: fades to what it asks for (auto: shown until non-touch input).
    void set_mode(KeypadMode mode, double now);

    // Input's activity listener: Input::set_activity_listener(KeypadHider::listener, &hider).
    static void listener(void *user, InputSource source);
    // Input from `source` happened: in auto, a touch asks to show, anything else to hide.
    void on_activity(InputSource source);

    // Once per run-loop turn, before compositing: starts the fade that activity asked for at
    // `now`. A hide waits while `touches_down` (Input::live()) is not 0.
    void turn(double now, std::size_t touches_down);

    // Whether the pad is hidden or fading out (the target, not the opacity).
    bool hidden() const { return target_ == 0; }
    // The pad's opacity at `now`: 1 shown, 0 hidden, in between while fading.
    float opacity(double now) const;
    // Whether a fade is still running at `now` (the frontend presents while it does).
    bool fading(double now) const;

    // How many times it started to hide and to show.
    std::uint64_t hides() const { return hides_; }
    std::uint64_t shows() const { return shows_; }

  private:
    void fade_to(float target, double now);

    KeypadMode mode_;
    bool want_hidden_;
    float from_, target_;
    double start_ = 0, duration_ = 0;
    std::uint64_t hides_ = 0, shows_ = 0;
};

// Sets the game pad's host opacity (mt_input_pad_panel_view(0) through
// mt_uikit_set_host_opacity). Returns false while the game has no pad yet.
bool apply_keypad_opacity(float opacity);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYPAD_HIDE_H
