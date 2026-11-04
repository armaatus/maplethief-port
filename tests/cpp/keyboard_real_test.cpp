// The keyboard in the real game (#68, ADR 0048's addendum). The recompiled game boots from work/
// as MapleThief does with --fixed-step 1 (the virtual clock, 1 ms per run-loop turn); from the
// run loop's turn hook this test pushes synthetic SDL_EVENT_KEY_DOWN / SDL_EVENT_KEY_UP events
// (SDL_PushEvent) and pumps SDL's queue into the frontend's own input layer, as game.cpp does:
// Input (the activity listener) and Keyboard -> Actions -> the runtime's actions
// (maplethief/input.h). While a key is held it also pushes the key's repeats (every 33 ms, as an
// OS's auto-repeat), which must change nothing. Touches go through Input::touch, the path of
// every touch (ADR 0047). The hero is read from guest memory as in gamepad_real_test.cpp:
// SkyCanvas 0x11A950 -> the current state +0x0C (CStateGame) -> the hero +0x30 (CHero): x +0x04,
// y +0x08, state +0x10, HP +0x94, MP +0x98, the other-items inventory +0xD8, the shortcuts
// +0xE0 / +0xE4. Local only: it needs work/gen (python -m recomp all), the slice and the
// extracted bundle, and exits 77 (skipped) without them.
//
//   keyboard_real_test play OUT_DIR      (input.keyboard_play)
//     From a fresh install to the first map by keys alone (Enter: Play on the main menu, then
//     the intro's five speech boxes and MacArthur's four OKs); then walk right and left (the
//     arrows: hero x), jump (X: hero y), attack (Z: the hero's state), a skill (D, quick slot 3:
//     Double Step, MP), the potions (S and A, quick slots 2 and 1: the Blue / Red Potion counts,
//     MP and HP), the same by the number keys 3, 1 and 2, the alternatives Space and Left Ctrl,
//     the screens (Esc: the popup menu and back, M: the map and its view, C: the cash shop), and
//     focus lost while Right is held (the walk stops; the key's repeats do not restart it). The
//     new character has no skill, so the test gives it Double Step as gamepad_real_test does.
//
//   keyboard_real_test hold touch|keys OUT_FILE   (input.keyboard_hold, twice, then compared)
//     The same keys into the first map, then left 1 s, attack 1 s, right 1.5 s with attack
//     held 0.5 s of it, either as touches on the key pad (the d-pad's arms, the attack key) or as
//     the Left, Z and Right keys (with their repeats), on the same turns. Writes the hero at
//     every game tick of that stretch to OUT_FILE: the two files must be identical
//     (platform/keyboard_test.cmake compares them).
//
//   keyboard_real_test menus STEP DATA_DIR   (input.keyboard_menus: the steps fresh, new, start
//                                             and delete in turn on one data directory,
//                                             platform/keyboard_test.cmake)
//     A keyboard-only run through the main menu and the character screens, by the key codes
//     their buttons send (docs/controls/keyboard.md, "Menus before the game"):
//       fresh   a fresh install: Enter is Play, a new game in slot 0
//       new     one slot: Enter (Play: ChooseCharacterView), N asks for a new character, Esc
//               says No, N and Enter make slot 1
//       start   two slots: Enter, Esc (Back), Enter, Enter (the first slot; the focus goes
//               on to Start), Enter
//       delete  two slots: Enter (Play), Right + Enter (the 2nd slot); the Delete key does
//               nothing; Down x2 (from Start) + Enter (the Delete button) + Enter (No), Enter +
//               Left + Enter (Yes: slot 1 deleted), Esc (Back)
//       early   a fresh install (its own data directory): Enter at 0.05 s, while the engine is
//               still in CStateTitle, is dropped by the focus (#129) and the run goes on;
//               Enter at 3.5 s is Play
//     On these UIKit screens the keys move the controller focus (ADR 0051) as the game's do.
//       script  a development aid: MT_KEY_SCRIPT="T:KEY,T:tapX/Y,...,END" (SDL key names),
//               printing the shell's views and the panel as they change
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "clock.h"
#include "focus.h"
#include "input.h"
#include "keyboard.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "uikit/uikit_internal.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

