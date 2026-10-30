// Controller navigation in the real game (#69, ADR 0051): every menu, screen and popup done
// with a virtual gamepad only, no taps. The recompiled game boots from work/ as MapleThief does
// with --fixed-step 1 (the virtual clock, 1 ms per run-loop turn) on a data directory the
// caller gives; from the run loop's turn hook the test drives the frontend's own input layer:
// an SDL3 virtual Xbox 360 pad (tests/cpp/virtual_pad.h) -> Gamepads -> Actions -> Focus (the
// UIKit screens' focus, focus.h) or the runtime's actions (maplethief/input.h). The checks read
// the uikit trace (the button actions that ran, "uikit: action -[Class selector:]"), the save
// files and guest memory. Local only: it needs work/gen (python -m recomp all), the slice and
// the extracted bundle, and exits 77 (skipped) without them.
//
//   menu_nav_real_test MODE DATA_DIR OUT_DIR [STEP...]
//
// MODE is a flow (main_menu, terms, seed, create, select, delete, game; tutorial, hunt, shops:
// #127, past the tutorial) or "explore", which runs the STEPs "T:press BUTTON [HOLD]",
// "T:repeat BUTTON EVERY UNTIL [HOLD]", "T:stick AXIS VALUE HOLD", "T:trigger AXIS HOLD",
// "T:shot NAME", "T:state", "T:hero [EVERY UNTIL]", "T:mobs", "T:shop", "T:widgets" and the
// tasks "T:load" and "T:walkto X" (SDL's gamepad names: a, b, x, y, back, start, leftshoulder,
// dpup, ...; axes leftx, lefty, lefttrigger, righttrigger) for finding a way through the game.
// Shots (OUT_DIR/NAME.png) are for looking only: they stay in the test output directory and are
// never committed. The flows are in run_mode below, and platform/menu_nav_test.cmake runs them
// in order on one data directory (the character flows; tutorial then hunt for the save shops
// loads).
#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "actions.h"
#include "clock.h"
#include "focus.h"
#include "gamepad.h"
#include "guest_state.h"
#include "input.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "present.h"
#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u64 = std::uint64_t;

using namespace guest_state;

// Guest facts (docs/controls/keypad.md, docs/re/program-flow.md; the readers: guest_state.h).
constexpr u32 k_data_mgr = 0x5BAC1; // CDataMgr::GetInstance

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    g_failures += ok ? 0 : 1;
}

// CDataMgr::GetInstance(): the menu's data (+0xA8 the open submenu, +0xB0 the tab, +0xCC the
// shortcut panel open, docs/controls/keypad.md).
u32 data_mgr(mt_cpu *cpu) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, k_data_mgr, nullptr, 0, k_data_mgr);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}
i32 menu_tab(mt_cpu *cpu) { return static_cast<i32>(rd(cpu, data_mgr(cpu) + 0xB0)); }
i32 menu_sub(mt_cpu *cpu) { return static_cast<i32>(rd(cpu, data_mgr(cpu) + 0xA8)); }
bool shortcuts_open(mt_cpu *cpu) {
    const u32 m = data_mgr(cpu);
    return m != 0 && cpu->mem[m + 0xCC] != 0;
}

// --- the run --------------------------------------------------------------------------------

struct Step {
    double at;
    std::function<void(mt_cpu *)> run;
};

// A task of a flow that reacts to the game (#127): `step` runs every turn from the task's start
// with the seconds since then, until it returns true; past `timeout` the flow fails there.
struct Task {
    std::string what;
    double timeout;
    std::function<bool(mt_cpu *, double)> step;
};

struct Run {
    std::string mode;
    fs::path data, out;
    fe::VirtualClock clock;
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Focus focus{&input};
    fe::Screen screen{1};
    std::vector<Step> steps;
    std::size_t next_step = 0;
    u64 turns = 0;
    double first = 0, t = 0;
    bool done = false;
    std::unique_ptr<virtual_pad::Pad> pad;
    std::vector<std::string> log; // every runtime log line (trace "uikit", "focus")
    std::deque<Task> tasks;       // run one after the other, after the timed steps
    double task_start = -1;
    std::vector<SDL_GamepadButton> held; // the pad's buttons the tasks hold down
};
Run *g_run = nullptr;
void run_tasks(Run &r, mt_cpu *cpu);

void on_log(void *, const char *line) {
    std::fprintf(stderr, "%s\n", line);
    if (g_run != nullptr) {
        g_run->log.emplace_back(line);
    }
}

void pump(Run &r) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        r.input.handle(e);
        r.gamepads.handle(e);
    }
}

int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.t = now - r.first;
    pump(r);
    r.actions.turn();
    r.focus.turn(cpu, now);
    while (r.next_step < r.steps.size() && r.t >= r.steps[r.next_step].at) {
        r.steps[r.next_step++].run(cpu);
        pump(r); // the pad's change reaches the action layer in this turn
    }
    run_tasks(r, cpu);
    r.clock.advance(1000000);
    return r.done ? 1 : 0;
}

int on_present(void *, mt_cpu *, const mt_gles_frame *frame) {
    g_run->screen.take_gl_frame(*frame);
    return 0;
}

void at(Run &r, double t, std::function<void(mt_cpu *)> fn) {
    r.steps.push_back({t, std::move(fn)});
}

// A button of the pad: down at t, up `hold` seconds later.
void press(Run &r, double t, SDL_GamepadButton b, double hold = 0.1) {
    at(r, t, [&r, b](mt_cpu *) { r.pad->button(b, true); });
    at(r, t + hold, [&r, b](mt_cpu *) { r.pad->button(b, false); });
}
// The left stick pushed along `axis` to `value` for `hold` seconds.
void stick(Run &r, double t, SDL_GamepadAxis axis, Sint16 value, double hold) {
    at(r, t, [&r, axis, value](mt_cpu *) { r.pad->axis(axis, value); });
    at(r, t + hold, [&r, axis](mt_cpu *) { r.pad->axis(axis, 0); });
}

// The screen composited now (the GL frame in its place), to OUT_DIR/name.png.
void shot(Run &r, double t, const std::string &name) {
    at(r, t, [&r, name](mt_cpu *cpu) {
        if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
            return;
        }
        SDL_Surface *s = SDL_CreateSurfaceFrom(
            static_cast<int>(r.screen.width()), static_cast<int>(r.screen.height()),
            SDL_PIXELFORMAT_ABGR8888, const_cast<std::uint8_t *>(r.screen.pixels().data()),
            static_cast<int>(r.screen.width() * 4));
        const std::string path = (r.out / (name + ".png")).string();
        if (s != nullptr) {
            SDL_SavePNG(s, path.c_str());
            SDL_DestroySurface(s);
        }
        std::printf("shot %s at %.2f s\n", path.c_str(), r.t);
    });
}

std::string focus_text(const Run &r) {
    const fe::FocusTarget *f = r.focus.focused();
    if (!r.focus.active()) {
        return "focus off";
    }
    return f == nullptr ? std::string("focus on, nothing focused")
                        : "focus on -[" + f->target + " " + f->action + "]";
}

void state(Run &r, double t) {
    at(r, t, [&r](mt_cpu *cpu) {
        std::printf("state at %.2f s: panel %d, pad live %d, game mode %d, popup %d, menu tab %d "
                    "sub %d shortcuts %d, %s, %zu targets\n",
                    r.t, static_cast<int>(mt_input_panel()), mt_input_pad_live(), game_mode(cpu),
                    popup_id(cpu), menu_tab(cpu), menu_sub(cpu), shortcuts_open(cpu) ? 1 : 0,
                    focus_text(r).c_str(), r.focus.targets().size());
        std::printf("  hero: %s\n", hero_text(cpu, hero(cpu)).c_str());
        for (const fe::FocusTarget &f : r.focus.targets()) {
            std::printf("  %s (%g, %g, %g, %g) -[%s %s]\n", f.view_class.c_str(),
                        static_cast<double>(f.x), static_cast<double>(f.y),
                        static_cast<double>(f.w), static_cast<double>(f.h), f.target.c_str(),
                        f.action.c_str());
        }
        std::fflush(stdout);
    });
}

