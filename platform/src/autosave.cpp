// Autosave on quit and in the background (#150); see autosave.h and docs/re/autosave.md.
#include "autosave.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/objc/objc.h"

namespace maplethief::frontend {

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

// The offsets the guards read (docs/re/autosave.md).
constexpr u32 kCanvasState = 0x0C;   // SkyCanvas: the engine's current IState
constexpr u32 kStateHero = 0x30;     // CStateGame: the hero
constexpr u32 kStateArea = 0x84;     // CStateGame: the map's area ...
constexpr u32 kStateMap = 0x88;      // ... and map
constexpr u32 kStateWarp = 0xB4;     // CStateGame (CMapleWorld::changeMap): a warp is pending
constexpr u32 kHeroFlags = 0x0C;     // CHero: bit 1 in the air, 2 on a ladder, 3 attacking
constexpr u32 kHeroAction = 0x10;    // CHero: 0 dying, 1 dead, 2 stand, 3 walk, 4 climb, ...
constexpr u32 kHeroKnockBack = 0x119; // CHero: thrown back by a hit (CHero::TakeDamage)
constexpr u32 kMgrScroll = 0x108;    // CDataMgr: a scroll is being applied
constexpr u32 kMgrSlot = 0x4E4;      // CDataMgr: the slot in play

constexpr u32 kHeroAirborne = 0x2, kHeroAttacking = 0x8;
constexpr i32 kActionDying = 0, kActionDead = 1, kActionStand = 2, kActionWalk = 3,
              kActionClimb = 4;

// The game's order: 0x54BF9's compare chain (0x8D4C9's for -10 and the cash shop's -26 are the
// same). Kinds from the maps themselves (docs/re/autosave.md).
constexpr NoSaveMap kNoSaveMaps[] = {
    {2, 8, "tutorial"},          {2, 9, "tutorial"},          {3, 8, "boss"},
    {4, 17, "boss"},             {7, 4, "boss"},              {9, 5, "boss"},
    {10, 5, "boss"},             {2, 6, "climb"},             {2, 7, "climb"},
    {6, 1, "climb"},             {6, 2, "climb"},             {9, 9, "climb"},
    {9, 10, "climb"},            {0, 5, "Monster Carnival"},  {4, 18, "Monster Carnival"},
    {6, 3, "Monster Carnival"},
};

bool read_byte(const GuestRead32 &read, u32 addr, u32 *out) {
    u32 v = 0;
    if (!read(addr, &v)) {
        return false;
    }
    *out = v & 0xFFu;
    return true;
}

// Whether [addr, addr + 4) is game memory: an image segment or the committed heap.
bool readable(const mt_cpu *cpu, u32 addr) {
    const std::uint64_t end = std::uint64_t{addr} + 4;
    mt_mem *mem = cpu != nullptr ? mt_mem_from_cpu(cpu) : nullptr;
    if (mem == nullptr || cpu->mem == nullptr || addr == 0 || end > cpu->mem_size) {
        return false;
    }
    u32 n = 0;
    const mt_segment *segs = mt_mem_segments(mem, &n);
    for (u32 i = 0; segs != nullptr && i < n; i++) {
        if (addr >= segs[i].vmaddr && end <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize) {
            return true;
        }
    }
    mt_heap_stats heap{};
    mt_heap_get_stats(mem, &heap);
    return addr >= MT_GUEST_HEAP_BASE && end <= heap.committed;
}

// A guest call from the host with every register kept.
u32 call(mt_cpu *cpu, u32 fn, u32 a0, u32 a1) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 args[2] = {a0, a1};
    const u32 r0 = mt_call_guest(cpu, fn, args, 2, fn & ~1u);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

} // namespace

const char *autosave_guard_name(AutosaveGuard guard) {
    switch (guard) {
    case AutosaveGuard::kNone:
        return "none";
    case AutosaveGuard::kNotInGame:
        return "not in the game";
    case AutosaveGuard::kLoading:
        return "loading";
    case AutosaveGuard::kScene:
        return "scene";
    case AutosaveGuard::kNotField:
        return "not the field";
    case AutosaveGuard::kPopup:
        return "popup";
    case AutosaveGuard::kWarp:
        return "warp";
    case AutosaveGuard::kKnockBack:
        return "knock-back";
    case AutosaveGuard::kScroll:
        return "scroll";
    case AutosaveGuard::kDying:
        return "dying";
    case AutosaveGuard::kNoSaveMap:
        return "no-save map";
    case AutosaveGuard::kHeroBusy:
        return "hero busy";
    }
    return "?";
}

