// Controller navigation for the UIKit screens (#69, ADR 0051): a focus that the directions move
// over the controls a tap can reach, confirm taps, back taps the screen's close / back / No
// button, and lists scroll by a drag.
//
//   Actions (actions.h)   a device control -> an action, by context (menus: up, down, left,
//     |                   right, ok, back, ...), whatever the device (a gamepad, #68's keys)
//     v
//   Focus::action         the actions' sink. While the focus is active the actions are the
//     |                   focus's; otherwise they go on to the runtime (mt_input_press /
//     |                   mt_input_release), as before. A release goes where its press went.
//     v
//   Focus::turn           once per run-loop turn: the reachable controls (mt_uikit_controls,
//                         the runtime's view of its UIKit tree), the focus kept on them, the
//                         queued actions applied as touches through Input::touch (virtual
//                         fingers), the held direction repeated.
//
// Active: on the screens the game drives through UIKit buttons that have no key code: when
// the key pad shows no panel (mt_input_panel() -1: the main menu, the terms, the character
// select and their alerts, Help, About) or the cash shop's (panel 9, its eight tiles), and at
// least one control is reachable. Every other panel has key codes for its buttons (#67), so
// the actions go to the game there. While active the focus takes every action: one that it has
// no use for (map, plus, ...) is dropped, so no stray key code reaches a game state that the
// UIKit screen drives (on the character select, '*' is Delete).
//
// Confirm is a touch: down on the press at the focused control's tap point, up on the release
// (at least one turn later), the same UITouch path as a finger, so the control highlights,
// sends its touch-down and touch-up-inside actions and plays its sounds as on a tap. Back is
// the same touch on the screen's back control (back_rank: close, back, No, cancel); with none
// on the screen it does nothing. A direction moves the focus to the nearest control that way
// (focus_next); with none, a scroll view on the screen that can go that way is dragged (a page
// of a paging one, 60 % of its height or width otherwise) through a touch, so that more
// controls come into reach. A held direction repeats after kRepeatDelay, every kRepeatEvery.
//
// The focus: kept on its control while that stays reachable. A control that loses the focus by
// going out of reach (a screen opened over it) gets it back when it comes back (the screen
// closed), unless something newer with a default appeared in front of it. When controls appear
// that were not reachable before (a screen opens, an alert pops up), the focus goes to the best
// of them by default_rank (Play, the terms' agreement, the first slot, an alert's Yes or OK),
// else, if its control went away, to the first new one in reading order. With nothing new and
// its control gone, it goes to the nearest one.
//
// The popup menu (panel 1): its tabs' contents are GL touch widgets that only a touch on the
// tab enables (TouchWidget_OnEvent 0xA0EAD, case 1); the key that opens a tab (OK, Menu_OnKey)
// leaves its list empty. So OK there, with no tab open (CDataMgr +0xA8 0) on a tab that has
// contents (+0xB0 1-5: item, equip, skill, quest, option), is a touch on that tab
// (popup_menu_tab_point); every other key of the popup menu goes to the game as before, and
// inside an open tab the keys work (the cursor, OK uses or equips, back closes the tab).
//
// The highlight (focus_highlight) is drawn by the presenter over the window, after the screen
// image: the game's frame, the shots (--shot) and the device goldens never contain it. It shows
// while the focus is active and has been used (an action since it became active).
//
// Single thread: the frontend's (the guest's main thread, between run-loop turns).
#ifndef MAPLETHIEF_PLATFORM_FOCUS_H
#define MAPLETHIEF_PLATFORM_FOCUS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "actions.h"
#include "input.h"
#include "maplethief/recomp/recomp.h"
#include "scaling.h"

namespace maplethief::frontend {

// A reachable control or scroll view (maplethief/uikit.h mt_uikit_control).
struct FocusTarget {
    enum class Kind { kButton, kControl, kScroll };
    std::uint32_t object = 0;
    Kind kind = Kind::kButton;
    std::uint32_t order = 0;
    float x = 0, y = 0, w = 0, h = 0; // screen points: the part a touch can hit
    float px = 0, py = 0;             // where a touch reaches it
    std::string action;               // the selector a tap sends ("" for a scroll view)
    std::string target;               // the class of its target
    std::string view_class;
    float offset[2] = {0, 0}, max_offset[2] = {0, 0}, page[2] = {0, 0}; // scroll views

