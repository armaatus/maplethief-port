// The settings menu (#89, ADR 0076): the port's own overlay, drawn over the game, that shows and
// changes every setting of the registry (settings.h) and the preset, with pad, keyboard, mouse
// and touch. Nothing in it names a setting: its tabs are the registry's groups that have rows,
// its rows the registry's rows of a group (then the group's action rows, menu_actions()), each
// with its label, value (setting_text), help, and the values setting_choices offers.
//
//   open        F1, or on a gamepad Guide or both stick clicks (L3 + R3): the controls ADR
//               0054 kept free. A tap or click on the gear (gear_rect: in the bars beside the
//               image when there is room, else the image's top right corner once a touch
//               screen was used). Never in a scripted run (set_allowed(false)).
//   while open  every key, gamepad, mouse and touch event goes to the menu, none to the game,
//               and the game is paused: game.cpp gives the guest no run-loop turn until it
//               closes, as for the remap overlay (ADR 0054), with ADR 0063's lifecycle rules.
//   top line    the preset: Original, Original + controller, Enhanced or Custom (and "None yet"
//               when nothing chose one), its summary; Left / Right picks another (apply_preset).
//   tabs        the groups: Video, Audio, Controls, Gameplay, Accessibility, Assists, Saves,
//               Other (only those with rows). Assists carry a warning line: they change how
//               the game plays, and no preset turns one on.
//   move        Up / Down (arrows, W / S, the d-pad, the left stick); a mouse hovering a row
//   change      Left / Right (arrows, A / D, the d-pad, the stick): step_setting; Enter, Space or
//               South: the next value, or an action row's action; a click or tap on a row's
//               "<" or ">" (elsewhere on the row: the next value)
//   tabs        Tab / Shift+Tab, Q / E, Page Up / Down, the shoulder buttons (LB / RB); a click
//               or tap on a tab
//   scroll      the mouse wheel; a click or tap on the list's ^ and v
//   close       Esc, Backspace, F1, the gamepad's East (B), Start or Guide; the X at the top
//               right. The menu's changes are then written to settings.ini (game.cpp,
//               save_menu_changes: replayed over the file, so a one-run flag is not written).
//
// A change applies at once when the row says Applies::kAtOnce and the frontend can (the apply
// callback, live_settings.h); otherwise the row shows "next start" until the value is back to
// what this run started with.
//
// Drawn by draw() with Canvas (canvas.h) over a copy of the composited screen, as the remap
// overlay is, with the glyphs of the pad in use (glyphs.h). With a row selected that has
// SettingInfo::test_pattern (the picture adjustments, #187), the list's slots that show no such
// row are a test pattern instead: a grey ramp, and the game's own picture left undimmed, which
// the present stage's colour step grades with the menu. Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_SETTINGS_MENU_H
#define MAPLETHIEF_PLATFORM_SETTINGS_MENU_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "actions.h"
#include "frame_stats.h"
#include "gamepad.h"
#include "glyphs.h"
#include "input.h"
#include "scaling.h"
#include "settings.h"