// Guest facts (docs/re/program-flow.md, docs/formats/save.md).
constexpr u32 k_canvas = 0x11A950;        // SkyCanvas::GetInstance's instance
constexpr u32 k_last_frame = 0x11A4D8;    // CMTimer's lastFrame: set before every tick
constexpr u32 k_rebuild_skills = 0x7FD69; // (skill list, branch, tier)
constexpr i32 k_red_potion = 3;           // item_etc ids
constexpr i32 k_blue_potion = 6;
constexpr i32 k_double_step = 3; // skill.dat: Rogue, tier 1, active, MP 6 at level 1

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    g_failures += ok ? 0 : 1;
}
void note(const std::string &what) {
    std::printf("     %s\n", what.c_str());
    std::fflush(stdout);
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

u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

struct Hero {
    u32 addr = 0;
    i32 x = 0, y = 0, flags = 0, state = 0, hp = 0, mp = 0;
};

// The hero, if the current state has one (CStateGame +0x30).
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
    h.flags = static_cast<i32>(rd(cpu, h.addr + 0x0C));
    h.state = static_cast<i32>(rd(cpu, h.addr + 0x10));
    h.hp = static_cast<i32>(rd(cpu, h.addr + 0x94));
    h.mp = static_cast<i32>(rd(cpu, h.addr + 0x98));
    return h;
}

// The count of other-item `id` in the hero's inventory (+0xD8: 48 item pointers).
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

std::string hero_text(const Hero &h) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "x %d y %d state %d flags 0x%X HP %d MP %d", h.x, h.y, h.state,
                  static_cast<unsigned>(h.flags), h.hp, h.mp);
    return buf;
}

u32 g_eagl = 0; // the game's EAGLView, once seen

// The shell's own views on screen (the game's classes, not UIKit's), front-most last, with the
// ones that are hidden or transparent left out: "MainMenuView ChooseCharacterView".
void shell_views(mt_cpu *cpu, u32 obj, std::string *out) {
    const mt_ui::View *v = mt_ui::view_of(cpu, obj);
    if (v == nullptr || v->hidden || v->model.alpha <= 0.0) {
        return;
    }
    const char *name = mt_objc_class_name(mt_objc_current(), rd(cpu, obj));
    if (name != nullptr && std::strncmp(name, "UI", 2) != 0 && std::strncmp(name, "CA", 2) != 0) {
        *out += (out->empty() ? "" : " ") + std::string(name);
        if (std::strcmp(name, "EAGLView") == 0) {
            g_eagl = obj;
        }
    }
    for (const u32 sub : v->subviews) {
        shell_views(cpu, sub, out);
    }
}
std::string screen(mt_cpu *cpu) {
    std::string out;
    for (const u32 w : mt_ui::state(cpu).windows) {
        shell_views(cpu, w, &out);
    }
    return out;
}
// EAGLView.selectedCharacterIndex: the slot ChooseCharacterView (or the engine) chose.
i32 selected_slot(mt_cpu *cpu) {
    return g_eagl != 0 ? static_cast<i32>(mt_objc_send0(cpu, g_eagl, "selectedCharacterIndex"))
                       : -9;
}

// --- the run --------------------------------------------------------------------------------

struct Step {
    double at;
    std::function<void(mt_cpu *)> run;
};

// The hero at one game tick.
struct Sample {
    double t;
    Hero hero;
};

struct Run {
    std::string mode, variant;
    fs::path data;
    fe::VirtualClock clock;
    fe::Input input;
    fe::Actions actions{&input};
    fe::Keyboard keyboard{&actions};
    fe::Focus focus{&input}; // the UIKit screens' focus, as in the game (ADR 0051)
    std::vector<Step> steps;
    std::size_t next_step = 0;
    u64 turns = 0;
    double first = 0, t = 0;
    bool done = false;
    // The hero at every tick from `sampling` on.
    bool sampling = false;
    i64 last_tick = 0;
    std::vector<Sample> samples;
    // The keys held now (their repeats are pushed every kRepeat seconds) and key activity.
    std::vector<SDL_Keycode> held;
    double next_repeat = 0;
    u64 keyboard_activity = 0, repeats = 0;
    std::string last_screen;
};
Run *g_run = nullptr;
constexpr double kRepeat = 0.033;

void on_activity(void *user, fe::InputSource source) {
    if (source == fe::InputSource::kKeyboard) {
        static_cast<Run *>(user)->keyboard_activity++;
    }
}

void pump(Run &r) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        r.input.handle(e);
        r.keyboard.handle(e);
    }
}

void push_key(SDL_Keycode key, bool down, bool repeat) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
    e.key.down = down;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

