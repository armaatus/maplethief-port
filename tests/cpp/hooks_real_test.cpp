// Guest function hooks in the real game (#143, ADR 0069): the main menu's three Nexon "NGB"
// portal items, with the OpenPortalURLAndExit hook (maplethief/hooks.h) on and off.
//
// The recompiled game boots from work/ as MapleThief does with --fixed-step 1 (the virtual
// clock, 1 ms per run-loop turn) from a fresh data directory, to the main menu. From the run
// loop's turn hook the test finds the engine's state (SkyCanvas 0x11A950 -> the current state
// +0x0C) and waits until it is CStateMainMenu (vtable slot 6 = OnKeyPressed 0x9462D). Then it
// puts the menu where the portal items are (docs/re/network.md: popup 1, sub-menu 3, the
// highlighted item +0x2C = 2, 3 or 4) and sends OK (-5) to CStateMainMenu::OnKeyPressed, the
// game's own key handler, as a key pad or menu button does. That handler passes the item's URL
// to OpenPortalURLAndExit 0x22955, which ends in exit(1) on the device.
//
//   hooks_real_test on OUT_DIR    (runtime.hooks_portal_on)
//     The hook on: items 2, 3 and 4 one after the other. Each call returns into the menu
//     (the hook handled it, the original's exit never ran), and the game goes on: more turns
//     and GL frames for 2 s, then the test ends the run itself (stopped, not exited).
//
//   hooks_real_test off OUT_DIR   (runtime.hooks_portal_off)
//     The hook registered but off, as the original: item 2 ends the game with exit(1) inside
//     the key handler, and the run's outcome is "exited" with status 1.
//
// Local only: it needs work/gen (python -m recomp all), the slice and the extracted bundle, and
// exits 77 (skipped) without them. Output (the data directory) goes to the test output
// directory (build/, never committed).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "clock.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Guest facts (docs/re/network.md, the decompiler of CStateMainMenu::OnKeyPressed 0x9462D).
constexpr u32 k_canvas = 0x11A950;        // SkyCanvas::GetInstance's instance; +0x0C the state
constexpr u32 k_on_key_pressed = 0x9462D; // CStateMainMenu::OnKeyPressed(state, key), slot 6
constexpr u32 k_popup_ptr = 0xF1434;      // -> the open popup's id (-1: none; the portals: 1)
constexpr u32 k_busy_ptr = 0xF144C;       // -> an object whose +0x14 must be 0 for OK
constexpr u32 k_sub_menu = 0x11AA04;      // the popup's sub-menu (the portals: 3)
constexpr u32 k_item = 0x2C;              // CStateMainMenu: the highlighted item (2, 3, 4)
constexpr u32 k_key_ok = static_cast<u32>(-5);

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    g_failures += ok ? 0 : 1;
}

