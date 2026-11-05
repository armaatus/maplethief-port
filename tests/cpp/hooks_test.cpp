// Guest function hooks (#143, ADR 0069) with synthetic guest functions: the registry
// (maplethief/hooks.h) and MT_FN_HOOK, the generated code's way in (maplethief/recomp/calls.h),
// written here as the recompiler writes a hooked function. A replacement, a wrapper that calls
// the original with changed arguments and changes its result, a handler that passes, switching
// on and off between calls, the statistics, and the port's fixes of original bugs (#155): the
// OpenPortalURLAndExit hook on a guest with a URL in an image segment, and the early Play guard
// on a guest with SkyCanvas's global and a state; and the quest page fix (#167) on a guest
// with a data manager and stand-ins for the game's quest lookups. The real game's hooked calls are
// runtime.hooks_portal_on/off (hooks_real_test.cpp) and platform.early_tap(_fixed)
// (platform/input_test.cmake), and play.popup_menu_tabs for the quest pages.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_fn = MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT;

// The "original": r0 = r0 * 10 + r1, and it counts its calls; it returns to lr like any lifted
// function.
int g_original_calls = 0;
void original(mt_cpu *MT_RESTRICT cpu) {
    g_original_calls++;
    cpu->r[0] = cpu->r[0] * 10u + cpu->r[1];
}

// The hooked function as the recompiler writes it (recomp/lift/core.py).
void hooked(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_fn, original); }

int replace(mt_cpu *cpu, const mt_hook_call *call) {
    CHECK(call->fn == k_fn);
    CHECK(call->original == original);
    mt_ret_u32(cpu, 7u);
    return MT_HOOK_HANDLED;
}

// Doubles the first argument, calls the original, adds 1 to its result.
int wrap(mt_cpu *cpu, const mt_hook_call *call) {
    *static_cast<int *>(call->user) += 1;
    cpu->r[0] *= 2u;
    call->original(cpu);
    cpu->r[0] += 1u;
    return MT_HOOK_HANDLED;
}

int pass(mt_cpu *cpu, const mt_hook_call *) {
    cpu->r[1] = 5u; // seen by the original that runs after it
    return MT_HOOK_PASS;
}

u32 run(mt_cpu &cpu, u32 a, u32 b) {
    cpu.r[0] = a;
    cpu.r[1] = b;
    cpu.r[MT_LR] = 0x1235u;
    hooked(&cpu);
    CHECK(cpu.r[MT_LR] == 0x1235u);
    return cpu.r[0];
}

struct Fresh {
    Fresh() {
        mt_hook_reset();
        g_original_calls = 0;
    }
    ~Fresh() { mt_hook_reset(); }
};

} // namespace

TEST_CASE("hooks: with no handler the original runs and the call is counted") {
    Fresh fresh;
    mt_cpu cpu{};
    CHECK(run(cpu, 3, 4) == 34u);
    CHECK(g_original_calls == 1);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 1u);
    CHECK(st.passed == 1u);
    CHECK(st.handled == 0u);
    CHECK(mt_hook_enabled(k_fn) == 0);
}

TEST_CASE("hooks: only the hookable functions take a handler") {
    Fresh fresh;
    CHECK(mt_hook_hookable(k_fn) == 1);
    CHECK(mt_hook_hookable(k_fn & ~1u) == 0); // keyed by the function pointer, Thumb bit set
    CHECK(mt_hook_hookable(0x65B55u) == 0);
    CHECK(mt_hook_register(0x65B55u, replace, nullptr, 1) == 0);
    CHECK(mt_hook_register(k_fn, nullptr, nullptr, 1) == 0);
    CHECK(mt_hook_set_enabled(k_fn, 1) == 0); // no handler yet
    CHECK(mt_hook_enabled(k_fn) == 0);
    // An address the runtime does not know is never handled: the original runs.
    mt_cpu cpu{};
    CHECK(mt_hook_entry(&cpu, 0x65B55u, original) == 0);
}

TEST_CASE("hooks: a replacement does the call and the original does not run") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, replace, nullptr, 1) == 1);
    CHECK(mt_hook_enabled(k_fn) == 1);
    CHECK(run(cpu, 3, 4) == 7u);
    CHECK(g_original_calls == 0);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 1u);
    CHECK(st.handled == 1u);
    CHECK(st.passed == 0u);
}

TEST_CASE("hooks: a wrapper calls the original with changed arguments and changes its result") {
    Fresh fresh;
    mt_cpu cpu{};
    int calls = 0;
    REQUIRE(mt_hook_register(k_fn, wrap, &calls, 1) == 1);
    CHECK(run(cpu, 3, 4) == 65u); // (3 * 2) * 10 + 4 + 1
    CHECK(calls == 1);
    CHECK(g_original_calls == 1);
    CHECK(mt_hook_get_stats(k_fn).handled == 1u);
}

TEST_CASE("hooks: a handler that passes lets the original run after it") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, pass, nullptr, 1) == 1);
    CHECK(run(cpu, 3, 4) == 35u);
    CHECK(g_original_calls == 1);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.passed == 1u);
    CHECK(st.handled == 0u);
}

TEST_CASE("hooks: switched off and on at run time, between calls") {
    Fresh fresh;
    mt_cpu cpu{};
    REQUIRE(mt_hook_register(k_fn, replace, nullptr, 0) == 1);
    CHECK(mt_hook_enabled(k_fn) == 0);
    CHECK(run(cpu, 1, 2) == 12u); // off: the original
    CHECK(mt_hook_set_enabled(k_fn, 1) == 1);
    CHECK(run(cpu, 1, 2) == 7u);
    CHECK(mt_hook_set_enabled(k_fn, 0) == 1);
    CHECK(run(cpu, 1, 2) == 12u);
    // A second registration replaces the handler and keeps the numbers.
    int calls = 0;
    REQUIRE(mt_hook_register(k_fn, wrap, &calls, 1) == 1);
    CHECK(run(cpu, 1, 2) == 23u);
    mt_hook_unregister(k_fn);
    CHECK(mt_hook_enabled(k_fn) == 0);
    CHECK(run(cpu, 1, 2) == 12u);
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 5u);
    CHECK(st.handled == 2u);
    CHECK(st.passed == 3u);
    CHECK(g_original_calls == 4);
}

