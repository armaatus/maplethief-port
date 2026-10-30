// The remap overlay (#70, ADR 0054): the port's own screen, drawn over the game, that lists the
// actions with their keys and gamepad controls and rebinds them.
//
//   open        F1, or on a gamepad the Guide button or both stick clicks (L3 + R3): neither
//               is bound by default, so opening never sends an action to the game. Both are
//               reserved (bindings.h): they cannot be bound.
//   while open  every key and gamepad event goes to the overlay, none to the game, and the
//               game is paused: game.cpp gives the guest no run-loop turn until it closes
//               (as an iOS suspend: the engine folds the gap into its timers).
//   move        Up / Down (arrows, the d-pad, the left stick)
//   choose      Enter, Space or the gamepad's South (A on an Xbox pad); on an action: press
//               the new key or gamepad button (or a trigger, or the d-pad) within 5 s, which
//               becomes that action's only key, or only gamepad control, in that context. F1
//               or Guide stops waiting. A key already bound to another action there leaves
//               it (shown, with a warning if that action has no key left).
//   rows        the actions of the field, then of menus; Nintendo buttons (by position or by
//               label: Left / Right or choose); Reset to defaults; Save (applies, writes the
//               bindings file, closes); Cancel
//   close       Esc, the gamepad's East (B), F1 or Guide: Cancel (nothing changes)
//
// The overlay edits a copy of the bindings; only Save changes Actions, Gamepads' face buttons
// and the file. Drawn by draw() into a copy of the composited screen (canvas.h), with the
// glyphs of the pad in use (glyphs.h).
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_REMAP_H
#define MAPLETHIEF_PLATFORM_REMAP_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "gamepad.h"
#include "glyphs.h"

namespace maplethief::frontend {

// The name the overlay shows for an action ("Attack", "Quick slot 1", "Cash shop").
const char *action_title(std::uint32_t action);

class Remap {
  public:
    // How long a rebind waits for a key or button.
    static constexpr double kCaptureSeconds = 5.0;
    // The list's rows on screen at once.
    static constexpr std::size_t kVisibleRows = 15;

    enum class RowKind { kHeader, kAction, kFace, kReset, kSave, kCancel };
    struct Row {
        RowKind kind;
        ActionSlot slot; // kHeader: its context; kAction: the action
    };

    // `actions` and `gamepads` (may be null in tests) get the bindings on Save.
    Remap(Actions *actions, Gamepads *gamepads);

    // Where Save writes (bindings_path()); "" leaves Save with an error.
    void set_path(const std::string &path) { path_ = path; }
    const std::string &path() const { return path_; }

    bool is_open() const { return open_; }
    // Opens on the bindings in effect, the first action selected; lifts every held key and pad
    // control (the game must not keep walking).
    void open(double now);
    // Closes without changing anything.
    void cancel();

    // An SDL event at time `now` (seconds, any clock). Returns true if the overlay took it:
    // while open every key, gamepad button and axis, mouse and touch event, and the event that
    // opens it. The caller hands the events it does not take to the game's devices.
    bool handle(const SDL_Event &e, double now);
    // Every event, after the devices had it: the glyph set follows the pad in use.
    void track(const SDL_Event &e);
    // The rebind's timeout.
    void tick(double now);
    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }

    // Draws the overlay over `rgba` (width x height, top row first, opaque; or premultiplied,
    // as GPU presentation's UIKit layer, ADR 0068's addendum), `scale` pixels per screen point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // The glyph set: the pad in use (track()).
    const GlyphTracker &glyphs() const { return glyphs_; }

    // State, for the tests and the logs.
    const std::vector<Row> &rows() const { return rows_; }
    std::size_t selected() const { return selected_; }
    std::size_t first_visible() const { return top_; }
    bool capturing() const { return capturing_; }
    const BindingSet &edit() const { return edit_; }
    const std::string &status() const { return status_; }
    unsigned saves() const { return saves_; }

  private:
    void build_rows();
    void move(int delta);
    void choose(double now);
    void capture(const Control &c);
    void save();
    void say(std::string text, bool warning);
    bool handle_closed(const SDL_Event &e, double now);
    void scroll_to_selection();

    Actions *actions_;
    Gamepads *gamepads_;
    std::string path_;
    GlyphTracker glyphs_;
    bool open_ = false;
    bool dirty_ = false;
    BindingSet edit_;
    std::vector<Row> rows_;
    std::size_t selected_ = 0, top_ = 0;
    bool capturing_ = false;
    double capture_until_ = 0;
    double now_ = 0;
    std::string status_;
    bool status_warning_ = false;
    unsigned saves_ = 0;
    // The left stick as Up / Down while open (an edge, with a margin), and the triggers.
    int stick_ = 0;
    bool trigger_[2] = {false, false};
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_REMAP_H
