// Shops 0, 1 and 16 from an existing merchant (#426, restored_content; docs/re/original-bugs.md
// "Unreachable content"); see maplethief/hooks.h.
//
// ScriptNative_showMarket 0x998D1 (L) is the script native showMarket (39): it reads its argument
// (Lua::getParam 0x993D9, Lua::stackSlot 0x9912D, the slot's +4) and calls the shop screen
// 0x4628D (the shop object **0xF1454, shop, L + 4). L + 4 is the CStateGame: its area +0x84, map
// +0x88, chat box +0x5C. Every merchant script's main is one showMarket call, so the handler
// knows the merchant by the shop he opens and the map he stands on.
//
// For one of the three merchants it builds the list box a script builds with initMsg,
// regMsgNPC, regSelList and showMsg(2), with the chat box's own functions: CChatBox::Clear
// 0x35B35, CChatBox::AddNpcPage 0x358DD (copies the text, CMsg::CMsg 0x31D95), AddRow 0x357B5
// (copies the key; the box looks it up in Scripts.strings when it draws, and a text that is no
// key is drawn as it is) and CChatBox::Show 0x3573D (box, 0, 2). The texts are written below the
// guest's sp for those calls only.
//
// The box's close, CChatBox::Close 0x35E89, runs Lua::runCallback 0x9CE2D (L) after OK, which
// calls the callback regCallBack stored at L + 8 and clears it. The handler puts a marker there
// (an address in the image, never a TFunc); the hook on runCallback finds it, reads the answer
// (CChatBox::GetAnswer 0x31E11: the chosen row's value, the shop) and opens that shop as
// showMarket does. Back closes the box without the callback, as any conversation; the marker is
// then still at L + 8, so a later close finds it under a box that is not this one (any other
// answer) and gets the value that was there before back.
//
// The shop screen keeps every market.dat row whose shop byte is the shop's (it counts them with
// 0x39EA9 and reads each cell with 0x398D5 from the file's bytes at *0x11A994), and the 18 unused
// rows at the file's end are all zero: shop 0, equipment 0, price 0. For shop 0 the call above
// sets those rows' shop byte to 0xFF and puts the zeros back after it, so the screen lists shop
// 0's 61 listings and no free item; the guest's table is the file's again before the call
// returns, and no other call sees the change.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_get_param = 0x993D9u;   // Lua::getParam (L, n): the n-th argument's index
constexpr u32 k_stack_slot = 0x9912Du;  // Lua::stackSlot (L, index): the slot, value at +4
constexpr u32 k_shop_ptr = 0xF1454u;    // -> -> the shop object, the shop screen's first argument
constexpr u32 k_shop_open = 0x4628Du;   // the shop screen (shop object, shop, L + 4)
constexpr u32 k_market_ptr = 0x11A994u; // -> market.dat's bytes: header (rows +2), 7-byte rows
constexpr u32 k_market_header = 6u;
constexpr u32 k_market_stride = 7u;
constexpr std::uint8_t k_no_shop = 0xFFu; // an unused row's shop byte during shop 0's call
constexpr u32 k_box_clear = 0x35B35u;     // CChatBox::Clear (box)
constexpr u32 k_box_npc_page = 0x358DDu;  // CChatBox::AddNpcPage (box, text)
constexpr u32 k_box_add_row = 0x357B5u;   // AddRow (box, key, value)
constexpr u32 k_box_show = 0x3573Du;      // CChatBox::Show (box, kind, mode)
constexpr u32 k_box_answer = 0x31E11u;    // CChatBox::GetAnswer (box)
constexpr u32 k_lua_game = 0x4u;          // L + 4: the CStateGame
constexpr u32 k_lua_callback = 0x8u;      // L + 8: regCallBack's function
constexpr u32 k_game_box = 0x5Cu;         // CStateGame +0x5C: the chat box
constexpr u32 k_game_area = 0x84u;        // CStateGame +0x84 / +0x88: the map shown
constexpr u32 k_game_map = 0x88u;
constexpr u32 k_box_mode = 0x8u; // the box's mode (2: a list of rows)
constexpr u32 k_list_mode = 2u;
constexpr u32 k_marker = 0x998D1u;       // at L + 8 while the list box is open: no TFunc
constexpr std::size_t k_text_max = 255u; // a text's bytes at most (the rest is cut)

