// Remapped bindings in the real game (#70, ADR 0054). The recompiled game boots from work/ as
// MapleThief does with --fixed-step 1 (the virtual clock, 1 ms per run-loop turn), with a data
// directory whose bindings.ini remaps attack in the field to V and to the right stick's click.
// The turn hook routes SDL's events as game.cpp does: the remap overlay first (Remap), then
// Input, Gamepads (an SDL3 virtual Xbox 360 pad) and Keyboard -> Actions -> the runtime's actions.
// The bindings are read as run_game applies them (load_bindings over the defaults, into Actions
// and Gamepads). The hero is read from guest memory as in keyboard_real_test.cpp (CHero x +0x04,
// state +0x10, HP +0x94, the other-items inventory +0xD8). Local only: it needs work/gen, the
// slice and the extracted bundle, and exits 77 (skipped) without them.
//
//   remap_real_test first DATA_DIR    (input.remap_play, platform/remap_test.cmake)
//     A fresh install with the bindings file. Enter to the first map; V attacks, Z (the old key)
//     does not; the pad's right stick click attacks, X and the right trigger (its old controls)
//     do not. Then the remap overlay in the field: F1, quick slot 1, Enter, Q, Save: the file
//     now binds slot1 to Q, Q drinks a Red Potion and A (its old key) does not.
//   remap_real_test again DATA_DIR    (the same data directory: a restart)
//     The saved game (Enter, Enter: the character select and Start; the new character's save
//     plays the intro again, by Enter) to the field: the file's bindings hold: V and the right
//     stick attack, Z, X and the right trigger do not, Q drinks, A does not.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "actions.h"
#include "bindings.h"
#include "clock.h"
#include "gamepad.h"
#include "input.h"
#include "keyboard.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "remap.h"
#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

constexpr u32 k_canvas = 0x11A950;     // SkyCanvas::GetInstance's instance
constexpr u32 k_last_frame = 0x11A4D8; // CMTimer's lastFrame: set before every tick
constexpr i32 k_red_potion = 3;        // item_etc id

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
i64 rd64(const mt_cpu *cpu, u32 addr) {
    i64 v = 0;
    std::memcpy(&v, cpu->mem + addr, 8);
    return v;
}
void wr(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

struct Hero {
    u32 addr = 0;
    i32 x = 0, y = 0, state = 0, hp = 0, mp = 0;
};
Hero hero(const mt_cpu *cpu) {
    Hero h;
    const u32 canvas = rd(cpu, k_canvas);
    const u32 state = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
    h.addr = state != 0 ? rd(cpu, state + 0x30) : 0;
    if (h.addr == 0) {
        return h;
    }
    h.x = static_cast<i32>(rd(cpu, h.addr + 0x04));
    h.y = static_cast<i32>(rd(cpu, h.addr + 0x08));
    h.state = static_cast<i32>(rd(cpu, h.addr + 0x10));
    h.hp = static_cast<i32>(rd(cpu, h.addr + 0x94));
    h.mp = static_cast<i32>(rd(cpu, h.addr + 0x98));
    return h;
}
i32 item_count(const mt_cpu *cpu, u32 hero_addr, i32 id) {
    const u32 items = rd(cpu, hero_addr + 0xD8);
    i32 n = 0;
    for (u32 i = 0; items != 0 && i < 48; i++) {
        const u32 item = rd(cpu, items + 4 * i);
        if (item != 0 && static_cast<i32>(rd(cpu, item + 0x90)) == id) {
            n += static_cast<std::int16_t>(rd(cpu, item + 0x100) & 0xFFFF);
        }
    }
    return n;
}

struct Step {
    double at;
    std::function<void(mt_cpu *)> run;
};
struct Sample {
    double t;
    Hero hero;
};

struct Run {
    std::string mode;
    fs::path data;
    std::string bindings_file;
    fe::VirtualClock clock;
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Keyboard keyboard{&actions};
    fe::Remap remap{&actions, &gamepads};
    std::unique_ptr<virtual_pad::Pad> pad;
    std::vector<Step> steps;
    std::size_t next_step = 0;
    u64 turns = 0;
    double first = 0, t = 0;
    bool done = false;
    bool sampling = false;
    i64 last_tick = 0;
    std::vector<Sample> samples;
};
Run *g_run = nullptr;

// game.cpp's handle_event: the overlay first, the devices with what it does not take.
void pump(Run &r) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (!r.remap.handle(e, r.t)) {
            r.input.handle(e);
            r.gamepads.handle(e);
            r.keyboard.handle(e);
        }
        r.remap.track(e);
    }
}

void push_key(SDL_Keycode key, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.down = down;
    SDL_PushEvent(&e);
}

int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.t = now - r.first;
    pump(r);
    r.remap.tick(r.t);
    r.actions.turn();
    while (r.next_step < r.steps.size() && r.t >= r.steps[r.next_step].at) {
        r.steps[r.next_step++].run(cpu);
        pump(r);
    }
    if (r.sampling) {
        const i64 tick = rd64(cpu, k_last_frame);
        if (tick != r.last_tick) {
            r.last_tick = tick;
            r.samples.push_back(Sample{r.t, hero(cpu)});
        }
    }
    r.clock.advance(1000000);
    return r.done ? 1 : 0;
}

