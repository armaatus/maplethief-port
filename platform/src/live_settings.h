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
//   audio_mono                 mt_audio_set_mono (#195; the next render)
//   audio_device               AudioDevice::set_device (#195: the stream reopens on it; without
//                              a device stream, --no-audio or a capture, the next start)
//   right_touch                Input
//   hide_keypad                KeypadHider::set_mode (it fades)
//   rumble, rumble_strength    Rumble::configure
//   perf_overlay               PerfOverlay (never in a run that writes images)
//   fullscreen, vsync          Desktop::apply, then follow_vsync (#145, ADR 0077): Game::vsync
//                              and the frame telemetry follow, so the paced clock re-plans
//                              (#80, ADR 0075)
//   pause_unfocused            Lifecycle::set_pause_unfocused (a change while unfocused pauses
//                              or resumes at once)
//   render_scale               the frontend's render-scale change (#152: mt_gles_set_render_scale
//                              and the textures that follow it, between two run-loop turns; the
//                              settings menu holds the turn, so it shows when the menu closes)
//   border, border_image       Presenter::set_border (#157, ADR 0085; the next present)
//   colorblind_filter          Presenter::set_colorblind (#158, ADR 0093; the next upload or
//                              present)
//   brightness, contrast, saturation, gamma   Presenter::set_grade (#187, ADR 0093's addendum;
//                              the next upload or present, as the filter)
//   screen_shake               mt_hooks_install_screen_shake (#158; the next CStateGame::Paint)
//   flashes                    mt_hooks_install_flashes (#186; the next flash fill)
//   auto_loot                  install_auto_loot (#175; the next drop update)
//   keypad_layout              install_keypad_layout (#159, ADR 0086; the next touch, GL frame
//                              and composite)
//   menu_repeat, confirm_guard, focus_highlight, button_prompts, button_labels, game_speed,
//   fast_forward, autosave, widescreen_hud   read from the live Settings by game.cpp's turn hook
//                              every turn (#147, #146, ADR 0101, #151, #150): game_speed then
//                              writes the game's speed table and CMTimer's interval at the next
//                              turn
//   pause_on_disconnect        read from the live Settings by game.cpp's event handler (#184)
//   pad_pointer                read from the live Settings by game.cpp's turn hook (#185)
//
// This is the one path for every Applies::kAtOnce row: the settings menu's changes, and
// game.cpp's apply_settings_at_once (the start, the fullscreen hotkeys) for the desktop's rows.
// Everything else is read once at start-up (gfx_backend, hd_text, gapless_music: the game
// opens its music players at start-up; audio_pack, texture_pack, language, ...). A child that makes
// a row live adds its case here and sets the row's .applies.
#ifndef MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H
#define MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>

#include "settings.h"

namespace maplethief::frontend {

class Presenter;
class PresentPass;
class Desktop;
class FrameStats;
class Lifecycle;
class Input;
class KeypadHider;
class Rumble;
class FocusMute;
class AudioDevice;
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
    AudioDevice *audio = nullptr;   // audio_device: the device stream, if one is open
    PerfOverlay *perf = nullptr;
    Desktop *desktop = nullptr;     // fullscreen, vsync
    bool *vsync = nullptr;          // Game::vsync, which follow_vsync keeps current
    FrameStats *stats = nullptr;    // ... and the telemetry's
    Lifecycle *lifecycle = nullptr; // pause_unfocused
    // render_scale: the frontend's change to N times (0: auto), or empty (the next start).
    std::function<bool(std::uint32_t scale)> render_scale;
    // keypad_layout: installs a layout (install_keypad_layout), or empty (the next start).
    std::function<void(const KeypadLayout &layout)> keypad_layout;
    // The turn hook reads the rows above that say so from the live Settings (game.cpp).
    bool turn_reads_settings = false;
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

// The settings menu's Saves actions (#89 with #148, ADR 0078; save_tools.h), on `loc`
// (save_location() in the game), each returning the menu's status line:
//   export_saves_action   a zip of the save files in <data dir>/exports/, named by `stamp`
//                         (backup_stamp_now())
//   import_saves_action   what the player put in <data dir>/import/ (made if missing): the newest
//                         .zip there by name, else the folder's save files; checked, the files it
//                         replaces backed up first. Only where no character is loaded: the key
//                         pad's `panel` (mt_input_panel) is -1 on the title, the main menu and the
//                         slot screens; in the field the game holds the loaded slot in memory and
//                         would save over it (ADR 0078)
//   backups_action        how many backups each slot has, and the imports'
struct SaveLocation;
std::string export_saves_action(const SaveLocation &loc, const std::string &stamp);
std::string import_saves_action(const SaveLocation &loc, std::int32_t panel,
                                const std::string &stamp);
std::string backups_action(const SaveLocation &loc);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_LIVE_SETTINGS_H