// The merchants (docs/re/original-bugs.md, Unreachable content): (area, map) and the shop his
// script opens -> the restored shop offered beside it.
struct Merchant {
    i32 area, map, shop, restored;
};
constexpr Merchant k_merchants[] = {
    {0, 0, 5, 16}, // Etc00 on 0.0, where the smith whose recipes take shop 16's ores stands
    {9, 0, 8, 0},  // Equip90 on 9.0: every equipment item (Equip60 on 6.0 opens shop 8 too)
    {9, 0, 13, 1}, // Etc90 on 9.0: every consumable
};

// The texts (UTF-8): the merchant's line, his usual shop's row and the restored shops' rows.
struct Texts {
    std::string line = "What are you looking for?";
    std::string usual = "My usual goods";
    std::string shop0 = "Every piece of equipment";
    std::string shop1 = "Every supply";
    std::string shop16 = "Ores and gems for the smith";
};
Texts g_texts; // set before the guest runs

// The list box open (guest thread only): its L, the value L + 8 held, the two shops offered.
struct Pending {
    bool open = false;
    u32 lua = 0, callback = 0;
    i32 usual = -1, restored = -1;
};
Pending g_pending;
std::atomic<std::uint64_t> g_boxes{0}, g_opened{0};

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

u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args, u32 site) {
    return mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), site);
}

// `text` as a C string below sp (sp moves down past it, 8-aligned); its guest address.
u32 push_text(mt_cpu *cpu, const std::string &text) {
    const std::size_t n = text.size() < k_text_max ? text.size() : k_text_max;
    const u32 at = (cpu->r[MT_SP] - static_cast<u32>(n + 1)) & ~7u;
    std::memcpy(cpu->mem + at, text.data(), n);
    cpu->mem[at + n] = 0;
    cpu->r[MT_SP] = at;
    return at;
}

// The unused (all-zero) rows of the guest's market.dat get shop byte `to`; their offsets.
std::vector<u32> mark_unused_rows(mt_cpu *cpu, std::uint8_t to) {
    std::vector<u32> rows;
    const u32 table = read_word(cpu, k_market_ptr);
    if (!in_guest(cpu, table, k_market_header)) {
        return rows;
    }
    const u32 n = cpu->mem[table + 2u] | (static_cast<u32>(cpu->mem[table + 3u]) << 8);
    for (u32 i = 0;
         i < n && in_guest(cpu, table + k_market_header + (i + 1u) * k_market_stride, 0u); i++) {
        const u32 at = table + k_market_header + i * k_market_stride;
        bool zero = true;
        for (u32 b = 0; b < k_market_stride; b++) {
            zero = zero && cpu->mem[at + b] == 0u;
        }
        if (zero) {
            cpu->mem[at] = to;
            rows.push_back(at);
        }
    }
    return rows;
}

const std::string &row_text(i32 shop) {
    return shop == 0 ? g_texts.shop0 : (shop == 1 ? g_texts.shop1 : g_texts.shop16);
}

int show_market(mt_cpu *cpu, const mt_hook_call *) {
    const u32 lua = cpu->r[0];
    const u32 game = read_word(cpu, lua + k_lua_game);
    if (!in_guest(cpu, lua, k_lua_callback + 4u) || !in_guest(cpu, game, k_game_area + 8u)) {
        return MT_HOOK_PASS;
    }
    const u32 index = call(cpu, k_get_param, {lua, 1u}, MT_HOOK_FN_LUA_SHOW_MARKET);
    const u32 slot = call(cpu, k_stack_slot, {lua, index}, MT_HOOK_FN_LUA_SHOW_MARKET);
    const auto shop = static_cast<i32>(read_word(cpu, slot + 4u));
    const i32 restored =
        mt_restored_shop_for(static_cast<i32>(read_word(cpu, game + k_game_area)),
                             static_cast<i32>(read_word(cpu, game + k_game_map)), shop);
    const u32 box = read_word(cpu, game + k_game_box);
    if (restored < 0 || !in_guest(cpu, box, k_box_mode + 4u)) {
        cpu->r[0] = lua;
        return MT_HOOK_PASS; // the usual shop, as the original
    }
    const u32 sp = cpu->r[MT_SP];
    const u32 line = push_text(cpu, g_texts.line);
    const u32 usual = push_text(cpu, g_texts.usual);
    const u32 extra = push_text(cpu, row_text(restored));
    call(cpu, k_box_clear, {box}, MT_HOOK_FN_LUA_SHOW_MARKET);
    call(cpu, k_box_npc_page, {box, line}, MT_HOOK_FN_LUA_SHOW_MARKET);
    call(cpu, k_box_add_row, {box, usual, static_cast<u32>(shop)}, MT_HOOK_FN_LUA_SHOW_MARKET);
    call(cpu, k_box_add_row, {box, extra, static_cast<u32>(restored)}, MT_HOOK_FN_LUA_SHOW_MARKET);
    cpu->r[MT_SP] = sp;
    g_pending = Pending{true, lua, read_word(cpu, lua + k_lua_callback), shop, restored};
    write_word(cpu, lua + k_lua_callback, k_marker);
    call(cpu, k_box_show, {box, 0u, k_list_mode}, MT_HOOK_FN_LUA_SHOW_MARKET);
    g_boxes.fetch_add(1, std::memory_order_relaxed);
    mt_tracef("hooks", "showMarket(%d) on (%d, %d): a list box, shop %d or %d", shop,
              static_cast<int>(read_word(cpu, game + k_game_area)),
              static_cast<int>(read_word(cpu, game + k_game_map)), shop, restored);
    return MT_HOOK_HANDLED; // the native returns no value
}

