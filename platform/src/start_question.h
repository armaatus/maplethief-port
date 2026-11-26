// #198's start-up question after a crash, asked in the window (#416, ADR 0151's addendum): where
// an SDL message box cannot be answered with every input method of the platform (the Nintendo
// Switch, which has none; a Steam Deck's Game Mode; platform_caps.h's kCapMessageBox), the same
// words and buttons as the box (crash_report.h: safe_start_question_text, recovery_question_text,
// start_choice_label) on a screen drawn like the preset picker (preset_picker.h): the Canvas, the
// pad's glyphs, keys, the pad, the mouse and touch.
//
//   the buttons   two (Start with default settings, Start normally) without a recovery point,
//                 ADR 0151's four with one (Resume, Resume with default settings, Start with
//                 default settings, Start normally). The first is selected.
//   the input     Up / Down (W / S, the d-pad, the left stick) move; Return, Space, A and Start
//                 choose; 1 to 4 choose that button; Escape and B choose Start normally (the
//                 last button, as the box's Escape). A click or a tap chooses the button under it.
//
// main.cpp asks it once the window and its renderer exist; the safe start's settings apply after
// it (the renderer re-opened if the backend changes, as after the preset picker).
#ifndef MAPLETHIEF_PLATFORM_START_QUESTION_H
#define MAPLETHIEF_PLATFORM_START_QUESTION_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "crash_report.h"
#include "glyphs.h"
#include "platform_caps.h"
#include "scaling.h"
#include "settings.h"

namespace maplethief::frontend {

// Whether the question is asked in the window: `platform` has no kCapMessageBox, or the run is in
// a Steam Deck's Game Mode (`gamescope`: file_dialog.h's dialog_place test).
constexpr bool start_question_in_window(bool gamescope,
                                        SettingsPlatform platform = kSettingsPlatform) {
    return !platform_has(kCapMessageBox, platform) || gamescope;
}

class StartQuestion {
  public:
    // The layout, in screen points: the text from the top, the buttons above the hint line.
    static constexpr int kTextX = 24, kTextY = 16, kLineH = 10, kParagraphGap = 5;
    static constexpr int kListX = 24, kListW = 432, kRowH = 30, kHintY = 290;
    // The characters of the 5x7 font a text line holds.
    static constexpr std::size_t kColumns = 72;

    // `text`: the question (paragraphs split by "\n\n", lines by "\n"). `buttons`, top to
    // bottom; the last one is Escape's and B's.
    StartQuestion(const std::string &text, std::vector<std::string> buttons);

    const std::vector<std::string> &buttons() const { return buttons_; }
    // The text's lines as drawn ("" is a paragraph's gap).
    const std::vector<std::string> &lines() const { return lines_; }
    std::size_t selected() const { return selected_; }
    std::optional<std::size_t> chosen() const { return chosen_; }
    bool done() const { return chosen_.has_value(); }
    // The top of the first button.
    int list_y() const { return kHintY - 6 - static_cast<int>(buttons_.size()) * kRowH; }

    // A key, gamepad button or axis event; true if taken. Mouse and finger events go to
    // pointer() with their screen point.
    bool handle(const SDL_Event &e);
    // A pointer at screen point `p`: pressed (`down`), released (`up`), or moved (neither).
    void pointer(const ScreenPoint &p, bool down, bool up);
    // Every event: the glyphs follow the pad in use.
    void track(const SDL_Event &e);
    // The button at screen point (x, y), or -1.
    int button_at(float x, float y) const;

    bool dirty() const { return dirty_; }
    // Draws the question over `rgba` (width x height, top row first), `scale` pixels per point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

  private:
    void move(int delta);
    void choose(std::size_t button);

    std::vector<std::string> buttons_;
    std::vector<std::string> lines_;
    std::size_t selected_ = 0;
    std::optional<std::size_t> chosen_;
    int pressed_ = -1; // the button a pointer went down on
    int stick_ = 0;    // the left stick as Up / Down (an edge, with a margin)
    bool dirty_ = true;
    GlyphTracker glyphs_;
};

// Shows `question` in the window until a button is chosen (its index) or the window closes
// (std::nullopt, *quit true). std::nullopt with *quit false if the screen cannot be shown.
std::optional<std::size_t> run_start_question(SDL_Window *window, SDL_Renderer *renderer,
                                              const Settings &settings, StartQuestion *question,
                                              bool *quit);

// decide_start's prompts on this window: the two-button and the four-button question. A window
// closed in the question, or a screen that cannot be shown, answers Start normally; *quit says
// the window closed.
StartPrompts window_start_prompts(SDL_Window *window, SDL_Renderer *renderer,
                                  const Settings &settings, bool *quit);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_START_QUESTION_H