namespace {

// Wrappers (#166 x #167) that log their way in and out, and add to the result on the way out:
// the log shows the chain's order, the result each level's work.
std::string g_log;
int wrap_a(mt_cpu *cpu, const mt_hook_call *call) {
    g_log += "a(";
    call->original(cpu);
    cpu->r[0] += 100u;
    g_log += ")a";
    return MT_HOOK_HANDLED;
}
int wrap_b(mt_cpu *cpu, const mt_hook_call *call) {
    g_log += "b(";
    call->original(cpu);
    cpu->r[0] += 1000u;
    g_log += ")b";
    return MT_HOOK_HANDLED;
}
int wrap_pass(mt_cpu *, const mt_hook_call *) {
    g_log += "p";
    return MT_HOOK_PASS;
}
// The handler: wraps the original and logs.
int handler_h(mt_cpu *cpu, const mt_hook_call *call) {
    CHECK(call->original == original); // the handler's original is the function's own
    g_log += "h(";
    call->original(cpu);
    cpu->r[0] += 10000u;
    g_log += ")h";
    return MT_HOOK_HANDLED;
}

} // namespace

TEST_CASE("hooks: wrappers run outside the handler, the last wrapped outermost") {
    Fresh fresh;
    g_log.clear();
    mt_cpu cpu{};
    // A wrapper before the handler is registered: the order is the same.
    REQUIRE(mt_hook_wrap(k_fn, wrap_a, nullptr, 1) == 1);
    REQUIRE(mt_hook_register(k_fn, handler_h, nullptr, 1) == 1);
    REQUIRE(mt_hook_wrap(k_fn, wrap_b, nullptr, 1) == 1);
    CHECK(mt_hook_wrapped(k_fn, wrap_a) == 1);
    CHECK(mt_hook_wrapped(k_fn, wrap_b) == 1);
    CHECK(run(cpu, 1, 2) == 11112u); // 12 + 10000 + 100 + 1000
    CHECK(g_log == "b(a(h()h)a)b");
    CHECK(g_original_calls == 1);
    // The handler off: the wrappers wrap the original.
    g_log.clear();
    CHECK(mt_hook_set_enabled(k_fn, 0) == 1);
    CHECK(run(cpu, 1, 2) == 1112u);
    CHECK(g_log == "b(a()a)b");
    // A wrapper off, then its place taken again by the same wrapper: its place is kept.
    g_log.clear();
    CHECK(mt_hook_set_enabled(k_fn, 1) == 1);
    REQUIRE(mt_hook_wrap(k_fn, wrap_a, nullptr, 0) == 1);
    CHECK(mt_hook_wrapped(k_fn, wrap_a) == 0);
    CHECK(run(cpu, 1, 2) == 11012u);
    CHECK(g_log == "b(h()h)b");
    g_log.clear();
    REQUIRE(mt_hook_wrap(k_fn, wrap_a, nullptr, 1) == 1);
    CHECK(run(cpu, 1, 2) == 11112u);
    CHECK(g_log == "b(a(h()h)a)b");
    // The handler unregistered: the wrappers stay; a wrapper removed: the rest stay.
    g_log.clear();
    mt_hook_unregister(k_fn);
    mt_hook_unwrap(k_fn, wrap_b);
    CHECK(mt_hook_wrapped(k_fn, wrap_b) == 0);
    CHECK(run(cpu, 1, 2) == 112u);
    CHECK(g_log == "a()a");
    // A wrapper that passes: the next level in runs after it.
    g_log.clear();
    REQUIRE(mt_hook_wrap(k_fn, wrap_pass, nullptr, 1) == 1);
    CHECK(run(cpu, 1, 2) == 112u);
    CHECK(g_log == "pa()a");
    const mt_hook_stats st = mt_hook_get_stats(k_fn);
    CHECK(st.entries == 6u);
    CHECK(st.handled == 6u);
    CHECK(st.passed == 0u);
    // All off: the original, counted as passed.
    g_log.clear();
    mt_hook_unwrap(k_fn, wrap_a);
    mt_hook_unwrap(k_fn, wrap_pass);
    CHECK(run(cpu, 1, 2) == 12u);
    CHECK(g_log.empty());
    CHECK(mt_hook_get_stats(k_fn).passed == 1u);
}

TEST_CASE("hooks: a function takes four wrappers, and a wrapper is refused where a handler is") {
    Fresh fresh;
    CHECK(mt_hook_wrap(0x65B55u, wrap_a, nullptr, 1) == 0);
    CHECK(mt_hook_wrap(k_fn, nullptr, nullptr, 1) == 0);
    CHECK(mt_hook_wrapped(0x65B55u, wrap_a) == 0);
    CHECK(mt_hook_wrap(k_fn, wrap_a, nullptr, 1) == 1);
    CHECK(mt_hook_wrap(k_fn, wrap_b, nullptr, 1) == 1);
    CHECK(mt_hook_wrap(k_fn, wrap_pass, nullptr, 1) == 1);
    CHECK(mt_hook_wrap(k_fn, wrap, nullptr, 1) == 1);
    CHECK(mt_hook_wrap(k_fn, handler_h, nullptr, 1) == 0); // no place left
    CHECK(mt_hook_wrap(k_fn, wrap_a, nullptr, 1) == 1);    // one there: changed in place
    mt_hook_reset();
    CHECK(mt_hook_wrapped(k_fn, wrap_a) == 0);
    CHECK(mt_hook_wrap(k_fn, handler_h, nullptr, 1) == 1);
}

