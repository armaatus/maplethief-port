// Button prompts and the focus markers the engine does not draw (#146, ADR 0051's addendum):
// with a gamepad or the keyboard, every screen shows where the player is and which button does
// what.
//
//   prompts   the glyph of the bound control (glyphs.h) next to a key pad button: a popup's OK /
//             Yes / No / Cancel / - / +, the death popup's four choices, the NPC chat's Accept /
//             Refuse / OK, the shop's Buy / Sell / tabs / Charge / close, the map's buttons, the
//             cut scenes' strip, the character select's New, and back on the screens the
//             controller focus drives (focus.h); on the popup menu (GL widgets, no UIKit
//             button) its close and, with no tab open, the tab keys at the tab bar's ends. The
//             table is kPromptSpots (prompts.cpp).
//   markers   a frame (the focus highlight's) around the selection where the engine draws no
//             cursor: the popup menu's item and equipment lists (CDataMgr +0xD8, +0xDC over the
//             list widget's cells). The engine draws its own on the popup menu's tab bar, the
//             NPC chat's quest rows, the shop's list (once a row is chosen) and the world map;
//             the popups have none, and their buttons get prompts.
//
// Which glyphs: the device used last (DeviceTracker): a key press shows key caps, a gamepad press
// (or a pad connecting) that pad's set (GlyphTracker: Xbox, PlayStation, Nintendo), a mouse click
// or a touch nothing (a touch player needs no prompt). The control shown is the one the bindings
// give the action in menus (Actions::bindings(), so a remap shows; prompt_control).
//
// Drawn by the port into the window only, after the screen image's read-backs and captures, as
// the focus highlight, the frame-time overlay and the cash shop's price line: --shot,
// --shot-gl, --capture and the device goldens never contain them. HintLayer draws the prompts
// with Canvas into a transparent layer (one texture), the markers go through
// Presenter::draw_highlight. Settings (settings.h): focus_highlight (the focus highlight and these
// markers) and button_prompts.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_PROMPTS_H
#define MAPLETHIEF_PLATFORM_PROMPTS_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "focus.h"
#include "gamepad.h"
#include "glyphs.h"
#include "maplethief/input.h"
#include "maplethief/recomp/recomp.h"
#include "scaling.h"

namespace maplethief::frontend {

// The device whose controls the prompts show.
enum class PromptDevice { kNone, kKeyboard, kGamepad };
// "none", "keyboard", "gamepad".
const char *prompt_device_name(PromptDevice device);

// The device used last:
//   a key down (not a repeat)                          the keyboard
//   a pad connecting, a pad button, an axis past half  the gamepad
//   a mouse button or a finger down                    none (a touch player)
//   the last pad disconnecting while it was shown      none
class DeviceTracker {
  public:
    void handle(const SDL_Event &e);
    PromptDevice device() const { return device_; }
    // How often the device changed (for logs and tests).
    unsigned changes() const { return changes_; }

  private:
    void use(PromptDevice d);

