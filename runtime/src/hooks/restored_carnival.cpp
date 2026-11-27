// Monster Carnival from a Spiegelman NPC (#427, restored_content; docs/re/original-bugs.md
// "Unreachable content"); see maplethief/hooks.h.
//
// CMMap::LoadObj 0xD259 (map, area, map) reads /map/<area><map>.obj into the CMMap
// (docs/formats/obj.md): the object count +0x44, the Obj* array +0x48 with its count +0x4C and
// 6 at +0x50, the name array +0x54 with its count +0x58 (map_objects.h). The wrapper runs the
// original, then for a town of the table below builds both arrays again one longer, the NPC's
// Obj and its name last (mt_map_objects_detail::insert). Everything that reads the list later (the
// sprites of 0x86289, the draw 0x8572D, the talk 0x63191, the NPC state pass 0x839BD) runs after
// it.
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

#include "map_objects.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

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

// The map's object list one longer, `npc` last, unless the map has a kind-42 object already
// (the carnival takes the first).
void add_npc(mt_cpu *cpu, u32 map, const mt_restored_carnival_npc &npc) {
    const u32 count = mt_map_objects_detail::count(cpu, map);
    for (u32 i = 0; i < count; i++) {
        if (mt_map_objects_detail::kind_at(cpu, map, i) == k_carnival_kind) {
            return;
        }
    }
    if (mt_map_objects_detail::insert(cpu, map, count, static_cast<std::uint16_t>(npc.cell),
                                      k_carnival_kind, npc.script, MT_HOOK_FN_MAP_LOAD_OBJ)) {
        g_npcs.fetch_add(1, std::memory_order_relaxed);
    }
}

int load_obj(mt_cpu *cpu, const mt_hook_call *hook) {
    const u32 map = cpu->r[0];
    const auto area = static_cast<i32>(cpu->r[1]);
    const auto number = static_cast<i32>(cpu->r[2]);
    hook->original(cpu);
    list_tickets(cpu);
    zero_first_score(cpu);
    if (const mt_restored_carnival_npc *npc = mt_restored_carnival_npc_on(area, number)) {
        add_npc(cpu, map, *npc);
        mt_tracef("hooks", "CMMap::LoadObj(%d, %d): %s at cell %d", area, number, npc->script,
                  npc->cell);
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