TEST_CASE("hooks: OpenPortalURLAndExit returns instead of quitting when on") {
    Fresh fresh;
    // A guest with a __TEXT segment holding the URL, as the game's __cstring does.
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const std::string url = "http://w.nexonmobile.com/CP/gift.php?GV=35";
    mt_segment seg{};
    std::strcpy(seg.name, "__TEXT");
    seg.vmaddr = 0x4000;
    seg.vmsize = 0x1000;
    seg.filesize = static_cast<u32>(url.size() + 1);
    seg.initprot = MT_PROT_READ | MT_PROT_EXECUTE;
    REQUIRE(mt_mem_map_segment(mem, &seg, reinterpret_cast<const std::uint8_t *>(url.c_str()),
                               url.size() + 1) == 0);
    const u32 regs[] = {0x5000, 0x4000, 0x11, 0x22, 0x44, 0x55, 0x66,  0x77,
                        0x88,   0x99,   0xAA, 0xBB, 0xCC, 0xDD, 0x1235};
    const auto call = [&] {
        for (u32 i = 0; i < 15; i++) {
            cpu.r[i] = regs[i];
        }
        g_original_calls = 0;
        hooked(&cpu);
    };
    mt_hooks_install_portal_no_exit(1);
    CHECK(mt_hook_enabled(k_fn) == 1);
    call();
    CHECK(g_original_calls == 0); // the stand-in for exit(1) never ran
    for (u32 i = 4; i < 15; i++) {
        CHECK(cpu.r[i] == regs[i]); // r4-r11, sp and lr as the caller left them
    }
    // Off: the original runs, as on the device.
    mt_hooks_install_portal_no_exit(0);
    CHECK(mt_hook_enabled(k_fn) == 0);
    call();
    CHECK(g_original_calls == 1);
    // A URL outside the image is not read (logged as "?").
    mt_hook_set_enabled(k_fn, 1);
    cpu.r[1] = 0x00800000u;
    g_original_calls = 0;
    hooked(&cpu);
    CHECK(g_original_calls == 0);
    mt_mem_destroy(mem);
}

namespace {

constexpr u32 k_play = MT_HOOK_FN_MAIN_MENU_CLICK_PLAY;
void hooked_play(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_play, original); }

void put32(mt_cpu &cpu, u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }

} // namespace

TEST_CASE("hooks: Play before the engine's main menu is dropped when on (#122)") {
    Fresh fresh;
    // A guest whose __DATA holds SkyCanvas's global 0x11A950, as the game's image does, with a
    // canvas and a current state on the heap. The state's vtable pointer says CStateTitle
    // (0x119C98) until the engine enters the main menu.
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const std::uint8_t none[4] = {};
    mt_segment seg{};
    std::strcpy(seg.name, "__DATA");
    seg.vmaddr = 0x11A000;
    seg.vmsize = 0x1000;
    seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
    REQUIRE(mt_mem_map_segment(mem, &seg, none, sizeof none) == 0);
    const u32 canvas = mt_heap_calloc(mem, 1, 0x100);
    const u32 state = mt_heap_calloc(mem, 1, 0x40);
    REQUIRE(canvas != 0);
    REQUIRE(state != 0);
    put32(cpu, canvas + 0xC, state);
    const u32 regs[] = {0x5000, 0x6000, 0x7000, 0x22, 0x44, 0x55, 0x66,  0x77,
                        0x88,   0x99,   0xAA,   0xBB, 0xCC, 0xDD, 0x1235};
    const auto click = [&] {
        for (u32 i = 0; i < 15; i++) {
            cpu.r[i] = regs[i];
        }
        g_original_calls = 0;
        hooked_play(&cpu);
        return g_original_calls;
    };
    mt_hooks_install_early_play_guard(1);
    CHECK(mt_hook_enabled(k_play) == 1);
    // No canvas yet (the global 0): nothing to tell, the original runs.
    CHECK(click() == 1);
    put32(cpu, 0x11A950, canvas);
    // In CStateTitle: dropped. The original (which would make the ChooseCharacterView) never
    // runs, and the callee-saved registers are as the caller left them.
    put32(cpu, state, 0x119C98);
    CHECK(click() == 0);
    for (u32 i = 4; i < 15; i++) {
        CHECK(cpu.r[i] == regs[i]);
    }
    // In any other state (another vtable pointer): Play as the original.
    put32(cpu, state, 0x119D60);
    CHECK(click() == 1);
    // Off, in CStateTitle: the original runs, as on the device (and its drawRect: raises).
    put32(cpu, state, 0x119C98);
    mt_hooks_install_early_play_guard(0);
    CHECK(click() == 1);
    const mt_hook_stats stats = mt_hook_get_stats(k_play);
    CHECK(stats.entries == 4u);
    CHECK(stats.handled == 1u);
    CHECK(stats.passed == 3u);
    mt_mem_destroy(mem);
}

TEST_CASE("hooks: the Play guard reads nothing in a guest without the game's image") {
    Fresh fresh;
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    mt_hooks_install_early_play_guard(1);
    hooked_play(&cpu);
    CHECK(g_original_calls == 1);
    mt_mem_destroy(mem);
}

TEST_CASE("hooks: fix_original_bugs installs every fix, on or off together (#155)") {
    Fresh fresh;
    mt_hooks_install_original_bug_fixes(1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_MAIN_MENU_CLICK_PLAY) == 1);
    mt_hooks_install_original_bug_fixes(0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_MAIN_MENU_CLICK_PLAY) == 0);
    // Registered either way, so mt_hook_set_enabled can switch them at run time.
    CHECK(mt_hook_set_enabled(MT_HOOK_FN_MAIN_MENU_CLICK_PLAY, 1) == 1);
}