int run_callback(mt_cpu *cpu, const mt_hook_call *) {
    const u32 lua = cpu->r[0];
    if (!g_pending.open || lua != g_pending.lua ||
        read_word(cpu, lua + k_lua_callback) != k_marker) {
        return MT_HOOK_PASS;
    }
    g_pending.open = false;
    const u32 game = read_word(cpu, lua + k_lua_game);
    const u32 box = in_guest(cpu, game, k_game_box + 4u) ? read_word(cpu, game + k_game_box) : 0u;
    const auto answer = static_cast<i32>(
        in_guest(cpu, box, k_box_mode + 4u) && read_word(cpu, box + k_box_mode) == k_list_mode
            ? call(cpu, k_box_answer, {box}, MT_HOOK_FN_LUA_RUN_CALLBACK)
            : 0xFFFFFFFFu);
    if (answer != g_pending.usual && answer != g_pending.restored) {
        // Not the list box (closed by Back before): the callback that was there runs, as ever.
        write_word(cpu, lua + k_lua_callback, g_pending.callback);
        cpu->r[0] = lua;
        return MT_HOOK_PASS;
    }
    write_word(cpu, lua + k_lua_callback, 0u); // runCallback clears it before the call
    const u32 shop = read_word(cpu, read_word(cpu, k_shop_ptr));
    const std::vector<u32> unused =
        answer == 0 ? mark_unused_rows(cpu, k_no_shop) : std::vector<u32>{};
    call(cpu, k_shop_open, {shop, static_cast<u32>(answer), game}, MT_HOOK_FN_LUA_RUN_CALLBACK);
    for (const u32 at : unused) {
        cpu->mem[at] = 0u; // the file's bytes again
    }
    g_opened.fetch_add(1, std::memory_order_relaxed);
    mt_tracef("hooks", "Lua::runCallback(0x%08X): shop %d", static_cast<unsigned>(lua), answer);
    mt_ret_u32(cpu, 0u); // no callback ran: the box clears itself
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" std::int32_t mt_restored_shop_for(std::int32_t area, std::int32_t map,
                                             std::int32_t shop) noexcept {
    for (const Merchant &m : k_merchants) {
        if (m.area == area && m.map == map && m.shop == shop) {
            return m.restored;
        }
    }
    return -1;
}

extern "C" void mt_hooks_install_restored_shops(int enabled,
                                                const mt_restored_shop_texts *texts) noexcept {
    if (texts != nullptr) {
        const auto set = [](std::string &to, const char *from) {
            if (from != nullptr && from[0] != '\0') {
                to = from;
            }
        };
        set(g_texts.line, texts->line);
        set(g_texts.usual, texts->usual);
        set(g_texts.shop0, texts->shop0);
        set(g_texts.shop1, texts->shop1);
        set(g_texts.shop16, texts->shop16);
    }
    g_pending = Pending{};
    mt_hook_register(MT_HOOK_FN_LUA_SHOW_MARKET, show_market, nullptr, enabled);
    mt_hook_register(MT_HOOK_FN_LUA_RUN_CALLBACK, run_callback, nullptr, enabled);
}

extern "C" mt_restored_shop_stats mt_hooks_restored_shop_stats(void) noexcept {
    return {g_boxes.load(std::memory_order_relaxed), g_opened.load(std::memory_order_relaxed)};
}