namespace maplethief::frontend {

// An action row (#89): a button in a tab, not a setting in the file. The frontend gives each
// key its handler (SettingsMenu::set_action); a row without one says it is not available.
struct MenuActionInfo {
    const char *key; // lower case, unique among the actions
    SettingGroup group;
    const char *label;
    const char *help;
    // At the bottom of every tab, after the group's rows (`group` is then not a tab of its own).
    bool every_tab = false;
    // A question to confirm first ("Quit MapleThief?"): choosing the row turns its value into
    // No (focused first) or Yes; Left / Right pick, choosing again confirms, and only Yes runs
    // the action. Moving away, a tab, Esc or the gamepad's East cancel it.
    const char *confirm = nullptr;
    const char *button = "Open..."; // the row's value
};
// The action rows, in the order the menu lists them in their tabs:
//   bindings       Controls  the remap overlay (remap.h, ADR 0054)
//   controls_card  Controls  #193: the controls card (controls_card.h), over the menu
//   game_options   Gameplay  the game's own options (sound, speed, vibration): its popup menu's
//                            option tab, from the field (live_settings.h's GameOptionsOpener)
//   export_saves   Saves     #148; #192: the system's save dialog (save_menu.h)
//   import_saves   Saves     #148; #192: the system's open dialog
//   backups        Saves     #148; #192: the list of backups (open_list), each one restorable
//   data_location  Saves     #263: Game folder or User folder; choosing it copies the data to
//                            the other one and closes the game (data_switch.h; not on iOS)
//   open_data_folder  Saves  #192: the data folder in Finder, Explorer or the file manager
//   open_logs_folder  Saves  #198: the log files' folder, the same way (desktop)
//                            (not on iOS)
//   achievements   Gameplay  #154: the summary, then each local achievement in turn
//   keypad_layout_edit  Controls  #159: the touch key pad editor (keypad_editor.h)
//   quit           every tab #179: "Quit MapleThief?" then the same quit as Cmd-Q
//                            (SDL_EVENT_QUIT: applicationWillTerminate:, autosave = on_quit
//                            first). Not on iOS, where apps do not quit themselves.
std::span<const MenuActionInfo> menu_actions();
const MenuActionInfo *find_menu_action(const char *key);

// The frame-time cost the menu shows beside the rows with SettingInfo::frame_cost: the last
// second's frontend time per window frame, the costliest frame's, and the frame rate.
std::string frame_cost_text(const RecentStats &recent);

// Where the gear that opens the menu goes, in drawable pixels: in the bar to the right of the
// image if it is wide enough, else in the bar above it, else over the image's top right corner
// (`over_image`). Inside the safe area. `density`: drawable pixels per window point.
struct GearPlace {
    Rect rect;
    bool over_image = false;
};
GearPlace gear_place(const Placement &placement, float density);
// With widescreen (#86, ADR 0090's addendum): beside the wide picture (`wide`, the placement
// with its rectangle: Presenter::wide_placement) so that the gear keeps clear of the wings; where
// no bar fits beside it (a 16:9 window), gear_place(image, ...) as before, over the wings' corner
// rather than over the screen image. Equal to gear_place(image, density) when widescreen is off.
GearPlace gear_place(const Placement &image, const Placement &wide, float density);
inline constexpr float kGearPoints = 26; // its size in window points

// Whether `e` is a press on a touch screen (a finger of a direct touch device, not a trackpad
// and not SDL's touch made from the mouse): the gear may then show over the image.
bool is_screen_finger_down(const SDL_Event &e);
// Whether `e` presses the gear at `gear` (drawable pixels): the left mouse button (not one SDL
// made from a touch or a pen) or a finger on a touch screen, at a point inside it.
bool gear_pressed(const SDL_Event &e, SDL_Window *window, const Rect &gear);

// The gear, drawn into the window only (after the read-backs, as the focus highlight): never in
// a shot, a golden or a capture.
class GearButton {
  public:
    GearButton() = default;
    GearButton(const GearButton &) = delete;
    GearButton &operator=(const GearButton &) = delete;
    ~GearButton() { stop(); }
    // Draws it at `rect` (drawable pixels); `faint` over the image. False if it cannot.
    bool draw(SDL_Renderer *renderer, const Rect &rect, bool faint);
    // Frees its texture (before the renderer goes).
    void stop();
    // Its look at `size` x `size` pixels (premultiplied RGBA), for the tests.
    static std::vector<std::uint8_t> pixels(int size);

  private:
    SDL_Texture *texture_ = nullptr;
    int size_ = 0;
};

// A list that an action row opens in place of its tab's rows (#192: Saves -> Backups). Up / Down
// move, LT or Shift with Up / Down, Page Up / Down and LB / RB page (ADR 0114), choosing a row
// asks `question` first when it gives one (No focused, as a confirm row: Left / Right pick,
// choose again), then calls `choose` and the list closes. Esc, Backspace or the gamepad's East
// go back to the tab (cancelling a question first); a tab closes it too.
struct MenuListRow {
    std::string label;
    std::string value; // the value column's text
};
struct MenuList {
    std::string title; // the tab's note line
    std::string help;  // the help lines
    std::vector<MenuListRow> rows;
    std::function<std::string(std::size_t row)> question; // "" (or none): no question
    std::function<std::string(std::size_t row)> choose;   // the status line
};

class SettingsMenu {
  public:
    // The list's rows on screen at once, and the layout in screen points (480 x 320).
    static constexpr std::size_t kVisibleRows = 10;
    static constexpr int kPresetY = 32, kPresetH = 24;
    static constexpr int kTabsY = 60, kTabH = 14;
    static constexpr int kListY = 92, kRowH = 16;
    static constexpr int kListX = 14, kListW = 430;
    static constexpr int kValueX = 238, kValueEnd = 420; // the value's "<" ... ">"
    static constexpr int kScrollX = 446, kScrollW = 22;  // the list's ^ and v
    static constexpr int kCloseX = 446, kCloseY = 10, kCloseW = 22;
    static constexpr std::size_t kLineChars = 73; // characters on the note, help, status lines