    PromptDevice device_ = PromptDevice::kNone;
    std::vector<SDL_JoystickID> pads_;
    unsigned changes_ = 0;
};

// How a prompt is drawn: the device, and for a gamepad its type (GlyphTracker::type()) and the
// face-button mode (Gamepads::face_buttons()).
struct PromptStyle {
    PromptDevice device = PromptDevice::kNone;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    FaceButtons face = FaceButtons::kPosition;
    bool operator==(const PromptStyle &) const = default;
};

// The control that shows `action` in menus for `device`: of the bindings that give `action` in
// the menu context, the gamepad's first (the d-pad, a button, a trigger), or the keyboard's
// preferred key (Enter for OK, Esc for back, M for the map) if it is one of them, else its first.
// False when nothing of that device is bound to it (or the device is none).
bool prompt_control(const std::vector<Binding> &bindings, std::uint32_t action, PromptDevice device,
                    Control *out);

// A prompt: the glyph of `action`'s control, its middle at y, beside a button: left of it,
// ending at `before`, or right of it, starting at `after` (screen points), whatever its width.
// It goes on the side `right` asks for, or the other one where that leaves the screen.
struct Prompt {
    std::uint32_t action = MT_ACTION_NONE;
    int before = 0, after = 0, y = 0;
    bool right = false;
    bool operator==(const Prompt &) const = default;
};
// The glyph's left edge for a prompt `w` points wide on a screen `screen_w` points wide.
int prompt_left(const Prompt &p, int w, int screen_w);

// The action whose glyph a control gets (MT_ACTION_NONE: none), by its target's class and
// selector (FocusTarget::target, ::action).
std::uint32_t prompt_action(const std::string &target, const std::string &action);

// The prompts for the reachable controls (Focus::targets()) on key pad panel `panel`: each
// control in the table (none on the field's panel 0: its key pad is the game's own), ending
// where the table says the button's art begins (a control as wide as the screen: at its right
// end), centred on it vertically; with the controller focus active, back at the control back
// touches (focus_back_target), unless the table gave one already.
std::vector<Prompt> prompts_for(const std::vector<FocusTarget> &targets, std::int32_t panel,
                                bool focus_active);

// The popup menu (panel 1): CDataMgr's tab (+0xB0), open tab (+0xA8: 0 none, 1 items, 2
// equipment, ...), in the item and equipment lists the cursor's cell, and the game mode
// (0x11AA00: 1 while the popup menu is up).
struct PopupMenu {
    std::int32_t tab = 0, open = 0;
    bool cursor = false;
    Rect cell; // screen points, clipped to the list
    std::int32_t mode = 0;
};
// Reads it from guest memory (CDataMgr at 0x11A984). The cursor: CDataMgr +0xD8 (items) or
// +0xDC (equipment), an index into the list widget's cells (0xF14B8, 0xF14C0: each points at the
// variable that holds the widget; its cells at +0x00 (count at +0x04), each with its rect at
// +0x20 plus the list's offset at +0x08); clipped to the widget's rect (+0x1C). False without a
// CDataMgr.
bool read_popup_menu(const mt_cpu *cpu, PopupMenu *out);
// The popup menu's own prompts (it has no UIKit button): back by its close (X at (398, 28)) and,
// with no tab open, the tab keys (minus, plus) at the tab bar's ends.
std::vector<Prompt> popup_menu_prompts(const PopupMenu &menu);

// The focused control to highlight: Focus::highlighted() (active, and an action used since), or
// while a gamepad or the keyboard is the device in use, the focused control as soon as the focus
// is active, so that a pad player sees where the focus is before the first press.
const FocusTarget *focus_highlight_target(const Focus &focus, PromptDevice device);

// What the port draws over the window this turn (besides Focus's own highlight).
struct Hints {
    std::vector<Prompt> prompts;
    std::vector<Rect> markers; // screen points
};

// The source of the popup menu's state (tests replace it); the default is read_popup_menu.
using PopupMenuSource = bool (*)(void *user, const mt_cpu *cpu, PopupMenu *out);

// The hints of a turn: the device (from the SDL events), the prompts and markers the screen
// gets (both empty while the device is none: a touch player sees nothing new).
class HintTracker {
  public:
    // Every SDL event (after the devices had it).
    void handle(const SDL_Event &e) { device_.handle(e); }
    void set_menu_source(PopupMenuSource source, void *user);
    // Once per run-loop turn, after Focus::turn: the hints for key pad panel `panel`.
    // `prompts` (button_prompts) and `markers` (focus_highlight) switch each part. The popup
    // menu's hints show on panel 1, and on panel -1 while the game mode is the popup menu's
    // and the focus is off: a popup closed over the popup menu (its Save) leaves the panel at
    // -1 (disableAlertKey, ADR 0051's #127 addendum) with the menu still up.
    void turn(const mt_cpu *cpu, const Focus &focus, std::int32_t panel, bool prompts,
              bool markers);

    PromptDevice device() const { return device_.device(); }
    const DeviceTracker &devices() const { return device_; }
    const Hints &hints() const { return hints_; }

  private:
    DeviceTracker device_;
    PopupMenuSource menu_ = nullptr;
    void *menu_user_ = nullptr;
    Hints hints_;
};

// Draws the prompts with `canvas` (screen points), each glyph on a dark backing so it reads over
// any art. Prompts whose action has no control for the style's device are skipped. Returns how
// many it drew.
int draw_prompts(Canvas &canvas, const std::vector<Prompt> &prompts, const PromptStyle &style,
                 const std::vector<Binding> &bindings);
// The focus highlight's frame (Presenter::draw_highlight's colours: dark outside, yellow inside,
// two points out from the rect) with `canvas`: for images of the window made without a renderer
// (the real-asset tests' window shots).
void draw_marker(Canvas &canvas, const Rect &r);

// The prompts' layer in the window: a transparent RGBA texture of the image's size times the
// placement's scale, drawn with Canvas when the prompts, style or bindings change, and rendered
// over the placed image.
class HintLayer {
  public:
    HintLayer() = default;
    HintLayer(const HintLayer &) = delete;
    HintLayer &operator=(const HintLayer &) = delete;
    ~HintLayer() { stop(); }

    // Draws `prompts` over `placement`'s image (nothing when empty). Call after the read-backs
    // and captures, before SDL_RenderPresent. False (logged once) if the texture cannot be made.
    bool draw(SDL_Renderer *renderer, const Placement &placement,
              const std::vector<Prompt> &prompts, const PromptStyle &style,
              const std::vector<Binding> &bindings);
    // Frees the texture (before the renderer goes).
    void stop();
    // How often the layer was drawn anew (for logs and tests).
    unsigned redraws() const { return redraws_; }

  private:
    SDL_Texture *texture_ = nullptr;
    int scale_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<Prompt> drawn_;
    PromptStyle drawn_style_;
    std::vector<Binding> drawn_bindings_;
    bool told_ = false;
    unsigned redraws_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PROMPTS_H
