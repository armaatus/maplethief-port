// Quality-of-life Assists (#90, ADR 0092): turbo_attack and auto_potion; the controller extras
// (#172, ADR 0094): auto_run and hold_toggle. See assists.h.
#include "assists.h"

#include <SDL3/SDL.h>

#include "actions.h"
#include "maplethief/input.h"

namespace maplethief::frontend {

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 kCanvasState = 0x0C; // SkyCanvas: the engine's current IState
constexpr u32 kStateHero = 0x30;   // CStateGame: the hero
constexpr u32 kStateArea = 0x84;   // CStateGame: the map's area ...
constexpr u32 kStateMap = 0x88;    // ... and map
constexpr u32 kHeroAction = 0x10;  // CHero: 0 dying, 1 dead, 2 stand, ...
constexpr i32 kActionDying = 0, kActionDead = 1;
constexpr u32 kSlotItem = 1;   // a quick slot's kind: an item (2 a skill, 0 the attack)
constexpr u32 kTypePotion = 1; // item_etc.dat type 1
constexpr std::int32_t kFieldPanel = 0;

constexpr u32 kSlotActions[] = {MT_ACTION_NONE, MT_ACTION_SLOT1, MT_ACTION_SLOT2, MT_ACTION_SLOT3,
                                MT_ACTION_SLOT4};

void runtime_press(u32 action, bool down) {
    if (down) {
        mt_input_press(action);
    } else {
        mt_input_release(action);
    }
}

i32 as_i32(u32 v) { return static_cast<i32>(v); }

const char *direction_word(u32 action) { return action == MT_ACTION_LEFT ? "left" : "right"; }

// The presses since `seen` (0 if the count went back: a new Actions).
std::uint64_t since(std::uint64_t now, std::uint64_t seen) { return now >= seen ? now - seen : 0; }

} // namespace

const char *latch_end_name(LatchEnd e) {
    switch (e) {
    case LatchEnd::kNone:
        return "none";
    case LatchEnd::kAgain:
        return "pressed again";
    case LatchEnd::kOther:
        return "the other direction";
    case LatchEnd::kField:
        return "the field left (a menu, a popup, a scene)";
    case LatchEnd::kDeath:
        return "the hero dying";
    case LatchEnd::kMap:
        return "a new map";
    case LatchEnd::kOff:
        return "the settings off";
    case LatchEnd::kOverlay:
        return "a port overlay";
    }
    return "?";
}

bool hp_under(std::int32_t hp, std::int32_t max_hp, std::uint32_t percent) {
    return percent > 0 && hp > 0 && max_hp > 0 &&
           std::int64_t{hp} * 100 < std::int64_t{max_hp} * percent;
}

AssistView read_assist_view(const GuestRead32 &read) {
    AssistView v;
    u32 canvas = 0, state = 0, vptr = 0, mode = 0, popup = 0;
    if (!read(kAutosaveCanvas, &canvas) || canvas == 0 || !read(canvas + kCanvasState, &state) ||
        state == 0 || !read(state, &vptr) || vptr != kAutosaveGameVptr ||
        !read(state + kStateHero, &v.hero) || v.hero == 0 || !read(kAutosaveGameMode, &mode) ||
        !read(kAutosavePopup, &popup)) {
        v.hero = 0;
        return v;
    }
    v.field = mode == 0 && as_i32(popup) == -1;
    u32 area = 0, map = 0;
    if (read(state + kStateArea, &area) && read(state + kStateMap, &map)) {
        v.area = as_i32(area);
        v.map = as_i32(map);
    }
    u32 action = 0, hp = 0, max_hp = 0;
    if (!read(v.hero + kHeroAction, &action) || !read(v.hero + kHeroHp, &hp) ||
        !read(v.hero + kHeroMaxHp, &max_hp)) {
        v.field = false;
        return v;
    }
    v.hp = as_i32(hp);
    v.max_hp = as_i32(max_hp);
    v.alive = as_i32(action) != kActionDying && as_i32(action) != kActionDead && v.hp > 0;
    u32 items = 0;
    if (!read(v.hero + kHeroItems, &items) || items == 0) {
        return v;
    }
    for (u32 slot = 1; slot <= 4 && v.potion_slot == 0; slot++) {
        u32 kind = 0, id = 0;
        if (!read(v.hero + kHeroQuickSlots + 8 * slot, &kind) ||
            !read(v.hero + kHeroQuickSlots + 8 * slot + 4, &id) || kind != kSlotItem) {
            continue;
        }
        for (u32 i = 0; i < kHeroItemCount; i++) {
            u32 item = 0, item_id = 0, type = 0, restores = 0, count = 0;
            if (!read(items + 4 * i, &item) || item == 0 || !read(item + kItemId, &item_id) ||
                item_id != id) {
                continue;
            }
            // KeyAttack uses the first item with the id; so does this.
            if (read(item + kItemType, &type) && type == kTypePotion &&
                read(item + kItemHp, &restores) && as_i32(restores) > 0 &&
                read(item + kItemCount, &count) && as_i32(count) > 0) {
                v.potion_slot = slot;
                v.potion_id = as_i32(id);
                v.potion_hp = as_i32(restores);
                v.potion_count = as_i32(count);
            }
            break;
        }
    }
    return v;
}