// The button actions that ran since log line `from` ("-[Class selector:]").
std::vector<std::string> actions_since(const Run &r, std::size_t from) {
    static const std::regex line(R"(uikit: action -\[(\w+) ([\w:]+)\])");
    std::vector<std::string> out;
    for (std::size_t i = from; i < r.log.size(); i++) {
        std::smatch m;
        if (std::regex_search(r.log[i], m, line)) {
            out.push_back("-[" + m[1].str() + " " + m[2].str() + "]");
        }
    }
    return out;
}

// Whether `want` ran in this order since `from` (other actions may come between).
void check_actions(const Run &r, std::size_t from, const std::vector<std::string> &want,
                   const std::string &what) {
    const std::vector<std::string> got = actions_since(r, from);
    std::size_t i = 0;
    for (const std::string &a : got) {
        if (i < want.size() && a == want[i]) {
            i++;
        }
    }
    std::string text;
    for (const std::string &a : got) {
        text += " " + a;
    }
    check(i == want.size(), what + " (actions:" + text + ")");
}

bool logged(const Run &r, std::size_t from, const std::string &needle) {
    for (std::size_t i = from; i < r.log.size(); i++) {
        if (r.log[i].find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::uintmax_t file_size(const Run &r, const char *name) {
    std::error_code ec;
    const std::uintmax_t n = fs::file_size(r.data / "Documents" / name, ec);
    return ec ? 0 : n;
}

// A checkpoint: remembers the log position at `t`, runs `fn(cpu, from)` at `t2`.
void between(Run &r, double t, double t2, std::function<void(mt_cpu *, std::size_t)> fn) {
    auto from = std::make_shared<std::size_t>(0);
    at(r, t, [&r, from](mt_cpu *) { *from = r.log.size(); });
    at(r, t2, [fn, from](mt_cpu *cpu) { fn(cpu, *from); });
}

void attach_pad(Run &r) {
    at(r, 0.5, [&r](mt_cpu *) {
        r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
        check(r.pad->ok(), "an Xbox 360 virtual pad is attached " + r.pad->error);
    });
}

void finish(Run &r, double t) {
    at(r, t, [&r](mt_cpu *) {
        const fe::Focus::Stats &s = r.focus.stats();
        std::printf(
            "focus: %llu confirms, %llu backs, %llu moves, %llu scrolls, %llu dropped, "
            "%llu forwarded\n",
            static_cast<unsigned long long>(s.taps), static_cast<unsigned long long>(s.backs),
            static_cast<unsigned long long>(s.moves), static_cast<unsigned long long>(s.scrolls),
            static_cast<unsigned long long>(s.dropped),
            static_cast<unsigned long long>(s.forwarded));
        check(r.input.live() == 0, "no touch is left down");
        r.done = true;
    });
}

constexpr SDL_GamepadButton A = SDL_GAMEPAD_BUTTON_SOUTH;
constexpr SDL_GamepadButton B = SDL_GAMEPAD_BUTTON_EAST;
constexpr SDL_GamepadButton UP = SDL_GAMEPAD_BUTTON_DPAD_UP;
constexpr SDL_GamepadButton DOWN = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
constexpr SDL_GamepadButton LEFT = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
constexpr SDL_GamepadButton RIGHT = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
constexpr SDL_GamepadButton START = SDL_GAMEPAD_BUTTON_START;
constexpr SDL_GamepadButton SELECT = SDL_GAMEPAD_BUTTON_BACK;

constexpr SDL_GamepadButton X = SDL_GAMEPAD_BUTTON_WEST;
constexpr SDL_GamepadButton Y = SDL_GAMEPAD_BUTTON_NORTH;
constexpr SDL_GamepadButton LB = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
constexpr SDL_GamepadButton RB = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;

// --- tasks: flows that react to the game (#127) ------------------------------------------------
//
// The way past the tutorial and through the shops is too long for fixed times: each task waits
// for what the game shows (the key pad's panel, the game mode, the hero's map, position and
// quests, guest_state.h) and presses the pad's buttons until it has happened, so a frame more or
// less of loading moves nothing. Still the virtual pad only: no taps, no writes to guest memory.

using Pred = std::function<bool(mt_cpu *)>;

// A button of the pad held down (or let go) by the tasks.
void hold(Run &r, SDL_GamepadButton b, bool down) {
    const auto it = std::find(r.held.begin(), r.held.end(), b);
    if (down && it == r.held.end()) {
        r.held.push_back(b);
        r.pad->button(b, true);
    } else if (!down && it != r.held.end()) {
        r.held.erase(it);
        r.pad->button(b, false);
    }
}
void release_all(Run &r) {
    while (!r.held.empty()) {
        hold(r, r.held.back(), false);
    }
}

std::string where(mt_cpu *cpu) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "panel %d, game mode %d, popup %d; ",
                  static_cast<int>(mt_input_panel()), game_mode(cpu), popup_id(cpu));
    return buf + hero_text(cpu, hero(cpu));
}

// The field: panel 0, game mode 0, no popup, a hero.
bool field(mt_cpu *cpu) {
    return mt_input_panel() == 0 && game_mode(cpu) == 0 && popup_id(cpu) == -1 &&
           hero(cpu).addr != 0;
}

void run_tasks(Run &r, mt_cpu *cpu) {
    if (r.tasks.empty() || r.pad == nullptr || r.done) {
        return;
    }
    Task &k = r.tasks.front();
    if (r.task_start < 0) {
        r.task_start = r.t;
    }
    const double el = r.t - r.task_start;
    if (k.step(cpu, el)) {
        std::printf("task done at %.2f s (%.1f s): %s\n", r.t, el, k.what.c_str());
        std::fflush(stdout);
        r.tasks.pop_front();
        r.task_start = -1;
    } else if (el > k.timeout) {
        release_all(r);
        check(false, "timed out after " + std::to_string(static_cast<int>(k.timeout)) +
                         " s: " + k.what + " (" + where(cpu) + ")");
        r.tasks.clear();
        r.done = true;
    }
    pump(r);
}

void then(Run &r, std::string what, double timeout, std::function<bool(mt_cpu *, double)> step) {
    r.tasks.push_back({std::move(what), timeout, std::move(step)});
}
void then_do(Run &r, std::string what, std::function<void(mt_cpu *)> fn) {
    then(r, std::move(what), 1, [fn](mt_cpu *cpu, double) {
        fn(cpu);
        return true;
    });
}
void then_check(Run &r, std::string what, Pred p) {
    then_do(r, what, [what, p](mt_cpu *cpu) { check(p(cpu), what + " (" + where(cpu) + ")"); });
}
void then_wait(Run &r, double s) {
    then(r, "wait " + std::to_string(s) + " s", s + 1,
         [s](mt_cpu *, double el) { return el >= s; });
}
// `b` held `hold_s` seconds, then `after` seconds with nothing held.
void then_press(Run &r, SDL_GamepadButton b, double hold_s = 0.1, double after = 0.4) {
    then(r, std::string("press ") + SDL_GetGamepadStringForButton(b), hold_s + after + 1,
         [&r, b, hold_s, after](mt_cpu *, double el) {
             hold(r, b, el < hold_s);
             return el >= hold_s + after;
         });
}
// Waits until `p` has held for `stable` seconds.
void then_wait_for(Run &r, std::string what, double timeout, Pred p, double stable = 0) {
    auto since = std::make_shared<double>(-1);
    then(r, std::move(what), timeout, [p, stable, since](mt_cpu *cpu, double el) {
        if (!p(cpu)) {
            *since = -1;
            return false;
        }
        if (*since < 0) {
            *since = el;
        }
        return el - *since >= stable;
    });
}
void then_wait_field(Run &r, double timeout = 20) {
    then_wait_for(r, "the field", timeout, field, 0.5);
}

// Walks to x (the d-pad held that way, let go within `tol`). Where the hero stops short (a
// step, a ledge), A jumps with the direction still held.
void then_walk_to(Run &r, int x, int tol = 4) {
    struct S {
        int dir = 0, last_x = INT32_MIN;
        double settled = -1, moved = 0, jumped = -10;
    };
    auto s = std::make_shared<S>();
    then(r, "walk to x " + std::to_string(x), 30, [&r, x, tol, s](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        const int d = x - h.x;
        if (std::abs(d) <= tol) {
            hold(r, LEFT, false);
            hold(r, RIGHT, false);
            hold(r, A, false);
            if (s->settled < 0) {
                s->settled = el;
            }
            if (el - s->settled < 0.3) {
                return false;
            }
            if (std::abs(x - hero(cpu).x) <= tol + 4) {
                return true;
            }
            s->dir = 0; // slid past: walk back
        }
        s->settled = -1;
        const int want = d > 0 ? 1 : -1;
        if (want != s->dir) {
            hold(r, want > 0 ? LEFT : RIGHT, false);
            s->dir = want;
            s->moved = el;
        }
        hold(r, want > 0 ? RIGHT : LEFT, true);
        if (h.x != s->last_x) {
            s->last_x = h.x;
            s->moved = el;
        }
        if (el - s->moved > 0.3 && el - s->jumped > 0.8) { // stuck: jump on
            s->jumped = el;
        }
        hold(r, A, el - s->jumped < 0.15);
        return false;
    });
}

// Climbs the rope or ladder over the hero: A (jump) with up held, until the hero stands at y
// or above. Jumps again only while not on it.
void then_climb(Run &r, int y) {
    auto start = std::make_shared<int>(INT32_MIN);
    then(r, "climb to y " + std::to_string(y), 12, [&r, y, start](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (*start == INT32_MIN) {
            *start = h.y;
        }
        if (h.y <= y) {
            hold(r, A, false);
            hold(r, UP, false);
            return true;
        }
        hold(r, UP, true);
        const bool climbing = h.y < *start - 55; // above a jump's height
        hold(r, A, !climbing && std::fmod(el, 2.0) >= 0.1 && std::fmod(el, 2.0) < 0.25);
        return false;
    });
    then_wait(r, 0.3);
}

// Up at a portal until the hero is on map (area, map), in the field.
void then_portal(Run &r, int area, int map) {
    then(r, "the portal to map " + std::to_string(area) + "." + std::to_string(map), 30,
         [&r, area, map](mt_cpu *cpu, double el) {
             const Hero h = hero(cpu);
             if (h.on(area, map)) {
                 hold(r, UP, false);
                 return field(cpu);
             }
             hold(r, UP, std::fmod(el, 1.5) < 0.4);
             return false;
         });
    then_wait_field(r);
}

// Talks to the NPC the hero stands at (up) and goes through the dialog with A (OK, -5), again
// and again, until `done` holds in the field.
void then_talk(Run &r, std::string what, Pred done, double timeout = 90) {
    struct S {
        double chat = -1, fieldt = -1;
    };
    auto s = std::make_shared<S>();
    then(r, "talk: " + what, timeout, [&r, done, s](mt_cpu *cpu, double el) {
        if (mt_input_panel() == 5 || game_mode(cpu) == 4) {
            hold(r, UP, false);
            if (s->chat < 0) {
                s->chat = el;
            }
            s->fieldt = -1;
            hold(r, A, el - s->chat >= 1.0 && std::fmod(el - s->chat, 1.2) < 0.1);
            return false;
        }
        s->chat = -1;
        hold(r, A, false);
        if (!field(cpu)) {
            s->fieldt = -1;
            hold(r, UP, false);
            return false;
        }
        if (s->fieldt < 0) {
            s->fieldt = el;
        }
        const double f = el - s->fieldt;
        if (f >= 0.5 && done(cpu)) {
            hold(r, UP, false);
            return true;
        }
        hold(r, UP, f >= 0.5 && std::fmod(f - 0.5, 2.0) < 0.4);
        return false;
    });
}

// Where a fight goes on after the hero falls: the death popup's Return to town (OK, -5), then
// from the town (map `town`) left or right to `portal_x` and through it to the fight's map.
struct Comeback {
    i32 town_area, town_map, portal_x, area, map;
};

// Fights on the hero's floor until `done`: walks to the nearest mob of `want` (fighting what is
// in the way first; with none, the nearest mob, else on), attacks it with X, drinks a Red
// Potion (Y, quick slot 1) when HP is low. A fall is no failure: the hero comes back (`back`)
// and fights on; the falls are counted in the log.
void then_fight(Run &r, std::string what, i32 want, Pred done, double timeout, Comeback back) {
    struct S {
        int facing = 0, wander = -1, last_x = INT32_MIN, falls = 0;
        double attack = -10, potion = -10, turn = -10, moved = 0;
        bool away = false;
    };
    auto s = std::make_shared<S>();
    then(r, "fight: " + what, timeout, [&r, want, done, back, s](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (done(cpu)) {
            release_all(r);
            std::printf("fight over at %.2f s: %d falls; %s\n", r.t, s->falls,
                        hero_text(cpu, h).c_str());
            return true;
        }
        if (s->away || h.hp <= 0 || !field(cpu)) { // fallen: to town and back
            if (!s->away) {
                s->away = true;
                s->falls++;
                release_all(r);
                std::printf("fight: the hero fell at %.2f s (%s)\n", r.t, where(cpu).c_str());
            }
            if (h.on(back.area, back.map) && field(cpu) && h.hp > 0) {
                release_all(r);
                s->away = false;
                return false;
            }
            if (!field(cpu)) { // the death popup: OK is Return to town
                hold(r, LEFT, false);
                hold(r, RIGHT, false);
                hold(r, UP, false);
                hold(r, A, popup_id(cpu) != -1 && std::fmod(el, 1.0) < 0.1);
                return false;
            }
            hold(r, A, false);
            if (h.on(back.town_area, back.town_map)) {
                const int d = back.portal_x - h.x;
                hold(r, LEFT, d < -4);
                hold(r, RIGHT, d > 4);
                hold(r, UP, std::abs(d) <= 4 && std::fmod(el, 1.5) < 0.4);
            }
            return false;
        }
        if (h.hp < h.level * 5 && el - s->potion > 2.0) {
            s->potion = el;
        }
        hold(r, Y, el - s->potion < 0.1);
        const Mob *target = nullptr;
        const std::vector<Mob> all = mobs(cpu);
        // Whatever is in reach first (it would knock the hero back), then the nearest `want`,
        // then the nearest other one.
        for (const int pass : {0, 1, 2}) {
            for (const Mob &m : all) {
                const int dist = std::abs(m.x - h.x);
                if (m.hp <= 0 || std::abs(m.y - h.y) > 6 || (pass == 0 && dist > 50) ||
                    (pass == 1 && m.id != want) || (pass == 2 && dist > 160)) {
                    continue;
                }
                if (target == nullptr || std::abs(m.x - h.x) < std::abs(target->x - h.x)) {
                    target = &m;
                }
            }
            if (target != nullptr) {
                break;
            }
        }
        int go = 0;
        if (target == nullptr) { // on, turning where the hero stops
            go = s->wander;
            if (h.x != s->last_x) {
                s->last_x = h.x;
                s->moved = el;
            } else if (el - s->moved > 0.5) {
                s->wander = -s->wander;
                s->moved = el;
            }
        } else {
            const int dx = target->x - h.x;
            if (std::abs(dx) > 30) {
                go = dx > 0 ? 1 : -1;
            } else if (s->facing != (dx >= 0 ? 1 : -1) && el - s->turn > 0.6) {
                s->turn = el; // a short step to face it
                s->facing = dx >= 0 ? 1 : -1;
            }
        }
        const bool turning = el - s->turn < 0.05;
        if (go != 0) {
            s->facing = go;
        }
        hold(r, LEFT, go < 0 || (turning && s->facing < 0));
        hold(r, RIGHT, go > 0 || (turning && s->facing > 0));
        if (target != nullptr && go == 0 && !turning && el - s->attack > 0.45) {
            s->attack = el;
        }
        hold(r, X, el - s->attack < 0.1);
        if (std::getenv("MT_FIGHT_TRACE") != nullptr && std::fmod(el, 0.25) < 0.001) {
            std::printf("fight %.2f: hero x %d y %d HP %d facing %d go %d; target %s\n", el, h.x,
                        h.y, h.hp, s->facing, go,
                        target == nullptr
                            ? "none"
                            : (std::to_string(target->id) + " x " + std::to_string(target->x) +
                               " HP " + std::to_string(target->hp))
                                  .c_str());
        }
        return false;
    });
}

// The popup menu's option tab, Save, Yes: the game writes theif<slot> and quest<slot>
// (CDataMgr::SaveSaveData, SaveQuestData), then OK, and back to the field.
void then_save(Run &r) {
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_press(r, LEFT); // from the status tab back round to the option tab
    then_wait_for(r, "the option tab", 5, [](mt_cpu *cpu) { return menu_tab(cpu) == 5; });
    then_press(r, A);
    then_wait_for(r, "save? (popup 13)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 13; });
    then_press(r, A);
    then_wait_for(r, "saved (popup 2)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 2; });
    then_press(r, A);
    then_press(r, B);
    then_press(r, B);
    then_wait_field(r);
}

// The main menu to the game: B closes the promo banner where it shows (a fresh install), A on
// Play; with a save, A on the first slot, right x3, up x2 to Start, A.
bool focused(const Run &r, const char *action) {
    const fe::FocusTarget *f = r.focus.focused();
    return r.focus.active() && f != nullptr && f->action == action;
}
void then_new_game(Run &r) {
    then_wait_for(
        r, "the promo banner", 15,
        [&r](mt_cpu *) {
            for (const fe::FocusTarget &f : r.focus.targets()) {
                if (f.action == "clickADClose:") {
                    return true;
                }
            }
            return false;
        },
        1.0);
    then_press(r, B);
    then_wait_for(
        r, "the focus on Play", 5, [&r](mt_cpu *) { return focused(r, "clickPlay:"); }, 0.5);
    then_press(r, A);
}
void then_load_slot0(Run &r) {
    then_wait_for(
        r, "the focus on Play", 15, [&r](mt_cpu *) { return focused(r, "clickPlay:"); }, 0.5);
    then_press(r, A);
    then_wait_for(r, "the focus on the first slot", 5,
                  [&r](mt_cpu *) { return focused(r, "chooseCharacterOne:"); });
    then_press(r, A);
    for (const SDL_GamepadButton b : {RIGHT, RIGHT, RIGHT, UP, UP}) {
        then_press(r, b, 0.1, 0.2);
    }
    then_wait_for(r, "the focus on Start", 5, [&r](mt_cpu *) { return focused(r, "clickStart:"); });
    then_press(r, A);
    then_wait_for(r, "the game", 30, field, 1.0);
}

void then_finish(Run &r) {
    then_do(r, "finish", [&r](mt_cpu *) {
        release_all(r);
        check(r.input.live() == 0, "no touch is left down");
        r.done = true;
    });
}

// The save files' map and chapter (docs/formats/save.md: theif +0x41C area, +0x41D map;
// quest +0 chapter).
std::vector<std::uint8_t> read_save(const Run &r, const char *name) {
    std::ifstream in(r.data / "Documents" / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// --- explore ----------------------------------------------------------------------------------

bool explore(Run &r, int argc, char **argv) {
    attach_pad(r);
    double end = 1;
    for (int i = 0; i < argc; i++) {
        const std::string step = argv[i];
        const auto colon = step.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const double t = std::atof(step.substr(0, colon).c_str());
        end = std::max(end, t + 1);
        char what[32] = "", name[64] = "";
        double v1 = 0, v2 = 0, v3 = 0.1;
        const int n = std::sscanf(step.c_str() + colon + 1, "%31s %63s %lf %lf %lf", what, name,
                                  &v1, &v2, &v3);
        const std::string w = what;
        if (w == "press" && n >= 2) {
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
            press(r, t, b, n >= 3 ? v1 : 0.1);
        } else if (w == "repeat" && n >= 4) { // "T:repeat BUTTON EVERY UNTIL [HOLD]"
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
            for (double u = t; u <= v2 + 1e-9; u += std::max(v1, 0.2)) {
                press(r, u, b, v3);
            }
            end = std::max(end, v2 + 1);
        } else if (w == "stick" && n == 4) {
            stick(r, t, SDL_GetGamepadAxisFromString(name), static_cast<Sint16>(v1), v2);
        } else if (w == "trigger" && n == 3) {
            const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
            at(r, t, [&r, a](mt_cpu *) { r.pad->trigger(a, 1.0f); });
            at(r, t + v1, [&r, a](mt_cpu *) { r.pad->trigger(a, 0.0f); });
        } else if (w == "tap" && n >= 2) { // exploring only: the flows never tap
            const float x = static_cast<float>(std::atof(name)), y = static_cast<float>(v1);
            at(r, t, [&r, x, y](mt_cpu *) {
                r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, x, y);
            });
            at(r, t + (n >= 4 ? v2 : 0.1), [&r, x, y](mt_cpu *) {
                r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_ENDED, x, y);
            });
        } else if (w == "shot" && n == 2) {
            shot(r, t, name);
        } else if (w == "state") {
            state(r, t);
        } else if (w == "load") { // "T:load": slot 0 into the game (tasks, from the start)
            then_load_slot0(r);
        } else if (w == "walkto" && n >= 2) { // "T:walkto X": a task, after the ones before
            then_walk_to(r, std::atoi(name));
        } else if (w == "shop") {
            at(r, t, [&r](mt_cpu *cpu) {
                const Shop s = shop(cpu);
                std::printf("shop at %.2f s: %X tab %d selected %d kind %d rows %d\n", r.t, s.addr,
                            s.tab, s.selected, s.kind, s.rows);
                for (i32 i = 0; i < s.rows && i < 40; i++) {
                    const ShopRow w = shop_row(cpu, s, i);
                    std::printf("  row %d: id %d price %d\n", i, w.id, w.price);
                }
                for (i32 i = 0; i < 48; i++) {
                    const EtcItem e = etc_item(cpu, hero(cpu), i);
                    if (e.id >= 0) {
                        std::printf("  etc %d: id %d type %d count %d max %d price %d\n", i, e.id,
                                    e.type, e.count, e.max, e.price);
                    }
                }
                std::fflush(stdout);
            });
        } else if (w == "mobs") {
            at(r, t, [&r](mt_cpu *cpu) {
                for (const Mob &m : mobs(cpu)) {
                    std::printf("mob at %.2f s: id %d x %d y %d HP %d\n", r.t, m.id, m.x, m.y,
                                m.hp);
                }
            });
        } else if (w == "hero") { // "T:hero [EVERY UNTIL]": the hero's line, once or repeated
            const double every = n >= 3 ? std::atof(name) : 0, until = n >= 3 ? v1 : t;
            for (double u = t; u <= until + 1e-9; u += every > 0 ? every : 1e9) {
                at(r, u, [&r](mt_cpu *cpu) {
                    const Hero h = hero(cpu);
                    std::printf("hero at %.2f s: panel %d mode %d popup %d %s\n", r.t,
                                static_cast<int>(mt_input_panel()), game_mode(cpu), popup_id(cpu),
                                hero_text(cpu, h).c_str());
                });
                end = std::max(end, u + 1);
            }
        } else if (w == "widgets") {
            at(r, t, [](mt_cpu *cpu) {
                for (u32 g = 0xF149C; g <= 0xF14C4; g += 4) {
                    const u32 wd = rd(cpu, rd(cpu, g));
                    if (wd == 0) {
                        continue;
                    }
                    std::printf("widget %X: on %d rect (%d,%d,%d,%d) items %u\n", g,
                                cpu->mem[wd + 12], (int)rd(cpu, wd + 28), (int)rd(cpu, wd + 32),
                                (int)rd(cpu, wd + 36), (int)rd(cpu, wd + 40), rd(cpu, wd + 4));
                    for (u32 i = 0; i < rd(cpu, wd + 4) && i < 64; i++) {
                        const u32 it = rd(cpu, rd(cpu, wd) + 4 * i);
                        std::printf("  item %u: rect (%d,%d,%d,%d) off (%d,%d) abs %d id %d v74 %d "
                                    "b %02x %02x %02x %02x\n",
                                    i, (int)rd(cpu, it + 0x20), (int)rd(cpu, it + 0x24),
                                    (int)rd(cpu, it + 0x28), (int)rd(cpu, it + 0x2c),
                                    (int)rd(cpu, it + 8), (int)rd(cpu, it + 12),
                                    cpu->mem[it + 0x5c], (int)rd(cpu, it + 0x60),
                                    (int)rd(cpu, it + 0x74), cpu->mem[it + 0x10],
                                    cpu->mem[it + 0x11], cpu->mem[it + 0x12], cpu->mem[it + 0x13]);
                    }
                }
                std::fflush(stdout);
            });
        } else {
            return false;
        }
    }
    finish(r, end);
    return true;
}

// --- the flows --------------------------------------------------------------------------------

// The main menu: the banner's close (B), Help, About with its links (down scrolls the text),
// Game Center, More Games, then Play.
void main_menu(Run &r) {
    attach_pad(r);
    // The menu has slid in at 3 s (#122).
    between(r, 2.9, 3.4, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickADClose:]"},
                      "B closes the promo banner (its close is the screen's back button)");
    });
    press(r, 3.0, B);
    at(r, 3.45, [&r](mt_cpu *) {
        check(r.focus.active() && focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "the focus is on and on Play: " + focus_text(r));
    });
    // Help: down, A; B closes it.
    press(r, 3.5, DOWN);
    press(r, 3.8, A);
    at(r, 4.6, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView closeHelp:]",
              "Help is open, the focus on its close: " + focus_text(r));
    });
    press(r, 4.7, B);
    between(r, 3.45, 5.4, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickHelp:]", "-[MainMenuView closeHelp:]"},
                      "down + A opens Help, B closes it");
    });
    // About: down (the focus is back on Help), A; down to the first link, A; down again
    // scrolls the text (no control below), which brings the other three links; A on each; B
    // closes About.
    press(r, 5.5, DOWN);
    press(r, 5.8, A);
    at(r, 6.5, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView closeAbout:]",
              "About is open, the focus on its close: " + focus_text(r));
    });
    press(r, 6.6, DOWN);
    press(r, 6.9, A);
    press(r, 7.2, DOWN); // nothing below: the text scrolls
    at(r, 8.9, [&r](mt_cpu *) {
        check(r.focus.stats().scrolls == 1 &&
                  focus_text(r) == "focus on -[MainMenuView clickLinkURL2:]",
              "down past the first link drags About's text; the focus goes to the 2nd link: " +
                  focus_text(r));
    });
    press(r, 9.0, A);
    press(r, 9.3, DOWN);
    press(r, 9.6, A);
    press(r, 9.9, DOWN);
    press(r, 10.2, A);
    press(r, 10.5, B);
    between(r, 5.45, 11.2, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickAbout:]", "-[MainMenuView clickLinkURL1:]",
                       "-[MainMenuView clickLinkURL2:]", "-[MainMenuView clickLinkURL3:]",
                       "-[MainMenuView clickLinkURL4:]", "-[MainMenuView closeAbout:]"},
                      "About: A on each of its four links (down scrolls the text to them), B "
                      "closes it");
        check(logged(r, from, "openURL:] http://maplestory.nexon.net: not opened"),
              "the links' openURL: are refused, as with taps (ADR 0049)");
    });
    // Game Center (right of About), More Games (right again), then back left and up to Play.
    press(r, 11.4, RIGHT);
    press(r, 11.7, A);
    press(r, 12.6, RIGHT);
    press(r, 12.9, A);
    between(r, 11.35, 13.6, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickGameCenter:]", "-[MainMenuView clickMoreGame:]"},
                      "right + A: Game Center, right + A: More Games");
        check(logged(r, from, "presentModalViewController:GKLeaderboardViewController"),
              "Game Center's leaderboard is presented (and dismissed, ADR 0034)");
    });
    press(r, 13.7, LEFT);
    at(r, 14.0, [&r](mt_cpu *) { // held up: repeats down the column to Play
        r.pad->button(UP, true);
    });
    at(r, 15.0, [&r](mt_cpu *) { r.pad->button(UP, false); });
    at(r, 15.1, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "left, then up held (repeating), reaches Play: " + focus_text(r));
    });
    press(r, 15.3, A);
    between(r, 15.2, 17.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickPlay:]"}, "A on Play starts the game");
        check(file_size(r, "theif0") == 1124 && file_size(r, "quest0") == 4781,
              "a fresh install's Play writes slot 0 (theif0 1124 bytes, quest0 4781)");
        check(!r.focus.active(), "the focus is off in the game (" + focus_text(r) + ")");
    });
    finish(r, 17.1);
}

