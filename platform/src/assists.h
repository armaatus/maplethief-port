// Quality-of-life Assists (#90, ADR 0092, docs/re/qol.md): turbo_attack and auto_potion; the
// controller extras (#172, ADR 0094): auto_run and hold_toggle; and auto_loot (#175, at the end
// of this file: a guest hook, not a press). The others are the frontend pressing and releasing
// the game's own actions (maplethief/input.h) the way a player's key would, so the game's rules
// stay its own: a held walk still blocks a press, a dead hero still ignores one, the item is used
// by CHero::KeyAttack 0x66DD9 as from its key.
//
//   turbo_attack  while the player holds attack in the field (Actions::holding(ATTACK), the key
//                 pad's panel 0), the attack is released and pressed again (both queued in one
//                 turn, so the runtime delivers them a tick apart and the hold is never lost)
//                 every kTurboTicks game ticks, while nothing else waits to be delivered: the
//                 game fires an attack once per press (docs/ANALYSIS.md, Input). Never in a
//                 menu or a popup, where attack is OK: there the hold is the player's own.
//   auto_potion   in the field, the hero alive and its HP under the setting's percentage of its
//                 max HP: the quick slot (1-4) that holds an HP potion the hero carries is
//                 pressed for kPotionHoldTicks ticks (a tap), then nothing for
//                 kPotionCooldownTicks. The quick slots are CHero +0xE0 + 8n {kind, id} (kind 1
//                 an item), read the way CHero::KeyAttack reads them; the item is the one of the
//                 48 at CHero +0xD8 whose id (+0x90) it is, of type 1 (a potion, +0x8C) with HP
//                 (+0xC4) to restore and a count (+0x100). The first such slot wins.
//                 SkyCanvas::Tick takes one key a tick, attack first, and a held walk blocks
//                 every press (docs/ANALYSIS.md, Input), so a press then would be lost: it waits
//                 while the player walks or holds that slot. A held attack is let go for the
//                 tap (with turbo_attack on it is pressed again after it; without, a held attack
//                 does nothing more anyway), and turbo does not pulse meanwhile. It also waits
//                 while a walk is latched (below).
//   walk latch    (auto_run, hold_toggle = walk | all) the frontend presses left or right itself
//                 and keeps it pressed: a second finger on the d-pad. The runtime counts holds,
//                 so the player's own press and release of that direction stay theirs. Only
//                 walking latches: the game holds nothing else (CStateGame::DoHeroKeyAction
//                 0x8163D); attack, jump, the quick slots, OK and back fire once per press. A
//                 latch starts only in the field, alive:
//                   auto_run    a press of the port's auto_run action (MT_ACTION_AUTO_RUN) latches
//                               the direction held now (the last pressed of those held), else the
//                               last one pressed in the field; a second press ends it. The
//                               press takes effect when its control is let go (a click): L3
//                               is half of the settings menu's L3 + R3 chord, and a chord
//                               must never latch, whichever stick goes down first.
//                   hold_toggle a press of left or right in the field latches it; a second press
//                   (walk)      of the latched direction ends it.
//                 It ends (LatchEnd) on: auto_run's or the direction's second press; the other
//                 direction (which latches instead with hold_toggle's walk); the field left (the
//                 key pad's panel not 0, game mode not 0, a popup: a menu, a popup, a scene, a map
//                 loading); the hero dying or dead; a new map (area, map or hero changed); both
//                 settings off; a port overlay opening (drop_latches, as it lets go of the keys).
//                 A latched walk blocks the game's other presses as a held one does: attacks and
//                 skills do nothing, auto_potion waits.
//   fast-forward  (hold_toggle = fast_forward | all) a press of the fast_forward action starts it
//   toggle        and the next one stops it (fast_forward_latched(); game.cpp ORs it with the
//                 held key). It is the port's clock, never a press to the game, so it works in
//                 menus as the held key does; a port overlay opening ends it.
//
// "The field" is the engine's CStateGame (vtable 0x119B78) in game mode 0 (0x11AA00) with no
// popup (0x108C64 == -1) and the key pad's panel 0; "alive" is CHero +0x10 not 0 (dying) or 1
// (dead) and HP above 0. Every read goes through a GuestRead32 (autosave.h), so a synthetic
// guest tests it.
//
// Time is game ticks (mt_input_get_stats().ticks: CMTimer's lastFrame changing), so game_speed
// and fast-forward scale it as they scale the game. Single thread: the turn hook's.
#ifndef MAPLETHIEF_PLATFORM_ASSISTS_H
#define MAPLETHIEF_PLATFORM_ASSISTS_H

#include <cstdint>
#include <functional>

#include "autosave.h"
#include "settings.h"