static std::uint32_t runtime_held(std::uint32_t action) { return mt_input_held(action); }

Assists::Assists(Press press, Held held)
    : press_(press ? std::move(press) : Press(runtime_press)),
      held_(held ? std::move(held) : Held(runtime_held)) {}

void Assists::turn(const GuestRead32 &read, const Settings &settings, const PlayerHolds &holds,
                   std::int32_t panel, std::uint64_t ticks, bool idle, double t) {
    // The toggles' presses since the last turn (none at the first: earlier ones are not new).
    // Two presses of one action within a turn count as one.
    std::uint64_t left = 0, right = 0, auto_run = 0, ff = 0;
    if (seen_) {
        left = since(holds.left_presses, seen_left_);
        right = since(holds.right_presses, seen_right_);
        auto_run = since(holds.auto_run_presses, seen_auto_run_);
        ff = since(holds.ff_presses, seen_ff_);
    }
    seen_ = true;
    seen_left_ = holds.left_presses;
    seen_right_ = holds.right_presses;
    seen_auto_run_ = holds.auto_run_presses;
    seen_ff_ = holds.ff_presses;
    if (left > 0 || right > 0) {
        const bool both = left > 0 && right > 0;
        last_direction_ = both ? (holds.left ? MT_ACTION_LEFT : MT_ACTION_RIGHT)
                               : (left > 0 ? MT_ACTION_LEFT : MT_ACTION_RIGHT);
    }
    // The fast-forward toggle: the port's clock, in any context.
    if (!hold_toggle_fast_forward(settings.hold_toggle)) {
        ff_latched_ = false;
    } else if (ff > 0) {
        ff_latched_ = !ff_latched_;
        SDL_Log("assists: hold_toggle: fast-forward %s at %.3f s", ff_latched_ ? "on" : "off", t);
    }
    const bool walk_on = settings.auto_run || hold_toggle_walk(settings.hold_toggle);
    if (!settings.turbo_attack && settings.auto_potion == AutoPotion::kOff && potion_down_ == 0 &&
        !walk_on && latch_ == MT_ACTION_NONE) {
        turbo_active_ = false;
        return;
    }
    const AssistView v = read_assist_view(read);
    walk(v, settings, holds, panel, left, right, auto_run, t);
    // A latched walk blocks the slot's press as the player's own walk does.
    PlayerHolds now = holds;
    now.walk = holds.walk || latch_ != MT_ACTION_NONE;
    potion(v, settings, now, panel, ticks, idle, t);
    if (potion_down_ == 0) {
        turbo(v, settings.turbo_attack, holds.attack, panel, ticks, idle);
    }
}

void Assists::turn(const mt_cpu *cpu, const Settings &settings, const Actions &actions, double t) {
    const mt_input_stats s = mt_input_get_stats();
    PlayerHolds holds;
    holds.attack = actions.holding(MT_ACTION_ATTACK);
    holds.left = actions.holding(MT_ACTION_LEFT);
    holds.right = actions.holding(MT_ACTION_RIGHT);
    holds.walk = holds.left || holds.right;
    for (u32 slot = 1; slot <= 4; slot++) {
        holds.slot[slot] = actions.holding(kSlotActions[slot]);
    }
    holds.left_presses = actions.presses(MT_ACTION_LEFT, Context::kField);
    holds.right_presses = actions.presses(MT_ACTION_RIGHT, Context::kField);
    holds.auto_run_presses = actions.presses(MT_ACTION_AUTO_RUN, Context::kField);
    holds.ff_presses = actions.presses(MT_ACTION_FAST_FORWARD, Context::kField) +
                       actions.presses(MT_ACTION_FAST_FORWARD, Context::kMenu);
    turn(guest_reader(cpu), settings, holds, mt_input_panel(), s.ticks, s.queued == 0, t);
}