    enum class ItemKind { kPreset, kSetting, kAction, kListRow };
    struct Item {
        ItemKind kind;
        const SettingInfo *setting = nullptr;   // kSetting
        const MenuActionInfo *action = nullptr; // kAction
        std::size_t row = 0;                    // kListRow: list().rows[row]
    };

    // A changed row's apply: true if it now applies, false if it waits for the next start.
    using ApplyFn = std::function<bool(const SettingInfo &row, const Settings &settings)>;
    // An action's handler, called when it is chosen: the status line to show ("" for none).
    // It may close the menu (close()) or open another overlay.
    using ActionFn = std::function<std::string()>;

    // `rows`: the registry (settings_registry(); tests pass their own). `actions`, `gamepads`,
    // `input` (any may be null): what opening releases, so nothing stays held in the game.
    SettingsMenu(std::span<const SettingInfo> rows, Actions *actions, Gamepads *gamepads,
                 Input *input);

    // Scripted runs: false, and nothing opens it.
    void set_allowed(bool allowed) { allowed_ = allowed; }
    bool allowed() const { return allowed_; }
    void set_apply(ApplyFn apply) { apply_ = std::move(apply); }
    void set_action(const std::string &key, ActionFn fn);
    // An action row whose value and help change (#263: Saves -> Data location): asked at each
    // draw, so it must not touch the disk. Without one, the row's `button` and `help`.
    struct ActionView {
        std::string value;
        std::string help;
    };
    using ViewFn = std::function<ActionView()>;
    void set_action_view(const std::string &key, ViewFn fn);
    // A question asked when the row is chosen, in place of MenuActionInfo::confirm: shown in the
    // help lines with No focused (Left / Right pick, choose again); "" runs the action at once
    // (its handler then says why it can't).
    using QuestionFn = std::function<std::string()>;
    void set_action_question(const std::string &key, QuestionFn fn);
    // A tab's note line from the frontend (#263: the Saves tab shows the data folder), in place
    // of the tab's own.
    using NoteFn = std::function<std::string()>;
    void set_tab_note(SettingGroup group, NoteFn fn);
    // The values this run started with (the "next start" marks compare with them).
    void set_started(const Settings &started) { started_ = started; }
    // The numbers frame_cost rows show (frame_cost_text): the game's last second, set by the
    // frontend as the menu opens (the game is paused while it is open).
    void set_frame_cost(const RecentStats &recent);
    // The settings in effect, which the menu edits in place (kept by pointer). Nothing opens
    // the menu before.
    void attach(Settings *live) { live_ = live; }

    bool is_open() const { return open_; }
    // Opens on the tab and row it was last on; lifts every held key, pad control and touch.
    void open();
    void close();

    // An SDL event: a key, gamepad button or axis, or the mouse wheel. While closed only the
    // hotkeys (F1, Guide, L3 + R3), if allowed. Returns true if the menu took it: while open
    // every key and gamepad event. Mouse buttons, motion and fingers go to pointer() instead,
    // once the caller has their screen point (while open the caller drops them from the game).
    bool handle(const SDL_Event &e);
    // A pointer (the mouse, or a finger on a touch screen) at screen point `p`: pressed
    // (`down`), released (`up`), or moved (neither).
    void pointer(const ScreenPoint &p, bool down, bool up);
    // Every event: the glyph set follows the pad in use.
    void track(const SDL_Event &e);

    // A list in place of the tab's rows (MenuList), from an action's handler; items() are then
    // the preset line (never selected) and its rows. An empty list is not opened.
    void open_list(MenuList list);
    void close_list();
    bool in_list() const { return list_open_; }
    const MenuList &list() const { return list_; }
    // The status line, set from outside (a file dialog's answer, #192).
    void set_status(std::string text) { say(std::move(text)); }

    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }
    // Draws the menu over `rgba` (width x height, top row first, opaque or premultiplied),
    // `scale` pixels per screen point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // The menu's changes since the last take: the apply_setting calls it made, (key, value) in
    // order, "preset" for a preset. game.cpp replays them over settings.ini (save_menu_changes).
    std::vector<std::pair<std::string, std::string>> take_changes();
    bool changed() const { return !changes_.empty(); }

