// The first-launch preset picker (#144, ADR 0070): the port's own screen, shown before the game
// boots, that lists the presets (settings.h's Preset) with one line each and sets the one the
// player picks.
//
//   when        the game, an interactive run (wants_preset_picker), and no preset chosen yet:
//               no settings.ini, or one from before the presets (no `preset` key). A `preset`
//               line in the file or --preset skip it; so do every scripted or headless run.
//   lines       Original, Original + controller, Enhanced; and "Keep my settings" (custom)
//               when a settings file from before the presets exists, selected first so that
//               nothing changes by accident. Without a file Original is selected first.
//   move        Up / Down (arrows, W / S, the d-pad, the left stick); a mouse hovering a line
//   choose      Enter, Space, 1-4, the gamepad's South (A on an Xbox pad) or Start; a click or
//               a tap on a line (pressed and released on the same line)
//   quit        closing the window: the run ends without writing anything
//   then        (main.cpp, #193) a second step offers the controls card (controls_card.h): "Show
//               controls" (selected first) or "Start playing", chosen as a preset is; the card
//               opens once the game is up (GameOptions::show_controls_card)
//
// Drawn by draw() with Canvas (canvas.h) on a black 480 x 320-point screen, with the glyphs of
// the pad in use (glyphs.h), and shown through a Presenter in the window's scale mode, so a
// pointer maps to screen points as the game's do (scaling.h). The choice is applied with
// apply_preset and written to settings.ini by main.cpp before the game starts.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_PRESET_PICKER_H
#define MAPLETHIEF_PLATFORM_PRESET_PICKER_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "game.h"
#include "glyphs.h"
#include "scaling.h"
#include "settings.h"

namespace maplethief::frontend {

// Whether the picker shows: the game runs, nothing chose a preset (settings.preset empty after
// the file and the command line), and the run is interactive: no --frames, --seconds,
// --capture, --shot, --tap / --swipe, --fixed-step, --audio-capture, --minimise, --background
// or --quit, and a video driver other than SDL's "dummy" and "offscreen" (`video_driver` may
// be null: not known).
bool wants_preset_picker(bool game, const Settings &settings, const GameOptions &options,
                         const char *video_driver);

class PresetPicker {
  public:
    // The lines' layout, in screen points.
    static constexpr int kListX = 24, kListY = 62, kListW = 432, kRowH = 46;

    // `keep`: offer "Keep my settings" (an existing settings file without a preset). `offer`:
    // after the preset, the second step that offers the controls card (#193).
    explicit PresetPicker(bool keep, bool offer = false);

    // The lines, top to bottom (Preset::kCustom is "Keep my settings").
    const std::vector<Preset> &lines() const { return lines_; }
    std::size_t selected() const { return selected_; }
    // The line chosen, once the player chose one.
    std::optional<Preset> chosen() const { return chosen_; }
    // The second step (#193): shown after a preset was chosen; its answer (true: "Show
    // controls"), once given. The picker is done when both are (or without the step).
    bool offering() const { return chosen_.has_value() && offer_ && !show_controls_; }
    std::optional<bool> show_controls() const { return show_controls_; }
    bool done() const { return chosen_.has_value() && (!offer_ || show_controls_.has_value()); }
    // The lines on screen now: the presets', or the second step's two.
    std::size_t count() const { return chosen_ && offer_ ? 2 : lines_.size(); }

    // A key, gamepad button or axis event. Returns true if the picker took it. Mouse and finger
    // events go to pointer() instead, once the caller has their screen point.
    bool handle(const SDL_Event &e);
    // A pointer (the mouse, or a finger on a touch screen) at screen point `p`: pressed
    // (`down`), released (`up`), or moved (neither).
    void pointer(const ScreenPoint &p, bool down, bool up);
    // Every event: the glyph set follows the pad in use.
    void track(const SDL_Event &e);
    // The line at screen point (x, y), or -1.
    int line_at(float x, float y) const;

    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }
    // Draws the picker over `rgba` (width x height, top row first), `scale` pixels per point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

  private:
    void move(int delta);
    void choose(std::size_t line);

    std::vector<Preset> lines_;
    std::size_t selected_ = 0;
    std::optional<Preset> chosen_;
    bool offer_ = false;
    std::optional<bool> show_controls_;
    int pressed_ = -1; // the line a pointer went down on
    int stick_ = 0;    // the left stick as Up / Down (an edge, with a margin)
    bool dirty_ = true;
    GlyphTracker glyphs_;
};

// Shows the picker in the window until the player chooses (returns the line's preset, custom
// for "Keep my settings") or closes the window (std::nullopt, *quit true). Draws through a
// Presenter of its own in `settings`' scale mode and border colour; std::nullopt with *quit
// false if it cannot (logged), and the game starts without a preset. With `show_controls`, the
// second step too (#193): *show_controls is the answer (false if the window closed).
std::optional<Preset> run_preset_picker(SDL_Window *window, SDL_Renderer *renderer,
                                        const Settings &settings, bool keep, bool *quit,
                                        bool *show_controls = nullptr);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRESET_PICKER_H