// After every run-loop turn, as the frontend's turn hook: the held keys' repeats, SDL's events
// into Input and Keyboard, the synthetic touches, then the script's steps that are due, the hero
// at a new tick, and 1 ms more of virtual time.
int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.t = now - r.first;
    if (!r.held.empty() && r.t >= r.next_repeat) {
        for (const SDL_Keycode k : r.held) {
            push_key(k, true, true);
            r.repeats++;
        }
        r.next_repeat = r.t + kRepeat;
    }
    pump(r);
    r.actions.turn();
    r.focus.turn(cpu, now);
    while (r.next_step < r.steps.size() && r.t >= r.steps[r.next_step].at) {
        r.steps[r.next_step++].run(cpu);
        pump(r); // the key reaches the action layer in this turn
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

// A touch tap at a screen point (as --tap: down, up 0.1 s later).
void tap(Run &r, double t, float x, float y) {
    at(r, t, [&r, x, y](mt_cpu *) {
        r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, x, y);
    });
    at(r, t + 0.1, [&r, x, y](mt_cpu *) {
        r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_ENDED, x, y);
    });
}

// A key: down at t (then its repeats while held), up `hold` seconds later.
void key(Run &r, double t, SDL_Keycode k, double hold = 0.1) {
    at(r, t, [&r, k](mt_cpu *) {
        push_key(k, true, false);
        r.held.push_back(k);
        r.next_repeat = r.t + 0.25; // the first repeat comes later, as an OS's does
    });
    at(r, t + hold, [&r, k](mt_cpu *) {
        r.held.erase(std::remove(r.held.begin(), r.held.end(), k), r.held.end());
        push_key(k, false, false);
    });
}

// The samples in [from, to).
std::vector<Sample> between(const Run &r, double from, double to) {
    std::vector<Sample> out;
    for (const Sample &s : r.samples) {
        if (s.t >= from && s.t < to) {
            out.push_back(s);
        }
    }
    return out;
}

bool saw_state(const std::vector<Sample> &v, i32 state) {
    return std::any_of(v.begin(), v.end(), [&](const Sample &s) { return s.hero.state == state; });
}

// Logs the shell's views whenever they change (from `from` on, every 0.1 s).
void watch_screen(Run &r, double from, double to) {
    for (double t = from; t < to; t += 0.1) {
        at(r, t, [&r](mt_cpu *cpu) {
            const std::string s = screen(cpu) + ", slot " + std::to_string(selected_slot(cpu));
            if (s != r.last_screen) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.1f s: ", r.t);
                note(buf + s + ", panel " + std::to_string(mt_input_panel()));
                r.last_screen = s;
            }
        });
    }
}

bool has_file(const Run &r, const char *name) { return fs::exists(r.data / "Documents" / name); }

// Into the first map by keys: Enter on the main menu (Play), then Enter for the intro's five
// speech boxes and MacArthur's four OKs.
void to_field(Run &r) {
    key(r, 4.5, SDLK_RETURN);
    for (const double t : {30.0, 34.0, 38.0, 42.0, 46.0, 51.0, 52.0, 53.0, 54.0}) {
        key(r, t, SDLK_RETURN);
    }
    at(r, 57.0, [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.addr != 0 && mt_input_panel() == 0 && mt_input_pad_live() != 0,
              "Enter went through the main menu, the intro's speech boxes and MacArthur's "
              "dialog: the first map, the key pad's panel 0 live, the hero at " +
                  hero_text(h));
        r.sampling = true;
    });
}

// --- play -------------------------------------------------------------------------------------

// Gives the new character Double Step on quick slot 3, as the game would after the first job
// advancement: job tier 1, the skill list rebuilt by the game (0x7FD69), level 1.
void give_double_step(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    wr(cpu, h.addr + 0x8C, 1);
    const u32 list = rd(cpu, h.addr + 0x114);
    call(cpu, k_rebuild_skills, {list, 0, 1});
    bool found = false;
    for (u32 i = 0; i < 13; i++) {
        if (static_cast<i32>(rd(cpu, list + 8 + 8 * i)) == k_double_step) {
            wr(cpu, list + 0xC + 8 * i, 1);
            found = true;
        }
    }
    wr(cpu, h.addr + 0xE0 + 8 * 3, 2); // kind 2: a skill
    wr(cpu, h.addr + 0xE4 + 8 * 3, static_cast<u32>(k_double_step));
    check(found, "the skill list of a first-job Rogue (0x7FD69) has Double Step: on quick slot 3");
}

void walk(Run &r, double t0, SDL_Keycode k, bool right, const std::string &what) {
    key(r, t0, k, 1.0);
    at(r, t0 + 1.5, [&r, t0, right, what](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 - 0.1, t0 + 1.5);
        const i32 dx = v.empty() ? 0 : v.back().hero.x - v.front().hero.x;
        check((right ? dx >= 50 : dx <= -50) && saw_state(v, 3),
              what + ": x " +
                  (v.empty() ? "?"
                             : std::to_string(v.front().hero.x) + " -> " +
                                   std::to_string(v.back().hero.x)));
    });
}

