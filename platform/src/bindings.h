// The bindings file (#70, ADR 0054): bindings.ini next to settings.ini in the data directory
// (or --bindings FILE), which overrides rows of the binding table (actions.h's
// default_bindings(), #67 and #68). Pure parsing and writing plus the table edits the remap
// overlay (remap.h) makes; game.cpp and main.cpp read and write the file.
//
//   # MapleThief bindings
//   nintendo_buttons = position
//   [field]
//   attack = key Z, key Left Ctrl, button x, trigger righttrigger
//   jump = key X, key Space, key Left Alt, key Right Alt, button a
//   [menu]
//   ok = key Return, key Space, key Z, button a
//
// The format, in full:
//   - UTF-8 (a leading byte order mark is skipped); lines end in LF or CR LF. A file over
//     kMaxBindingsBytes is ignored as a whole (the defaults).
//   - Blank lines and lines whose first non-blank character is '#' are comments.
//   - `[field]` and `[menu]` start a context: the field is the key pad's panel 0, menus every
//     other screen (actions.h's Context). Before the first section only `nintendo_buttons =
//     position | label` may stand (gamepad.h's FaceButtons).
//   - In a section, `ACTION = CONTROL, CONTROL, ...` gives the action every control it has in
//     that context: its other controls there are unbound, and each listed control leaves the
//     action it had there. `ACTION = none` unbinds it (with a warning). Actions are
//     maplethief/input.h's names (up down left right jump attack slot1-slot4 ok back menu map
//     cash_shop minus plus revive grade_revive, and the port's fast_forward auto_run page
//     pointer_tap), in any case. An action without a line keeps its default controls, less any
//     a later line takes.
//   - A control is a kind and a name, by SDL's names (any case):
//       key NAME         SDL_GetKeyFromName: Z, Return, Left Ctrl, F5, Keypad Enter ... The
//                        comma key is `key Comma` (a comma separates controls).
//       button NAME      SDL_GetGamepadButtonFromString: a b x y (the Xbox layout's positions:
//                        a is the bottom face button on every pad), back guide start leftstick
//                        rightstick leftshoulder rightshoulder misc1 paddle1-4 touchpad ...;
//                        also south, east, west, north. Not the d-pad: that is `direction`.
//       hold NAME        a button (the names of `button`) held kHoldSeconds, only while
//                        Gamepads' hold gate is open (the shown pointer, ADR 0107); a shorter
//                        press is the button's own
//       trigger NAME     lefttrigger, righttrigger (also left, right)
//       direction NAME   up, down, left, right: the d-pad and the left stick merged
//   - Reserved, never bound: key F1 and button guide (they open the settings menu, ADR 0076,
//     or the remap overlay in a scripted run).
//   - Problems are reported with the file and the line ("bindings.ini:7: unknown action
//     'atack'") and the line, or the one control, is skipped: an unknown key, action or
//     control, a malformed line, a section other than field and menu, an action given twice in
//     one section (the later line is skipped), a control given to two actions of one section
//     (the later one is skipped), a reserved control. Never fatal.
//   - After the file, every action that had a control in a context by default and has none now
//     gets a warning ("bindings.ini: attack has no control in the field"); not the port's own
//     actions (fast_forward, #151), which are optional.
#ifndef MAPLETHIEF_PLATFORM_BINDINGS_H
#define MAPLETHIEF_PLATFORM_BINDINGS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "actions.h"
#include "gamepad.h"

namespace maplethief::frontend {

inline constexpr const char *kBindingsFileName = "bindings.ini";
inline constexpr std::size_t kMaxBindingsBytes = 64 * 1024;

// What a bindings file sets: the table and the face buttons' mode.
struct BindingSet {
    std::vector<Binding> table = default_bindings();
    FaceButtons face = FaceButtons::kPosition;
};

// The menus' own controls: never bound (F1, the gamepad's Guide).
bool reserved_control(const Control &c);

// A control from its text ("key Left Ctrl", "button a"); false with `why` otherwise.
bool parse_control(std::string_view text, Control *out, std::string *why);
// The control as a bindings file writes it ("key Left Ctrl", "button a", "trigger
// righttrigger", "direction up", "key Comma"); parse_control reads it back.
std::string control_text(const Control &c);

// The keyboard's controls or the gamepad's (directions, buttons and triggers).
enum class Device { kKeyboard, kGamepad };
Device device_of(const Control &c);

// The controls bound to `action` in `context`, in table order.
std::vector<Control> controls_for(const std::vector<Binding> &table, Context context,
                                  std::uint32_t action);
// Gives `action` exactly `controls` in `context`: its other controls there are unbound and each
// listed control leaves the action it had there. Rows left with nothing are dropped.
void set_controls(std::vector<Binding> *table, Context context, std::uint32_t action,
                  const std::vector<Control> &controls);
// The remap overlay's edit: `control` becomes `action`'s only control of its device in
// `context` (its controls of the other device stay); the action it had there loses it. Returns
// that action (MT_ACTION_NONE if none, or it was `action`).
std::uint32_t bind_control(std::vector<Binding> *table, Context context, std::uint32_t action,
                           const Control &control);

// The (context, action) pairs that have controls in `table` or in the defaults: what the file
// lists and the overlay shows, field first, each in mt_input_action order.
struct ActionSlot {
    Context context;
    std::uint32_t action;
};
std::vector<ActionSlot> action_slots(const std::vector<Binding> &table);

// Warnings for every (context, action) bound by default that has no control in `table` (and,
// with `device`, none of that device): "attack has no control in the field".
std::vector<std::string> unbound_warnings(const std::vector<Binding> &table);

// Parses a bindings file's text over `set` (whose table is normally default_bindings()).
// Returns the problems and warnings, each "FILE:LINE: ..." or "FILE: ..." with `file` as given
// (a file name for the messages), in order; empty if there were none.
std::vector<std::string> parse_bindings(std::string_view text, const std::string &file,
                                        BindingSet *set);

// The file's text for `set`: a comment header, nintendo_buttons, then [field] and [menu] with
// one line for every action_slots() entry (an action left unbound: `= none`).
// parse_bindings(serialise_bindings(s)) gives s's bindings back.
std::string serialise_bindings(const BindingSet &set);

// Whether two tables bind the same (control, context) pairs to the same actions, whatever
// their row order.
bool same_bindings(const std::vector<Binding> &a, const std::vector<Binding> &b);

// The file: <data dir>/bindings.ini, or the --bindings path.
std::string bindings_path(const std::string &data_dir, const std::string &flag);
// Reads `path` over `set`, logging each problem (never fatal). A missing file leaves `set` as it
// is (the defaults). Returns false if the file exists but could not be read.
bool load_bindings(const std::string &path, BindingSet *set);
// Writes `set` to `path` through a temporary file renamed over it. False (logged) if it cannot.
bool save_bindings(const std::string &path, const BindingSet &set);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_BINDINGS_H
