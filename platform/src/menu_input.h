// Menu comforts for controllers and keys (#147, ADR 0051's addendum): held directions repeat on
// the key pad's menu panels, and a popup that just opened can't be answered by a button mashed
// in the field.
//
//   Actions (actions.h)   a device control -> an action, by context
//     v
//   Focus (focus.h)       the UIKit screens' focus (panel -1 and 9); what it passes on:
//     v
//   MenuInput::action     Focus's forward sink: the repeat and the guard, then on to the
//     |                   runtime (mt_input_press / mt_input_release), which delivers one step
//     |                   per game tick (ADR 0048)
//   MenuInput::turn       once per run-loop turn: the popup seen, the repeat due
//
// Repeat (menu_repeat). The game's popup menu (panel 1), NPC chat (5), shop (6) and map (8) move
// their cursor once per key press, and the on-screen key pad never repeats. A direction pressed
// on one of them repeats after kRepeatDelay, then every kRepeatEvery: a release and a press of
// that action, so the game sees another key press. Only the last direction pressed repeats. A
// repeat waits while the runtime still has events queued, so repeats never pile up and the tick
// still gets one key at a time. It stops on the release, when the panel changes (the shop closed,
// a popup opened over it) and when the setting goes off. The UIKit screens and the cash shop
// repeat in Focus (Focus::kRepeatDelay, the same timing).
//
// The quantity popup (ADR 0114). A popup over a menu panel takes the key pad's panel 7; popup 5
// ("how many", the shop's amount: left / right +-1, up / down +-10) is the one popup whose
// directions step a value, so with menu_repeat its held directions repeat too, at the same
// timing. The repeat stops when the popup changes.
//
// Paging (ADR 0114). While the port's page action is held (Actions::holding(MT_ACTION_PAGE): LT
// in menus, Shift on the keyboard), a press of up or down where a list that pages owns the
// focus (the PageSink; list_keys.h's list_page: the shop's list, the popup menu's item and
// equipment grids) never reaches the game: its press and release are dropped, and a page is
// queued, applied at the first turn the runtime has nothing queued (after the keys pressed
// before it). With menu_repeat a held page repeats like a direction. Anywhere else the page
// action changes nothing: the direction goes on.
//
// Guard (confirm_guard). In the field, A is jump and the keyboard's X attack; on a popup they are
// OK and back, so a button mashed in the field confirms or cancels a popup that opens under it
// (the death popup: Return to town, or a revive; a notice). The open popup is the game's popup
// id (0x108C64, -1 none; Popup_Open 0x3EAA1 sets it, Popup_Close 0x3E945 resets it), read at
// every turn and at every press; a change to another id is a popup opening. For kGuardTime after
// that, and while a control that went down in the field and confirms, cancels or revives in menus
// is still held (Actions::held_from_field), a press of ok, back, revive or grade revive is
// dropped: the press and its release. A release whose press went to the game goes on.
//
// Both are off by default (the original). Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_MENU_INPUT_H
#define MAPLETHIEF_PLATFORM_MENU_INPUT_H

#include <cstdint>
#include <vector>

#include "actions.h"
#include "maplethief/input.h"
#include "maplethief/recomp/recomp.h"

namespace maplethief::frontend {

// Whether held directions repeat on key pad panel `panel` (mt_input_panel): 1, 5, 6 and 8.
bool menu_repeat_panel(std::int32_t panel);
// The quantity popup's id ("how many", the shop's amount).
inline constexpr std::int32_t kQuantityPopup = 5;
// Whether held directions repeat on panel `panel` with popup `popup` open (-1: none): the menu
// panels, and panel 7 over the quantity popup.
bool menu_repeat_at(std::int32_t panel, std::int32_t popup);
// Whether the guard drops `action` on a popup that just opened: ok, back, revive, grade revive.
bool confirm_guarded(std::uint32_t action);
// The game's open popup (0x108C64), -1 for none. -1 without a cpu, and in a guest address space
// (mt_mem) whose mapped segments do not hold 0x108C64 (no game image: nothing is read).
std::int32_t menu_popup_id(const mt_cpu *cpu);

// The open popup's id (-1: none). The default is menu_popup_id.
using PopupSource = std::int32_t (*)(void *user, mt_cpu *cpu);
// Seconds now. The default is mt_cf_absolute_time (the run loop's clock).
using ClockSource = double (*)(void *user);
// Whether the runtime still has actions to deliver. The default reads mt_input_get_stats().
using BusySource = bool (*)(void *user);
// Paging: with `apply` false, whether a list that pages owns the focus on `panel`; with it
// true, the page itself (dir -1 up, +1 down). The default is list_page (list_keys.h).
using PageSink = bool (*)(void *user, mt_cpu *cpu, std::int32_t panel, int dir, bool apply);

class MenuInput {
  public:
    static constexpr double kRepeatDelay = 0.40;
    static constexpr double kRepeatEvery = 0.15;
    static constexpr double kGuardTime = 0.30;