// The Korean terms: A agrees (the focus starts on the agreement); then the two help pages:
// right pages the help's scroll view, B closes.
void terms(Run &r) {
    attach_pad(r);
    at(r, 2.9, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[TermsView releaseAgreementButton:]",
              "the terms show; the focus is on the agreement: " + focus_text(r));
    });
    press(r, 3.0, B); // nothing to go back to: the terms stay
    press(r, 3.3, A);
    between(r, 2.95, 4.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(
            r, from, {"-[TermsView pressAgreementButton:]", "-[TermsView releaseAgreementButton:]"},
            "B does nothing on the terms; A agrees");
        check(file_size(r, "CTerms") == 2, "CTerms is written (2 bytes)");
    });
    press(r, 4.2, B); // the banner
    press(r, 4.5, DOWN);
    press(r, 4.8, A);
    press(r, 5.6, RIGHT);
    press(r, 7.0, B);
    between(r, 4.15, 7.7, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickADClose:]", "-[MainMenuView clickHelp:]",
                       "-[MainMenuView closeHelp:]"},
                      "B closes the banner, down + A opens Help, B closes it");
        check(logged(r, from, "drag ended, pages to page 1"),
              "right pages the Korean help to its 2nd page (a drag of its paging scroll view)");
    });
    finish(r, 7.8);
}