void skill(Run &r, double t0, SDL_Keycode k, const std::string &what) {
    const auto mp0 = std::make_shared<i32>(0);
    at(r, t0 - 0.1, [mp0](mt_cpu *cpu) {
        wr(cpu, hero(cpu).addr + 0x98, 10); // full MP (a new character's 10) for Double Step
        *mp0 = 10;
    });
    key(r, t0, k);
    at(r, t0 + 1.0, [&r, t0, mp0, what](mt_cpu *cpu) {
        const std::vector<Sample> v = between(r, t0 - 0.1, t0 + 1.0);
        const i32 mp = hero(cpu).mp;
        check(saw_state(v, 11) && mp == *mp0 - 6, what + " uses Double Step (state 11): MP " +
                                                      std::to_string(*mp0) + " -> " +
                                                      std::to_string(mp));
    });
}

// A potion: `blue` (MP, from MP 4) or red (HP, from HP 3).
void potion(Run &r, double t0, SDL_Keycode k, bool blue, const std::string &what) {
    const auto n0 = std::make_shared<i32>(0);
    at(r, t0 - 0.1, [n0, blue](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        *n0 = item_count(cpu, h.addr, blue ? k_blue_potion : k_red_potion);
        wr(cpu, h.addr + (blue ? 0x98 : 0x94), blue ? 4 : 3);
    });
    key(r, t0, k);
    at(r, t0 + 1.0, [n0, blue, what](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 n = item_count(cpu, h.addr, blue ? k_blue_potion : k_red_potion);
        const i32 v = blue ? h.mp : h.hp;
        check(n == *n0 - 1 && v == 10, what + " drinks a " + (blue ? "Blue" : "Red") + " Potion: " +
                                           std::to_string(*n0) + " -> " + std::to_string(n) + ", " +
                                           (blue ? "MP 4" : "HP 3") + " -> " + std::to_string(v));
    });
}