void at(Run &r, double t, std::function<void(mt_cpu *)> fn) {
    r.steps.push_back({t, std::move(fn)});
}
void key(Run &r, double t, SDL_Keycode k, double hold = 0.1) {
    at(r, t, [k](mt_cpu *) { push_key(k, true); });
    at(r, t + hold, [k](mt_cpu *) { push_key(k, false); });
}
void button(Run &r, double t, SDL_GamepadButton b, double hold = 0.1) {
    at(r, t, [&r, b](mt_cpu *) { r.pad->button(b, true); });
    at(r, t + hold, [&r, b](mt_cpu *) { r.pad->button(b, false); });
}
void trigger(Run &r, double t, SDL_GamepadAxis a, double hold = 0.1) {
    at(r, t, [&r, a](mt_cpu *) { r.pad->trigger(a, 0.9f); });
    at(r, t + hold, [&r, a](mt_cpu *) { r.pad->trigger(a, 0.0f); });
}

std::vector<Sample> between(const Run &r, double from, double to) {
    std::vector<Sample> out;
    for (const Sample &s : r.samples) {
        if (s.t >= from && s.t < to) {
            out.push_back(s);
        }
    }
    return out;
}
bool attacked(const std::vector<Sample> &v) {
    return std::any_of(v.begin(), v.end(),
                       [](const Sample &s) { return s.hero.state == 6 || s.hero.state == 7; });
}
std::string states(const std::vector<Sample> &v) {
    std::string out;
    for (const Sample &s : v) {
        out += " " + std::to_string(s.hero.state);
    }
    return out;
}

// A control pressed at t0 must (or must not) make the hero attack within 1 s.
void attack_check(Run &r, double t0, bool should, const std::string &what) {
    at(r, t0 + 1.0, [&r, t0, should, what](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 - 0.1, t0 + 1.0);
        check(!v.empty() && attacked(v) == should,
              what + (should ? " attacks" : " does not attack") + " (per tick:" + states(v) + ")");
    });
}

// A Red Potion by `k` from HP 3: drunk or not.
void potion_check(Run &r, double t0, SDL_Keycode k, bool should, const std::string &what) {
    const auto n0 = std::make_shared<i32>(0);
    at(r, t0 - 0.1, [n0](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        *n0 = item_count(cpu, h.addr, k_red_potion);
        wr(cpu, h.addr + 0x94, 3);
    });
    key(r, t0, k);
    at(r, t0 + 1.0, [n0, should, what](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 n = item_count(cpu, h.addr, k_red_potion);
        const bool drank = n == *n0 - 1 && h.hp > 3;
        check(drank == should, what + (should ? " drinks" : " does not drink") +
                                   " a Red Potion: " + std::to_string(*n0) + " -> " +
                                   std::to_string(n) + ", HP 3 -> " + std::to_string(h.hp));
        wr(cpu, h.addr + 0x94, 10);
    });
}

void in_field(Run &r, double t) {
    at(r, t, [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.addr != 0 && mt_input_panel() == 0 && mt_input_pad_live() != 0,
              r.mode + ": the first map, the key pad live (panel 0), the hero at x " +
                  std::to_string(h.x));
        r.sampling = true;
    });
}

