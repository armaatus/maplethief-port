// Game events (#72, ADR 0053); see maplethief/events.h. The guest facts are from
// docs/re/audio.md (Vibration) and the decompiler: CHero::TakeDamage 0x65B55 checks the hero's
// flags +0x0C for 0x10 (dead) and the damage for > 0 before it vibrates and subtracts it from
// HP +0x94; CHero::LevelUp 0x65089 raises the level +0x80 while it is below 99. Popup_Open
// 0x3EAA1, the revive 0x6140D and CDataMgr::LoadSaveData 0x39989: docs/re/cash-shop.md. The
// run events (#403): clearAll 0x996ED and setClassUp 0x997E1 (docs/re/script-vm.md), CMob::Die
// 0x73349 (docs/re/mechanics.md), CMapleWorld::changeMap 0x81369 (docs/re/audio.md, Map music),
// CStateGame::LoadMap 0x868D5 and its two callers (docs/re/program-flow.md, #424).
#include "maplethief/events.h"

#include <cstdint>

#include "events/guest_read.h"
#include "maplethief/diag.h"
#include "maplethief/recomp/calls.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

// CHero's fields (docs/formats/save.md, the decompiler).
constexpr u32 k_hero_flags = 0x0C;
constexpr u32 k_hero_flag_dead = 0x10;
constexpr u32 k_hero_level = 0x80;
constexpr u32 k_hero_hp = 0x94;
constexpr u32 k_hero_max_hp = 0x9C;
constexpr i32 k_max_level = 99;

mt_game_event_fn g_hook = nullptr;
void *g_user = nullptr;
mt_game_events_stats g_stats{};

using mt_events::rd32;
using mt_events::rdptr;

struct Listener {
    mt_game_event_fn fn = nullptr;
    void *user = nullptr;
};
Listener g_listeners[MT_GAME_EVENTS_MAX_LISTENERS];

void emit(mt_game_event &e, const char *what) {
    mt_tracef("events", "%s: hero 0x%08X value %d %d %d (HP %d / %d)", what,
              static_cast<unsigned>(e.hero), e.value, e.more[0], e.more[1], e.hp, e.max_hp);
    if (g_hook != nullptr) {
        g_hook(g_user, &e);
    }
    for (const Listener &l : g_listeners) {
        if (l.fn != nullptr) {
            l.fn(l.user, &e);
        }
    }
}

// CDataMgr's fields (docs/formats/save.md): the cached theif<slot> buffers +0x4DC (an array of
// 3 pointers, 0 before CDataMgr::LoadSlots).
constexpr u32 k_mgr_slot_buffers = 0x4DC;
constexpr u32 k_death_popup = 0x68;
constexpr i32 k_slots = 3;

// Popup_Open(mgr, id, text, kind): the death popup.
void popup_open(const mt_cpu *cpu) {
    if (cpu->r[1] != k_death_popup) {
        g_stats.ignored++;
        return;
    }
    mt_game_event e{};
    e.kind = MT_GAME_EVENT_DEATH_POPUP;
    e.value = static_cast<i32>(k_death_popup);
    g_stats.death_popups++;
    emit(e, "death popup");
}

// CDataMgr::LoadSaveData(mgr, slot): a new character when the slot has no cached buffer.
void load_save_data(const mt_cpu *cpu) {
    const u32 mgr = cpu->r[0];
    const i32 slot = static_cast<i32>(cpu->r[1]);
    i32 buffers = 0;
    if (slot < 0 || slot >= k_slots || !rd32(cpu, mgr + k_mgr_slot_buffers, &buffers)) {
        g_stats.ignored++;
        return;
    }
    i32 buffer = 0;
    const bool cached =
        buffers != 0 &&
        rd32(cpu, static_cast<u32>(buffers) + 4u * static_cast<u32>(slot), &buffer) && buffer != 0;
    mt_game_event e{};
    e.kind = cached ? MT_GAME_EVENT_LOAD_CHARACTER : MT_GAME_EVENT_NEW_CHARACTER;
    e.value = slot;
    g_stats.loads++;
    emit(e, cached ? "load character" : "new character");
}