void play(Run &r) {
    watch_screen(r, 2.0, 6.0);
    to_field(r);
    at(r, 57.5, [](mt_cpu *cpu) { give_double_step(cpu); });
    walk(r, 58.0, SDLK_RIGHT, true, "Right held 1 s walks right (state 3)");
    walk(r, 60.0, SDLK_LEFT, false, "Left held 1 s walks left (state 3)");
    key(r, 62.0, SDLK_X);
    at(r, 63.0, [&r](mt_cpu *) {
        const std::vector<Sample> v = between(r, 61.9, 63.0);
        i32 top = v.empty() ? 0 : v.front().hero.y;
        for (const Sample &s : v) {
            top = std::min(top, s.hero.y);
        }
        const bool jumped =
            !v.empty() && top <= v.front().hero.y - 20 && v.back().hero.y == v.front().hero.y;
        check(jumped && saw_state(v, 5),
              "X jumps (state 5): y " + (v.empty() ? "?"
                                                   : std::to_string(v.front().hero.y) + " up to " +
                                                         std::to_string(top) + " and back"));
    });
    key(r, 63.5, SDLK_Z);
    at(r, 64.5, [&r](mt_cpu *) {
        const std::vector<Sample> v = between(r, 63.4, 64.5);
        std::string states;
        for (const Sample &s : v) {
            states += " " + std::to_string(s.hero.state);
        }
        check(saw_state(v, 6) || saw_state(v, 7),
              "Z attacks (the hero's state 6 or 7, per tick:" + states + ")");
    });
    skill(r, 65.0, SDLK_D, "D (quick slot 3)");
    potion(r, 66.5, SDLK_S, true, "S (quick slot 2)");
    potion(r, 68.0, SDLK_A, false, "A (quick slot 1)");
    skill(r, 69.5, SDLK_3, "3 (quick slot 3)");
    potion(r, 71.0, SDLK_1, false, "1 (quick slot 1)");
    potion(r, 72.5, SDLK_2, true, "2 (quick slot 2)");
    // The alternatives: Space jumps, Left Ctrl attacks.
    key(r, 74.0, SDLK_SPACE);
    at(r, 75.0, [&r](mt_cpu *) {
        const std::vector<Sample> v = between(r, 73.9, 75.0);
        check(saw_state(v, 5), "Space jumps (state 5)");
    });
    key(r, 75.5, SDLK_LCTRL);
    at(r, 76.5, [&r](mt_cpu *) {
        const std::vector<Sample> v = between(r, 75.4, 76.5);
        check(saw_state(v, 6) || saw_state(v, 7), "Left Ctrl attacks (state 6 or 7)");
    });
    // The screens: Esc opens the popup menu (panel 1) and closes it, M the map (panel 8), whose
    // view M switches and Esc closes, C the cash shop (panel 9, or a popup, panel 7, while the
    // tutorial forbids it), which Esc closes.
    const auto panel_is = [&r](double t, std::initializer_list<i32> panels, std::string what) {
        const std::vector<i32> want(panels);
        at(r, t, [want, what](mt_cpu *) {
            const i32 p = mt_input_panel();
            check(std::find(want.begin(), want.end(), p) != want.end(),
                  what + " (panel " + std::to_string(p) + ")");
        });
    };
    key(r, 77.0, SDLK_ESCAPE);
    panel_is(77.9, {1}, "Esc in the field opens the popup menu");
    key(r, 78.0, SDLK_ESCAPE);
    panel_is(78.9, {0}, "Esc in the popup menu closes it");
    key(r, 79.0, SDLK_M);
    panel_is(79.9, {8}, "M opens the map");
    key(r, 80.0, SDLK_M);
    panel_is(80.9, {8}, "M on the map switches its view");
    key(r, 81.0, SDLK_ESCAPE);
    panel_is(81.9, {0}, "Esc closes the map");
    key(r, 82.0, SDLK_C);
    panel_is(82.9, {9, 7}, "C opens the cash shop (or its 'not now' popup)");
    key(r, 83.0, SDLK_ESCAPE);
    panel_is(83.9, {0}, "Esc closes it");
    // Focus lost while Right is held (and repeating): the walk stops, and the key's repeats do
    // not start it again.
    key(r, 84.0, SDLK_RIGHT, 2.0);
    at(r, 84.5, [](mt_cpu *) {
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        SDL_PushEvent(&e);
    });
    at(r, 86.0, [&r](mt_cpu *) {
        const std::vector<Sample> walking = between(r, 84.0, 84.5);
        const std::vector<Sample> after = between(r, 84.8, 86.0);
        const bool walked = !walking.empty() && walking.back().hero.x > walking.front().hero.x;
        const bool stopped =
            !after.empty() && after.back().hero.x == after.front().hero.x && !saw_state(after, 3);
        check(walked && stopped && r.actions.held() == 0,
              "focus lost while Right is held: the walk stops at x " +
                  (after.empty() ? std::string("?") : std::to_string(after.back().hero.x)) +
                  " and the key's repeats do not restart it");
    });
    at(r, 86.5, [&r](mt_cpu *) {
        check(r.repeats > 0 && r.actions.held() == 0,
              "the held keys' " + std::to_string(r.repeats) +
                  " repeats were ignored; nothing is held at the end");
        check(r.keyboard_activity > 0, "the activity listener heard " +
                                           std::to_string(r.keyboard_activity) +
                                           " key presses (#71)");
        r.done = true;
    });
}

// --- hold -------------------------------------------------------------------------------------

// The key pad's touch points (docs/controls/keypad.md's mask table): the d-pad's left and right
// arms and the attack key.
constexpr float k_left[2] = {35, 257};
constexpr float k_right[2] = {132, 257};
constexpr float k_attack[2] = {440, 290};
constexpr double k_hold_from = 57.5, k_hold_to = 63.5;

void hold(Run &r, bool touch) {
    to_field(r);
    const auto finger = [&r](double t, std::uint32_t id, std::uint32_t phase, const float *p) {
        at(r, t, [&r, id, phase, p](mt_cpu *) {
            r.input.touch(fe::InputSource::kFinger, id, phase, p[0], p[1]);
        });
    };
    if (touch) {
        finger(58.0, 1, MT_UIKIT_TOUCH_BEGAN, k_left);
        finger(59.0, 1, MT_UIKIT_TOUCH_ENDED, k_left);
        finger(59.5, 2, MT_UIKIT_TOUCH_BEGAN, k_attack);
        finger(60.5, 2, MT_UIKIT_TOUCH_ENDED, k_attack);
        finger(61.0, 1, MT_UIKIT_TOUCH_BEGAN, k_right);
        finger(61.5, 2, MT_UIKIT_TOUCH_BEGAN, k_attack);
        finger(62.0, 2, MT_UIKIT_TOUCH_ENDED, k_attack);
        finger(62.5, 1, MT_UIKIT_TOUCH_ENDED, k_right);
    } else {
        key(r, 58.0, SDLK_LEFT, 1.0);
        key(r, 59.5, SDLK_Z, 1.0);
        key(r, 61.0, SDLK_RIGHT, 1.5);
        key(r, 61.5, SDLK_Z, 0.5);
    }
    at(r, k_hold_to, [&r, touch](mt_cpu *) {
        const std::vector<Sample> v = between(r, k_hold_from, k_hold_to);
        i32 min_x = 1 << 30, max_x = -(1 << 30);
        for (const Sample &s : v) {
            min_x = std::min(min_x, s.hero.x);
            max_x = std::max(max_x, s.hero.x);
        }
        check(!v.empty() && max_x - min_x >= 50 && saw_state(v, 3) && saw_state(v, 6),
              std::string(r.variant) + ": " + std::to_string(v.size()) +
                  " ticks: the hero walked (x " + std::to_string(min_x) + ".." +
                  std::to_string(max_x) + ") and attacked");
        if (!touch) {
            check(r.repeats > 0, "the held keys' " + std::to_string(r.repeats) +
                                     " repeats changed nothing (the same file as the touches)");
        }
        r.done = true;
    });
}

