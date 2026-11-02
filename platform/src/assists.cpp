// Quality-of-life Assists (#90, ADR 0092): turbo_attack and auto_potion; see assists.h.
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

} // namespace

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

Assists::Assists(Press press) : press_(press ? std::move(press) : Press(runtime_press)) {}

void Assists::turn(const GuestRead32 &read, const Settings &settings, bool attack_held,
                   std::int32_t panel, std::uint64_t ticks, bool idle, double t) {
    const std::uint32_t percent = auto_potion_percent(settings.auto_potion);
    if (!settings.turbo_attack && percent == 0) {
        turbo_active_ = false;
        return;
    }
    const AssistView v = read_assist_view(read);
    turbo(v, settings.turbo_attack, attack_held, panel, ticks, idle);
    potion(v, percent, panel, ticks, t);
}

void Assists::turn(const mt_cpu *cpu, const Settings &settings, const Actions &actions, double t) {
    const mt_input_stats s = mt_input_get_stats();
    turn(guest_reader(cpu), settings, actions.holding(MT_ACTION_ATTACK), mt_input_panel(), s.ticks,
         s.queued == 0, t);
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

void Assists::potion(const AssistView &v, std::uint32_t percent, std::int32_t panel,
                     std::uint64_t ticks, double t) {
    if (percent == 0 || panel != kFieldPanel || !v.field || !v.alive || v.potion_slot == 0 ||
        ticks < potion_after_ || !hp_under(v.hp, v.max_hp, percent)) {
        return;
    }
    const u32 action = kSlotActions[v.potion_slot];
    press_(action, true);
    press_(action, false);
    potion_after_ = ticks + kPotionCooldownTicks;
    stats_.potions++;
    stats_.last_slot = v.potion_slot;
    SDL_Log("assists: auto_potion: HP %d of %d is under %u %%: quick slot %u (item %d, HP +%d, "
            "%d left) at %.3f s",
            v.hp, v.max_hp, percent, v.potion_slot, v.potion_id, v.potion_hp, v.potion_count, t);
}

} // namespace maplethief::frontend