// The game state (CStateGame, docs/re/script-vm.md): the hero +0x30, the quest manager +0x58
// (its chapter +0x04), the pending warp's byte +0xB4. The script natives reach it at vm +0x04.
constexpr u32 k_vm_world = 0x04;
constexpr u32 k_world_hero = 0x30;
constexpr u32 k_world_quests = 0x58;
constexpr u32 k_quests_chapter = 0x04;
constexpr u32 k_world_warp_pending = 0xB4;
// CMob's id (+0xA0); the bosses, whose death queues a scene (docs/re/mechanics.md).
constexpr u32 k_mob_id = 0xA0;

bool boss(i32 id) { return id == 64 || (id >= 66 && id <= 69); }

// clearAll(vm): the chapter that ends.
void clear_all(const mt_cpu *cpu) {
    const u32 world = rdptr(cpu, cpu->r[0] + k_vm_world);
    const u32 quests = world != 0 ? rdptr(cpu, world + k_world_quests) : 0;
    mt_game_event e{};
    if (quests == 0 || !rd32(cpu, quests + k_quests_chapter, &e.value)) {
        g_stats.ignored++;
        return;
    }
    e.kind = MT_GAME_EVENT_CHAPTER_END;
    e.hero = rdptr(cpu, world + k_world_hero);
    g_stats.chapter_ends++;
    emit(e, "chapter end");
}

// CHero::SetClassUp(hero, branch, tier).
void set_class_up(const mt_cpu *cpu) {
    i32 flags = 0;
    if (!rd32(cpu, cpu->r[0] + k_hero_flags, &flags)) {
        g_stats.ignored++;
        return;
    }
    mt_game_event e{};
    e.kind = MT_GAME_EVENT_JOB_ADVANCE;
    e.hero = cpu->r[0];
    e.value = static_cast<i32>(cpu->r[1]);
    e.more[0] = static_cast<i32>(cpu->r[2]);
    g_stats.job_advances++;
    emit(e, "job advance");
}

// CMob::Die(mob, hero): bosses only.
void mob_die(const mt_cpu *cpu) {
    mt_game_event e{};
    if (!rd32(cpu, cpu->r[0] + k_mob_id, &e.value) || !boss(e.value)) {
        g_stats.ignored++;
        return;
    }
    e.kind = MT_GAME_EVENT_BOSS_KILLED;
    e.hero = cpu->r[1];
    g_stats.boss_kills++;
    emit(e, "boss killed");
}

// The warp changeMap reported last, until the map load that follows it (load_map).
struct Warp {
    u32 world = 0;
    i32 area = -1;
    i32 map = -1;
};
Warp g_warp;

// CMapleWorld::changeMap(world, area, map, portal, ...): not while a warp is pending.
void change_map(const mt_cpu *cpu) {
    const u32 world = cpu->r[0];
    i32 pending = 0;
    // The byte +0xB4 (little-endian: the low byte of the word read at it).
    if (!rd32(cpu, world + k_world_warp_pending, &pending) ||
        (static_cast<u32>(pending) & 0xFFu) != 0) {
        g_stats.ignored++;
        return;
    }
    mt_game_event e{};
    e.kind = MT_GAME_EVENT_MAP_CHANGE;
    e.hero = rdptr(cpu, world + k_world_hero);
    e.value = static_cast<i32>(cpu->r[1]);
    e.more[0] = static_cast<i32>(cpu->r[2]);
    e.more[1] = static_cast<i32>(cpu->r[3]);
    g_stats.map_changes++;
    g_warp = {world, e.value, e.more[0]};
    emit(e, "map change");
}

// CStateGame::LoadMap(world): every map load. From LoadGame (the bl at 0x87380) it is the game's
// entry, no change; from the loader's mode 1 (CLoaderThread::Load, the bl at 0x6AEE6) it loads
// the target +0x98/+0x9C, which changeMap stored (a warp, reported there) or a scene wrote
// directly (commands 28 and 52, docs/re/qol.md): a map change with no portal.
constexpr u32 k_world_target_area = 0x98;
constexpr u32 k_world_target_map = 0x9C;
constexpr u32 k_lr_from_loader = 0x6AEEB;