// --- menus ------------------------------------------------------------------------------------

// The screen and the key pad's panel now, for a check's text.
std::string where(mt_cpu *cpu) {
    const std::string s = screen(cpu);
    return "[" + (s.empty() ? std::string("no shell view") : s) + ", panel " +
           std::to_string(mt_input_panel()) + "]";
}
bool shows(mt_cpu *cpu, const char *view) { return screen(cpu).find(view) != std::string::npos; }

std::string files(const Run &r) {
    std::string out;
    for (const char *f : {"theif0", "theif1", "theif2"}) {
        if (has_file(r, f)) {
            out += (out.empty() ? "" : " ") + std::string(f);
        }
    }
    return out.empty() ? "no slot" : out;
}

// A check at time t: `ok(cpu)`, described by `what` plus the screen and the slots.
void check_at(Run &r, double t, std::function<bool(mt_cpu *)> ok, const std::string &what) {
    at(r, t, [&r, ok, what](mt_cpu *cpu) {
        check(ok(cpu), r.variant + ": " + what + " " + where(cpu) + ", " + files(r));
    });
}

// The steps of input.keyboard_menus, in this order on one data directory
// (platform/keyboard_test.cmake). Keys only, except where a step says "touch": what no key code
// does (docs/controls/keyboard.md, "Menus before the game"), left to #69.
void menus(Run &r, const std::string &step) {
    watch_screen(r, 1.0, 12.0);
    double end = 12.0;
    if (step == "fresh") {
        // A fresh install: Enter is Play, which starts a new game in slot 0.
        check_at(
            r, 3.4, [](mt_cpu *cpu) { return shows(cpu, "MainMenuView"); }, "the main menu is up");
        key(r, 3.5, SDLK_RETURN);
        check_at(
            r, 6.0,
            [&r](mt_cpu *cpu) {
                return hero(cpu).addr != 0 && has_file(r, "theif0") && !has_file(r, "theif1") &&
                       !shows(cpu, "MainMenuView");
            },
            "Enter on the main menu starts a new game as Play does: the game state has a hero, "
            "slot 0 is written");
        end = 6.5;
    } else if (step == "new") {
        // One slot: Enter presses the focused Play (ADR 0051: a touch), which opens
        // ChooseCharacterView; N ('#') is New: the engine's own popup (panel 7), Esc says No,
        // Enter Yes.
        key(r, 3.5, SDLK_RETURN);
        check_at(
            r, 4.4,
            [](mt_cpu *cpu) { return shows(cpu, "ChooseCharacterView") && hero(cpu).addr == 0; },
            "Enter with a saved character: the focused Play opens ChooseCharacterView, no game "
            "yet");
        key(r, 4.5, SDLK_N);
        check_at(
            r, 5.4, [](mt_cpu *) { return mt_input_panel() == 7; },
            "N ('#', the select's New) asks 'create a new character?' (the popup panel 7)");
        key(r, 5.5, SDLK_ESCAPE);
        check_at(
            r, 6.4, [&r](mt_cpu *) { return mt_input_panel() == -1 && !has_file(r, "theif1"); },
            "Esc says No: the popup closes, no new slot");
        key(r, 6.5, SDLK_N);
        check_at(r, 7.4, [](mt_cpu *) { return mt_input_panel() == 7; }, "N asks again");
        key(r, 7.5, SDLK_RETURN);
        check_at(
            r, 9.0,
            [&r](mt_cpu *cpu) {
                return hero(cpu).addr != 0 && has_file(r, "theif1") && !has_file(r, "theif2");
            },
            "Enter says Yes: a new game in slot 1");
        end = 9.5;
    } else if (step == "start") {
        // Two slots: Enter opens the select, Esc is its Back (the view goes), Enter opens it
        // again; Enter chooses the focused first slot and the focus goes to Start, Enter starts.
        key(r, 3.5, SDLK_RETURN);
        key(r, 4.5, SDLK_ESCAPE);
        check_at(
            r, 5.4,
            [](mt_cpu *cpu) {
                return hero(cpu).addr == 0 && shows(cpu, "MainMenuView") &&
                       !shows(cpu, "ChooseCharacterView");
            },
            "Enter, Esc: the select's Back, the main menu again without ChooseCharacterView");
        key(r, 5.5, SDLK_RETURN);
        check_at(
            r, 6.4,
            [](mt_cpu *cpu) { return hero(cpu).addr == 0 && shows(cpu, "ChooseCharacterView"); },
            "Enter: the select again, no game yet");
        key(r, 6.5, SDLK_RETURN); // the first slot: the focus goes on to Start (ADR 0102)
        key(r, 8.5, SDLK_RETURN); // Start
        check_at(
            r, 10.0,
            [&r](mt_cpu *cpu) {
                return hero(cpu).addr != 0 && has_file(r, "theif0") && has_file(r, "theif1") &&
                       !has_file(r, "theif2");
            },
            "Enter on the first slot (the focus then on Start), Enter (Start): the game runs "
            "with a saved character, the slots unchanged");
        end = 10.5;
    } else if (step == "delete") {
        // Two slots, by keys alone: Enter (Play), Right and Enter choose the second slot. The
        // Delete key does nothing here (ADR 0051: '*' would start the engine's delete with
        // nothing on the screen). Down, Down (from Start): Delete; Enter asks, the focus on No:
        // Enter keeps both slots. Enter asks again; Left (Yes) and Enter delete the slot.
        key(r, 3.0, SDLK_RETURN);
        key(r, 4.0, SDLK_RIGHT);
        key(r, 4.3, SDLK_RETURN);
        check_at(
            r, 5.0,
            [](mt_cpu *cpu) {
                return shows(cpu, "ChooseCharacterView") && selected_slot(cpu) == 1;
            },
            "Enter (Play) opens ChooseCharacterView, Right and Enter choose slot 1");
        key(r, 5.1, SDLK_DELETE);
        key(r, 5.5, SDLK_DOWN);   // from Start, where choosing the slot left the focus: New
        key(r, 5.8, SDLK_DOWN);   // Delete
        key(r, 6.1, SDLK_RETURN); // Delete: the alert
        key(r, 6.6, SDLK_RETURN); // its No
        check_at(
            r, 7.4,
            [&r](mt_cpu *cpu) {
                return has_file(r, "theif0") && has_file(r, "theif1") &&
                       shows(cpu, "ChooseCharacterView") && mt_input_panel() == -1;
            },
            "the Delete key does nothing; the Delete button asks, Enter on its No: both slots "
            "stay");
        key(r, 7.5, SDLK_RETURN); // Delete again (the focus is back on it)
        key(r, 8.0, SDLK_LEFT);   // Yes
        key(r, 8.5, SDLK_RETURN);
        check_at(
            r, 9.4, [&r](mt_cpu *) { return has_file(r, "theif0") && !has_file(r, "theif1"); },
            "Enter (Delete), Left (Yes), Enter: slot 1 is deleted");
        key(r, 9.5, SDLK_ESCAPE);
        check_at(
            r, 10.4,
            [](mt_cpu *cpu) {
                return hero(cpu).addr == 0 && shows(cpu, "MainMenuView") &&
                       !shows(cpu, "ChooseCharacterView");
            },
            "Esc is the select's Back: the main menu, ChooseCharacterView gone, no game");
        end = 11.5;
    } else if (step == "early") {
        // A fresh install, Enter at 0.05 s: the main menu is up, the focus on Play, but the
        // engine is still in CStateTitle (#122), so the focus drops the confirm (#129) and the
        // run goes on. Enter at 3.5 s is Play, as in the fresh step.
        check_at(
            r, 0.04,
            [&r](mt_cpu *cpu) {
                return fe::focus_engine_at_title(cpu) && r.focus.active() &&
                       r.focus.focused() != nullptr && r.focus.focused()->action == "clickPlay:";
            },
            "at 0.04 s the engine is in CStateTitle and the focus is on Play");
        key(r, 0.05, SDLK_RETURN);
        check_at(
            r, 0.6,
            [&r](mt_cpu *cpu) {
                return !fe::focus_engine_at_title(cpu) && r.focus.stats().early == 1 &&
                       r.focus.stats().taps == 0 && shows(cpu, "MainMenuView") &&
                       !shows(cpu, "ChooseCharacterView") && hero(cpu).addr == 0;
            },
            "Enter at 0.05 s is dropped: no character select, no exception, the engine at its "
            "main menu by 0.6 s");
        key(r, 3.5, SDLK_RETURN);
        check_at(
            r, 6.0,
            [&r](mt_cpu *cpu) {
                return r.focus.stats().taps == 1 && hero(cpu).addr != 0 && has_file(r, "theif0") &&
                       !shows(cpu, "MainMenuView");
            },
            "Enter at 3.5 s is Play: a new game in slot 0");
        end = 6.5;
    } else if (step == "script") {
        // A development aid: the script in MT_KEY_SCRIPT, "T:KEY,...,T:tapX/Y,...,END" with SDL
        // key names (SDL_GetKeyFromName); prints the screens it passes.
        const char *spec = std::getenv("MT_KEY_SCRIPT");
        const std::string s = spec != nullptr ? spec : "";
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t comma = s.find(',', pos);
            const std::string item =
                s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? s.size() : comma + 1;
            const std::size_t c1 = item.find(':');
            if (c1 == std::string::npos) {
                end = std::atof(item.c_str());
                continue;
            }
            const double t = std::atof(item.substr(0, c1).c_str());
            const std::string name = item.substr(c1 + 1);
            float x = 0, y = 0;
            if (std::sscanf(name.c_str(), "tap%f/%f", &x, &y) == 2) {
                tap(r, t, x, y);
            } else {
                key(r, t, SDL_GetKeyFromName(name.c_str()));
            }
        }
        watch_screen(r, 12.0, end);
    } else {
        check(false, "a menus step: fresh, new, start, delete, early or script, not " + step);
    }
    at(r, end, [&r](mt_cpu *cpu) {
        note("end: hero " + std::to_string(hero(cpu).addr != 0) + ", " + files(r));
        r.done = true;
    });
}

