// The running cash shop (#156, ADR 0072); see maplethief/cashshop.h. The store's policy (pays
// through the game's own CHero::setMeso), the counters per save slot (the game events of the
// observed functions, ADR 0053) and the quotes of the open screen (a run-loop turn source).
//
// Guest facts (docs/re/cash-shop.md, docs/controls/keypad.md, docs/re/cpp-classes.md): the
// engine's state is SkyCanvas (0x11A950) +0xC; a CStateGame has the vtable pointer 0x119B78
// (its vtable 0x119B70 + 8) and its hero at +0x30: level +0x80, meso +0xD0. The game mode is
// 0x11AA00 (0xB the cash shop), the open popup 0x108C64 (0x68 the death popup).
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "maplethief/cashshop.h"
#include "maplethief/cf.h"
#include "maplethief/events.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "snapshot/snapshot_internal.h"
#include "stubs/stubs_internal.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr u32 k_canvas = 0x11A950;
constexpr u32 k_canvas_state = 0x0C;
constexpr u32 k_game_vptr = 0x119B78;
constexpr u32 k_state_hero = 0x30;
constexpr u32 k_hero_level = 0x80;
constexpr u32 k_hero_meso = 0xD0;
constexpr u32 k_game_mode = 0x11AA00;
constexpr i32 k_mode_cash_shop = 0xB;
constexpr u32 k_popup_id = 0x108C64;
constexpr i32 k_death_popup = 0x68;
constexpr u32 k_exp_to_level_up = 0x65919; // CHero::ExpToLevelUp(hero)
constexpr u32 k_set_meso = 0x618E1;        // CHero::setMeso(hero, delta, log)

constexpr u32 k_shop_products[] = {MT_CASH_INITIALIZE_SP, MT_CASH_SP_MARBLE, MT_CASH_EXP40,
                                   MT_CASH_EXP80,         MT_CASH_MESOPACK3, MT_CASH_MESOPACK4,
                                   MT_CASH_MESOPACK5,     MT_CASH_MESOPACK6};
constexpr u32 k_revives[] = {MT_CASH_POW, MT_CASH_COMEBACK};

struct State {
    u32 mode = MT_CASH_SHOP_OFF;
    bool installed = false;
    mt_cash_knobs knobs = *mt_cash_default_knobs();
    i32 slot = -1;               // the slot in play (-1: none chosen yet)
    mt_cash_counters counters{}; // the slot's
    bool down = false;           // the death popup was seen and the hero not revived since
    // The hero as of the last turn (or payment).
    i32 level = 0, meso = 0;
    i32 exp_level = -1, exp_need = 0; // ExpToLevelUp's answer for exp_level
    mt_cash_view view{};
    mt_cash_stats stats{};
};
State g;

void log(const char *fmt, ...) MT_STUBS_PRINTF(1, 2);
void log(const char *fmt, ...) {
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    mt_stubs_impl::log("cash shop: %s", text);
}

