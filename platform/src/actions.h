// Device controls to game actions (#67, ADR 0048): the fixed layer between the platform's
// devices and the runtime's actions (maplethief/input.h).
//
//   a device       Gamepads (gamepad.h), the keyboard (keyboard.h, #68)
//     -> Actions::control(source, control, down)
//          the binding table: (control, context) -> action, context by the key pad's panel
//          (field: panel 0; menus: every other panel and none)
//     -> the sink: mt_input_press / mt_input_release (the runtime delivers to the guest),
//        or, for the few actions without a key code in a panel, a synthetic touch through
//        Input::touch (touch_fallbacks()).
//   The port's own actions (MT_ACTION_FIRST_PORT on: fast_forward, #151) never go to the sink:
//   they are held here, and the frontend asks holding() each turn.
//
// The table is data (gamepad and keyboard rows); #70 loads a bindings file and edits rows. A
// control's press resolves its action once, by the context at that moment, and its release
// releases that same action, whatever the context is by then (a menu that opens while a
// direction is held does not leave the walk held).
//
// Single thread: the frontend's (the guest's main thread, between run-loop turns).
#ifndef MAPLETHIEF_PLATFORM_ACTIONS_H
#define MAPLETHIEF_PLATFORM_ACTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "input.h"

namespace maplethief::frontend {

// Whether `action` is one of the port's own (maplethief/input.h's MT_ACTION_FIRST_PORT on),
// which the frontend handles and the guest never gets.
bool port_action(std::uint32_t action);

// Which bindings apply: the field (the key pad's panel 0) or menus, popups and screens.
enum class Context { kField, kMenu };
Context context_of_panel(std::int32_t panel);
const char *context_name(Context context);

// The four directions of a gamepad: the d-pad and the stick's 8-way emulation merged
// (gamepad.h). The arrow keys are key rows of their own (default_keyboard_bindings()).
enum class Direction : int { kUp = 0, kDown = 1, kLeft = 2, kRight = 3 };
const char *direction_name(Direction d);

// A device control that a binding names.
enum class ControlKind : std::uint8_t {
    kDirection,      // code: Direction
    kGamepadButton,  // code: SDL_GamepadButton (not the d-pad: that is kDirection)
    kGamepadTrigger, // code: SDL_GamepadAxis (the triggers, pressed past a threshold)
    kKey,            // code: SDL_Keycode, the key's label in the layout (#68, keyboard.h)
};
struct Control {
    ControlKind kind = ControlKind::kDirection;
    int code = 0;
    bool operator==(const Control &o) const { return kind == o.kind && code == o.code; }
};
// "direction up", "button south", "trigger left_trigger", "key Z" (SDL_GetKeyName).
std::string control_name(const Control &c);

// One row: what a control does in each context (MT_ACTION_NONE: nothing).
struct Binding {
    Control control;
    std::uint32_t field = 0;
    std::uint32_t menu = 0;
};

// docs/controls/keypad.md's proposed gamepad mapping (#66), by SDL3's positional names.
std::vector<Binding> default_gamepad_bindings();
// docs/controls/keyboard.md's default keys (#68): arrows, Z / X, A S D F and 1-4, Enter, Esc,
// M, C; in menus Enter / Z / Space confirm and Esc / X / Backspace go back, the gamepad's OK and
// back. By SDL_Keycode, so a key is the one its label names in the player's layout.
std::vector<Binding> default_keyboard_bindings();
// Every default row: the gamepad's, then the keyboard's (what Actions starts with).
std::vector<Binding> default_bindings();

// A touch for an action that has no key code in a panel (ADR 0048 lists the cases): a drag
// from (x0, y0) to (x1, y1) in screen points, or a tap where they are equal.
struct TouchFallback {
    std::int32_t panel;
    std::uint32_t action;
    float x0, y0, x1, y1;
    const char *what;
};
const std::vector<TouchFallback> &touch_fallbacks();

// Where actions go: a press (down) or release. The default is the runtime's
// mt_input_press / mt_input_release.
using ActionSink = void (*)(void *user, std::uint32_t action, bool down);
// The key pad's panel now; the default is mt_input_panel.
using PanelSource = std::int32_t (*)(void *user);

class Actions {
  public:
    explicit Actions(Input *input);

    void set_sink(ActionSink sink, void *user);
    void set_panel_source(PanelSource source, void *user);

    std::vector<Binding> &bindings() { return bindings_; }
    const std::vector<Binding> &bindings() const { return bindings_; }
    // The action `c` is bound to in `context` (the first row that binds it), or NONE.
    std::uint32_t action_for(const Control &c, Context context) const;
    Context context() const;

    // A control of `source` went down or up. Returns the action it pressed or released (NONE:
    // unbound, or a touch fallback).
    std::uint32_t control(InputSource source, const Control &c, bool down);
    // Lifts every control `source` holds (a gamepad unplugged, the window losing focus).
    void release_source(InputSource source);
    // Once per run-loop turn: moves the synthetic touches on.
    void turn();

    // The controls held now.
    std::size_t held() const { return held_.size(); }
    // Whether a control held now went down in the field and does `menu_action` in menus (#147:
    // a confirm or back button still held from the field when a popup opens, MenuInput).
    bool held_from_field(std::uint32_t menu_action) const;
    // Whether a held control pressed `action` (the port's actions: fast_forward).
    bool holding(std::uint32_t action) const;
    // Synthetic touches still running.
    std::size_t gestures() const { return gestures_.size(); }

  private:
    struct Held {
        InputSource source;
        Control control;
        std::uint32_t action;
        Context context; // where it went down
    };
    struct Gesture {
        InputSource source;
        std::uint32_t id;
        const TouchFallback *fallback;
        int step; // 0 began, 1..kMoves moved, then ended
    };

    void start_gesture(InputSource source, const TouchFallback &f);

    Input *input_;
    ActionSink sink_ = nullptr;
    void *sink_user_ = nullptr;
    PanelSource panel_ = nullptr;
    void *panel_user_ = nullptr;
    std::vector<Binding> bindings_;
    std::vector<Held> held_;
    std::vector<Gesture> gestures_;
    std::uint32_t next_gesture_id_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_ACTIONS_H