// A fresh install's Play by the pad: slot 0.
void seed(Run &r) {
    attach_pad(r);
    press(r, 3.0, B);
    press(r, 3.5, A);
    at(r, 5.0, [&r](mt_cpu *) {
        check(file_size(r, "theif0") == 1124, "the pad's Play on a fresh install writes slot 0");
    });
    finish(r, 5.1);
}

// The character select (slot 0 saved): New, its alert's Yes: slot 1, the game loads.
void create(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play: the focus is on it (the banner is not shown again)
    at(r, 3.9, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView chooseCharacterOne:]",
              "the character select; the focus on the first slot: " + focus_text(r));
    });
    // The buttons are a column right of the slots (Start, New, Delete): right past the slots
    // reaches Delete, the lowest; up is New. A; the alert's Yes is focused.
    press(r, 4.0, RIGHT);
    press(r, 4.3, RIGHT);
    press(r, 4.6, RIGHT);
    press(r, 4.9, UP);
    at(r, 5.15, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickNew:]",
              "right, right, right, up: New: " + focus_text(r));
    });
    press(r, 5.2, A);
    // The alert is the engine's popup (CStateMainMenu opens it for New's '#': panel 7) under
    // the shell's alert view: its keys are the popup's (A yes, B no), as on every popup.
    at(r, 5.9, [&r](mt_cpu *cpu) {
        check(mt_input_panel() == 7 && popup_id(cpu) >= 0 && !r.focus.active(),
              "New asks: the engine's popup " + std::to_string(popup_id(cpu)) +
                  " on panel 7; the keys go to the game (" + focus_text(r) + ")");
    });
    press(r, 6.0, A);
    between(r, 2.9, 8.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView clickNew:]"},
                      "A Play, the pad to New, A");
        check(file_size(r, "theif1") == 1124 && file_size(r, "quest1") == 4781,
              "A on the alert (OK, -5: Yes) writes slot 1, and the game loads");
    });
    finish(r, 8.1);
}