std::span<const NoSaveMap> no_save_maps() { return kNoSaveMaps; }

const NoSaveMap *find_no_save_map(int area, int map) {
    for (const NoSaveMap &m : kNoSaveMaps) {
        if (m.area == area && m.map == map) {
            return &m;
        }
    }
    return nullptr;
}

std::string AutosaveCheck::describe() const {
    char text[160];
    switch (guard) {
    case AutosaveGuard::kNone:
        std::snprintf(text, sizeof text, "slot %d, map %d.%d", slot, area, map);
        break;
    case AutosaveGuard::kNotInGame:
        std::snprintf(text, sizeof text, "not in the game (no CStateGame)");
        break;
    case AutosaveGuard::kLoading:
        std::snprintf(text, sizeof text, "loading (game mode %d)", mode);
        break;
    case AutosaveGuard::kScene:
        std::snprintf(text, sizeof text, "a scene is playing (game mode %d)", mode);
        break;
    case AutosaveGuard::kNotField:
        std::snprintf(text, sizeof text, "not in the field or the popup menu (game mode %d)",
                      mode);
        break;
    case AutosaveGuard::kPopup:
        std::snprintf(text, sizeof text, "a popup is open (popup %d)", popup);
        break;
    case AutosaveGuard::kWarp:
        std::snprintf(text, sizeof text, "a map change is pending");
        break;
    case AutosaveGuard::kKnockBack:
        std::snprintf(text, sizeof text, "the hero is thrown back by a hit");
        break;
    case AutosaveGuard::kScroll:
        std::snprintf(text, sizeof text, "a scroll is being applied");
        break;
    case AutosaveGuard::kDying:
        std::snprintf(text, sizeof text, "the hero is %s (action %d)",
                      action == kActionDying ? "dying" : "dead", action);
        break;
    case AutosaveGuard::kNoSaveMap: {
        const NoSaveMap *m = find_no_save_map(area, map);
        std::snprintf(text, sizeof text, "map %d.%d, a %s map where the game cannot save", area,
                      map, m != nullptr ? m->kind : "?");
        break;
    }
    case AutosaveGuard::kHeroBusy:
        std::snprintf(text, sizeof text,
                      "the hero is busy: not standing, walking or climbing (action %d, flags "
                      "0x%X)",
                      action,
                      static_cast<unsigned>(flags & 0xFFu));
        break;
    }
    return text;
}

