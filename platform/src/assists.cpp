// Quality-of-life Assists (#90, ADR 0092): turbo_attack and auto_potion; auto_loot (#175, ADR
// 0092's addendum); see assists.h.
#include "assists.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "actions.h"
#include "maplethief/hooks.h"
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

static std::uint32_t runtime_held(std::uint32_t action) { return mt_input_held(action); }

Assists::Assists(Press press, Held held)
    : press_(press ? std::move(press) : Press(runtime_press)),
      held_(held ? std::move(held) : Held(runtime_held)) {}

void Assists::turn(const GuestRead32 &read, const Settings &settings, const PlayerHolds &holds,
                   std::int32_t panel, std::uint64_t ticks, bool idle, double t) {
    if (!settings.turbo_attack && settings.auto_potion == AutoPotion::kOff && potion_down_ == 0) {
        turbo_active_ = false;
        return;
    }
    const AssistView v = read_assist_view(read);
    potion(v, settings, holds, panel, ticks, idle, t);
    if (potion_down_ == 0) {
        turbo(v, settings.turbo_attack, holds.attack, panel, ticks, idle);
    }
}

void Assists::turn(const mt_cpu *cpu, const Settings &settings, const Actions &actions, double t) {
    const mt_input_stats s = mt_input_get_stats();
    PlayerHolds holds;
    holds.attack = actions.holding(MT_ACTION_ATTACK);
    holds.walk = actions.holding(MT_ACTION_LEFT) || actions.holding(MT_ACTION_RIGHT);
    for (u32 slot = 1; slot <= 4; slot++) {
        holds.slot[slot] = actions.holding(kSlotActions[slot]);
    }
    turn(guest_reader(cpu), settings, holds, mt_input_panel(), s.ticks, s.queued == 0, t);
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

// --- auto_loot (#175) ----------------------------------------------------------------------------

LootBox auto_loot_reach(const LootBox &drop) {
    return {drop.left - kAutoLootReachX, drop.top - kAutoLootReachY, drop.right + kAutoLootReachX,
            drop.bottom + kAutoLootReachY};
}

bool loot_boxes_meet(const LootBox &hero, const LootBox &drop) {
    // 0x6CF01: no when right <= hero.left, bottom <= hero.top, hero.right <= left or
    // hero.bottom <= top.
    return drop.right > hero.left && drop.bottom > hero.top && hero.right > drop.left &&
           hero.bottom > drop.top;
}

bool auto_loot_active(const GuestRead32 &read, std::int32_t panel) {
    if (panel != kFieldPanel) {
        return false;
    }
    const AssistView v = read_assist_view(read);
    return v.field && v.alive;
}

namespace {

struct AutoLoot {
    bool installed = false;
    CpuReader reader = nullptr;
    std::int32_t (*panel)() = nullptr;
    AutoLootStats stats;
};
AutoLoot g_loot;

void store32(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

// CMapleObject_Touches(object r0, left r1, top r2, right r3, bottom [sp]) 0x6CF01.
int on_object_touches(mt_cpu *cpu, const mt_hook_call *call) {
    if (cpu->r[14] != kDropContactReturn) {
        return MT_HOOK_PASS; // the hero's or a mob's touch
    }
    g_loot.stats.tests++;
    const GuestRead32 read = g_loot.reader(cpu);
    // The stack is neither an image segment nor the heap: guest_reader does not read it.
    const u32 sp = cpu->r[13];
    if (cpu->mem == nullptr || sp < 4 || std::uint64_t{sp} + 4 > cpu->mem_size ||
        !auto_loot_active(read, g_loot.panel())) {
        return MT_HOOK_PASS;
    }
    u32 bottom = 0;
    std::memcpy(&bottom, cpu->mem + sp, 4);
    const u32 hero = cpu->r[0];
    const LootBox drop = {as_i32(cpu->r[1]), as_i32(cpu->r[2]), as_i32(cpu->r[3]), as_i32(bottom)};
    const LootBox wide = auto_loot_reach(drop);
    cpu->r[1] = static_cast<u32>(wide.left);
    cpu->r[2] = static_cast<u32>(wide.top);
    cpu->r[3] = static_cast<u32>(wide.right);
    store32(cpu, sp, static_cast<u32>(wide.bottom));
    call->original(cpu);
    store32(cpu, sp, bottom); // the caller's outgoing argument, as it was
    g_loot.stats.widened++;
    if (cpu->r[0] != 0) {
        // The original computed the hero's box (0x6CE5D) at +0x40: would the drop's own rect
        // have met it?
        u32 box[4] = {};
        bool known = true;
        for (u32 i = 0; i < 4; i++) {
            known = known && read(hero + kObjectBox + 4 * i, &box[i]);
        }
        const LootBox h = {as_i32(box[0]), as_i32(box[1]), as_i32(box[2]), as_i32(box[3])};
        if (known && !loot_boxes_meet(h, drop)) {
            g_loot.stats.reached++;
        }
    }
    return MT_HOOK_HANDLED;
}

std::int32_t runtime_panel() { return mt_input_panel(); }

} // namespace

void install_auto_loot(bool on, CpuReader reader, std::int32_t (*panel)()) {
    g_loot.reader = reader != nullptr ? reader : guest_reader;
    g_loot.panel = panel != nullptr ? panel : runtime_panel;
    if (on) {
        // Registering again replaces the handler (and survives a test's mt_hook_reset).
        mt_hook_register(MT_HOOK_FN_OBJECT_TOUCHES, on_object_touches, nullptr, 1);
    } else if (g_loot.installed) {
        mt_hook_unregister(MT_HOOK_FN_OBJECT_TOUCHES);
    }
    if (on != g_loot.installed) {
        SDL_Log("assists: auto_loot %s", on ? "on" : "off");
    }
    g_loot.installed = on;
}

bool auto_loot_installed() { return g_loot.installed; }

const AutoLootStats &auto_loot_stats() { return g_loot.stats; }

} // namespace maplethief::frontend
