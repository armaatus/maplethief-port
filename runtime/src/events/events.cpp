// Game events (#72, ADR 0053); see maplethief/events.h. The guest facts are from
// docs/re/audio.md (Vibration) and the decompiler: CHero::TakeDamage 0x65B55 checks the hero's
// flags +0x0C for 0x10 (dead) and the damage for > 0 before it vibrates and subtracts it from
// HP +0x94; CHero::LevelUp 0x65089 raises the level +0x80 while it is below 99. Popup_Open
// 0x3EAA1, the revive 0x6140D and CDataMgr::LoadSaveData 0x39989: docs/re/cash-shop.md.
#include "maplethief/events.h"

#include <cstdint>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/memory.h"
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

// Whether [addr, addr + 4) is guest memory that is there: an image segment or the committed
// heap (as the input module reads, runtime/src/input/input.cpp). A synthetic guest may have no
// image, and its arena's other pages are not readable.
bool readable(const mt_cpu *cpu, u32 addr) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + 4;
    if (addr == 0 || end > cpu->mem_size) {
        return false;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr) {
        return false;
    }
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (u32 i = 0; segs != nullptr && i < n; i++) {
        if (addr >= segs[i].vmaddr &&
            end <= static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize) {
            return true;
        }
    }
    mt_heap_stats heap{};
    mt_heap_get_stats(mem, &heap);
    return addr >= MT_GUEST_HEAP_BASE && end <= heap.committed;
}

bool rd32(const mt_cpu *cpu, u32 addr, i32 *out) {
    if (!readable(cpu, addr)) {
        return false;
    }
    std::memcpy(out, cpu->mem + addr, 4);
    return true;
}

struct Listener {
    mt_game_event_fn fn = nullptr;
    void *user = nullptr;
};
Listener g_listeners[MT_GAME_EVENTS_MAX_LISTENERS];

void emit(mt_game_event &e, const char *what) {
    mt_tracef("events", "%s: hero 0x%08X value %d (HP %d / %d)", what,
              static_cast<unsigned>(e.hero), e.value, e.hp, e.max_hp);
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

} // namespace

void mt_observe_entry(mt_cpu *cpu, std::uint32_t fn) noexcept {
    g_stats.entries++;
    if (fn == MT_EVENTS_FN_POPUP_OPEN) {
        popup_open(cpu);
        return;
    }
    if (fn == MT_EVENTS_FN_LOAD_SAVE_DATA) {
        load_save_data(cpu);
        return;
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
