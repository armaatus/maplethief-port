// What the real-asset tests read of the game's state in guest memory (#127): the game mode,
// the open popup, the hero, its quests and inventory, the mobs of the map. Read only: nothing
// here writes guest memory. The layouts come from the decompiles named on each reader and from
// docs/formats/save.md (whose fields are the hero's and the quest manager's, saved one to one).
// Addresses are armv7 VM addresses.
#ifndef MAPLETHIEF_TESTS_GUEST_STATE_H
#define MAPLETHIEF_TESTS_GUEST_STATE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/recomp/cpu.h"

namespace guest_state {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_game_mode = 0x11AA00; // 0 field, 1 popup menu, 2 shop, 3 map, 4 NPC chat, 0xB cash
constexpr u32 k_popup_id = 0x108C64;  // the open popup (-1 none)
constexpr u32 k_canvas = 0x11A950;    // SkyCanvas::GetInstance's instance
constexpr u32 k_mobs = 0x11A9EC;      // CMobs::CMobs's list of the map's mobs (PtrArray_New100)

inline u32 rd(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
inline i32 rdi(const mt_cpu *cpu, u32 addr) { return static_cast<i32>(rd(cpu, addr)); }
inline i32 rd16(const mt_cpu *cpu, u32 addr) {
    return static_cast<std::int16_t>(rd(cpu, addr) & 0xFFFF);
}

// The game's lists (PtrArray: count +4, items +8; 0x25380 / 0x25148).
inline std::vector<u32> list_items(const mt_cpu *cpu, u32 list, u32 max = 100) {
    std::vector<u32> out;
    const u32 n = list != 0 ? std::min(rd(cpu, list + 4), max) : 0;
    const u32 items = list != 0 ? rd(cpu, list + 8) : 0;
    for (u32 i = 0; items != 0 && i < n; i++) {
        out.push_back(rd(cpu, items + 4 * i));
    }
    return out;
}

inline i32 game_mode(const mt_cpu *cpu) { return rdi(cpu, k_game_mode); }
inline i32 popup_id(const mt_cpu *cpu) { return rdi(cpu, k_popup_id); }

// The hero: SkyCanvas -> the current state +0x0C (CStateGame) -> CHero +0x30: x +0x04, y +0x08,
// flags +0x0C (0x10: dead), state +0x10 (2 standing, 3 walking, 5 jumping, 6 / 7 attacking, 11
// a skill; docs/ANALYSIS.md), HP +0x94, MP +0x98, max HP +0x9C, max MP +0xA0 (CHero::LevelUp
// 0x65089), level +0x80, EXP +0x90, meso +0xD0, the other items +0xD8 (48 item pointers: id
// +0x90, count +0x100); its world +0x150: area +0x84, map +0x88, the quest manager +0x58
// (chapter +0x04, the main quests' states +0x08 + 4n, the side quests' +0x19C + 4n, the active
// quests +0x32C).
struct Hero {
    u32 addr = 0, world = 0, quests = 0;
    i32 x = 0, y = 0, hp = 0, level = 0, exp = 0, meso = 0, area = -1, map = -1, chapter = -1;
    i32 flags = 0, state = 0, mp = 0, max_hp = 0, max_mp = 0;
    bool on(i32 a, i32 m) const { return addr != 0 && area == a && map == m; }
};
inline Hero hero(const mt_cpu *cpu) {
    Hero h;
    const u32 canvas = rd(cpu, k_canvas);
    const u32 st = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
    h.addr = st != 0 ? rd(cpu, st + 0x30) : 0;
    if (h.addr == 0) {
        return h;
    }
    h.x = rdi(cpu, h.addr + 0x04);
    h.y = rdi(cpu, h.addr + 0x08);
    h.flags = rdi(cpu, h.addr + 0x0C);
    h.state = rdi(cpu, h.addr + 0x10);
    h.hp = rdi(cpu, h.addr + 0x94);
    h.mp = rdi(cpu, h.addr + 0x98);
    h.max_hp = rdi(cpu, h.addr + 0x9C);
    h.max_mp = rdi(cpu, h.addr + 0xA0);
    h.level = rdi(cpu, h.addr + 0x80);
    h.exp = rdi(cpu, h.addr + 0x90);
    h.meso = rdi(cpu, h.addr + 0xD0);
    h.world = rd(cpu, h.addr + 0x150);
    if (h.world != 0) {
        h.area = rdi(cpu, h.world + 0x84);
        h.map = rdi(cpu, h.world + 0x88);
        h.quests = rd(cpu, h.world + 0x58);
        h.chapter = h.quests != 0 ? rdi(cpu, h.quests + 0x04) : -1;
    }
    return h;
}

// A quest's state as the scripts' getQuestState(sort, n) reads it: 0 not started, 1 active,
// 2 done (sort 0: the chapter's main quests, quest0's flags_a; sort 1: the side quests,
// flags_b). -1 without a hero.
inline i32 quest_state(const mt_cpu *cpu, const Hero &h, u32 sort, u32 n) {
    if (h.quests == 0 || n >= 100) {
        return -1;
    }
    return rdi(cpu, h.quests + (sort == 0 ? 0x08 : 0x19C) + 4 * n);
}

// An active quest (CDataMgr::SaveQuestData 0x38F55): sort +0x04, number +0x08, requirements
// +0x24 (kind +0x04: 1 = defeat; target +0x14, needed +0x18, progress +0x1C).
struct Requirement {
    i32 kind = 0, target = 0, needed = 0, progress = 0;
};
struct ActiveQuest {
    i32 sort = 0, num = 0;
    std::vector<Requirement> reqs;
};
inline std::vector<ActiveQuest> active_quests(const mt_cpu *cpu, const Hero &h) {
    std::vector<ActiveQuest> out;
    if (h.quests == 0) {
        return out;
    }
    for (const u32 q : list_items(cpu, rd(cpu, h.quests + 0x32C), 10)) {
        if (q == 0) {
            continue;
        }
        ActiveQuest a;
        a.sort = rdi(cpu, q + 0x04);
        a.num = rdi(cpu, q + 0x08);
        for (const u32 req : list_items(cpu, rd(cpu, q + 0x24), 5)) {
            if (req != 0) {
                a.reqs.push_back({rdi(cpu, req + 0x04), rdi(cpu, req + 0x14) & 0xFF,
                                  rd16(cpu, req + 0x18), rd16(cpu, req + 0x1C)});
            }
        }
        out.push_back(a);
    }
    return out;
}
// The progress of active quest (sort, n)'s first defeat requirement, or -1.
inline i32 defeat_progress(const mt_cpu *cpu, const Hero &h, i32 sort, i32 n) {
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        for (const Requirement &q : a.reqs) {
            if (a.sort == sort && a.num == n && q.kind == 1) {
                return q.progress;
            }
        }
    }
    return -1;
}
inline bool quest_active(const mt_cpu *cpu, const Hero &h, i32 sort, i32 n) {
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        if (a.sort == sort && a.num == n) {
            return true;
        }
    }
    return false;
}