int boot(Run &r, const std::vector<std::uint8_t> &slice, bool fresh) {
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    if (fresh) {
        fs::remove_all(r.data);
    }
    fs::create_directories(r.data);
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
    const mt_input_stats st = mt_input_get_stats();
    std::printf(
        "input: %llu presses, %llu releases (%llu through the pad's keys, %llu key "
        "events) in %llu steps over %llu ticks; %llu waits for SkyCanvas\n",
        static_cast<unsigned long long>(st.presses), static_cast<unsigned long long>(st.releases),
        static_cast<unsigned long long>(st.pad_edges),
        static_cast<unsigned long long>(st.key_events), static_cast<unsigned long long>(st.steps),
        static_cast<unsigned long long>(st.ticks), static_cast<unsigned long long>(st.waits_busy));
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

int usage() {
    std::printf("usage: keyboard_real_test play OUT_DIR | hold touch|keys OUT_FILE | menus STEP "
                "DATA_DIR\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string variant;
    fs::path out;
    if (mode == "play" && argc == 3) {
        out = argv[2];
    } else if ((mode == "hold" || mode == "menus") && argc == 4) {
        variant = argv[2];
        out = argv[3];
        if (mode == "hold" && variant != "touch" && variant != "keys") {
            return usage();
        }
    } else {
        return usage();
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    Run r;
    // The data directory: emptied (Play starts a new game), except for the later menu steps,
    // which go on with what the earlier ones saved.
    bool fresh = true;
    if (mode == "play") {
        fs::create_directories(out);
        r.data = out / "data-play";
    } else if (mode == "hold") {
        if (out.has_parent_path()) {
            fs::create_directories(out.parent_path());
        }
        r.data = out.parent_path() / ("data-hold" + variant);
    } else {
        r.data = out;
        fresh = variant == "fresh" || variant == "early";
    }
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_EVENTS): %s\n", SDL_GetError());
        return 1;
    }
    r.mode = mode;
    r.variant = variant;
    r.input.set_activity_listener(on_activity, &r);
    r.focus.attach(r.actions);
    g_run = &r;
    if (mode == "play") {
        play(r);
    } else if (mode == "hold") {
        hold(r, variant == "touch");
    } else {
        menus(r, variant);
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE), fresh);
    if (mode == "hold") {
        // The hero at every tick of the held stretch: the file the two runs must share.
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        for (const Sample &s : between(r, k_hold_from, k_hold_to)) {
            char line[160];
            std::snprintf(line, sizeof line, "%.3f %s\n", s.t, hero_text(s.hero).c_str());
            f << line;
        }
        check(static_cast<bool>(f), "the per-tick hero written to " + out.string());
    }
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