// --- PopupMenu_QuestTabOnKey 0x5462D: the quest page fix (#167) ------------------------------

namespace {

constexpr u32 k_quest_tab = MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY;
constexpr u32 k_mgr = 0x1000;    // the data manager (CDataMgr)
constexpr u32 k_owner = 0x2000;  // the handler's third argument: +0x58 the quest lists
constexpr u32 k_quests = 0x3000; // the quest lists
constexpr u32 k_scroll = 0x3800; // the keys' scroll object: +4 the first row shown
constexpr u32 k_main = 0x4000;   // what the main quest lookup 0x7A19D answers
constexpr u32 k_sides = 0x5000;  // the side quest lookup 0x7A10D answers k_sides + 0x100 * index
constexpr u32 k_garbage = 0xDEADBEE0u;

struct QuestGuest {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x10000);
    mt_cpu cpu{};
    QuestGuest() {
        cpu.mem = mem.data();
        cpu.mem_size = mem.size();
        cpu.r[MT_SP] = 0x8000u;
        put(k_owner + 0x58, k_quests);
        put(k_mgr + 0x10C, k_scroll);
        put(k_mgr + 0x530, k_garbage); // never written before the first touch on a quest
    }
    void put(u32 addr, u32 v) { std::memcpy(&mem[addr], &v, 4); }
    u32 get(u32 addr) const {
        u32 v = 0;
        std::memcpy(&v, &mem[addr], 4);
        return v;
    }
};

int g_lookups = 0;
// The original's effect on the page: OK (-5) opens it when it is closed (+0xAC 1), B (-16)
// closes it; any other key leaves it. It returns 0x77 in r0 and clobbers r2.
void quest_tab_original(mt_cpu *MT_RESTRICT cpu) {
    g_original_calls++;
    u32 open = 0;
    std::memcpy(&open, cpu->mem + cpu->r[0] + 0xAC, 4);
    const auto key = static_cast<std::int32_t>(cpu->r[1]);
    if (key == -5 && open == 0) {
        open = 1;
    } else if (key == -16) {
        open = 0;
    }
    std::memcpy(cpu->mem + cpu->r[0] + 0xAC, &open, 4);
    cpu->r[0] = 0x77u;
    cpu->r[2] = 0x5555u;
}
void hooked_quest_tab(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_quest_tab, quest_tab_original); }

void main_quest_lookup(mt_cpu *MT_RESTRICT cpu) {
    g_lookups++;
    cpu->r[0] = cpu->r[0] == k_quests ? k_main : 0u;
}
void side_quest_lookup(mt_cpu *MT_RESTRICT cpu) {
    g_lookups++;
    cpu->r[0] = cpu->r[0] == k_quests && cpu->r[1] < 3u ? k_sides + 0x100u * cpu->r[1] : 0u;
}

const u32 k_regs[] = {k_mgr, 0,    k_owner, 0x33, 0x44, 0x55, 0x66,  0x77,
                      0x88,  0x99, 0xAA,    0xBB, 0xCC, 0xDD, 0x1235};

// The key through the hooked function, as Menu_OnKey calls it; checks r4-lr are kept.
void key(QuestGuest &g, std::int32_t k) {
    for (u32 i = 0; i < 15; i++) {
        g.cpu.r[i] = k_regs[i];
    }
    g.cpu.r[1] = static_cast<u32>(k);
    hooked_quest_tab(&g.cpu);
    CHECK(g.cpu.r[0] == 0x77u); // the original's result
    for (u32 i = 4; i < 15; i++) {
        CHECK(g.cpu.r[i] == k_regs[i]);
    }
}

} // namespace

// glColor4x's import stub (0xF0F10), which the flashes' wrapper calls after a flash fill.
u32 g_gl_color[4] = {};
int g_gl_colors = 0;
void gl_color4x(mt_cpu *MT_RESTRICT cpu) {
    g_gl_colors++;
    for (u32 i = 0; i < 4; i++) {
        g_gl_color[i] = cpu->r[i];
    }
}

mt_guest_fn hooks_test_dispatch(u32 target) {
    return target == 0x7A19Du   ? main_quest_lookup
           : target == 0x7A10Du ? side_quest_lookup
           : target == 0xF0F10u ? gl_color4x
                                : nullptr;
}

TEST_CASE("hooks: OK on the quest tab stores the quest whose page it opened (#167)") {
    Fresh fresh;
    g_lookups = 0;
    QuestGuest g;
    mt_hooks_install_quest_page_fix(1);
    CHECK(mt_hook_enabled(k_quest_tab) == 1);
    // The main quest (byte +0xB4 set): OK opens its page and +0x530 is the main quest.
    g.mem[k_mgr + 0xB4] = 1;
    key(g, -5);
    CHECK(g.get(k_mgr + 0xAC) == 1u);
    CHECK(g.get(k_mgr + 0x530) == k_main);
    CHECK(g_lookups == 1);
    // OK with the page open (the original's other branch), and B: no lookup, +0x530 kept.
    key(g, -5);
    key(g, -16);
    CHECK(g.get(k_mgr + 0xAC) == 0u);
    CHECK(g.get(k_mgr + 0x530) == k_main);
    CHECK(g_lookups == 1);
    // A side quest: row +0xE4 1 with the list scrolled by 1 is side quest 2.
    g.mem[k_mgr + 0xB4] = 0;
    g.put(k_mgr + 0xE4, 1);
    g.put(k_scroll + 4, 1);
    key(g, -1); // down: no page
    CHECK(g.get(k_mgr + 0x530) == k_main);
    key(g, -5);
    CHECK(g.get(k_mgr + 0xAC) == 1u);
    CHECK(g.get(k_mgr + 0x530) == k_sides + 0x200u);
    CHECK(g_lookups == 2);
    key(g, -16);
    // A row with no quest (the lookup answers 0): +0x530 stays.
    g.put(k_mgr + 0xE4, 5);
    key(g, -5);
    CHECK(g.get(k_mgr + 0x530) == k_sides + 0x200u);
    key(g, -16);
    const mt_hook_stats st = mt_hook_get_stats(k_quest_tab);
    CHECK(st.entries == 8u);
    CHECK(st.handled == 8u);
    CHECK(g_original_calls == 8);
}

