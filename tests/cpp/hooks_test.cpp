// Guest function hooks (#143, ADR 0069) with synthetic guest functions: the registry
// (maplethief/hooks.h) and MT_FN_HOOK, the generated code's way in (maplethief/recomp/calls.h),
// written here as the recompiler writes a hooked function. A replacement, a wrapper that calls
// the original with changed arguments and changes its result, a handler that passes, switching
// on and off between calls, the statistics, and the port's fixes of original bugs (#155): the
// OpenPortalURLAndExit hook on a guest with a URL in an image segment, and the early Play guard
// on a guest with SkyCanvas's global and a state. The real game's hooked calls are
// runtime.hooks_portal_on/off (hooks_real_test.cpp) and platform.early_tap(_fixed)
// (platform/input_test.cmake).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

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