void load_map(const mt_cpu *cpu) {
    const u32 world = cpu->r[0];
    const Warp warp = g_warp;
    g_warp = {};
    mt_game_event e{};
    if (cpu->r[14] != k_lr_from_loader || !rd32(cpu, world + k_world_target_area, &e.value) ||
        !rd32(cpu, world + k_world_target_map, &e.more[0]) ||
        (warp.world == world && warp.area == e.value && warp.map == e.more[0])) {
        g_stats.ignored++;
        return;
    }
    e.kind = MT_GAME_EVENT_MAP_CHANGE;
    e.hero = rdptr(cpu, world + k_world_hero);
    e.more[1] = -1;
    g_stats.map_changes++;
    emit(e, "map change (a scene's load)");
}

} // namespace

void mt_observe_entry(mt_cpu *cpu, std::uint32_t fn) noexcept {
    g_stats.entries++;
    switch (fn) {
    case MT_EVENTS_FN_POPUP_OPEN:
        popup_open(cpu);
        return;
    case MT_EVENTS_FN_LOAD_SAVE_DATA:
        load_save_data(cpu);
        return;
    case MT_EVENTS_FN_CLEAR_ALL:
        clear_all(cpu);
        return;
    case MT_EVENTS_FN_SET_CLASS_UP:
        set_class_up(cpu);
        return;
    case MT_EVENTS_FN_MOB_DIE:
        mob_die(cpu);
        return;
    case MT_EVENTS_FN_CHANGE_MAP:
        change_map(cpu);
        return;
    case MT_EVENTS_FN_LOAD_MAP:
        load_map(cpu);
        return;
    default:
        break;
    }
    const u32 hero = cpu->r[0];
    mt_game_event e{};
    e.hero = hero;
    i32 flags = 0;
    if (!rd32(cpu, hero + k_hero_flags, &flags) || !rd32(cpu, hero + k_hero_hp, &e.hp) ||
        !rd32(cpu, hero + k_hero_max_hp, &e.max_hp)) {
        g_stats.ignored++;
        return;
    }
    if (fn == MT_EVENTS_FN_TAKE_DAMAGE) {
        e.kind = MT_GAME_EVENT_HERO_DAMAGE;
        e.value = static_cast<i32>(cpu->r[1]);
        if ((static_cast<u32>(flags) & k_hero_flag_dead) != 0 || e.value <= 0) {
            g_stats.ignored++;
            return;
        }
        g_stats.hero_damage++;
        emit(e, "hero damage");
    } else if (fn == MT_EVENTS_FN_LEVEL_UP) {
        e.kind = MT_GAME_EVENT_LEVEL_UP;
        i32 level = 0;
        if (!rd32(cpu, hero + k_hero_level, &level) || level >= k_max_level) {
            g_stats.ignored++;
            return;
        }
        e.value = level + 1;
        g_stats.level_ups++;
        emit(e, "level-up");
    } else if (fn == MT_EVENTS_FN_REVIVE) {
        e.kind = MT_GAME_EVENT_REVIVED;
        g_stats.revives++;
        emit(e, "revived");
    } else {
        g_stats.ignored++;
    }
}

void mt_game_events_set_hook(mt_game_event_fn hook, void *user) noexcept {
    g_hook = hook;
    g_user = user;
}

int mt_game_events_add_listener(mt_game_event_fn fn, void *user) noexcept {
    if (fn == nullptr) {
        return -1;
    }
    Listener *free_slot = nullptr;
    for (Listener &l : g_listeners) {
        if (l.fn == fn && l.user == user) {
            return 0;
        }
        if (l.fn == nullptr && free_slot == nullptr) {
            free_slot = &l;
        }
    }
    if (free_slot == nullptr) {
        return -1;
    }
    free_slot->fn = fn;
    free_slot->user = user;
    return 0;
}

void mt_game_events_remove_listener(mt_game_event_fn fn, void *user) noexcept {
    for (Listener &l : g_listeners) {
        if (l.fn == fn && l.user == user) {
            l = Listener{};
        }
    }
}

mt_game_events_stats mt_game_events_get_stats(void) noexcept { return g_stats; }

void mt_game_events_reset_stats(void) noexcept { g_stats = mt_game_events_stats{}; }