TEST_CASE("hooks: with the quest page fix off, OK leaves the data manager's quest unset") {
    Fresh fresh;
    g_lookups = 0;
    QuestGuest g;
    mt_hooks_install_quest_page_fix(0);
    g.mem[k_mgr + 0xB4] = 1;
    key(g, -5);
    CHECK(g.get(k_mgr + 0xAC) == 1u);
    // What the page's paint then reads at 0x4F1C4 (+0x14 of this word): on the device, as in
    // the port before #167, a fault.
    CHECK(g.get(k_mgr + 0x530) == k_garbage);
    CHECK(g_lookups == 0);
    CHECK(g_original_calls == 1);
}

namespace {

// A wrapper around the quest page fix, as the platform's list scroll (#166) is: after the
// original and the fix, it sees what the fix stored.
u32 g_seen_page_quest = 0;
int after_quest_tab(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 mgr = cpu->r[0];
    call->original(cpu);
    std::memcpy(&g_seen_page_quest, cpu->mem + mgr + 0x530, 4);
    return MT_HOOK_HANDLED;
}

} // namespace

TEST_CASE("hooks: a wrapper around the quest page fix runs after it, the fix on or off") {
    Fresh fresh;
    g_lookups = 0;
    QuestGuest g;
    REQUIRE(mt_hook_wrap(k_quest_tab, after_quest_tab, nullptr, 1) == 1); // first, as the game
    mt_hooks_install_quest_page_fix(1);
    g.mem[k_mgr + 0xB4] = 1;
    g_seen_page_quest = 0;
    key(g, -5);
    CHECK(g.get(k_mgr + 0x530) == k_main);
    CHECK(g_seen_page_quest == k_main); // the fix had stored it
    CHECK(g_original_calls == 1);
    key(g, -16);
    // The fix off: the wrapper still runs, around the original alone.
    mt_hooks_install_quest_page_fix(0);
    g.put(k_mgr + 0x530, k_garbage);
    key(g, -5);
    CHECK(g_seen_page_quest == k_garbage);
    CHECK(g_original_calls == 3);
    CHECK(g_lookups == 1);
    CHECK(mt_hook_get_stats(k_quest_tab).handled == 3u);
}

namespace {

// CStateGame::Paint (#158) as the recompiler writes it, around an "original" that records the
// shake amplitude it sees and does what the test asks of the frame.
constexpr u32 k_paint = MT_HOOK_FN_STATE_GAME_PAINT;
std::int32_t g_amp_seen = 0;
std::function<void(mt_cpu *)> g_frame;
void paint_original(mt_cpu *MT_RESTRICT cpu) {
    g_original_calls++;
    if (cpu->r[0] != 0u) {
        std::memcpy(&g_amp_seen, cpu->mem + cpu->r[0] + 0x488u, 4);
    }
    if (g_frame) {
        g_frame(cpu);
    }
    cpu->r[0] = 0x1234u; // the frame's own r0, kept by the wrapper
}
void hooked_paint(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_paint, paint_original); }

std::int32_t get_amp(const mt_cpu &cpu, u32 self) {
    std::int32_t v = 0;
    std::memcpy(&v, cpu.mem + self + 0x488u, 4);
    return v;
}
void set_amp(mt_cpu &cpu, u32 self, std::int32_t v) { std::memcpy(cpu.mem + self + 0x488u, &v, 4); }

// The scene runner (CStateGame::PaintExitToMenu), which Paint calls before it draws the shake:
// its "original" records the amplitude it sees and runs the test's commands.
constexpr u32 k_runner = MT_HOOK_FN_SCENE_RUNNER;
std::int32_t g_runner_saw = 0;
std::function<void(mt_cpu *)> g_commands;
void runner_original(mt_cpu *MT_RESTRICT cpu) {
    std::memcpy(&g_runner_saw, cpu->mem + cpu->r[0] + 0x488u, 4);
    if (g_commands) {
        g_commands(cpu);
    }
}
void hooked_runner(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(k_runner, runner_original); }

} // namespace