// Two slots saved: Delete with nothing chosen (the shell's own alert: B is its OK), then the
// first slot, Start.
void select(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play
    press(r, 4.0, RIGHT);
    press(r, 4.3, RIGHT);
    press(r, 4.6, RIGHT); // Delete
    press(r, 4.9, A);
    at(r, 5.6, [&r](mt_cpu *) {
        check(mt_input_panel() == -1 &&
                  focus_text(r) == "focus on -[ChooseCharacterView confirmAlert:]",
              "Delete with no character chosen: the shell's alert (no engine popup), the focus "
              "on its OK: " +
                  focus_text(r));
    });
    press(r, 5.7, B);
    at(r, 6.5, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickDelete:]",
              "B closes it (its OK is the back button), the focus back on Delete: " +
                  focus_text(r));
    });
    press(r, 6.6, LEFT);
    press(r, 6.9, LEFT);
    press(r, 7.2, LEFT);
    press(r, 7.5, A); // the first slot
    press(r, 8.6, RIGHT);
    press(r, 8.9, RIGHT);
    press(r, 9.2, RIGHT);
    press(r, 9.5, UP);
    press(r, 9.8, UP);
    at(r, 10.1, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickStart:]",
              "right, right, right, up, up: Start: " + focus_text(r));
    });
    press(r, 10.2, A);
    between(r, 2.9, 11.5, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView clickDelete:]",
                       "-[ChooseCharacterView confirmAlert:]",
                       "-[ChooseCharacterView chooseCharacterOne:]",
                       "-[ChooseCharacterView clickStart:]"},
                      "Play, Delete, its alert's OK, the first slot, Start");
        check(mt_input_panel() == 3 && !r.focus.active(),
              "Start loads the game: the key pad's panel 3 (" + focus_text(r) + ")");
        check(file_size(r, "theif0") == 1124 && file_size(r, "theif1") == 1124,
              "both slots are still saved");
    });
    finish(r, 11.6);
}

