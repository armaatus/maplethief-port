// Game events (#72, ADR 0053); see maplethief/events.h. The guest facts are from
// docs/re/audio.md (Vibration) and the decompiler: CHero::TakeDamage 0x65B55 checks the hero's
// flags +0x0C for 0x10 (dead) and the damage for > 0 before it vibrates and subtracts it from
// HP +0x94; CHero::LevelUp 0x65089 raises the level +0x80 while it is below 99.
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

void emit(mt_game_event &e, const char *what) {
    mt_tracef("events", "%s: hero 0x%08X value %d (HP %d / %d)", what,
              static_cast<unsigned>(e.hero), e.value, e.hp, e.max_hp);
    if (g_hook != nullptr) {
        g_hook(g_user, &e);
    }
}

} // namespace

void mt_observe_entry(mt_cpu *cpu, std::uint32_t fn) noexcept {
    g_stats.entries++;
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
    } else {
        g_stats.ignored++;
    }
}

void mt_game_events_set_hook(mt_game_event_fn hook, void *user) noexcept {
    g_hook = hook;
    g_user = user;
}

mt_game_events_stats mt_game_events_get_stats(void) noexcept { return g_stats; }

void mt_game_events_reset_stats(void) noexcept { g_stats = mt_game_events_stats{}; }