// Whether [addr, addr + 4) is guest memory that is there: an image segment or the committed
// heap (as the events module reads; a synthetic guest's other pages are not readable).
bool readable(const mt_cpu *cpu, u32 addr) {
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + 4;
    mt_mem *mem = cpu != nullptr ? mt_mem_from_cpu(cpu) : nullptr;
    if (mem == nullptr || addr == 0 || end > cpu->mem_size) {
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
i32 rd(const mt_cpu *cpu, u32 addr, i32 dflt) {
    i32 v = dflt;
    rd32(cpu, addr, &v);
    return v;
}

// The hero of the CStateGame in play, or 0.
u32 hero_of(const mt_cpu *cpu) {
    i32 canvas = 0, st = 0, vptr = 0, hero = 0;
    if (!rd32(cpu, k_canvas, &canvas) || canvas == 0 ||
        !rd32(cpu, static_cast<u32>(canvas) + k_canvas_state, &st) || st == 0 ||
        !rd32(cpu, static_cast<u32>(st), &vptr) || static_cast<u32>(vptr) != k_game_vptr ||
        !rd32(cpu, static_cast<u32>(st) + k_state_hero, &hero)) {
        return 0;
    }
    return static_cast<u32>(hero);
}

// A guest call from the host with every register kept (the turn source and the policy run
// inside the guest's own code).
u32 call(mt_cpu *cpu, u32 fn, const u32 *args, u32 argc) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args, argc, fn & ~1u);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

// The hero's level and meso now, and the EXP the level needs (the game's CHero::ExpToLevelUp,
// called once per level).
void read_hero(mt_cpu *cpu, u32 hero) {
    g.level = rd(cpu, hero + k_hero_level, 0);
    g.meso = rd(cpu, hero + k_hero_meso, 0);
    if (g.level != g.exp_level) {
        g.exp_level = g.level;
        g.exp_need = static_cast<i32>(call(cpu, k_exp_to_level_up, &hero, 1));
    }
}

void write_counters() {
    if (g.slot < 0) {
        return;
    }
    char path[1100];
    if (mt_cash_shop_counters_path(g.slot, path, sizeof path) != 0) {
        return;
    }
    char text[1024];
    const std::size_t n = mt_cash_counters_format(&g.counters, text, sizeof text);
    if (mt_saves_replace_file(path, text, n) == 0) {
        g.stats.writes++;
    } else {
        log("cannot write %s", path);
    }
}

void load_counters(i32 slot) {
    g.slot = slot;
    char path[1100];
    mt_cash_counters_reset(&g.counters, 0);
    if (mt_cash_shop_counters_path(slot, path, sizeof path) != 0) {
        return;
    }
    std::size_t size = 0;
    char *text = mt_stubs_impl::read_file(path, &size);
    if (text != nullptr) {
        mt_cash_counters_parse(text, size, &g.counters);
        std::free(text);
    }
}

void reset_counters(i32 level, const char *why) {
    mt_cash_counters_reset(&g.counters, level);
    g.stats.resets++;
    log("the counters of slot %d start afresh at level %d (%s)", g.slot, level, why);
    write_counters();
}

// --- the game events --------------------------------------------------------------------------

void on_event(void *, const mt_game_event *e) {
    if (e == nullptr || g.mode != MT_CASH_SHOP_MESO) {
        return;
    }
    switch (e->kind) {
    case MT_GAME_EVENT_LOAD_CHARACTER:
        load_counters(e->value);
        g.down = false;
        break;
    case MT_GAME_EVENT_NEW_CHARACTER:
        g.slot = e->value;
        g.down = false;
        reset_counters(1, "a new character");
        break;
    case MT_GAME_EVENT_LEVEL_UP:
        if (g.slot >= 0) {
            reset_counters(e->value, "a level-up");
        }
        break;
    case MT_GAME_EVENT_DEATH_POPUP:
        if (!g.down && g.slot >= 0) {
            g.down = true;
            mt_cash_counters_death(&g.counters);
            g.stats.deaths++;
            log("a fall at level %d: %u since the last level-up, %u revive(s) bought", g.level,
                g.counters.deaths, g.counters.paid_revives);
            write_counters();
        }
        break;
    case MT_GAME_EVENT_REVIVED:
        g.down = false;
        break;
    default:
        break;
    }
}

i64 quote(u32 product) {
    return g.exp_level >= 0 ? mt_cash_price(&g.knobs, product, g.exp_need, g.meso,
                                            mt_cash_steps(&g.counters, product))
                            : -1;
}

// --- the run-loop turn: the hero, the open screen, its prices ------------------------------

void on_turn(mt_cpu *cpu, void *) {
    mt_cash_view v{};
    v.slot = g.slot;
    if (g.mode != MT_CASH_SHOP_MESO) {
        g.view = v;
        return;
    }
    const u32 hero = hero_of(cpu);
    if (hero == 0) {
        g.view = v;
        return;
    }
    read_hero(cpu, hero);
    // A level the counters were not kept at (a level gained while the shop was off, or a
    // counters file of another character): they start afresh.
    if (g.slot >= 0 && g.level > 0 && g.counters.level != g.level) {
        if (g.counters.level == 0) {
            g.counters.level = g.level;
            write_counters();
        } else {
            reset_counters(g.level, "the hero's level differs from the counters'");
        }
    }
    v.level = g.level;
    v.meso = g.meso;
    const u32 *products = nullptr;
    u32 n = 0;
    if (rd(cpu, k_game_mode, -1) == k_mode_cash_shop) {
        v.screen = MT_CASH_SCREEN_SHOP;
        products = k_shop_products;
        n = sizeof k_shop_products / sizeof k_shop_products[0];
    } else if (rd(cpu, k_popup_id, -1) == k_death_popup) {
        v.screen = MT_CASH_SCREEN_DEATH_POPUP;
        products = k_revives;
        n = sizeof k_revives / sizeof k_revives[0];
    }
    for (u32 i = 0; i < n; i++) {
        mt_cash_quote &q = v.quotes[v.count++];
        q.product = products[i];
        q.steps = mt_cash_steps(&g.counters, products[i]);
        q.price = quote(products[i]);
    }
    g.view = v;
}

// --- the store's policy -------------------------------------------------------------------------

int policy(void *, mt_cpu *cpu, const char *product_id) {
    const i32 product = mt_cash_product_from_id(product_id);
    if (product < 0 || g.knobs.share_pct[product] == 0) {
        g.stats.refused++;
        log("%s is not sold for meso: cancelled", product_id != nullptr ? product_id : "?");
        return 0;
    }
    const u32 hero = hero_of(cpu);
    if (hero == 0 || g.slot < 0) {
        g.stats.refused++;
        log("%s: no hero in play: cancelled", product_id);
        return 0;
    }
    read_hero(cpu, hero);
    const u32 p = static_cast<u32>(product);
    const u32 steps = mt_cash_steps(&g.counters, p);
    const i64 price = quote(p);
    if (price <= 0 || price > INT32_MAX) {
        g.stats.refused++;
        return 0;
    }
    // CHero::setMeso(hero, -price, 1): refuses with "Not enough Mesos." (CHero.setMeso.1) when
    // the hero cannot pay, else logs "Lost Meso(s)(-N)" in the game's message lines.
    const u32 args[3] = {hero, static_cast<u32>(-static_cast<i32>(price)), 1u};
    const u32 paid = call(cpu, k_set_meso, args, 3) & 0xFFu;
    if (paid == 0) {
        g.stats.refused++;
        log("%s costs %lld meso at level %d (step %u); the hero has %d: not enough, cancelled",
            mt_cash_product_title(p), static_cast<long long>(price), g.level, steps, g.meso);
        return 0;
    }
    mt_cash_counters_bought(&g.counters, p);
    g.stats.sold++;
    g.stats.last_price = price;
    log("sold %s for %lld meso at level %d (step %u; meso %d -> %d)", mt_cash_product_title(p),
        static_cast<long long>(price), g.level, steps, g.meso, rd(cpu, hero + k_hero_meso, 0));
    g.meso = rd(cpu, hero + k_hero_meso, 0);
    write_counters();
    return 1;
}

void turn_source(mt_cpu *cpu, void *user) { on_turn(cpu, user); }

} // namespace

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// The hero as the shop last saw it, the slot's counters and the view are the state's; the mode,
// the knobs and the stats are the session's and stay. The counters file is not rewritten: the
// next purchase or death writes the restored counters, as the game's own saves are left alone.

