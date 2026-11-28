// The keyboard (#68, ADR 0048 addendum): SDL3 key events as controls of the action layer
// (actions.h), ControlKind::kKey with the event's SDL_Keycode. The rows are
// default_keyboard_bindings() (docs/controls/keyboard.md), so a key resolves its action by the
// context of its press (field or menu) and its release releases that action, and the runtime
// delivers it one step per game tick (ADR 0048), as a gamepad button.
//
//   key down     a press, once: SDL's key repeats (event.key.repeat) are ignored, so a held
//                key is one press until it is released, as a held touch or pad button is. A
//                key pressed while Cmd (macOS) or the Windows key is held is the system's
//                (Cmd+Q, Cmd+M, Cmd+H) and is not pressed.
//   key up       the release of that key, if it was pressed.
//   modifiers    Ctrl, Alt and Shift are the desktop's chords too (Ctrl+1 ... Ctrl+5, Alt+Enter;
//                desktop.h), so their press is held back (#381): let go first, the key is
//                pressed and released at once (a tap still attacks); another key going down
//                presses it first; held kChordSeconds (turn()), it is pressed. A desktop hotkey
//                taken while it is held back (chord_taken) swallows it and its release, so Ctrl+1
//                sizes the window without an attack. Something that lifts the keyboard
//                (Actions::release_source: an overlay opening) drops it too.
//   focus lost, window hidden or minimised
//                every held key is released; a key still held when focus comes back counts
//                again only when it is let go and pressed (its repeats are ignored).
//
// Activity (#71): Input::handle reports every key press that is not a repeat to the activity
// listener (ADR 0047), bound or not; Keyboard does not report it a second time.
//
// Text: the game has no text entry (docs/ANALYSIS.md), so the frontend never starts SDL's text
// input; letters are keys, never characters.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_KEYBOARD_H
#define MAPLETHIEF_PLATFORM_KEYBOARD_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "actions.h"

namespace maplethief::frontend {

class Keyboard {
  public:
    explicit Keyboard(Actions *actions);

    // An SDL event: key down and up, focus lost, hidden, minimised. Returns true if it pressed
    // or released an action.
    bool handle(const SDL_Event &e);
    // Releases every held key.
    void release_all();
    // The desktop took a hotkey with `mod` held (desktop.h): the modifiers held back for it
    // never press, nor release.
    void chord_taken(SDL_Keymod mod);
    // Once per run-loop turn at `now` (seconds, the guest's clock): a modifier held back
    // kChordSeconds is pressed.
    void turn(double now);
    static constexpr double kChordSeconds = 0.25;
    // Modifiers held back now (tests).
    std::size_t held_back() const { return held_back_.size(); }

  private:
    struct HeldBack {
        SDL_Keycode key;
        double since;        // the turn's time when it went down
        std::uint64_t lifts; // Actions::lifts(kKeyboard) then
    };
    bool press(SDL_Keycode key);
    // Presses every held-back modifier.
    bool flush_held_back();
    // Drops the held-back modifiers a lift (an overlay opening) let go of.
    void drop_lifted();

    Actions *actions_;
    std::vector<HeldBack> held_back_;
    std::vector<SDL_Keycode> swallowed_; // their releases are not the game's
    double now_ = 0;                     // the last turn's time
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYBOARD_H