    MenuInput();

    // Focus's forward sink: focus.set_forward(MenuInput::sink, &menu_input).
    static void sink(void *user, std::uint32_t action, bool down);

    void set_repeat(bool on);
    void set_guard(bool on);
    bool repeat() const { return repeat_; }
    bool guard() const { return guard_; }
    // The bindings' held controls (the guard's "still held from the field"); nullptr: none.
    void set_actions(const Actions *actions) { actions_ = actions; }
    void set_forward(ActionSink sink, void *user);
    void set_panel_source(PanelSource source, void *user);
    void set_popup_source(PopupSource source, void *user);
    void set_clock(ClockSource source, void *user);
    void set_busy_source(BusySource source, void *user);
    void set_page_sink(PageSink sink, void *user);

    // An action on its way to the game.
    void action(std::uint32_t action, bool down);
    // Once per run-loop turn at time `now` (seconds).
    void turn(mt_cpu *cpu, double now);
    // A save state is restored (#203, save_state_input.h), after every control was let go: the
    // repeat, the pages, the guard and the presses it still waited to see released are
    // forgotten (the restore replaces the runtime's input).
    void restart();

    // The popup seen last (-1: none), and whether the guard holds now.
    std::int32_t popup() const { return popup_; }
    bool guarding(double now) const;

    struct Stats {
        std::uint64_t repeats = 0;   // repeated direction presses
        std::uint64_t waits = 0;     // turns a due repeat waited for the runtime's queue
        std::uint64_t popups = 0;    // popups seen opening
        std::uint64_t guarded = 0;   // presses dropped by the guard
        std::uint64_t held = 0;      // of them, past kGuardTime: a field button still held
        std::uint64_t forwarded = 0; // actions passed on
        std::uint64_t pages = 0;     // pages applied (repeats included)
        std::uint64_t page_repeats = 0;
    };
    const Stats &stats() const { return stats_; }

  private:
    void forward(std::uint32_t action, bool down);
    void observe(double now);
    void flush_pages();

    bool repeat_ = false;
    bool guard_ = false;
    const Actions *actions_ = nullptr;
    ActionSink forward_ = nullptr;
    void *forward_user_ = nullptr;
    PanelSource panel_ = nullptr;
    void *panel_user_ = nullptr;
    PopupSource popup_source_ = nullptr;
    void *popup_user_ = nullptr;
    ClockSource clock_ = nullptr;
    void *clock_user_ = nullptr;
    BusySource busy_ = nullptr;
    void *busy_user_ = nullptr;
    PageSink page_ = nullptr;
    void *page_user_ = nullptr;
    mt_cpu *cpu_ = nullptr; // the last turn's

    std::uint32_t held_[4] = {}; // the presses of each direction that went to the game
    int repeat_dir_ = -1;        // the direction that repeats, or -1
    std::int32_t repeat_panel_ = -1;
    std::int32_t repeat_popup_ = -1; // panel 7: the popup it repeats on
    bool repeat_page_ = false;       // the repeat is a page
    std::uint32_t paged_[4] = {};    // direction presses taken as pages, their releases to come
    std::vector<int> pages_;         // pages to apply (-1 up, +1 down), in order
    std::int32_t pages_panel_ = -1;  // the panel they were asked on
    double repeat_at_ = 0;
    std::int32_t popup_ = -1;                     // the popup seen last
    double opened_at_ = -1e9;                     // when it opened
    std::uint32_t dropped_[MT_ACTION_COUNT] = {}; // presses dropped, their releases to come
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_MENU_INPUT_H
