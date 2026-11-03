// The key pad editor (#159, ADR 0086): the port's own mode, drawn over the game, in which the
// player drags, pinches and fades the touch key pad's groups (keypad_layout.h) on the real pad.
//
//   open        the settings menu's action row (#89: game.cpp's request_keypad_editor), or
//               --edit-keypad once the game has its pad. The game is paused while it is open,
//               as for the remap overlay: game.cpp gives the guest no run-loop turn, and
//               composites the screen again at the paused time with the layout being edited,
//               panel 0 shown (mt_uikit_set_host_reveal) and not faded (hide_keypad)
//   touch       a finger or the mouse on a group selects it and drags it; a second finger
//               pinches it (its size, about its centre; the midpoint moves it too); the bar's
//               buttons: Size - / +, Opacity - / +, Reset (the selected group), Reset all,
//               Save, Cancel
//   keys        Tab / Shift+Tab the next / previous group; arrows move it 1 point (Shift: 10);
//               + and - its size (5 %); [ and ] its opacity (10 %); R resets it, Backspace or
//               Delete resets all; Enter saves; Esc cancels. The mouse wheel sizes the group
//               under the pointer (else the selected one)
//   gamepad     the d-pad moves (2 points), LB / RB the previous / next group, X / Y size - / +,
//               Back resets the group, A or Start saves, B cancels
//   save        closes with the layout edited: game.cpp installs it and writes keypad_layout to
//               settings.ini; cancel closes with the layout it opened on
//
// Every edit is clamped as the layout draws it (keypad_clamped: on the screen). The editor
// draws only outlines, labels and its bar, over the composited screen in the window (never in
// --shot or the device goldens: like the remap overlay, game.cpp draws it into the shown copy).
//
// Single thread: the frontend's. No runtime: it only edits a KeypadLayout.
#ifndef MAPLETHIEF_PLATFORM_KEYPAD_EDITOR_H
#define MAPLETHIEF_PLATFORM_KEYPAD_EDITOR_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "keypad_layout.h"

namespace maplethief::frontend {

class KeypadEditor {
  public:
    static constexpr std::uint32_t kScaleStep = 5, kOpacityStep = 10;     // percent
    static constexpr int kMoveStep = 1, kMoveStepFast = 10, kPadStep = 2; // screen points

    // The bar's buttons, left to right.
    enum class Button { kSizeDown, kSizeUp, kFadeDown, kFadeUp, kReset, kResetAll, kSave, kCancel };
    static constexpr int kButtonCount = 8;
    // The bar's place on the screen (points) and a button's rect.
    static constexpr int kBarY = 3, kBarH = 20;
    static KeypadRect button_rect(Button b);
    static const char *button_label(Button b);

    bool is_open() const { return open_; }
    // Opens on `current`, the first group selected.
    void open(const KeypadLayout &current);
    // Closes without saving: the layout it opened on.
    void cancel();
    // After a close: whether it was Save.
    bool saved() const { return saved_; }

    // The layout being edited (after a close: the one to keep: the saved or the original one).
    const KeypadLayout &layout() const { return layout_; }
    KeypadGroup selected() const { return selected_; }
    // Changes since open: the caller re-installs the layout when this moves.
    std::uint64_t version() const { return version_; }

    // A key, gamepad or mouse-wheel event. Returns true if the editor took it (every one while
    // open). Pointer presses and motion go to pointer() with their screen point.
    bool handle(const SDL_Event &e);
    // `id`'s pointer (the mouse, a finger) went down, moved or went up at screen point p.
    enum class Phase { kDown, kMove, kUp };
    void pointer(std::uint64_t id, Phase phase, KeypadPoint p);
    // The wheel at the pointer's last point: `steps` up (> 0) or down.
    void wheel(float steps);

    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }
    // Draws the outlines, the labels and the bar over `rgba` (width x height, top row first),
    // `scale` pixels per screen point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // What the status line says (tests).
    std::string status() const;

  private:
    struct Pointer {
        std::uint64_t id;
        KeypadPoint start, now;
    };
    void select(KeypadGroup g);
    void change(KeypadGroup g, const GroupLayout &value);
    void resize(KeypadGroup g, int delta);
    void fade(KeypadGroup g, int delta);
    void move(KeypadGroup g, int dx, int dy);
    void press(Button b);
    void close(bool save);
    void start_gesture();

    bool open_ = false, saved_ = false, dirty_ = false;
    KeypadLayout layout_, opened_;
    KeypadGroup selected_ = KeypadGroup::kDpad;
    std::uint64_t version_ = 0;
    std::vector<Pointer> pointers_;
    // The gesture: one pointer drags the group, two pinch it.
    bool gesture_ = false;
    KeypadGroup gesture_group_ = KeypadGroup::kDpad;
    GroupLayout gesture_from_;
    KeypadPoint last_pointer_ = {240, 160};
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYPAD_EDITOR_H