namespace maplethief::frontend {

class Actions;

// Guest facts (docs/re/qol.md).
inline constexpr std::uint32_t kHeroHp = 0x94;         // CHero: HP
inline constexpr std::uint32_t kHeroMaxHp = 0x9C;      // CHero: max HP
inline constexpr std::uint32_t kHeroItems = 0xD8;      // CHero: 48 item pointers (consumables)
inline constexpr std::uint32_t kHeroQuickSlots = 0xE0; // CHero: 5 x {kind, id}, slot 0 attack
inline constexpr std::uint32_t kItemType = 0x8C;       // item: item_etc.dat type, 1 a potion
inline constexpr std::uint32_t kItemId = 0x90;         // item: id
inline constexpr std::uint32_t kItemHp = 0xC4;         // item: HP restored (Elixirs: percent)
inline constexpr std::uint32_t kItemCount = 0x100;     // item: how many
inline constexpr std::uint32_t kHeroItemCount = 48;

inline constexpr std::uint64_t kTurboTicks = 6;           // a new press every 6 ticks
inline constexpr std::uint64_t kPotionHoldTicks = 2;      // the tap: 0.1 s at 50 ms
inline constexpr std::uint64_t kPotionCooldownTicks = 20; // 1 s at the game's 50 ms

// What the player holds now (Actions::holding): the field actions that decide whether a press
// of the Assists would reach the game; and the presses so far (Actions::presses) that the
// toggles count.
struct PlayerHolds {
    bool attack = false;
    bool walk = false; // left or right
    bool slot[5] = {}; // quick slots 1-4 (0 unused)
    bool left = false, right = false;
    // The auto_run action's control is down (L3, R): its press waits for the release (a click),
    // so L3 + R3 (the settings menu's chord) never latches a walk.
    bool auto_run = false;
    // Presses so far: left, right and auto_run in the field; fast_forward anywhere.
    std::uint64_t left_presses = 0, right_presses = 0, auto_run_presses = 0, ff_presses = 0;
};

// What `actions` holds now and its presses so far, as Assists::turn reads them.
PlayerHolds player_holds(const Actions &actions);

// Why a walk latch ended (Assists::Stats::last_end).
enum class LatchEnd { kNone, kAgain, kOther, kField, kDeath, kMap, kOff, kOverlay };
const char *latch_end_name(LatchEnd e);

// What the assists read of the game in one turn.
struct AssistView {
    bool field = false; // CStateGame, mode 0, no popup (the panel is the caller's)
    bool alive = false; // action not dying or dead, HP > 0
    std::uint32_t hero = 0;
    std::int32_t area = -1, map = -1; // CStateGame +0x84, +0x88
    std::int32_t hp = 0, max_hp = 0;
    // The first quick slot (1-4) holding an HP potion the hero has, or 0; its item.
    std::uint32_t potion_slot = 0;
    std::int32_t potion_id = -1, potion_hp = 0, potion_count = 0;
};
AssistView read_assist_view(const GuestRead32 &read);

// Whether `hp` of `max_hp` is under `percent` (0: never).
bool hp_under(std::int32_t hp, std::int32_t max_hp, std::uint32_t percent);

class Assists {
  public:
    // Where presses go: the runtime's mt_input_press / mt_input_release by default; and how
    // many presses of an action the runtime holds (mt_input_held).
    using Press = std::function<void(std::uint32_t action, bool down)>;
    using Held = std::function<std::uint32_t(std::uint32_t action)>;
    explicit Assists(Press press = nullptr, Held held = nullptr);

    // One run-loop turn. `holds`: what the player holds (Actions::holding), `panel` the key
    // pad's panel (mt_input_panel), `ticks` the game ticks so far, `idle`: no action waits to
    // be delivered (mt_input_get_stats().queued == 0), `t` the run's seconds (for the log).
    void turn(const GuestRead32 &read, const Settings &settings, const PlayerHolds &holds,
              std::int32_t panel, std::uint64_t ticks, bool idle, double t);
    // The same on the game: guest memory, Actions, mt_input_panel and mt_input_get_stats.
    void turn(const mt_cpu *cpu, const Settings &settings, const Actions &actions, double t);

    // The direction latched now (MT_ACTION_LEFT or MT_ACTION_RIGHT), or MT_ACTION_NONE.
    std::uint32_t walk_latch() const { return latch_; }
    // Whether hold_toggle has fast-forward on without its key held.
    bool fast_forward_latched() const { return ff_latched_; }
    // A port overlay opens (it lets go of every key): every latch ends, and the toggles' presses
    // not yet acted on are forgotten (L3 + R3 opening the settings menu: the L3 or R3 press that
    // began the chord neither latches auto_run nor toggles fast-forward).
    void drop_latches(double t);
    // A save state is restored (#203, save_state_input.h): the latches end as for an overlay
    // (their releases go out now, before the restore replaces the runtime's input), and the
    // turbo, potion and walk timing starts over. The press counts already seen stay seen.
    void restart(double t);

    struct Stats {
        std::uint64_t turbo_presses = 0; // release-and-press pairs turbo queued
        std::uint64_t potions = 0;       // quick-slot presses for a potion
        std::uint32_t last_slot = 0;     // the last one's slot
        std::uint64_t latches = 0;       // walk latches started
        std::uint64_t latch_ends = 0;    // and ended
        LatchEnd last_end = LatchEnd::kNone;
    };
    const Stats &stats() const { return stats_; }

