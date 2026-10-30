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

  private:
    Actions *actions_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYBOARD_H