    // State, for the tests and the logs.
    const std::vector<SettingGroup> &tabs() const { return tabs_; }
    std::size_t tab() const { return tab_; }
    const std::vector<Item> &items() const { return items_; } // the tab's: the preset first
    std::size_t selected() const { return selected_; }
    std::size_t first_visible() const { return top_; } // of items()[1...]
    const std::string &status() const { return status_; }
    // A confirm row's question is up (MenuActionInfo::confirm), and whether Yes is picked.
    bool confirming() const { return confirming_; }
    bool confirm_yes() const { return confirm_yes_; }
    // Whether a row's value waits for the next start.
    bool waits(const SettingInfo &row) const;
    // The item at screen point (x, y) of the list or the preset line; -1 if none.
    int item_at(float x, float y) const;
    // The tab at (x, y); -1 if none.
    int tab_at(float x, float y) const;
    // Where tab i is drawn (x, width), in screen points.
    std::pair<int, int> tab_box(std::size_t i) const;
    unsigned opens() const { return opens_; }
    // What draw() writes, as text: the tab's note line, item i's label and value, the help of the
    // selected item (unwrapped). dump(): the tab, its note, the visible rows ("> " selected,
    // "label | value"), the help lines as drawn and the status line, one per line.
    std::string note() const;
    std::string row_label(std::size_t i) const;
    std::string row_value(std::size_t i) const;
    std::string help() const;
    std::string dump() const;

  private:
    void build_items();
    void set_tab(std::size_t tab);
    void move(int delta);
    void step(int delta);
    void choose();
    void scroll(int delta);
    void scroll_to_selection();
    void after_change(const Settings &before, const std::string &key, const std::string &value);
    void say(std::string text);
    void cancel_confirm();
    bool hotkey(const SDL_Event &e) const;
    void page(int dir);
    std::optional<ActionView> action_view(const MenuActionInfo *a) const;

    std::span<const SettingInfo> rows_;
    std::vector<std::pair<const MenuActionInfo *, ActionFn>> handlers_;
    std::vector<std::pair<const MenuActionInfo *, ViewFn>> views_;
    std::vector<std::pair<const MenuActionInfo *, QuestionFn>> questions_;
    std::vector<std::pair<SettingGroup, NoteFn>> notes_;
    std::string confirm_question_; // a QuestionFn's question while confirming_
    Actions *actions_;
    Gamepads *gamepads_;
    Input *input_;
    ApplyFn apply_;
    Settings *live_ = nullptr;
    Settings started_;
    std::vector<const SettingInfo *> failed_; // kAtOnce rows the frontend could not apply
    RecentStats frame_cost_;
    bool allowed_ = true;
    bool open_ = false;
    bool dirty_ = false;
    std::vector<SettingGroup> tabs_;
    std::size_t tab_ = 0;
    std::vector<Item> items_;
    std::size_t selected_ = 1, top_ = 0;
    int pressed_what_ = 0, pressed_index_ = -1, pressed_zone_ = 0; // a pointer's press
    std::string status_;
    bool confirming_ = false, confirm_yes_ = false;
    std::vector<std::pair<std::string, std::string>> changes_;
    unsigned opens_ = 0;
    int stick_x_ = 0, stick_y_ = 0; // the left stick as the arrows (an edge, with a margin)
    bool left_trigger_ = false;     // LT held: Up / Down page a list (ADR 0114)
    bool list_open_ = false;
    MenuList list_;
    std::size_t list_from_ = 1; // the row that opened it, selected again when it closes
    GlyphTracker glyphs_;
};

// A mouse or finger event for the open menu: its window point to a screen point through
// `placement` (the window's, as Input maps touches), then SettingsMenu::pointer. Returns true if
// it was a pointer event (taken: the game never gets it while the menu is open); SDL's touches
// made from the mouse, mice made from touches or a pen, and a trackpad's fingers are taken and
// ignored. False for every other event.
bool menu_pointer_event(SettingsMenu &menu, const SDL_Event &e, SDL_Window *window,
                        const Placement &placement);

// The menu's part of the frontend's event routing (game.cpp's handle_event, after the remap
// overlay while that is open): while the menu is open, every key, gamepad and pointer event
// (handle, then menu_pointer_event); while closed, a hotkey that opens it, or (with `gear`, the
// gear's place in drawable pixels, when it shows) a press on the gear. Returns true if the menu
// took the event (the game's devices then never see it).
bool menu_route_event(SettingsMenu &menu, const SDL_Event &e, SDL_Window *window,
                      const Placement &placement, const Rect *gear);

// Replays the menu's changes (SettingsMenu::take_changes) over <dir>/settings.ini as it is now
// and writes it (save_settings): the file gets the keys the player changed in the menu and
// nothing a one-run flag set. False (logged) if it cannot be written.
bool save_menu_changes(const std::string &dir,
                       const std::vector<std::pair<std::string, std::string>> &changes);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SETTINGS_MENU_H