TEST_CASE("hooks: screen_shake scales a scene's shake amplitude for the frame only (#158)") {
    Fresh fresh;
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const u32 self = mt_heap_calloc(mem, 1, 0x4A0); // a CStateGame: vptr, +0x47E, +0x488
    REQUIRE(self != 0);
    put32(cpu, self, 0x11A000u);
    cpu.mem[self + 0x47E] = 1;
    set_amp(cpu, self, 21);
    const auto paint = [&] {
        g_original_calls = 0;
        g_amp_seen = -999;
        cpu.r[0] = self;
        cpu.r[MT_LR] = 0x1235u;
        hooked_paint(&cpu);
        CHECK(cpu.r[MT_LR] == 0x1235u);
        CHECK(cpu.r[0] == 0x1234u);
        CHECK(g_original_calls == 1);
        return g_amp_seen;
    };
    // 100 (the default): the hook is off and the frame sees the game's amplitude.
    mt_hooks_install_screen_shake(100);
    CHECK(mt_hook_enabled(k_paint) == 0);
    CHECK(paint() == 21);
    // 50: the frame sees half (rounded), and the game's value is back after it.
    const std::uint64_t scaled_before = mt_hooks_screen_shake_scaled();
    mt_hooks_install_screen_shake(50);
    CHECK(mt_hook_enabled(k_paint) == 1);
    CHECK(paint() == 11);
    CHECK(get_amp(cpu, self) == 21);
    CHECK(mt_hooks_screen_shake_scaled() == scaled_before + 1);
    // 0: no shake (Paint's rand[0, 0] - 0 is 0 on both axes), the game's value back after.
    mt_hooks_install_screen_shake(0);
    CHECK(paint() == 0);
    CHECK(get_amp(cpu, self) == 21);
    // Above 100 is 100.
    mt_hooks_install_screen_shake(250);
    CHECK(mt_hook_enabled(k_paint) == 0);
    CHECK(paint() == 21);
    mt_hooks_install_screen_shake(30);
    // The shake off (+0x47E clear): nothing is written.
    cpu.mem[self + 0x47E] = 0;
    CHECK(paint() == 21);
    cpu.mem[self + 0x47E] = 1;
    CHECK(paint() == 6);
    // The frame's scene runner starts a new shake (command 19, 40): the new value stays.
    g_frame = [&](mt_cpu *) { set_amp(cpu, self, 40); };
    CHECK(paint() == 6);
    CHECK(get_amp(cpu, self) == 40);
    // ... or stops it (command 20: all zero): it stays stopped.
    g_frame = [&](mt_cpu *) {
        cpu.mem[self + 0x47E] = 0;
        set_amp(cpu, self, 0);
    };
    CHECK(paint() == 12);
    CHECK(get_amp(cpu, self) == 0);
    // The frame ends the state (its memory reused: another vtable pointer): nothing written.
    cpu.mem[self + 0x47E] = 1;
    set_amp(cpu, self, 20);
    g_frame = [&](mt_cpu *) { put32(cpu, self, 0x119D60u); };
    CHECK(paint() == 6);
    CHECK(get_amp(cpu, self) == 6);
    g_frame = nullptr;
    // A null `this`: the original, nothing read or written.
    cpu.r[0] = 0;
    g_original_calls = 0;
    hooked_paint(&cpu);
    CHECK(g_original_calls == 1);
    mt_hooks_install_screen_shake(100);
    mt_mem_destroy(mem);
}

TEST_CASE(
    "hooks: screen_shake inside the frame: the scene runner's commands see the game's value") {
    Fresh fresh;
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const u32 self = mt_heap_calloc(mem, 1, 0x4A0);
    REQUIRE(self != 0);
    put32(cpu, self, 0x11A000u);
    // Paint as the game's: the runner first (0x88320), then the shake drawn from +0x488.
    std::int32_t drawn = -999;
    g_frame = [&](mt_cpu *c) {
        const u32 r0 = c->r[0];
        hooked_runner(c);
        c->r[0] = r0;
        drawn = get_amp(cpu, self);
    };
    const auto paint = [&] {
        drawn = -999;
        g_runner_saw = -999;
        cpu.r[0] = self;
        cpu.r[MT_LR] = 0x1235u;
        hooked_paint(&cpu);
        return drawn;
    };
    mt_hooks_install_screen_shake(40);
    CHECK(mt_hook_enabled(k_runner) == 1);
    // Command 19 (15) in a frame that had no shake: drawn at 40% at once, 15 again after.
    g_commands = [&](mt_cpu *) {
        cpu.mem[self + 0x47E] = 1;
        set_amp(cpu, self, 15);
    };
    CHECK(paint() == 6);
    CHECK(g_runner_saw == 0);
    CHECK(get_amp(cpu, self) == 15);
    // The next frames: the runner sees 15 (the game's), the frame draws 6.
    g_commands = nullptr;
    CHECK(paint() == 6);
    CHECK(g_runner_saw == 15);
    CHECK(get_amp(cpu, self) == 15);
    // A new command 19 (10) while it shakes: 4 drawn, 10 kept.
    g_commands = [&](mt_cpu *) { set_amp(cpu, self, 10); };
    CHECK(paint() == 4);
    CHECK(g_runner_saw == 15);
    CHECK(get_amp(cpu, self) == 10);
    // Command 20: all zero, nothing drawn, nothing written back.
    g_commands = [&](mt_cpu *) {
        cpu.mem[self + 0x47E] = 0;
        set_amp(cpu, self, 0);
    };
    CHECK(paint() == 0);
    CHECK(g_runner_saw == 10);
    CHECK(get_amp(cpu, self) == 0);
    // The runner ends the state (the scene's exit to the menu; the memory reused): nothing is
    // written after it, by the runner's wrapper or Paint's.
    cpu.mem[self + 0x47E] = 1;
    set_amp(cpu, self, 20);
    g_commands = [&](mt_cpu *) {
        put32(cpu, self, 0x119D60u);
        set_amp(cpu, self, 77);
    };
    CHECK(paint() == 77);
    CHECK(g_runner_saw == 20);
    CHECK(get_amp(cpu, self) == 77);
    // The runner outside a Paint (another `this`, or no frame): the original only.
    put32(cpu, self, 0x11A000u);
    g_commands = nullptr;
    set_amp(cpu, self, 20);
    cpu.r[0] = self;
    hooked_runner(&cpu);
    CHECK(g_runner_saw == 20);
    CHECK(get_amp(cpu, self) == 20);
    g_frame = nullptr;
    mt_hooks_install_screen_shake(100);
    CHECK(mt_hook_enabled(k_runner) == 0);
    mt_mem_destroy(mem);
}