// Two slots saved: choose the second, Delete, its alert's Yes (the engine's popup: A), then
// B is Back to the main menu.
void delete_flow(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play
    press(r, 4.0, RIGHT);
    press(r, 4.3, A); // the second slot
    press(r, 5.4, RIGHT);
    press(r, 5.7, RIGHT); // Delete
    at(r, 6.0, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickDelete:]",
              "right, A (the second slot), right, right: Delete: " + focus_text(r));
    });
    press(r, 6.1, A);
    // The shell's own alert (no engine popup): the focus starts on No, deleting takes a move.
    at(r, 6.8, [&r](mt_cpu *) {
        check(mt_input_panel() == -1 &&
                  focus_text(r) == "focus on -[ChooseCharacterView cancelAlert:]",
              "Delete asks; the focus on No: " + focus_text(r));
    });
    press(r, 6.9, LEFT);
    press(r, 7.2, A);
    at(r, 8.1, [&r](mt_cpu *) {
        check(file_size(r, "theif1") == 0 && file_size(r, "quest1") == 0 &&
                  file_size(r, "theif0") == 1124,
              "left, A (Yes) deletes the second character's files; the first stays");
        check(r.focus.active(), "the select screen again, the focus on: " + focus_text(r));
    });
    press(r, 8.2, B);
    between(r, 2.9, 9.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView chooseCharacterTwo:]",
                       "-[ChooseCharacterView clickDelete:]",
                       "-[ChooseCharacterView okDeleteAlert:]",
                       "-[ChooseCharacterView clickBack:]"},
                      "Play, the second slot, Delete, left + A: Yes, B: Back");
        check(focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "Back returns to the main menu, the focus on Play: " + focus_text(r));
    });
    finish(r, 9.1);
}

// A new game in the field by the pad only, then the screens of the field: an NPC's dialog, the
// popup menu (tabs, the item list, back, close), the cash shop's button and the map.
void game(Run &r) {
    attach_pad(r);
    // The main menu: B closes the banner, A is Play (no save: a new character, the intro).
    press(r, 3.0, B);
    press(r, 4.5, A);
    // The intro's speech boxes and MacArthur's first dialog: A (OK) each.
    for (const double t : {30.0, 34.0, 38.0, 42.0, 46.0, 51.0, 52.0, 53.0, 54.0}) {
        press(r, t, A);
    }
    at(r, 57.0, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "A through the intro: the field (panel 0, game mode 0)");
    });

    // NPC chat (panel 5, key codes: no focus): a step right, up talks to MacArthur, A is OK
    // through his pages and closes the dialog.
    press(r, 60.0, RIGHT, 0.3);
    press(r, 61.0, UP, 0.6);
    at(r, 62.0, [&r](mt_cpu *cpu) {
        check(mt_input_panel() == 5 && game_mode(cpu) == 4 && !r.focus.active(),
              "right, up: MacArthur's dialog (panel 5, game mode 4; the focus off: " +
                  focus_text(r) + ")");
    });
    press(r, 62.5, A);
    press(r, 65.0, UP, 0.6);
    for (const double t : {67.0, 69.0, 70.5, 72.0}) {
        press(r, t, A);
    }
    at(r, 73.5, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "A through every page: the dialog closed, the field again");
    });

    // The popup menu (panel 1, key codes; OK on a tab is a touch on it, ADR 0051).
    press(r, 76.0, START);
    at(r, 76.8, [](mt_cpu *cpu) {
        check(mt_input_panel() == 1 && game_mode(cpu) == 1 && menu_tab(cpu) == 0 &&
                  menu_sub(cpu) == 0,
              "Start opens the popup menu on its status tab");
    });
    press(r, 77.0, RIGHT);
    press(r, 77.6, A);
    at(r, 78.5, [&r](mt_cpu *cpu) {
        check(menu_tab(cpu) == 1 && menu_sub(cpu) == 1 && r.focus.stats().menu_tabs == 1,
              "right, A: the item tab opens, by a touch on it (its list's widgets enabled)");
    });
    press(r, 79.0, RIGHT); // the second item (Blue Potion)
    press(r, 79.6, A);     // chosen
    press(r, 80.4, A);     // used
    shot(r, 81.2, "items");
    press(r, 81.5, B);
    at(r, 82.3, [](mt_cpu *cpu) {
        check(game_mode(cpu) == 1 && menu_tab(cpu) == 1 && menu_sub(cpu) == 0,
              "B closes the tab: back to the tabs, the item tab still chosen");
    });
    press(r, 82.5, RIGHT);
    press(r, 83.0, RIGHT);
    press(r, 83.6, A);
    at(r, 84.4, [&r](mt_cpu *cpu) {
        check(menu_tab(cpu) == 3 && menu_sub(cpu) == 3 && r.focus.stats().menu_tabs == 2,
              "right, right, A: the skill tab opens, by a touch on it");
    });
    press(r, 85.0, B);
    at(r, 85.8, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "B on the skill tab closes the popup menu (the game's: the item tab's B closes "
              "only the tab)");
    });

    // The cash shop's button (LT): during the tutorial the game answers with its popup 0x25B
    // ("This cannot be used right now", panel 7); A closes it.
    at(r, 89.0, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f); });
    at(r, 89.3, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f); });
    at(r, 90.2, [](mt_cpu *cpu) {
        check(mt_input_panel() == 7 && popup_id(cpu) == 0x25B,
              "LT: the cash shop's answer in the tutorial, popup 0x25B (panel 7)");
    });
    press(r, 90.5, A);
    at(r, 91.5, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && popup_id(cpu) == -1, "A closes it: the field");
    });

    // The map: Back opens it, B closes it (Back there switches to the world map).
    press(r, 92.0, SELECT);
    at(r, 93.0, [](mt_cpu *cpu) {
        check(mt_input_panel() == 8 && game_mode(cpu) == 3, "Back opens the map");
    });
    press(r, 93.5, B);
    at(r, 94.5,
       [](mt_cpu *cpu) { check(mt_input_panel() == 0 && game_mode(cpu) == 0, "B closes it"); });
    finish(r, 95.0);
}

// --- past the tutorial (#127) ------------------------------------------------------------------
//
// A post-tutorial save made by the pad alone, in two launches on one data directory (the
// fixture input.menu_shops_save, platform/menu_nav_test.cmake). The way is the tutorial's own
// (script/common/Tutorial*.out, docs/ANALYSIS.md): MacArthur's quests 0-6 on maps 2.8 and
// 2.9 (talk, jump, climb, a portal, a snail), the left portal to Henesys (2.0, chapter 1) and
// elder Stan; then Stan's "Elder's Advice" (main quest 7, five Red Snails on 2.3), whose end
// opens the first side quests' offers. Each step checks the quest it completes.

Pred main_quest(u32 n, i32 state) {
    return [n, state](mt_cpu *cpu) { return quest_state(cpu, hero(cpu), 0, n) == state; };
}
Pred main_quests(u32 done_n, u32 active_n) {
    return [done_n, active_n](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return quest_state(cpu, h, 0, done_n) == 2 && quest_state(cpu, h, 0, active_n) == 1;
    };
}

