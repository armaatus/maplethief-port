// Monster Carnival from a Spiegelman NPC (#427, restored_content; docs/re/original-bugs.md
// "Unreachable content"); see maplethief/hooks.h.
//
// CMMap::LoadObj 0xD259 (map, area, map) reads /map/<area><map>.obj into the CMMap
// (docs/formats/obj.md): the object count +0x44, the Obj* array +0x48 with its count +0x4C and
// 6 at +0x50, the name array +0x54 with its count +0x58. Each Obj is 12 bytes: cell +0, kind +2,
// quest marker +4 (-1 none), visible +6, facing +8, frame +0xA. The wrapper runs the original,
// then for a town of the table below builds both arrays again one longer with the guest's new[]
// (0xF0E30, the loader's own allocator), adds the NPC's Obj and its name last, and frees the
// old arrays with delete[] (0xF0E28). Everything that reads the list later (the sprites of
// 0x86289, the draw 0x8572D, the talk 0x63191, the NPC state pass 0x839BD) runs after it.
//
// Kind 42 is NPC 38 (npc.dat), the sprite /sprite/mob/42.sprprj; the talk runs the script the
// object names, from script/chapter<c>/ or script/common/. CStateGame::StartMonsterCarnival
// 0x82271 (the script's initMonCa) takes the first kind-42 object of the current map as the
// way back, where the round's result box returns the hero.
//
// The first round's score: StartMonsterCarnival sets the flag +0x404, the phase +0x400 to -1,
// the mode +0x40C, the counts +0x410-+0x42C and the clock +0x430, but not the score +0x434,
// which only the result box's close (CStateGame::OnKeyPressed 0x8D4C9, at 0x92CFA) sets to 0
// after a round. The first round of a run starts from whatever the CStateGame's memory held
// (resultMonCa 0x819E5 then pays the band of that). The room's own load is the first map load
// with the flag set and the phase still -1: the wrapper sets the score to 0 there, as every later
// round finds it.
//
// The tickets: market.dat's bytes at *0x11A994 (a 6-byte header, the row count at +2, 7-byte
// rows: shop, table, item, price i32) end in 18 all-zero rows no shop lists. Each ticket goes on
// one of them (table 1, item_etc), once: a row that already lists it is kept.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_new_array = 0xF0E30u;    // operator new[]'s import stub
constexpr u32 k_delete_array = 0xF0E28u; // operator delete[]'s
constexpr u32 k_map_count = 0x44u;       // CMMap: the objects
constexpr u32 k_map_objs = 0x48u;
constexpr u32 k_map_objs_count = 0x4Cu;
constexpr u32 k_map_objs_step = 0x50u;
constexpr u32 k_map_names = 0x54u;
constexpr u32 k_map_names_count = 0x58u;
constexpr u32 k_array_step = 6u; // the loader's +0x50
constexpr u32 k_obj_size = 12u;
constexpr u32 k_obj_kind = 2u;
constexpr u32 k_obj_marker = 4u;
constexpr u32 k_obj_visible = 6u;
constexpr std::uint16_t k_carnival_kind = 42u; // NPC 38, the carnival's way back
constexpr u32 k_canvas_global = 0x11A950u;     // SkyCanvas: the engine's current state +0xC
constexpr u32 k_canvas_state = 0xCu;
constexpr u32 k_game_vptr = 0x119B78u; // a CStateGame's vtable pointer
constexpr u32 k_carnival_phase = 0x400u;
constexpr u32 k_carnival_on = 0x404u; // a byte
constexpr u32 k_carnival_score = 0x434u;
constexpr u32 k_market_ptr = 0x11A994u;
constexpr u32 k_market_header = 6u;
constexpr u32 k_market_stride = 7u;
constexpr std::uint8_t k_market_etc = 1u; // a row's table: item_etc