// The count of other-item `id` (item_etc) in the hero's inventory.
inline i32 item_count(const mt_cpu *cpu, const Hero &h, i32 id) {
    const u32 items = h.addr != 0 ? rd(cpu, h.addr + 0xD8) : 0;
    i32 n = 0;
    for (u32 i = 0; items != 0 && i < 48; i++) {
        const u32 item = rd(cpu, items + 4 * i);
        if (item != 0 && rdi(cpu, item + 0x90) == id) {
            n += rd16(cpu, item + 0x100);
        }
    }
    return n;
}

// The mobs of the map (the list at 0x11A9EC): x +0x04, y +0x08, HP +0x88, the mob.dat id +0xA0.
struct Mob {
    u32 addr = 0;
    i32 x = 0, y = 0, hp = 0, id = 0;
};
inline std::vector<Mob> mobs(const mt_cpu *cpu) {
    std::vector<Mob> out;
    for (const u32 m : list_items(cpu, rd(cpu, k_mobs))) {
        if (m != 0) {
            out.push_back({m, rdi(cpu, m + 0x04), rdi(cpu, m + 0x08), rdi(cpu, m + 0x88),
                           rdi(cpu, m + 0xA0)});
        }
    }
    return out;
}

// The current game state (SkyCanvas +0x0C): a CStateGame while the hero exists.
inline u32 game_state(const mt_cpu *cpu) {
    const u32 canvas = rd(cpu, k_canvas);
    return canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
}

// The drops on the map: CStateGame +0x40 is the CDropItems (CStateGame::LoadGame 0x86C1D), its
// list +0x08. Its update (0x5BC75, every tick) picks a lying drop up when the hero's box meets
// the drop's: unless the hero's inventory has no room (CHero::isAddInven), the drop flies to the
// hero and CHero::addInven takes it when it arrives (sound 0x1D). Each drop: x +0x04, y +0x08,
// state +0x10 (0 lying, 2 picked up), its item's id +0x90 (as an inventory item's), the ticks it
// stays +0x104, a refusal (no room) the byte +0x10C.
struct Drop {
    u32 addr = 0;
    i32 x = 0, y = 0, state = 0, id = 0, life = 0;
    bool refused = false;
};
inline std::vector<Drop> drops(const mt_cpu *cpu) {
    std::vector<Drop> out;
    const u32 st = rd(cpu, game_state(cpu) + 0x30) != 0 ? game_state(cpu) : 0;
    const u32 items = st != 0 ? rd(cpu, st + 0x40) : 0;
    for (const u32 d : list_items(cpu, items != 0 ? rd(cpu, items + 0x08) : 0)) {
        if (d != 0) {
            out.push_back({d, rdi(cpu, d + 0x04), rdi(cpu, d + 0x08), rdi(cpu, d + 0x10),
                           rdi(cpu, d + 0x90), rdi(cpu, d + 0x104), cpu->mem[d + 0x10C] != 0});
        }
    }
    return out;
}