void Assists::walk(const AssistView &v, const Settings &settings, const PlayerHolds &holds,
                   std::int32_t panel, std::uint64_t left, std::uint64_t right,
                   std::uint64_t auto_run, double t) {
    const bool toggle = hold_toggle_walk(settings.hold_toggle);
    const bool on = settings.auto_run || toggle;
    const bool field = panel == kFieldPanel && v.field;
    if (latch_ != MT_ACTION_NONE) {
        if (!on) {
            unlatch(LatchEnd::kOff, t);
        } else if (!field) {
            unlatch(LatchEnd::kField, t);
        } else if (!v.alive) {
            unlatch(LatchEnd::kDeath, t);
        } else if (v.hero != latch_hero_ || v.area != latch_area_ || v.map != latch_map_) {
            unlatch(LatchEnd::kMap, t);
        }
    }
    if (!on || !field || !v.alive) {
        return; // never a latch in a menu, a popup, a scene or dead
    }
    for (const u32 direction : {MT_ACTION_LEFT, MT_ACTION_RIGHT}) {
        if ((direction == MT_ACTION_LEFT ? left : right) == 0) {
            continue;
        }
        if (latch_ == direction) {
            if (toggle) {
                unlatch(LatchEnd::kAgain, t);
            }
            continue; // auto_run's latch: walking that way anyway
        }
        if (latch_ != MT_ACTION_NONE) {
            unlatch(LatchEnd::kOther, t);
        }
        if (toggle) {
            latch(direction, v, "hold_toggle", t);
        }
    }
    if (auto_run == 0 || !settings.auto_run) {
        return;
    }
    if (latch_ != MT_ACTION_NONE) {
        unlatch(LatchEnd::kAgain, t);
        return;
    }
    // The direction held now (the last pressed if both are), else the last one pressed.
    const auto held = [&holds](u32 d) { return d == MT_ACTION_LEFT ? holds.left : holds.right; };
    u32 direction = last_direction_;
    if (!(direction != MT_ACTION_NONE && held(direction))) {
        direction = holds.right ? MT_ACTION_RIGHT : holds.left ? MT_ACTION_LEFT : last_direction_;
    }
    if (direction == MT_ACTION_NONE) {
        SDL_Log("assists: auto_run: no direction walked yet: nothing to keep going at %.3f s", t);
        return;
    }
    latch(direction, v, "auto_run", t);
}

void Assists::latch(std::uint32_t direction, const AssistView &v, const char *by, double t) {
    press_(direction, true);
    latch_ = direction;
    latch_area_ = v.area;
    latch_map_ = v.map;
    latch_hero_ = v.hero;
    stats_.latches++;
    SDL_Log("assists: %s: walking %s on map %d.%d at %.3f s", by, direction_word(direction), v.area,
            v.map, t);
}

void Assists::unlatch(LatchEnd why, double t) {
    if (latch_ == MT_ACTION_NONE) {
        return;
    }
    press_(latch_, false);
    SDL_Log("assists: the latched walk %s ends: %s at %.3f s", direction_word(latch_),
            latch_end_name(why), t);
    latch_ = MT_ACTION_NONE;
    stats_.latch_ends++;
    stats_.last_end = why;
}

void Assists::drop_latches(double t) {
    unlatch(LatchEnd::kOverlay, t);
    if (ff_latched_) {
        SDL_Log("assists: hold_toggle: fast-forward off (a port overlay) at %.3f s", t);
    }
    ff_latched_ = false;
}

void Assists::turbo(const AssistView &v, bool on, bool attack_held, std::int32_t panel,
                    std::uint64_t ticks, bool idle) {
    const bool active = on && attack_held && panel == kFieldPanel && v.field && v.alive;
    if (!active) {
        turbo_active_ = false;
        return;
    }
    if (!turbo_active_) {
        // The player's own press starts it: the first repeat comes kTurboTicks later.
        turbo_active_ = true;
        turbo_since_ = ticks;
        return;
    }
    if (ticks - turbo_since_ < kTurboTicks || !idle) {
        return;
    }
    // Release, then press: queued together, delivered a tick apart, the hold kept.
    press_(MT_ACTION_ATTACK, false);
    press_(MT_ACTION_ATTACK, true);
    turbo_since_ = ticks;
    stats_.turbo_presses++;
}

void Assists::potion(const AssistView &v, const Settings &settings, const PlayerHolds &holds,
                     std::int32_t panel, std::uint64_t ticks, bool idle, double t) {
    if (potion_down_ != 0) {
        // The tap ends; the player's attack back if turbo repeats it and nothing holds it.
        if (ticks < potion_at_ + kPotionHoldTicks) {
            return;
        }
        press_(kSlotActions[potion_down_], false);
        potion_down_ = 0;
        potion_after_ = ticks + kPotionCooldownTicks;
        if (attack_paused_ && settings.turbo_attack && holds.attack &&
            held_(MT_ACTION_ATTACK) == 0) {
            press_(MT_ACTION_ATTACK, true);
            turbo_since_ = ticks;
        }
        attack_paused_ = false;
        return;
    }
    const std::uint32_t percent = auto_potion_percent(settings.auto_potion);
    if (percent == 0 || panel != kFieldPanel || !v.field || !v.alive || v.potion_slot == 0 ||
        ticks < potion_after_ || !hp_under(v.hp, v.max_hp, percent) || !idle || holds.walk ||
        holds.slot[v.potion_slot]) {
        return;
    }
    if (holds.attack && held_(MT_ACTION_ATTACK) > 0) {
        press_(MT_ACTION_ATTACK, false); // attack would win the tick
        attack_paused_ = true;
    }
    potion_down_ = v.potion_slot;
    potion_at_ = ticks;
    press_(kSlotActions[potion_down_], true);
    stats_.potions++;
    stats_.last_slot = v.potion_slot;
    SDL_Log("assists: auto_potion: HP %d of %d is under %u %%: quick slot %u (item %d, HP +%d, "
            "%d left) at %.3f s",
            v.hp, v.max_hp, percent, v.potion_slot, v.potion_id, v.potion_hp, v.potion_count, t);
}

} // namespace maplethief::frontend