namespace mt_cash_snap {
namespace {

static_assert(sizeof(mt_cash_counters) % 4 == 0 && sizeof(mt_cash_view) % 4 == 0,
              "fixed-width words only");

bool save_shop(mt_cpu *, mt_snap::Writer &w, std::string *) {
    w.u32(static_cast<u32>(g.slot));
    w.raw(&g.counters, sizeof g.counters);
    w.boolean(g.down);
    w.u32(static_cast<u32>(g.level));
    w.u32(static_cast<u32>(g.meso));
    w.u32(static_cast<u32>(g.exp_level));
    w.u32(static_cast<u32>(g.exp_need));
    w.raw(&g.view, sizeof g.view);
    return true;
}

bool load_shop(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    State s;
    if (!r.i32(&s.slot) || !r.raw(&s.counters, sizeof s.counters) || !r.boolean(&s.down) ||
        !r.i32(&s.level) || !r.i32(&s.meso) || !r.i32(&s.exp_level) || !r.i32(&s.exp_need) ||
        !r.raw(&s.view, sizeof s.view)) {
        return false;
    }
    if (s.view.count > MT_CASH_PRODUCT_COUNT) {
        return r.fail("cash shop view: too many quotes");
    }
    *apply = [s](mt_cpu *) {
        g.slot = s.slot;
        g.counters = s.counters;
        g.down = s.down;
        g.level = s.level;
        g.meso = s.meso;
        g.exp_level = s.exp_level;
        g.exp_need = s.exp_need;
        g.view = s.view;
    };
    return true;
}

} // namespace

extern const mt_snap::Module k_snap_cashshop;
const mt_snap::Module k_snap_cashshop = {"cashshop", save_shop, load_shop};

} // namespace mt_cash_snap