// The damage numbers floating on the screen: the list CDamageFloatings's constructor
// (0x36CE1) stores in a global, the PC-relative literal at 0x36D08 (+ 0x36D02).
inline u32 damage_numbers(const mt_cpu *cpu) {
    const u32 list = rd(cpu, rd(cpu, 0x36D08) + 0x36D02);
    return list != 0 ? std::min(rd(cpu, list + 4), 100u) : 0;
}

// The NPC shop (game mode 2), the object Shop_OnKey (0x46DB9) gets, the one *(*0xF1454)
// (CStateGame::OnKeyPressed's call at 0x90188): the open tab +0x5C (0 Buy, 1 Sell), the selected
// row of each tab +0x60 + 4 * tab (-1 none), the kind +0x90 (1: the shop sells the etc items
// and charges throwing stars), the Buy rows +0x94 (pointers to items: id +0x90, price +0x98)
// and their count +0x98. The Sell tab's rows are the hero's etc items (+0xD8), whose type is
// +0x8C (2 throwing stars), count +0x100, stack maximum +0xFC and price +0xA4.
struct Shop {
    u32 addr = 0;
    i32 tab = -1, selected = -1, kind = -1, rows = 0;
};
inline Shop shop(const mt_cpu *cpu) {
    Shop s;
    s.addr = rd(cpu, rd(cpu, 0xF1454));
    if (s.addr == 0) {
        return s;
    }
    s.tab = rdi(cpu, s.addr + 0x5C);
    s.selected =
        s.tab == 0 || s.tab == 1 ? rdi(cpu, s.addr + 0x60 + 4 * static_cast<u32>(s.tab)) : -1;
    s.kind = rdi(cpu, s.addr + 0x90);
    s.rows = rdi(cpu, s.addr + 0x98);
    return s;
}
// The Buy tab's row n: its item's id and price (0, -1 past the rows).
struct ShopRow {
    i32 id = -1, price = -1;
};
inline ShopRow shop_row(const mt_cpu *cpu, const Shop &s, i32 n) {
    const u32 rows = s.addr != 0 ? rd(cpu, s.addr + 0x94) : 0;
    const u32 item =
        rows != 0 && n >= 0 && n < s.rows ? rd(cpu, rows + 4 * static_cast<u32>(n)) : 0;
    if (item == 0) {
        return {};
    }
    return {rdi(cpu, item + 0x90), rdi(cpu, item + 0x98)};
}
// The hero's etc item in slot n (the Sell tab's row n): id, type, count, stack maximum, price.
struct EtcItem {
    i32 id = -1, type = -1, count = 0, max = 0, price = 0;
};
inline EtcItem etc_item(const mt_cpu *cpu, const Hero &h, i32 n) {
    const u32 items = h.addr != 0 ? rd(cpu, h.addr + 0xD8) : 0;
    const u32 item = items != 0 && n >= 0 && n < 48 ? rd(cpu, items + 4 * static_cast<u32>(n)) : 0;
    if (item == 0) {
        return {};
    }
    return {rdi(cpu, item + 0x90), rdi(cpu, item + 0x8C), rd16(cpu, item + 0x100),
            rdi(cpu, item + 0xFC), rdi(cpu, item + 0xA4)};
}

inline std::string hero_text(const mt_cpu *cpu, const Hero &h) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "map %d.%d x %d y %d HP %d level %d EXP %d meso %d chapter %d",
                  h.area, h.map, h.x, h.y, h.hp, h.level, h.exp, h.meso, h.chapter);
    std::string out = buf;
    out += ", main quests";
    for (u32 n = 0; n < 10; n++) {
        out += " " + std::to_string(quest_state(cpu, h, 0, n));
    }
    out += ", active";
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        out += " " + std::to_string(a.sort) + "." + std::to_string(a.num);
        for (const Requirement &q : a.reqs) {
            out += "[" + std::to_string(q.kind) + ":" + std::to_string(q.target) + " " +
                   std::to_string(q.progress) + "/" + std::to_string(q.needed) + "]";
        }
    }
    return out;
}

} // namespace guest_state

#endif // MAPLETHIEF_TESTS_GUEST_STATE_H