// The towns (docs/re/original-bugs.md, Unreachable content): each script's texts are the
// strings tables' ChapterN_MonsterCarnival_Spiegelman keys, its round's room lies in that
// chapter's world, and the NPC stands on the world's first town beside its merchants. Prices:
// a delegated call (ADR 0237).
constexpr mt_restored_carnival_npc k_npcs[] = {
    {0, 0, 19 * 40 + 14, "Spiegelman1", 47, 5, 300},   // Kerning City, beside Etc00 (shop 5)
    {4, 0, 21 * 45 + 20, "Spiegelman2", 48, 11, 1500}, // Orbis Village, beside Etc40 (11)
    {6, 0, 6 * 57 + 31, "Spiegelman3", 49, 15, 4000},  // Magatia Village, beside Equip60, Etc60
};

std::atomic<std::uint64_t> g_npcs{0}, g_tickets{0}, g_scores{0};

bool in_guest(const mt_cpu *cpu, u32 addr, u32 size) {
    return addr != 0u && static_cast<std::uint64_t>(addr) + size <= cpu->mem_size;
}

u32 read_word(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (in_guest(cpu, addr, 4u)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}

void write_word(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

void write_half(mt_cpu *cpu, u32 addr, std::uint16_t v) { std::memcpy(cpu->mem + addr, &v, 2); }

u32 call(mt_cpu *cpu, u32 fn, u32 arg) {
    return mt_call_guest(cpu, fn, &arg, 1u, MT_HOOK_FN_MAP_LOAD_OBJ);
}

// Each ticket on an unused row of the guest's market.dat, unless a row lists it already.
void list_tickets(mt_cpu *cpu) {
    const u32 table = read_word(cpu, k_market_ptr);
    if (!in_guest(cpu, table, k_market_header)) {
        return;
    }
    const u32 rows = cpu->mem[table + 2u] | (static_cast<u32>(cpu->mem[table + 3u]) << 8);
    const auto row = [table](u32 i) { return table + k_market_header + i * k_market_stride; };
    for (const mt_restored_carnival_npc &npc : k_npcs) {
        u32 free = rows;
        bool listed = false;
        for (u32 i = 0; i < rows && in_guest(cpu, row(i), k_market_stride) && !listed; i++) {
            const std::uint8_t *at = cpu->mem + row(i);
            listed = at[0] == npc.shop && at[1] == k_market_etc && at[2] == npc.ticket;
            bool zero = true;
            for (u32 b = 0; b < k_market_stride; b++) {
                zero = zero && at[b] == 0u;
            }
            free = zero && free == rows ? i : free;
        }
        if (listed || free == rows) {
            continue;
        }
        std::uint8_t *at = cpu->mem + row(free);
        at[0] = static_cast<std::uint8_t>(npc.shop);
        at[1] = k_market_etc;
        at[2] = static_cast<std::uint8_t>(npc.ticket);
        std::memcpy(at + 3, &npc.price, 4);
        g_tickets.fetch_add(1, std::memory_order_relaxed);
        mt_tracef("hooks", "market.dat row %u: item_etc %d in shop %d for %d meso",
                  static_cast<unsigned>(free), npc.ticket, npc.shop, npc.price);
    }
}

// A round's room loading (the carnival on, its phase still -1): the score from 0.
void zero_first_score(mt_cpu *cpu) {
    const u32 game = read_word(cpu, read_word(cpu, k_canvas_global) + k_canvas_state);
    if (read_word(cpu, game) != k_game_vptr || !in_guest(cpu, game, k_carnival_score + 4u) ||
        cpu->mem[game + k_carnival_on] == 0u ||
        read_word(cpu, game + k_carnival_phase) != 0xFFFFFFFFu) {
        return;
    }
    if (read_word(cpu, game + k_carnival_score) != 0u) {
        mt_tracef("hooks", "Monster Carnival: the score %d before the round set to 0",
                  static_cast<int>(read_word(cpu, game + k_carnival_score)));
    }
    write_word(cpu, game + k_carnival_score, 0u);
    g_scores.fetch_add(1, std::memory_order_relaxed);
}

// The map's object list one longer, `npc` last.
void add_npc(mt_cpu *cpu, u32 map, const mt_restored_carnival_npc &npc) {
    const u32 count = read_word(cpu, map + k_map_count);
    const u32 objs = read_word(cpu, map + k_map_objs);
    const u32 names = read_word(cpu, map + k_map_names);
    if (count > 0xFFu ||
        (count > 0u && (!in_guest(cpu, objs, 4u * count) || !in_guest(cpu, names, 4u * count)))) {
        return; // not a list the loader built
    }
    for (u32 i = 0; i < count; i++) {
        const u32 obj = read_word(cpu, objs + 4u * i);
        std::uint16_t kind = 0;
        if (in_guest(cpu, obj, k_obj_size)) {
            std::memcpy(&kind, cpu->mem + obj + k_obj_kind, 2);
        }
        if (kind == k_carnival_kind) {
            return; // the map has one: the carnival takes the first
        }
    }
    const u32 len = static_cast<u32>(std::strlen(npc.script));
    const u32 new_objs = call(cpu, k_new_array, 4u * (count + 1u));
    const u32 new_names = call(cpu, k_new_array, 4u * (count + 1u));
    const u32 obj = call(cpu, k_new_array, k_obj_size);
    const u32 name = call(cpu, k_new_array, len + 1u);
    if (!in_guest(cpu, new_objs, 4u * (count + 1u)) ||
        !in_guest(cpu, new_names, 4u * (count + 1u)) || !in_guest(cpu, obj, k_obj_size) ||
        !in_guest(cpu, name, len + 1u)) {
        return;
    }
    if (count > 0u) {
        std::memcpy(cpu->mem + new_objs, cpu->mem + objs, 4u * count);
        std::memcpy(cpu->mem + new_names, cpu->mem + names, 4u * count);
    }
    std::memset(cpu->mem + obj, 0, k_obj_size); // facing 0, frame 0
    write_half(cpu, obj, static_cast<std::uint16_t>(npc.cell));
    write_half(cpu, obj + k_obj_kind, k_carnival_kind);
    write_half(cpu, obj + k_obj_marker, 0xFFFFu);
    write_half(cpu, obj + k_obj_visible, 1u);
    std::memcpy(cpu->mem + name, npc.script, len + 1u);
    write_word(cpu, new_objs + 4u * count, obj);
    write_word(cpu, new_names + 4u * count, name);
    write_word(cpu, map + k_map_count, count + 1u);
    write_word(cpu, map + k_map_objs, new_objs);
    write_word(cpu, map + k_map_objs_count, count + 1u);
    write_word(cpu, map + k_map_objs_step, k_array_step);
    write_word(cpu, map + k_map_names, new_names);
    write_word(cpu, map + k_map_names_count, count + 1u);
    if (count > 0u) {
        call(cpu, k_delete_array, objs);
        call(cpu, k_delete_array, names);
    }
    g_npcs.fetch_add(1, std::memory_order_relaxed);
}

int load_obj(mt_cpu *cpu, const mt_hook_call *hook) {
    const u32 map = cpu->r[0];
    const auto area = static_cast<i32>(cpu->r[1]);
    const auto number = static_cast<i32>(cpu->r[2]);
    hook->original(cpu);
    list_tickets(cpu);
    zero_first_score(cpu);
    if (const mt_restored_carnival_npc *npc = mt_restored_carnival_npc_on(area, number)) {
        if (in_guest(cpu, map, k_map_names_count + 4u)) {
            add_npc(cpu, map, *npc);
            mt_tracef("hooks", "CMMap::LoadObj(%d, %d): %s at cell %d", area, number, npc->script,
                      npc->cell);
        }
    }
    return MT_HOOK_HANDLED; // void: nothing to return
}

} // namespace

extern "C" const mt_restored_carnival_npc *mt_restored_carnival_npc_on(std::int32_t area,
                                                                       std::int32_t map) noexcept {
    for (const mt_restored_carnival_npc &npc : k_npcs) {
        if (npc.area == area && npc.map == map) {
            return &npc;
        }
    }
    return nullptr;
}

extern "C" void mt_hooks_install_restored_carnival(int enabled) noexcept {
    mt_hook_register(MT_HOOK_FN_MAP_LOAD_OBJ, load_obj, nullptr, enabled);
}

extern "C" mt_restored_carnival_stats mt_hooks_restored_carnival_stats(void) noexcept {
    return {g_npcs.load(std::memory_order_relaxed), g_tickets.load(std::memory_order_relaxed),
            g_scores.load(std::memory_order_relaxed)};
}