  private:
    void turbo(const AssistView &v, bool on, bool attack_held, std::int32_t panel,
               std::uint64_t ticks, bool idle);
    void potion(const AssistView &v, const Settings &settings, const PlayerHolds &holds,
                std::int32_t panel, std::uint64_t ticks, bool idle, double t);
    // The walk latch's turn: its ends, then this turn's presses (the counts since the last).
    void walk(const AssistView &v, const Settings &settings, const PlayerHolds &holds,
              std::int32_t panel, std::uint64_t left, std::uint64_t right, std::uint64_t auto_run,
              double t);
    void latch(std::uint32_t direction, const AssistView &v, const char *by, double t);
    void unlatch(LatchEnd why, double t);

    Press press_;
    Held held_;
    Stats stats_;
    // Turbo: whether it was pulsing at the last turn, and the tick of the last press.
    bool turbo_active_ = false;
    std::uint64_t turbo_since_ = 0;
    // Auto-potion: no new press before this tick; the slot pressed now (0: none), since when,
    // and whether the player's attack was let go for it.
    std::uint64_t potion_after_ = 0;
    std::uint32_t potion_down_ = 0;
    std::uint64_t potion_at_ = 0;
    bool attack_paused_ = false;
    // The walk latch: the direction pressed (NONE: none), and the map and hero it began on; the
    // last direction pressed in the field; the fast-forward toggle.
    std::uint32_t latch_ = 0;
    std::int32_t latch_area_ = -1, latch_map_ = -1;
    std::uint32_t latch_hero_ = 0;
    std::uint32_t last_direction_ = 0;
    bool ff_latched_ = false;
    // An auto_run press waiting for its control to be let go; drop_latches forgets presses.
    bool auto_run_pending_ = false;
    bool forget_presses_ = false;
    // The press counts seen at the last turn (PlayerHolds); none before the first.
    bool seen_ = false;
    std::uint64_t seen_left_ = 0, seen_right_ = 0, seen_auto_run_ = 0, seen_ff_ = 0;
};

// --- auto_loot (#175, docs/re/qol.md#auto-loot) -------------------------------------------------
//
// The game picks a drop up on contact: CDropItems::Update 0x5BC75 asks, for each lying drop
// (state +0x10 = 0) whose box 0x6CE5D can compute, CMapleObject_Touches 0x6CF01 (hero, left,
// top, right, bottom at sp) whether the hero's box meets the drop's. Everything after a yes is
// the game's: meso always, an item if CHero::isAddInven 0x6003D finds room (else the drop is
// marked refused, +0x10C, and stays), then the drop flies to the hero and CHero::addInven
// 0x64309 takes it (stacks, the quest check, sound 0x1D).
//
// auto_loot hooks 0x6CF01 (ADR 0069) and, for the drop update's one call (the return address
// kDropContactReturn) in the field (read_assist_view's: CStateGame, mode 0, no popup; the key
// pad's panel 0) with the hero alive, widens the drop's rect by kAutoLootReach* on each side
// before the original test runs. Every other caller of 0x6CF01 (the hero's and mobs' touches)
// and every call outside the field passes. The stack argument (bottom) is restored after the
// call: the caller's frame is as the original leaves it.
inline constexpr std::uint32_t kDropContactReturn = 0x5BCF3; // after the bl at 0x5BCEE
inline constexpr std::int32_t kAutoLootReachX = 160;         // a third of the 480-point screen
inline constexpr std::int32_t kAutoLootReachY = 32;          // two map tiles up and down
inline constexpr std::uint32_t kObjectBox = 0x40;            // object: left, top, right, bottom

// A box as 0x6CE5D writes it and 0x6CF01 tests it: [left, right) x [top, bottom).
struct LootBox {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};
// The drop's rect widened by the reach.
LootBox auto_loot_reach(const LootBox &drop);
// Whether two boxes meet as 0x6CF01 decides it.
bool loot_boxes_meet(const LootBox &hero, const LootBox &drop);
// Whether auto_loot acts now: the field, the hero alive, the key pad's panel 0.
bool auto_loot_active(const GuestRead32 &read, std::int32_t panel);

struct AutoLootStats {
    std::uint64_t tests = 0;   // the drop update's contact tests seen (on)
    std::uint64_t widened = 0; // tests run with the wider rect (in the field)
    std::uint64_t reached = 0; // yes with the wider rect where the game's own rect said no
};

// Registers (on) or removes (off) the hook. Off: no handler, the original test runs. `reader`
// and `panel` replace guest_reader and mt_input_panel (tests).
using CpuReader = GuestRead32 (*)(const mt_cpu *cpu);
void install_auto_loot(bool on, CpuReader reader = nullptr, std::int32_t (*panel)() = nullptr);
bool auto_loot_installed();
const AutoLootStats &auto_loot_stats();

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_ASSISTS_H