// Launch 1: a new game, the tutorial, Stan, saved on Stan's ledge in Henesys.
void tutorial(Run &r) {
    attach_pad(r);
    then_new_game(r);
    then_wait_for(r, "the intro (map 2.10)", 60, [](mt_cpu *cpu) { return hero(cpu).on(2, 10); });
    // The intro's speech boxes (panel 3) and MacArthur's first dialog (panel 5): A each.
    then(r, "the intro: A through its boxes", 90, [&r](mt_cpu *cpu, double el) {
        const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
        hold(r, A, box && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_wait_field(r);
    then_check(r, "the intro ends in the field on map 2.8, quest 0 (Talking to NPCs) active",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 8) && main_quest(0, 1)(cpu); });
    // MacArthur (TutorialMaster01, x 152): talking, how to do a quest; gives How to Jump.
    then_walk_to(r, 152);
    then_talk(r, "MacArthur: quests 0 and 1 done, 2 (How to Jump) given", main_quests(1, 2));
    // Right to the step's wall, A with right held: the ledge (y 399); MacArthur at x 328.
    then_walk_to(r, 328);
    then_talk(r, "MacArthur: quest 2 done, 3 (climb a rope) given", main_quests(2, 3));
    then_walk_to(r, 282, 3); // the rope at x 279
    then_climb(r, 287);
    then_walk_to(r, 249);
    then_talk(r, "MacArthur: quest 3 done, 4 (a portal) given", main_quests(3, 4));
    then_walk_to(r, 372); // off the platform, to portal280 (x 376)
    then_portal(r, 2, 9);
    then_walk_to(r, 216);
    then_talk(r, "MacArthur: quest 4 done, 5 (attack a monster) given", main_quests(4, 5));
    then_walk_to(r, 178, 3); // the ladder at x 175
    then_climb(r, 319);
    then_fight(r, "the snail (quest 5)", 1,
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 5) >= 1; }, 60,
               {2, 8, 0, 2, 9});
    then_check(r, "the snail is defeated: quest 5's requirement 1/1",
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 5) >= 1; });
    then_walk_to(r, 98); // off the platform, to portal290 (x 104)
    then_portal(r, 2, 8);
    then_walk_to(r, 282, 3);
    then_climb(r, 287);
    then_walk_to(r, 249);
    then_talk(r, "MacArthur: quest 5 done, 6 (How it All Began) given", main_quests(5, 6));
    then_walk_to(r, 100); // portal281 (x 104): Henesys
    then_portal(r, 2, 0);
    then_check(r, "portal281 leads to Henesys (map 2.0), chapter 1",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0) && hero(cpu).chapter == 1; });
    then_walk_to(r, 502, 3); // the rope to Stan's ledge
    then_climb(r, 175);
    then_walk_to(r, 548);
    then_talk(r, "Stan: the tutorial's last quest done, Elder's Advice (main quest 7) given",
              [](mt_cpu *cpu) {
                  return quest_state(cpu, hero(cpu), 0, 7) == 1 &&
                         !quest_active(cpu, hero(cpu), 0, 40);
              });
    then_save(r);
    then_do(r, "the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> t = read_save(r, "theif0"), q = read_save(r, "quest0");
        check(t.size() == 1124 && t[0x41C] == 2 && t[0x41D] == 0 && q.size() == 4781 && q[0] == 1,
              "theif0 is saved in Henesys (area 2, map 0) and quest0 in chapter 1");
    });
    then_finish(r);
}

// Launch 2 (slot 0 saved by `tutorial`): Elder's Advice, five Red Snails on the Western Hill
// (2.3), back to Stan, saved.
void hunt(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_check(r, "the save loads in Henesys, quest 7 active",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0) && main_quest(7, 1)(cpu); });
    then_walk_to(r, 42); // down the steps, to portal200 (x 40)
    then_portal(r, 2, 3);
    then_fight(r, "five Red Snails (quest 7)", 2,
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 7) >= 5; }, 600,
               {2, 0, 42, 2, 3});
    then_check(r, "five Red Snails: quest 7's requirement 5/5",
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 7) >= 5; });
    then_walk_to(r, 900); // portal231 (x 904): Henesys, at portal200
    then_portal(r, 2, 0);
    then_walk_to(r, 502, 3);
    then_climb(r, 175);
    then_walk_to(r, 548);
    then_talk(r, "Stan: Elder's Advice (main quest 7) done", main_quest(7, 2));
    then_save(r);
    then_do(r, "the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> q = read_save(r, "quest0");
        check(q.size() == 4781 && q[0] == 1 && q[1 + 7] == 2,
              "quest0 is saved with main quest 7 done");
    });
    then_finish(r);
}

// --- the shops and a quest offer (#127) -------------------------------------------------------

// Whether the screen has a control whose action is `action` (focused or not).
bool shows(const Run &r, const char *action) {
    for (const fe::FocusTarget &f : r.focus.targets()) {
        if (f.action == action) {
            return true;
        }
    }
    return false;
}
// The shop (game mode 2) on the key pad's panel 6, no popup.
bool in_shop(mt_cpu *cpu) {
    return mt_input_panel() == 6 && game_mode(cpu) == 2 && popup_id(cpu) == -1;
}
// A trigger pulled all the way for 0.15 s, then let go for `after` seconds.
void then_pull(Run &r, SDL_GamepadAxis a, double after = 0.5) {
    then(r, std::string("pull ") + SDL_GetGamepadStringForAxis(a), after + 1.2,
         [&r, a, after](mt_cpu *, double el) {
             r.pad->trigger(a, el < 0.15 ? 1.0f : 0.0f);
             return el >= 0.15 + after;
         });
}
// Presses `b` until `p` holds (each press 0.1 s, 0.4 s apart), at most `presses` times.
void then_press_until(Run &r, std::string what, SDL_GamepadButton b, Pred p, int presses) {
    then(r, std::move(what), presses * 0.5 + 1, [&r, b, p](mt_cpu *cpu, double el) {
        if (p(cpu) && std::fmod(el, 0.5) >= 0.1) {
            hold(r, b, false);
            return true;
        }
        hold(r, b, std::fmod(el, 0.5) < 0.1);
        return false;
    });
}

// What a step of the flow remembers for a later check.
struct Before {
    i32 meso = 0, count = 0;
    std::size_t log = 0;
};

// Buys the Buy tab's row `row` (item `id` at `price`): down to it, A (popup 3, "buy?"), A
// (popup 111, bought), A (back to the list); the meso and the item's count in guest memory.
void then_buy(Run &r, i32 row, i32 id, i32 price, i32 gets) {
    auto b = std::make_shared<Before>();
    then_press_until(
        r, "the Buy tab's row " + std::to_string(row), DOWN,
        [row](mt_cpu *cpu) { return shop(cpu).selected == row; }, row + 2);
    then_do(r, "before buying", [b, id](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->count = item_count(cpu, hero(cpu), id);
    });
    then_check(r,
               "row " + std::to_string(row) + " sells item " + std::to_string(id) + " for " +
                   std::to_string(price) + " meso",
               [row, id, price](mt_cpu *cpu) {
                   const ShopRow w = shop_row(cpu, shop(cpu), row);
                   return w.id == id && w.price == price;
               });
    then_press(r, A);
    // Popup 5 asks how many (one, as it opens), popup 3 (a stack sold whole) whether to buy.
    then_wait_for(r, "how many / buy? (popup 5 or 3)", 5, [](mt_cpu *cpu) {
        return (popup_id(cpu) == 5 || popup_id(cpu) == 3) && mt_input_panel() == 7;
    });
    then_press(r, A);
    then_wait_for(r, "bought (popup 111)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 111; });
    then_do(r, "the purchase", [b, id, price, gets](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.meso == b->meso - price && item_count(cpu, h, id) == b->count + gets,
              "bought: meso " + std::to_string(b->meso) + " -> " + std::to_string(h.meso) +
                  ", item " + std::to_string(id) + " " + std::to_string(b->count) + " -> " +
                  std::to_string(item_count(cpu, h, id)));
    });
    then_press(r, A);
    then_wait_for(r, "the shop's list again", 5, in_shop, 0.3);
}

// The NPC the hero stands at (up) until its quest offer shows Accept and Refuse (A through the
// dialog before it).
void then_offer(Run &r, std::string npc) {
    auto chat = std::make_shared<double>(-1);
    then(r, "the offer of " + npc, 40, [&r, chat](mt_cpu *cpu, double el) {
        if (shows(r, "releaseAcceptButton:") && shows(r, "releaseRefuseButton:")) {
            release_all(r);
            return mt_input_panel() == 5;
        }
        if (mt_input_panel() == 5 || game_mode(cpu) == 4) {
            hold(r, UP, false);
            if (*chat < 0) {
                *chat = el;
            }
            hold(r, A, el - *chat >= 1.0 && std::fmod(el - *chat, 1.2) < 0.1);
            return false;
        }
        *chat = -1;
        hold(r, A, false);
        hold(r, UP, field(cpu) && std::fmod(el, 2.0) < 0.4);
        return false;
    });
}