// The new bindings in the field from t0 on: V and the right stick attack, Z, X and the right
// trigger do not.
void attacks(Run &r, double t0) {
    key(r, t0, SDLK_V);
    attack_check(r, t0, true, "V (the file's key)");
    key(r, t0 + 1.5, SDLK_Z);
    attack_check(r, t0 + 1.5, false, "Z (the old key)");
    button(r, t0 + 3.0, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    attack_check(r, t0 + 3.0, true, "the pad's right stick click (the file's button)");
    button(r, t0 + 4.5, SDL_GAMEPAD_BUTTON_WEST);
    attack_check(r, t0 + 4.5, false, "the pad's X (the old button)");
    trigger(r, t0 + 6.0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    attack_check(r, t0 + 6.0, false, "the pad's right trigger (the old trigger)");
}

void first(Run &r) {
    // Enter: Play, the intro's five speech boxes and MacArthur's four OKs (keyboard_real_test).
    key(r, 4.5, SDLK_RETURN);
    for (const double t : {30.0, 34.0, 38.0, 42.0, 46.0, 51.0, 52.0, 53.0, 54.0}) {
        key(r, t, SDLK_RETURN);
    }
    in_field(r, 57.0);
    attacks(r, 58.0);
    // The overlay: F1, down to quick slot 1 in the field, Enter, Q, then Save.
    key(r, 66.0, SDLK_F1);
    at(r, 66.2, [&r](mt_cpu *) {
        check(r.remap.is_open() && r.actions.held() == 0, "F1 in the field opens the overlay");
    });
    for (int i = 0; i < 6; i++) {
        key(r, 66.3 + 0.2 * i, SDLK_DOWN, 0.05); // Up Down Left Right Jump Attack -> slot1
    }
    at(r, 67.6, [&r](mt_cpu *) {
        const fe::Remap::Row &row = r.remap.rows()[r.remap.selected()];
        check(row.kind == fe::Remap::RowKind::kAction && row.slot.action == MT_ACTION_SLOT1 &&
                  row.slot.context == fe::Context::kField,
              "six Downs select quick slot 1 in the field");
    });
    key(r, 67.7, SDLK_RETURN);
    key(r, 67.9, SDLK_Q);
    at(r, 68.1, [&r](mt_cpu *) {
        check(r.remap.status() == "Quick slot 1 in the field: key Q",
              "Q is quick slot 1's key: " + r.remap.status());
        // Down to Save.
        while (r.remap.rows()[r.remap.selected()].kind != fe::Remap::RowKind::kSave) {
            push_key(SDLK_DOWN, true);
            push_key(SDLK_DOWN, false);
            pump(r);
        }
    });
    key(r, 68.3, SDLK_RETURN);
    at(r, 68.6, [&r](mt_cpu *) {
        std::ifstream in(r.bindings_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        check(!r.remap.is_open() && r.remap.saves() == 1 &&
                  text.find("\nslot1 = button y, key Q\n") != std::string::npos &&
                  text.find("\nattack = key V, button rightstick\n") != std::string::npos,
              "Save closes the overlay and writes " + r.bindings_file +
                  " (slot1 = button y, key Q; attack = key V, button rightstick)");
    });
    potion_check(r, 69.0, SDLK_Q, true, "Q (bound in the overlay)");
    potion_check(r, 70.5, SDLK_A, false, "A (its old key)");
    at(r, 72.0, [&r](mt_cpu *) { r.done = true; });
}

void again(Run &r) {
    // A saved character: Enter opens the engine's character select, Enter is Start, 1 s after
    // the first run's Play. The save is the new character's, so the intro plays again: the same
    // Enters, 1 s later.
    key(r, 3.5, SDLK_RETURN);
    key(r, 5.5, SDLK_RETURN);
    for (const double t : {31.0, 35.0, 39.0, 43.0, 47.0, 52.0, 53.0, 54.0, 55.0}) {
        key(r, t, SDLK_RETURN);
    }
    in_field(r, 58.0);
    attacks(r, 59.0);
    potion_check(r, 67.0, SDLK_Q, true, "Q (saved by the overlay in the first run)");
    potion_check(r, 68.5, SDLK_A, false, "A (its old key)");
    at(r, 70.0, [&r](mt_cpu *) { r.done = true; });
}

int boot(Run &r, const std::vector<std::uint8_t> &slice) {
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    mt_stubs_set_data_dir(r.data.string().c_str());
    mt_diag_configure_from_env();
    mt_gles_config gl{};
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
    check(outcome == MT_PROCESS_STOPPED && r.done,
          "the run went through its script and ended from the turn hook (" +
              std::to_string(r.turns) + " turns)");
    mt_process_destroy(p);
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    r.clock.uninstall();
    return 0;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc == 3 ? argv[1] : "";
    if (mode != "first" && mode != "again") {
        std::printf("usage: remap_real_test first|again DATA_DIR\n");
        return 2;
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    Run r;
    r.mode = mode;
    r.data = argv[2];
    if (mode == "first") {
        // A fresh install with a bindings file, as a player would write it.
        fs::remove_all(r.data);
        fs::create_directories(r.data);
        std::ofstream f(r.data / fe::kBindingsFileName, std::ios::binary);
        f << "# attack on V and the right stick's click\n"
             "[field]\n"
             "attack = key V, button rightstick\n";
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    // The bindings as run_game applies them (main.cpp reads the file).
    r.bindings_file = fe::bindings_path(r.data.string(), "");
    fe::BindingSet set;
    check(fe::load_bindings(r.bindings_file, &set), "read " + r.bindings_file);
    check(fe::controls_for(set.table, fe::Context::kField, MT_ACTION_ATTACK) ==
              std::vector<fe::Control>{
                  fe::Control{fe::ControlKind::kKey, static_cast<int>(SDLK_V)},
                  fe::Control{fe::ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_STICK}},
          "the file binds attack in the field to V and the right stick");
    r.actions.bindings() = set.table;
    r.gamepads.set_face_buttons(set.face);
    r.remap.set_path(r.bindings_file);
    r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
    if (!r.pad->ok()) {
        std::printf("FAIL: virtual pad: %s\n", r.pad->error.c_str());
        return 1;
    }
    g_run = &r;
    pump(r);
    r.gamepads.open_connected();
    if (mode == "first") {
        first(r);
    } else {
        again(r);
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE));
    r.pad.reset();
    g_run = nullptr;
    SDL_Quit();
    if (status != 0) {
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