u32 rd(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
void wr(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

// A guest call from the turn hook, the caller's registers kept.
u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

// The engine's current state, if it is CStateMainMenu; else 0.
u32 main_menu(const mt_cpu *cpu) {
    const u32 canvas = rd(cpu, k_canvas);
    const u32 state = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
    const u32 vtable = state != 0 ? rd(cpu, state) : 0;
    return vtable != 0 && rd(cpu, vtable + 6 * 4) == k_on_key_pressed ? state : 0;
}

struct Run {
    bool on = false;
    fe::VirtualClock clock;
    u64 turns = 0;
    double first = 0;
    double menu_at = -1; // when CStateMainMenu was first current
    double done_at = -1; // the "on" run: when the three items were sent
    u64 turns_after = 0;
    u32 frames_at_done = 0;
    u32 gl_frames = 0;
    bool sent = false;
    bool ended = false;
};
Run *g_run = nullptr;

int on_present(void *, mt_cpu *, const mt_gles_frame *) {
    g_run->gl_frames++;
    return 0;
}

// Puts the menu on the portal item and sends OK; returns when OnKeyPressed returns.
void send_portal_ok(mt_cpu *cpu, u32 state, u32 item) {
    const u32 popup = rd(cpu, k_popup_ptr);
    wr(cpu, popup, 1);
    wr(cpu, k_sub_menu, 3);
    wr(cpu, state + k_item, item);
    call(cpu, k_on_key_pressed, {state, k_key_ok});
}

int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    const double t = now - r.first;
    r.clock.advance(1000000);
    const u32 state = main_menu(cpu);
    if (r.menu_at < 0 && state != 0) {
        r.menu_at = t;
    }
    if (r.menu_at < 0) {
        if (t > 20.0) {
            check(false, "CStateMainMenu is the engine's state within 20 s");
            return 1;
        }
        return 0;
    }
    // 3 s after the menu came up (it has slid in, #122), once.
    if (!r.sent && t >= r.menu_at + 3.0) {
        r.sent = true;
        const u32 popup = rd(cpu, k_popup_ptr);
        const u32 busy = rd(cpu, rd(cpu, k_busy_ptr) + 0x14);
        char what[160];
        std::snprintf(what, sizeof what,
                      "the main menu at %.3f s: CStateMainMenu 0x%08X, popup id %d, OK not "
                      "blocked (+0x14 = %u)",
                      t, static_cast<unsigned>(state), static_cast<int>(rd(cpu, popup)),
                      static_cast<unsigned>(busy));
        check(popup != 0 && static_cast<int>(rd(cpu, popup)) == -1 && busy == 0, what);
        const u32 old_popup = rd(cpu, popup);
        const u32 old_sub = rd(cpu, k_sub_menu);
        const u32 old_item = rd(cpu, state + k_item);
        if (!r.on) {
            // As the original: exit(1) inside the handler ends the run here.
            send_portal_ok(cpu, state, 2);
            check(false, "the portal item ended the game (the call returned)");
            return 1;
        }
        for (const u32 item : {2u, 3u, 4u}) {
            const mt_hook_stats before = mt_hook_get_stats(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT);
            send_portal_ok(cpu, state, item);
            const mt_hook_stats after = mt_hook_get_stats(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT);
            check(after.entries == before.entries + 1 && after.handled == before.handled + 1 &&
                      after.passed == before.passed,
                  "item " + std::to_string(item) +
                      ": OK went through OpenPortalURLAndExit, which the hook handled, and "
                      "CStateMainMenu::OnKeyPressed returned to the run loop");
            check(main_menu(cpu) == state,
                  "item " + std::to_string(item) + ": CStateMainMenu is still the engine's state");
        }
        // The menu as it was: no popup open.
        wr(cpu, popup, old_popup);
        wr(cpu, k_sub_menu, old_sub);
        wr(cpu, state + k_item, old_item);
        r.done_at = t;
        r.frames_at_done = r.gl_frames;
        return 0;
    }
    if (r.done_at >= 0) {
        r.turns_after++;
        if (t >= r.done_at + 2.0) {
            check(r.turns_after >= 1900 && r.gl_frames > r.frames_at_done + 30 &&
                      main_menu(cpu) != 0,
                  "the game goes on after the portal items: " + std::to_string(r.turns_after) +
                      " turns, " + std::to_string(r.gl_frames - r.frames_at_done) +
                      " GL frames in 2 s, still in the main menu");
            r.ended = true;
            return 1;
        }
    }
    return 0;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int run(bool on, const fs::path &out) {
    const fs::path data = out / "data";
    fs::remove_all(data);
    fs::create_directories(data);
    Run r;
    r.on = on;
    g_run = &r;
    mt_hook_reset();
    mt_hooks_install_portal_no_exit(on ? 1 : 0);
    check(mt_hook_enabled(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT) == (on ? 1 : 0),
          std::string("the OpenPortalURLAndExit hook is registered and ") + (on ? "on" : "off"));

    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    mt_stubs_set_data_dir(data.string().c_str());
    mt_diag_configure_from_env();
    mt_gles_config gl{};
    gl.present = on_present;
    mt_gles_configure(&gl);
    mt_input_configure(nullptr);
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_import_config_from_env(&cfg.imports);
    cfg.app_main = mt_uikit_app_main;
    cfg.app_user = &ui;
    if (!r.clock.install()) {
        check(false, "the virtual clock is installed");
        return 1;
    }
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    mt_program prog = MT_PROGRAM_GENERATED;
    mt_process *p = mt_process_load(&cfg, &prog, slice.data(), slice.size());
    if (p == nullptr) {
        check(false, "the boot loads the slice");
        return 1;
    }
    const std::uint32_t outcome = mt_process_run(p);
    const mt_process_result *res = mt_process_result_of(p);
    if (outcome == MT_PROCESS_TRAPPED) {
        std::printf("trap kind %u at pc 0x%08X detail 0x%08X\n", res->trap_kind, res->trap_pc,
                    res->trap_detail);
    }
    const mt_hook_stats st = mt_hook_get_stats(MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT);
    if (on) {
        check(outcome == MT_PROCESS_STOPPED && r.ended,
              "the run went on until the test ended it (" + std::to_string(r.turns) + " turns)");
        check(st.entries == 3 && st.handled == 3 && st.passed == 0,
              "OpenPortalURLAndExit: 3 calls, 3 handled by the hook");
    } else {
        check(outcome == MT_PROCESS_EXITED && res->exit_status == 1 && r.sent,
              "the portal item ended the game with exit(1), as on the device (outcome " +
                  std::to_string(outcome) + ", status " + std::to_string(res->exit_status) + ")");
        check(st.entries == 1 && st.handled == 0 && st.passed == 1,
              "OpenPortalURLAndExit: 1 call, the original ran (the hook off)");
    }
    mt_process_destroy(p);
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    r.clock.uninstall();
    mt_hook_reset();
    g_run = nullptr;
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (argc != 3 || (mode != "on" && mode != "off")) {
        std::printf("usage: hooks_real_test on|off OUT_DIR\n");
        return 2;
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    const fs::path out = argv[2];
    fs::create_directories(out);
    if (run(mode == "on", out) != 0) {
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without generated code (work/gen: python -m recomp all)\n");
    return 77;
}

#endif
