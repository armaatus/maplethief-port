// The controls card (#193, ADR 0054's addendum): one page that shows how to play with the device
// in use, since the game has no controls screen of its own. Drawn in code with Canvas (shapes and
// the port's 5x7 font): no game art.
//
//   keyboard  a small keyboard with the keys bound in the field lit, then two lists, the field's
//             and the menus', each action with its key caps
//   gamepad   a pad (triggers, shoulders, d-pad, face buttons, View / Menu, the sticks) in the
//             glyphs of the pad in use (GlyphTracker: Xbox, PlayStation or Nintendo), the
//             triggers and shoulders labelled with their field action, then the same two lists
//             with that pad's glyphs
//   touch     the touch key pad's schematic in the player's layout (keypad_layout.h: the d-pad,
//             jump and attack, the quick slots moved and resized as drawn; Map, Bag and Cash)
//             with each key's name (the device none: a mouse click or a touch was the last input)
//
// The lists come from the live bindings (Actions::bindings(), so a remap shows at once): every
// (context, action) that action_slots() lists and that the device has a control for, now or by
// default (the pad has no field OK), so a new action shows without code here; the four
// directions as one Move line when each has a control of the device (else one line each); at
// most kCardMaxControls controls a line, in table order; "unbound" for an action with none of the
// device's. The settings menu's own controls (F1, Guide) close the field's list.
//
//   open      the settings menu's Controls tab, "Controls card"; and once after the first-launch
//             preset picker, whose second step offers it (preset_picker.h). Never in a scripted
//             run (game.cpp: only where the settings menu is allowed).
//   while     the game is paused as under the remap overlay (game.cpp's run_overlays); every key,
//   open      mouse, touch and pad button or axis event goes to the card.
//   close     any press: a key, a click, a tap, a pad button (not a key repeat, not a stick). The
//             press is taken and so is its release, so nothing reaches what is under the card.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_CONTROLS_CARD_H
#define MAPLETHIEF_PLATFORM_CONTROLS_CARD_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "gamepad.h"
#include "glyphs.h"
#include "hud_labels.h"
#include "keypad_layout.h"
#include "prompts.h"

namespace maplethief::frontend {

// The controls a line shows at most (the first ones the table gives the device).
inline constexpr std::size_t kCardMaxControls = 2;

// A line of the card's lists.
struct CardLine {
    Context context = Context::kField;
    // The action (the merged Move line: MT_ACTION_UP; the settings line: MT_ACTION_NONE).
    std::uint32_t action = MT_ACTION_NONE;
    std::string title;
    // The device's controls (Move: one per direction, up down left right); empty: unbound.
    std::vector<Control> controls;
    bool move = false;     // the four directions in one line
    bool settings = false; // the settings menu's own controls (F1, Guide): not a binding
};

// The card's name for an action in a context ("Popup menu", "Quick slot 1", "Page (hold)"); the
// remap overlay's (action_title) for the rest.
std::string card_title(Context context, std::uint32_t action);

// The lists for `device` (keyboard or gamepad; none gives the lines with no controls), field
// first, from `table`.
std::vector<CardLine> controls_card_lines(const std::vector<Binding> &table, PromptDevice device);

// A key of the touch key pad as the card draws it: its name, where it is drawn with the player's
// layout (screen points, 480 x 320), and the side its name goes on.
struct TouchCardKey {
    std::string title;
    KeypadRect rect;
    LabelSide side = LabelSide::kAbove;
    bool dpad = false;
};
std::vector<TouchCardKey> touch_card_keys(const KeypadLayout &layout);

// Draws the card over `canvas` (480 x 320 points): a backdrop over the whole screen, then the
// panel, for `style`'s device (none: touch), from `bindings` and the touch pad's `layout`.
void draw_controls_card(Canvas &canvas, const PromptStyle &style,
                        const std::vector<Binding> &bindings, const KeypadLayout &layout);

class ControlsCard {
  public:
    // `actions` (the bindings, and what opening lifts) and `gamepads` (the active pad, the face
    // buttons' mode) may be null in tests: the default bindings, the position mode.
    ControlsCard(Actions *actions, Gamepads *gamepads) : actions_(actions), gamepads_(gamepads) {}

    // The touch key pad's layout the touch card shows (settings' keypad_layout).
    void set_keypad_layout(const KeypadLayout &layout);

    bool is_open() const { return open_; }
    // Opens it; lifts every held key and pad control, as the remap overlay does.
    void open();
    // Closes it (a quit, the overlay cannot be shown); logged with `why`.
    void close(const char *why);

    // An SDL event, before the devices and the other overlays. Returns true if it took the
    // event: while open every input event (a press closes it); after it closed, the release of
    // the press that closed it. A pad's ADDED and REMOVED are never taken.
    bool handle(const SDL_Event &e);
    // Every event, after the devices had it: the device and the glyph set follow the input.
    void track(const SDL_Event &e);

    // The device until the card sees input of its own (the preset picker's, #193).
    void seed_device(PromptDevice device) { seed_ = device; }
    // The device and glyphs the card shows now.
    PromptStyle style() const;
    const GlyphTracker &glyphs() const { return glyphs_; }
    const DeviceTracker &devices() const { return devices_; }
    const std::vector<Binding> &bindings() const;

    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }
    // Draws it over `rgba` (width x height, top row first, opaque or premultiplied), `scale`
    // pixels per screen point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // How often it opened and closed (logs, tests).
    unsigned opens() const { return opens_; }
    unsigned closes() const { return closes_; }

  private:
    Actions *actions_;
    Gamepads *gamepads_;
    KeypadLayout layout_;
    GlyphTracker glyphs_;
    DeviceTracker devices_;
    PromptStyle drawn_;
    PromptDevice seed_ = PromptDevice::kNone;
    bool open_ = false;
    bool dirty_ = false;
    unsigned opens_ = 0, closes_ = 0;
    // The release to take after the closing press (0: none) and what it names.
    Uint32 swallow_type_ = 0;
    Uint64 swallow_what_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CONTROLS_CARD_H