// Launch 3 (slot 0 saved by `hunt`, Henesys): Mr. Wang's shop, the cash shop and the side
// quest of Humanoid (side quest 3, "8 Blue Snail Shells": level 7 and up).
void shops(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_check(r, "the save loads in Henesys with Elder's Advice (main quest 7) done",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0) && main_quest(7, 2)(cpu); });

    // Mr. Wang (Etc20, x 440 on Stan's ledge): his shop, panel 6, game mode 2.
    then_walk_to(r, 440);
    then_press_until(r, "Mr. Wang's shop", UP, in_shop, 6);
    then_check(r, "the shop: panel 6, game mode 2, the Buy tab, six rows (shop 3)",
               [](mt_cpu *cpu) {
                   const Shop s = shop(cpu);
                   return in_shop(cpu) && s.tab == 0 && s.kind == 1 && s.rows == 6;
               });
    then_check(r, "the Buy tab's Buy button shows", [&r](mt_cpu *) {
        return shows(r, "releaseBuyButton:") && shows(r, "releaseSellTabButton:");
    });
    // RB: the Sell tab ('6'); LB back to Buy ('4').
    then_press(r, RB);
    then_check(r, "RB: the Sell tab, its Sell button", [&r](mt_cpu *cpu) {
        return in_shop(cpu) && shop(cpu).tab == 1 && shows(r, "releaseSellButton:") &&
               shows(r, "releaseBuyTabButton:");
    });
    then_press(r, LB);
    then_check(r, "LB: the Buy tab again", [&r](mt_cpu *cpu) {
        return in_shop(cpu) && shop(cpu).tab == 0 && shows(r, "releaseBuyButton:");
    });
    // One Red Potion (item 3, 15 meso).
    then_buy(r, 0, 3, 15, 1);
    // Charge ('#', RT in menus): the Sell tab's throwing stars, refilled when the stack is
    // short. Bought just now the stack is full (200 of 200), so the shop opens nothing.
    then_buy(r, 2, 11, 170, 200);
    then_press(r, RB);
    auto stars = std::make_shared<i32>(-1);
    then_do(r, "the stars' slot", [stars](mt_cpu *cpu) {
        for (i32 i = 0; i < 48; i++) {
            if (etc_item(cpu, hero(cpu), i).id == 11) {
                *stars = i;
            }
        }
        check(*stars >= 0, "the Subi Throwing-Stars are in slot " + std::to_string(*stars));
    });
    then_press_until(
        r, "the stars' row on the Sell tab", DOWN,
        [stars](mt_cpu *cpu) { return shop(cpu).selected == *stars; }, 12);
    auto b = std::make_shared<Before>();
    then_do(r, "before Charge", [&r, b](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->log = r.log.size();
    });
    then_pull(r, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    then_do(r, "RT: Charge", [&r, b, stars](mt_cpu *cpu) {
        const EtcItem e = etc_item(cpu, hero(cpu), *stars);
        check(logged(r, b->log, "press slot4: Clet_HandleEvent(2, 35, 0)"),
              "RT sends '#' (35) to the shop as a key event");
        check(in_shop(cpu) && hero(cpu).meso == b->meso && e.count == e.max && e.type == 2,
              "Charge on a full stack of stars (" + std::to_string(e.count) + " of " +
                  std::to_string(e.max) + "): no popup, no meso spent (" + where(cpu) + ")");
    });
    then_press(r, B);
    then_wait_field(r);
    then_check(r, "B closes the shop: the field", field);

    // The cash shop: LT (-26), panel 9, game mode 11; the focus over its tiles, no purchase.
    then_do(r, "before the cash shop", [&r, b](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->log = r.log.size();
    });
    then_pull(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    then_wait_for(
        r, "the cash shop", 5,
        [&r](mt_cpu *cpu) {
            return mt_input_panel() == 9 && game_mode(cpu) == 11 && r.focus.active();
        },
        0.3);
    then_check(r, "the cash shop: the focus on its first tile (3,000 meso)",
               [&r](mt_cpu *) { return focused(r, "releaseMeso3000Button:"); });
    then_press(r, RIGHT);
    then_check(r, "right: the 10,000 meso tile",
               [&r](mt_cpu *) { return focused(r, "releaseMeso10000Button:"); });
    then_press(r, DOWN);
    then_check(r, "down: the EXP 80 tile under it",
               [&r](mt_cpu *) { return focused(r, "releaseExp80Button:"); });
    then_press(r, RIGHT);
    then_check(r, "right: the 120,000 meso tile",
               [&r](mt_cpu *) { return focused(r, "releaseMeso120000Button:"); });
    then_press(r, B);
    then_wait_field(r);
    then_do(r, "the cash shop closed", [&r, b](mt_cpu *cpu) {
        check(field(cpu), "B closes the cash shop (its Clear): the field (" + where(cpu) + ")");
        check_actions(r, b->log, {"-[TouchKeyPadView releaseClearButton:]"},
                      "B touched the cash shop's close");
        bool bought = false;
        for (const std::string &a : actions_since(r, b->log)) {
            bought = bought || a.find("CashShopKeyPadView") != std::string::npos;
        }
        check(!bought && hero(cpu).meso == b->meso, "no tile was pressed, no meso changed");
    });

    // Humanoid (Humanoid0, x 376 on the ground): Refuse, then Accept.
    then_walk_to(r, 42); // down the steps
    then_walk_to(r, 376);
    then_offer(r, "Humanoid");
    then_check(r, "the offer: side quest 3 not started", [](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), 1, 3) == 0 && !quest_active(cpu, hero(cpu), 1, 3);
    });
    then_press(r, B);
    then_wait_field(r);
    then_check(r, "B (Refuse): side quest 3 still not started, back in the field", [](mt_cpu *cpu) {
        return field(cpu) && quest_state(cpu, hero(cpu), 1, 3) == 0 &&
               !quest_active(cpu, hero(cpu), 1, 3);
    });
    then_offer(r, "Humanoid");
    then_press(r, A);
    then_wait_for(
        r, "the field", 20,
        [](mt_cpu *cpu) { return field(cpu) && quest_active(cpu, hero(cpu), 1, 3); }, 0.5);
    then_check(r, "A (Accept): side quest 3 active (state 1)", [](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), 1, 3) == 1 && quest_active(cpu, hero(cpu), 1, 3);
    });
    then_finish(r);
}

bool run_mode(Run &r, int argc, char **argv) {
    if (r.mode == "explore") {
        return explore(r, argc, argv);
    }
    if (argc != 0) {
        return false;
    }
    if (r.mode == "main_menu") {
        main_menu(r);
    } else if (r.mode == "terms") {
        terms(r);
    } else if (r.mode == "seed") {
        seed(r);
    } else if (r.mode == "create") {
        create(r);
    } else if (r.mode == "select") {
        select(r);
    } else if (r.mode == "delete") {
        delete_flow(r);
    } else if (r.mode == "game") {
        game(r);
    } else if (r.mode == "tutorial") {
        tutorial(r);
    } else if (r.mode == "hunt") {
        hunt(r);
    } else if (r.mode == "shops") {
        shops(r);
    } else {
        return false;
    }
    return true;
}

int boot(Run &r, const std::vector<std::uint8_t> &slice) {
    mt_cf_config cf = mt_cf_config_default();
    mt_cf_config_from_env(&cf); // MAPLETHIEF_LANGUAGE
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    mt_stubs_set_data_dir(r.data.string().c_str());
    mt_diag_config diag = mt_diag_config_default();
    diag.trace = "uikit,focus,input";
    diag.log = on_log;
    mt_diag_configure(&diag);
    mt_gles_config gl{};
    gl.present = on_present;
    mt_gles_configure(&gl);
    mt_input_configure(nullptr);
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_import_config_from_env(&cfg.imports);
    cfg.imports.log = on_log;
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
    check(!logged(r, 0, "uncaught NSException"), "no exception was raised");
    r.pad.reset();
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
    std::printf("usage: menu_nav_real_test main_menu|terms|seed|create|select|delete|game|"
                "tutorial|hunt|shops|explore DATA_DIR OUT_DIR [STEP...]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        return usage();
    }
    Run r;
    r.mode = argv[1];
    r.data = argv[2];
    r.out = argv[3];
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    fs::create_directories(r.data);
    fs::create_directories(r.out);
    // The pads' events reach the test without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    g_run = &r;
    r.focus.attach(r.actions);
    if (!run_mode(r, argc - 4, argv + 4)) {
        SDL_Quit();
        return usage();
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE));
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