extern "C" {

void mt_cash_shop_install(uint32_t mode) noexcept {
    g.mode = mode == MT_CASH_SHOP_MESO ? MT_CASH_SHOP_MESO : MT_CASH_SHOP_OFF;
    if (g.mode == MT_CASH_SHOP_MESO) {
        mt_storekit_set_policy(policy, nullptr);
        mt_game_events_add_listener(on_event, nullptr);
        mt_runloop_add_turn_source(turn_source, nullptr); // -1 if there already: fine
        g.installed = true;
    } else {
        mt_storekit_set_policy(nullptr, nullptr);
    }
}

void mt_cash_shop_uninstall(void) noexcept {
    mt_storekit_set_policy(nullptr, nullptr);
    mt_game_events_remove_listener(on_event, nullptr);
    mt_runloop_remove_turn_source(turn_source, nullptr);
    g = State{};
}

uint32_t mt_cash_shop_mode(void) noexcept { return g.mode; }

void mt_cash_shop_set_knobs(const mt_cash_knobs *knobs) noexcept {
    g.knobs = knobs != nullptr ? *knobs : *mt_cash_default_knobs();
}

int mt_cash_shop_counters_path(int32_t slot, char *out, uint32_t size) noexcept {
    if (out == nullptr || size == 0 || slot < 0 || slot > 9) {
        return -1;
    }
    const char *docs = mt_saves_documents_dir();
    if (docs == nullptr || docs[0] == '\0') {
        out[0] = '\0';
        return -1;
    }
    const int n = std::snprintf(out, size, "%s/" MT_SAVES_COUNTERS_PREFIX "%d", docs, slot);
    return n > 0 && static_cast<u32>(n) < size ? 0 : -1;
}

void mt_cash_shop_turn(mt_cpu *cpu) noexcept {
    if (cpu != nullptr) {
        on_turn(cpu, nullptr);
    }
}

mt_cash_view mt_cash_shop_view(void) noexcept { return g.view; }

int64_t mt_cash_shop_quote(uint32_t product) noexcept { return quote(product); }

mt_cash_counters mt_cash_shop_counters(void) noexcept { return g.counters; }

mt_cash_stats mt_cash_shop_get_stats(void) noexcept { return g.stats; }

} // extern "C"