    bool focusable() const { return kind != Kind::kScroll; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
};

// The control to go to from targets[from] in direction `d`, or -1. Only focusable targets
// count. A candidate lies that way: its centre at least 1 point past the focused centre on the
// direction's axis and its near edge not behind the focused near edge. The score is the
// distance on the axis plus twice the distance across it (0 while the two overlap across), so
// a control in line wins over a nearer one off to the side; ties go to the earlier target.
int focus_next(const std::vector<FocusTarget> &targets, int from, Direction d);
// The scroll view to drag for direction `d` when no control lies that way, or -1: the
// innermost one (the smallest) that holds the focused tap point and can still move that way
// (its content offset not at the end), else the largest one on the screen that can.
int focus_scroll(const std::vector<FocusTarget> &targets, int from, Direction d);
// The drag that scrolls targets[i] for `d`: from (x0, y0) to (x1, y1), screen points.
void focus_scroll_drag(const FocusTarget &t, Direction d, float *x0, float *y0, float *x1,
                       float *y1);

// Back on this screen: the rank of a control's action as its back button (lower is better;
// -1: not a back button). Data: the game's close, back, No and cancel selectors.
int back_rank(const std::string &action);
// The control the focus prefers when it appears (lower is better; -1: none).
int default_rank(const std::string &action);
// Whether the focus is active on panel `panel` (mt_input_panel): -1 and 9.
bool focus_panel(std::int32_t panel);

// The popup menu's tab `tab` (CDataMgr +0xB0: 0 status ... 5 option): where a touch opens it,
// in screen points (the tab bar widget 0xF14A0: six tabs of 54 points from x 75, y 260-295).
// False for a tab without contents to enable (0, status) or out of range.
bool popup_menu_tab_point(std::int32_t tab, float *x, float *y);

// The highlight of a focused control at screen rect (x, y, w, h) in drawable pixels: the rect
// through the placement (scaling.h's screen_to_drawable), grown by `outset` drawable pixels on
// every side and clipped to the placement's image. Empty (all 0) without a placement.
Rect focus_highlight(const Placement &placement, float x, float y, float w, float h, float outset);
// The highlight's line width in drawable pixels for a placement: 2 screen points, at least 2.
float focus_highlight_width(const Placement &placement);

// The reachable controls now. The default reads mt_uikit_controls(cpu).
using TargetSource = void (*)(void *user, mt_cpu *cpu, std::vector<FocusTarget> *out);
// The popup menu's state: its tab (CDataMgr +0xB0) and open tab (+0xA8, 0: none). False when
// the game has no CDataMgr yet. The default reads guest memory (CDataMgr at 0x11A984).
using MenuSource = bool (*)(void *user, mt_cpu *cpu, std::int32_t *tab, std::int32_t *open);

class Focus {
  public:
    // Seconds a held direction waits before it repeats, and between repeats.
    static constexpr double kRepeatDelay = 0.40;
    static constexpr double kRepeatEvery = 0.15;
    // The turns a scroll drag takes (moves).
    static constexpr int kDragMoves = 8;

    explicit Focus(Input *input);

    // Makes this the sink of `actions` (the actions it does not take go on to the runtime, or
    // to set_forward's sink).
    void attach(Actions &actions);
    void set_forward(ActionSink sink, void *user);
    void set_panel_source(PanelSource source, void *user);
    void set_target_source(TargetSource source, void *user);
    void set_menu_source(MenuSource source, void *user);

    // An action from the bindings (Actions' sink).
    void action(std::uint32_t action, bool down);
    // Once per run-loop turn at time `now` (seconds): refresh, focus, the queued actions, the
    // touches it is making, the repeat.
    void turn(mt_cpu *cpu, double now);

    bool active() const { return active_; }
    // The focused target, or nullptr.
    const FocusTarget *focused() const;
    // The target to highlight: the focused one while active and used; else nullptr.
    const FocusTarget *highlighted() const;
    const std::vector<FocusTarget> &targets() const { return targets_; }

    struct Stats {
        std::uint64_t taps = 0;    // confirm touches
        std::uint64_t backs = 0;   // back touches
        std::uint64_t moves = 0;   // focus moves by a direction
        std::uint64_t scrolls = 0; // scroll drags
        std::uint64_t dropped = 0; // actions taken while active and not used
        std::uint64_t forwarded = 0;
        std::uint64_t menu_tabs = 0; // popup menu tabs opened by a touch
    };
    const Stats &stats() const { return stats_; }

  private:
    struct Event {
        std::uint32_t action;
        bool down;
    };
    struct Touch {
        bool live = false;
        std::uint32_t id = 0;
        float x = 0, y = 0;
        bool release = false;   // its release came: end it at the next turn
        std::uint64_t turn = 0; // the turn it began
    };
    struct Drag {
        bool live = false;
        std::uint32_t id = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int step = 0;
    };

    void refresh(mt_cpu *cpu);
    void apply(const Event &e, double now);
    void menu_ok(mt_cpu *cpu, const Event &e);
    bool move(Direction d);
    void begin_touch(Touch &t, std::uint32_t id, float x, float y, const char *what);
    void end_touch(Touch &t);
    int index_of(std::uint32_t object) const;

    Input *input_;
    ActionSink forward_ = nullptr;
    void *forward_user_ = nullptr;
    PanelSource panel_ = nullptr;
    void *panel_user_ = nullptr;
    TargetSource source_ = nullptr;
    void *source_user_ = nullptr;
    MenuSource menu_ = nullptr;
    void *menu_user_ = nullptr;

    bool active_ = false;
    bool used_ = false;
    std::vector<FocusTarget> targets_;
    std::uint32_t focus_ = 0;         // the focused object, 0: none
    float focus_x_ = 0, focus_y_ = 0; // its last centre (for the nearest when it goes)
    // Controls that had the focus when they stopped being reachable, oldest first.
    static constexpr std::size_t kRemembered = 8;
    std::vector<std::uint32_t> remembered_;
    std::vector<Event> queue_;
    std::uint32_t taken_[64] = {}; // presses of each action the focus took (their releases too)
    // OK in the popup menu: decided at the next turn (a tab touch or the key).
    std::vector<Event> menu_queue_;
    std::uint32_t menu_taken_ = 0;     // presses taken for it, not yet released
    std::uint32_t menu_forwarded_ = 0; // of them, those that went on to the game as the key
    Touch confirm_, back_;
    Drag drag_;
    std::uint32_t next_drag_ = 0;
    int held_dir_ = -1; // the direction held for repeat, or -1
    double repeat_at_ = 0;
    std::uint64_t turns_ = 0;
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_FOCUS_H
