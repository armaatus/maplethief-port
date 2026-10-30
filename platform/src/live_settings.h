// Settings applied while the game runs (#89, ADR 0076): what the settings menu's changes do to
// the frontend's objects, and the menu's "Game options" action.
//
// apply_live_setting is the frontend's half of Applies::kAtOnce (settings.h): a row the menu
// changed, applied at once to the object that reads it. A row it does not know, or cannot
// apply now, returns false and waits for the next start (the menu marks it):
//
//   scale_mode, border_color   Presenter::set_scaling (the next present)
//   filter                     the present stage's pass, on SDL's GPU renderer only (else the
//                              next start: main.cpp asks for that renderer when a filter is set)
//   master_volume, music_volume, effects_volume   the mixer's bus gains (ADR 0045)
//   mute_unfocused             FocusMute (never under --audio-capture)
//   right_touch                Input
//   hide_keypad                KeypadHider::set_mode (it fades)
//   rumble, rumble_strength    Rumble::configure
//   perf_overlay               PerfOverlay (never in a run that writes images)
//
// Everything else is read once at start-up (render_scale, gfx_backend, hd_text, gapless_music:
// the game opens its music players at start-up). A child that makes a row live adds its case
// here and sets the row's .applies.
#ifndef MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H
#define MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>

#include "settings.h"

namespace maplethief::frontend {

class Presenter;
class PresentPass;
class Input;
class KeypadHider;
class Rumble;
class FocusMute;
class PerfOverlay;

// The objects a live change reaches (any may be null: that row then waits for the next start).
struct LiveTargets {
    Presenter *presenter = nullptr;
    PresentPass *filter_pass = nullptr;
    SDL_Renderer *renderer = nullptr; // the window's (a filter needs SDL's GPU renderer)
    Input *input = nullptr;
    KeypadHider *keypad = nullptr;
    Rumble *rumble = nullptr;
    FocusMute *focus_mute = nullptr;
    bool focus_mute_allowed = true; // false under --audio-capture
    PerfOverlay *perf = nullptr;
    double now = 0; // the run loop's time (the key pad's fade starts then)
};

// Applies `row`'s value in `settings` to its object. True if it now applies.
bool apply_live_setting(const LiveTargets &targets, const SettingInfo &row,
                        const Settings &settings);

// The settings menu's "Game options" (#89): the game's own options (sound, speed, vibration)
// live in the option tab of its popup menu (CDataMgr +0xB0 tab 5, docs/controls/keypad.md),
// which the field's Menu action (Start, Esc) opens. So from the field (the key pad's panel 0),
// one run-loop turn at a time: the Menu action pressed, released, then once the popup menu is up
// (panel 1) and has settled, a touch on its option tab (focus.h's popup_menu_tab_point), as a
// finger opens it. Anywhere else (the title, the main menu, a cut scene, a shop, a popup) it does
// nothing: those screens have no way to the options, and pressing Menu there means something
// else (back, in menus).
class GameOptionsOpener {
  public:
    using Press = std::function<void(std::uint32_t action, bool down)>; // mt_input_press/release
    using Panel = std::function<std::int32_t()>;                        // mt_input_panel
    using Touch = std::function<void(std::uint32_t phase, float x, float y)>; // a finger
    static constexpr double kWaitSeconds = 3.0;   // for the popup menu to come up
    static constexpr double kSettleSeconds = 0.3; // after it did, before the touch

    enum class State { kIdle, kPress, kRelease, kWaitMenu, kSettle, kLift };
    enum class Outcome { kNone, kOpened, kNotInField, kNoMenu };

    GameOptionsOpener(Press press, Panel panel, Touch touch);

    // Whether it can start now: the field.
    bool available() const;
    // Starts at the next turn; false (Outcome::kNotInField) outside the field.
    bool start();
    // Once per run-loop turn, at run-loop time `now`.
    void turn(double now);
    bool busy() const { return state_ != State::kIdle; }
    State state() const { return state_; }
    Outcome outcome() const { return outcome_; }

  private:
    Press press_;
    Panel panel_;
    Touch touch_;
    State state_ = State::kIdle;
    Outcome outcome_ = Outcome::kNone;
    double until_ = 0;
    float x_ = 0, y_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H