AutosaveCheck check_autosave(const GuestRead32 &read) {
    AutosaveCheck c;
    u32 canvas = 0, state = 0, vptr = 0;
    if (!read(kAutosaveCanvas, &canvas) || canvas == 0 ||
        !read(canvas + kCanvasState, &state) || state == 0 || !read(state, &vptr) ||
        vptr != kAutosaveGameVptr) {
        c.guard = AutosaveGuard::kNotInGame;
        return c;
    }
    c.state = state;
    u32 v = 0;
    if (!read(state + kStateHero, &c.hero) || c.hero == 0 ||
        !read(kAutosaveDataMgr, &c.data_mgr) || c.data_mgr == 0 || !read(kAutosaveGameMode, &v)) {
        c.guard = AutosaveGuard::kNotInGame;
        return c;
    }
    c.mode = static_cast<i32>(v);
    if (read(kAutosavePopup, &v)) {
        c.popup = static_cast<i32>(v);
    }
    if (read(state + kStateArea, &v)) {
        c.area = static_cast<i32>(v);
    }
    if (read(state + kStateMap, &v)) {
        c.map = static_cast<i32>(v);
    }
    if (read(c.hero + kHeroAction, &v)) {
        c.action = static_cast<i32>(v);
    }
    read(c.hero + kHeroFlags, &c.flags);
    if (read(c.data_mgr + kMgrSlot, &v)) {
        c.slot = static_cast<i32>(v);
    }
    u32 warp = 1, knock = 1, scroll = 1;
    const bool bytes = read_byte(read, state + kStateWarp, &warp) &&
                       read_byte(read, c.hero + kHeroKnockBack, &knock) &&
                       read_byte(read, c.data_mgr + kMgrScroll, &scroll);
    if (c.mode >= 6 && c.mode <= 8) {
        c.guard = AutosaveGuard::kLoading;
    } else if (c.mode == 9 || c.mode == 10) {
        c.guard = AutosaveGuard::kScene;
    } else if (c.mode != 0 && c.mode != 1) {
        c.guard = AutosaveGuard::kNotField;
    } else if (c.popup != -1) {
        c.guard = AutosaveGuard::kPopup;
    } else if (!bytes || c.area < 0 || c.map < 0 || c.action < 0 || c.slot < 0 || c.slot > 2) {
        c.guard = AutosaveGuard::kNotInGame; // a half-built game: nothing to save
    } else if (warp != 0) {
        c.guard = AutosaveGuard::kWarp;
    } else if (knock != 0) {
        c.guard = AutosaveGuard::kKnockBack;
    } else if (scroll != 0) {
        c.guard = AutosaveGuard::kScroll;
    } else if (c.action == kActionDying || c.action == kActionDead) {
        c.guard = AutosaveGuard::kDying;
    } else if (find_no_save_map(c.area, c.map) != nullptr) {
        c.guard = AutosaveGuard::kNoSaveMap;
    } else if ((c.flags & (kHeroAirborne | kHeroAttacking)) != 0 ||
               (c.action != kActionStand && c.action != kActionWalk && c.action != kActionClimb)) {
        c.guard = AutosaveGuard::kHeroBusy;
    } else {
        c.guard = AutosaveGuard::kNone;
    }
    return c;
}

AutosaveCheck check_autosave(const mt_cpu *cpu) {
    return check_autosave([cpu](u32 addr, u32 *out) {
        if (!readable(cpu, addr)) {
            return false;
        }
        std::memcpy(out, cpu->mem + addr, 4);
        return true;
    });
}

AutosaveResult autosave_now(mt_cpu *cpu, const char *trigger, double t) {
    AutosaveResult r;
    r.check = check_autosave(cpu);
    if (!r.check.ok()) {
        SDL_Log("autosave: not saved at %s (%.3f s): %s", trigger, t, r.check.describe().c_str());
        return r;
    }
    mt_objc *rt = mt_objc_current();
    const u32 pool = rt != nullptr ? mt_objc_pool_push(rt) : 0;
    const Uint64 t0 = SDL_GetTicksNS();
    call(cpu, kAutosaveSaveData, r.check.data_mgr, r.check.hero);
    call(cpu, kAutosaveSaveQuests, r.check.data_mgr, r.check.hero);
    if (rt != nullptr) {
        mt_objc_pool_pop(rt, pool);
    }
    r.saved = true;
    SDL_Log("autosave: slot %d saved at %s (%.3f s): map %d.%d, through the game's "
            "SaveSaveData and SaveQuestData (%.1f ms)",
            r.check.slot, trigger, t, r.check.area, r.check.map,
            static_cast<double>(SDL_GetTicksNS() - t0) / 1e6);
    return r;
}

bool Autosaver::before_deliver(mt_cpu *cpu, Autosave mode, const Lifecycle &lifecycle,
                               std::uint64_t turn, double t) {
    if (mode != Autosave::kOnQuit || turn == last_turn_) {
        return false;
    }
    const std::optional<AppMessageRecord> leave = lifecycle.leaving();
    if (!leave) {
        return false;
    }
    last_turn_ = turn;
    char trigger[96];
    std::snprintf(trigger, sizeof trigger, "%s (%s%s)",
                  leave->message == AppMessage::kWillTerminate ? "the quit" : "the background",
                  lifecycle_event_name(leave->cause), leave->scripted ? ", scripted" : "");
    const AutosaveResult r = autosave_now(cpu, trigger, t);
    if (r.saved) {
        stats_.saves++;
    } else {
        stats_.skips++;
    }
    stats_.last = r.check.guard;
    return r.saved;
}

} // namespace maplethief::frontend