namespace {

// The full-screen flashes (#186, docs/re/flashes.md) on a synthetic guest: SkyCanvas's global
// 0x11A950 in a __DATA segment, a canvas whose graphics context (+0x10) holds the colour
// (+0x14) and the alpha (+0x20), and the three fills as the recompiler writes them. The
// "originals" do what the game's do: Canvas_FillRectColor sets the context's colour and calls
// Canvas_FillRect, which calls FillRect with (surface, x, y, w, h at sp, context at sp + 4);
// FillRect's original records the alpha it draws with.
constexpr u32 k_flash_canvas_global = 0x11A950u;
u32 g_flash_canvas = 0, g_flash_stack = 0;
int g_fills = 0, g_fills_total = 0;
std::int32_t g_fill_alpha = 0;
u32 get32(const mt_cpu &cpu, u32 addr) {
    u32 v = 0;
    std::memcpy(&v, cpu.mem + addr, 4);
    return v;
}
void fill_rect_original(mt_cpu *MT_RESTRICT cpu) {
    g_fills++;
    g_fills_total++;
    const u32 context = get32(*cpu, cpu->r[MT_SP] + 4u);
    g_fill_alpha = static_cast<std::int32_t>(get32(*cpu, context + 0x20u));
}
void hooked_fill_rect(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_FILL_RECT, fill_rect_original);
}
void canvas_fill_original(mt_cpu *MT_RESTRICT cpu) {
    const u32 lr = cpu->r[MT_LR];
    const u32 sp = cpu->r[MT_SP];
    cpu->r[MT_SP] = g_flash_stack;
    put32(*cpu, g_flash_stack, cpu->r[3]);                   // h
    put32(*cpu, g_flash_stack + 4u, g_flash_canvas + 0x10u); // the context
    cpu->r[3] = cpu->r[2];                                   // w
    cpu->r[2] = cpu->r[1];                                   // y
    cpu->r[1] = cpu->r[0];                                   // x
    cpu->r[0] = 0x1000u;                                     // the surface
    cpu->r[MT_LR] = 0x23D7Bu;
    hooked_fill_rect(cpu);
    cpu->r[MT_SP] = sp;
    cpu->r[MT_LR] = lr;
}
void hooked_canvas_fill(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_CANVAS_FILL_RECT, canvas_fill_original);
}
u32 g_fill_colour = 0;
void canvas_fill_color_original(mt_cpu *MT_RESTRICT cpu) {
    const u32 lr = cpu->r[MT_LR];
    put32(*cpu, g_flash_canvas + 0x10u + 0x14u, g_fill_colour);
    cpu->r[MT_LR] = 0x23DBDu;
    hooked_canvas_fill(cpu);
    cpu->r[MT_LR] = lr;
}
void hooked_canvas_fill_color(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR, canvas_fill_color_original);
}

// A whole-screen fill of `colour` from return address `lr` through Canvas_FillRectColor: the
// alpha FillRect drew with, or -999 when it did not draw.
std::int32_t fill_color_from(mt_cpu &cpu, u32 lr, u32 colour) {
    g_fills = 0;
    g_fill_alpha = -999;
    g_fill_colour = colour;
    cpu.r[0] = 0;
    cpu.r[1] = 0;
    cpu.r[2] = 480;
    cpu.r[3] = 320;
    cpu.r[MT_LR] = lr;
    hooked_canvas_fill_color(&cpu);
    CHECK(cpu.r[MT_LR] == lr);
    CHECK(g_fills == (g_fill_alpha == -999 ? 0 : 1));
    return g_fill_alpha;
}
// The same through Canvas_FillRect, in the canvas's colour.
std::int32_t fill_from(mt_cpu &cpu, u32 lr) {
    g_fills = 0;
    g_fill_alpha = -999;
    cpu.r[0] = 0;
    cpu.r[1] = 0;
    cpu.r[2] = 480;
    cpu.r[3] = 320;
    cpu.r[MT_LR] = lr;
    hooked_canvas_fill(&cpu);
    CHECK(cpu.r[MT_LR] == lr);
    return g_fill_alpha;
}

int g_widescreen_fills = 0;
int widescreen_handler(mt_cpu *, const mt_hook_call *) {
    g_widescreen_fills++;
    return MT_HOOK_PASS;
}

} // namespace

TEST_CASE("hooks: mt_flash_scale_alpha scales a fill's alpha, opaque as 255 (#186)") {
    CHECK(mt_flash_scale_alpha(255u, 100u) == 255u);
    CHECK(mt_flash_scale_alpha(255u, 50u) == 128u); // 127.5 rounded up
    CHECK(mt_flash_scale_alpha(255u, 25u) == 64u);
    CHECK(mt_flash_scale_alpha(255u, 75u) == 191u);
    CHECK(mt_flash_scale_alpha(255u, 0u) == 0u);
    CHECK(mt_flash_scale_alpha(0xFFFFFFFFu, 50u) == 128u); // -1: opaque
    CHECK(mt_flash_scale_alpha(100u, 50u) == 50u);
    CHECK(mt_flash_scale_alpha(170u, 25u) == 43u); // a fade-in's step, 42.5
    CHECK(mt_flash_scale_alpha(300u, 100u) == 255u);
    CHECK(mt_flash_scale_alpha(255u, 250u) == 255u); // above 100 is 100
}

TEST_CASE("hooks: flashes scales the flash fills' alpha, skips them at 0, nothing else (#186)") {
    Fresh fresh;
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    const std::uint8_t none[4] = {};
    mt_segment seg{};
    std::strcpy(seg.name, "__DATA");
    seg.vmaddr = 0x11A000;
    seg.vmsize = 0x1000;
    seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
    REQUIRE(mt_mem_map_segment(mem, &seg, none, sizeof none) == 0);
    g_flash_canvas = mt_heap_calloc(mem, 1, 0x100);
    g_flash_stack = mt_heap_calloc(mem, 1, 0x40);
    REQUIRE(g_flash_canvas != 0);
    REQUIRE(g_flash_stack != 0);
    put32(cpu, k_flash_canvas_global, g_flash_canvas);
    const u32 alpha_at = g_flash_canvas + 0x10u + 0x20u;
    const u32 colour_at = g_flash_canvas + 0x10u + 0x14u;
    put32(cpu, alpha_at, 0xFFFFFFFFu); // opaque
    // Widescreen's FillRect handler stays the handler; the flashes wrap it.
    REQUIRE(mt_hook_register(MT_HOOK_FN_FILL_RECT, widescreen_handler, nullptr, 1) == 1);
    g_widescreen_fills = 0;
    g_gl_colors = 0;
    g_fills_total = 0;

    constexpr u32 level_up_white = 0x6200Fu, level_up_black = 0x62059u;
    constexpr u32 hp_bar = 0x5A4F1u; // any other caller of Canvas_FillRectColor
    constexpr u32 fade_in = 0x8A6EFu, fade_out = 0x8A793u;

    // 100 (the default): the hooks off; the fills draw as the game's.
    mt_hooks_install_flashes(100);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT) == 0);
    CHECK(fill_color_from(cpu, level_up_white, 0xFFFFFF) == -1);
    const mt_flash_stats before = mt_hooks_flash_stats();

    CHECK(g_gl_colors == 0);

    // 50: the level-up's white and black fills at half the alpha; the game's alpha back after,
    // and GL's current colour the game's fill's (glColor4x: 8.8 fixed, opaque 0x10000).
    mt_hooks_install_flashes(50);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT) == 1);
    CHECK(fill_color_from(cpu, level_up_white, 0xFFFFFF) == 128);
    CHECK(get32(cpu, alpha_at) == 0xFFFFFFFFu);
    CHECK(g_gl_colors == 1);
    CHECK(g_gl_color[0] == 0xFF00u);
    CHECK(g_gl_color[3] == 0x10000u);
    CHECK(fill_color_from(cpu, level_up_black, 0) == 128);
    put32(cpu, alpha_at, 255u); // the context's alpha after its reset (0x6225)
    CHECK(fill_color_from(cpu, level_up_white, 0xFFFFFF) == 128);
    CHECK(get32(cpu, alpha_at) == 255u);
    CHECK(g_gl_color[3] == 0xFF00u);
    put32(cpu, alpha_at, 0xFFFFFFFFu);
    // Each of the hero's six sites is a flash.
    for (const u32 site : {0x61CDFu, 0x61D33u, 0x6200Fu, 0x62059u, 0x621DFu, 0x62213u}) {
        CAPTURE(site);
        CHECK(fill_color_from(cpu, site, 0xFFFFFF) == 128);
    }
    // Another caller's fill (a HUD bar; the menu's dim, straight to FillRect): the game's alpha.
    CHECK(fill_color_from(cpu, hp_bar, 0xFFFFFF) == -1);
    g_fills = 0;
    cpu.r[MT_SP] = g_flash_stack;
    put32(cpu, g_flash_stack, 320);
    put32(cpu, g_flash_stack + 4u, g_flash_canvas + 0x10u);
    put32(cpu, alpha_at, 100u);
    cpu.r[MT_LR] = 0x56BF5u;
    hooked_fill_rect(&cpu);
    CHECK(g_fills == 1);
    CHECK(g_fill_alpha == 100);
    put32(cpu, alpha_at, 0xFFFFFFFFu);
    // A scene's fade-in from white is a flash; from black, or a fade-out, is not.
    put32(cpu, colour_at, 0xFFFFFF);
    CHECK(fill_from(cpu, fade_in) == 128);
    put32(cpu, alpha_at, 170u); // a fade-in's last steps: the game's alpha, scaled
    CHECK(fill_from(cpu, fade_in) == 85);
    CHECK(get32(cpu, alpha_at) == 170u);
    put32(cpu, alpha_at, 0xFFFFFFFFu);
    CHECK(fill_from(cpu, fade_out) == -1);
    put32(cpu, colour_at, 0);
    CHECK(fill_from(cpu, fade_in) == -1);
    mt_flash_stats s = mt_hooks_flash_stats();
    CHECK(s.marked == before.marked + 11u);
    CHECK(s.scaled == before.scaled + 11u);
    CHECK(s.hidden == before.hidden);
    CHECK(g_gl_colors == 11);

    // 0: the flashes are drawn at alpha 0 (unseen; FillRect's GL state as the game's); the
    // rest as the game's.
    mt_hooks_install_flashes(0);
    CHECK(fill_color_from(cpu, level_up_white, 0xFFFFFF) == 0);
    CHECK(fill_color_from(cpu, level_up_black, 0) == 0);
    CHECK(get32(cpu, alpha_at) == 0xFFFFFFFFu);
    CHECK(g_gl_color[0] == 0u);
    CHECK(g_gl_color[3] == 0x10000u);
    CHECK(fill_color_from(cpu, hp_bar, 0xFFFFFF) == -1);
    put32(cpu, colour_at, 0xFFFFFF);
    CHECK(fill_from(cpu, fade_in) == 0);
    s = mt_hooks_flash_stats();
    CHECK(s.marked == before.marked + 14u);
    CHECK(s.hidden == before.hidden + 3u);
    CHECK(g_gl_colors == 14);
    // Widescreen's handler saw exactly the fills that were drawn (the wrapper runs around it).
    CHECK(g_widescreen_fills == g_fills_total);

    // Back to 100: the original, and widescreen's handler is still there and on.
    mt_hooks_install_flashes(100);
    CHECK(fill_color_from(cpu, level_up_white, 0xFFFFFF) == -1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_FILL_RECT) == 1);
    CHECK(g_widescreen_fills == g_fills_total);
    mt_mem_destroy(mem);
}
