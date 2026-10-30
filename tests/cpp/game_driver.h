// The driver of the real-asset game tests that play with a virtual gamepad (#69, #127, #76):
// the recompiled game boots from work/ as MapleThief does with --fixed-step 1 (the virtual
// clock, 1 ms per run-loop turn) on a data directory the caller gives, and from the run loop's
// turn hook the test drives the frontend's own input layer: an SDL3 virtual Xbox 360 pad
// (virtual_pad.h) -> Gamepads -> Actions -> Focus (the UIKit screens' focus, focus.h) or the
// runtime's actions (maplethief/input.h). It reads the game from guest memory (guest_state.h)
// and the runtime's log. Shared by menu_nav_real_test.cpp (the menus, the tutorial, the shops)
// and play_real_test.cpp (the first maps for 15 game-minutes, #76).
//
// A flow is a list of timed steps (`at`, `press`, `shot`: at a virtual time) and of tasks (the
// `then_*` helpers): each task reacts to what the game shows and presses the pad's buttons until
// it has happened, so a frame more or less of loading moves nothing. No taps and no writes to
// guest memory: the tasks only read it (`data_mgr` calls a getter).
//
// Header-only: each test program is one translation unit that includes it once.
#ifndef MAPLETHIEF_TESTS_GAME_DRIVER_H
#define MAPLETHIEF_TESTS_GAME_DRIVER_H

#if !defined(MT_HAVE_GENERATED_CODE)
#error "game_driver.h needs the generated code (MT_HAVE_GENERATED_CODE)"
#endif

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

namespace game_driver {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u64 = std::uint64_t;

using namespace guest_state;

// Guest facts (docs/controls/keypad.md, docs/re/program-flow.md; the readers: guest_state.h).
constexpr u32 k_data_mgr = 0x5BAC1;    // CDataMgr::GetInstance
constexpr u32 k_last_frame = 0x11A4D8; // CMTimer's lastFrame (i64 ms): set before every tick

inline int g_failures = 0;
inline void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    g_failures += ok ? 0 : 1;
}

// CDataMgr::GetInstance(): the menu's data (+0xA8 the open submenu, +0xB0 the tab, +0xCC the
// shortcut panel open, docs/controls/keypad.md).
inline u32 data_mgr(mt_cpu *cpu) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, k_data_mgr, nullptr, 0, k_data_mgr);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}
inline i32 menu_tab(mt_cpu *cpu) { return static_cast<i32>(rd(cpu, data_mgr(cpu) + 0xB0)); }
inline i32 menu_sub(mt_cpu *cpu) { return static_cast<i32>(rd(cpu, data_mgr(cpu) + 0xA8)); }
inline bool shortcuts_open(mt_cpu *cpu) {
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
    // The runtime's trace channels that boot() enables (their lines go to `log`).
    std::string trace = "uikit,focus,input";
    // Run every turn after the steps and tasks (a flow's own monitor), if set.
    std::function<void(mt_cpu *)> on_turn;
    // The game's ticks: CMTimer's lastFrame (k_last_frame) changes once per tick.
    std::int64_t last_frame = 0;
    u64 ticks = 0;
    u64 gl_frames = 0; // the GL frames the game presented
    // An observer of every GL call (mt_gles_config.command), if set: for debugging a frame.
    void (*gl_command)(void *user, mt_cpu *cpu, const mt_gles_cmd *cmd) = nullptr;
    // The outcome of the run, for a flow's last checks (boot() fills them).
    std::uint32_t outcome = 0;
    mt_process_result result{};
};
inline Run *g_run = nullptr;
inline void run_tasks(Run &r, mt_cpu *cpu);

inline void on_log(void *, const char *line) {
    std::fprintf(stderr, "%s\n", line);
    if (g_run != nullptr) {
        g_run->log.emplace_back(line);
    }
}

inline void pump(Run &r) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        r.input.handle(e);
        r.gamepads.handle(e);
    }
}

inline int turn(void *, mt_cpu *cpu, double now, double) {
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
    std::int64_t frame = 0;
    std::memcpy(&frame, cpu->mem + k_last_frame, sizeof frame);
    if (frame != r.last_frame) {
        r.last_frame = frame;
        r.ticks++;
    }
    if (r.on_turn) {
        r.on_turn(cpu);
    }
    r.clock.advance(1000000);
    return r.done ? 1 : 0;
}

inline int on_present(void *, mt_cpu *, const mt_gles_frame *frame) {
    g_run->screen.take_gl_frame(*frame);
    g_run->gl_frames++;
    return 0;
}

inline void at(Run &r, double t, std::function<void(mt_cpu *)> fn) {
    r.steps.push_back({t, std::move(fn)});
}

// A button of the pad: down at t, up `hold` seconds later.
inline void press(Run &r, double t, SDL_GamepadButton b, double hold = 0.1) {
    at(r, t, [&r, b](mt_cpu *) { r.pad->button(b, true); });
    at(r, t + hold, [&r, b](mt_cpu *) { r.pad->button(b, false); });
}
// The left stick pushed along `axis` to `value` for `hold` seconds.
inline void stick(Run &r, double t, SDL_GamepadAxis axis, Sint16 value, double hold) {
    at(r, t, [&r, axis, value](mt_cpu *) { r.pad->axis(axis, value); });
    at(r, t + hold, [&r, axis](mt_cpu *) { r.pad->axis(axis, 0); });
}

// The screen composited now (the GL frame in its place), to OUT_DIR/name.png.
inline void shot(Run &r, double t, const std::string &name) {
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

inline std::string focus_text(const Run &r) {
    const fe::FocusTarget *f = r.focus.focused();
    if (!r.focus.active()) {
        return "focus off";
    }
    return f == nullptr ? std::string("focus on, nothing focused")
                        : "focus on -[" + f->target + " " + f->action + "]";
}

inline void state(Run &r, double t) {
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
inline std::vector<std::string> actions_since(const Run &r, std::size_t from) {
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
inline void check_actions(const Run &r, std::size_t from, const std::vector<std::string> &want,
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

inline bool logged(const Run &r, std::size_t from, const std::string &needle) {
    for (std::size_t i = from; i < r.log.size(); i++) {
        if (r.log[i].find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline std::uintmax_t file_size(const Run &r, const char *name) {
    std::error_code ec;
    const std::uintmax_t n = fs::file_size(r.data / "Documents" / name, ec);
    return ec ? 0 : n;
}

// A checkpoint: remembers the log position at `t`, runs `fn(cpu, from)` at `t2`.
inline void between(Run &r, double t, double t2, std::function<void(mt_cpu *, std::size_t)> fn) {
    auto from = std::make_shared<std::size_t>(0);
    at(r, t, [&r, from](mt_cpu *) { *from = r.log.size(); });
    at(r, t2, [fn, from](mt_cpu *cpu) { fn(cpu, *from); });
}

inline void attach_pad(Run &r) {
    at(r, 0.5, [&r](mt_cpu *) {
        r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
        check(r.pad->ok(), "an Xbox 360 virtual pad is attached " + r.pad->error);
    });
}

inline void finish(Run &r, double t) {
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
inline void hold(Run &r, SDL_GamepadButton b, bool down) {
    const auto it = std::find(r.held.begin(), r.held.end(), b);
    if (down && it == r.held.end()) {
        r.held.push_back(b);
        r.pad->button(b, true);
    } else if (!down && it != r.held.end()) {
        r.held.erase(it);
        r.pad->button(b, false);
    }
}
inline void release_all(Run &r) {
    while (!r.held.empty()) {
        hold(r, r.held.back(), false);
    }
}

inline std::string where(mt_cpu *cpu) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "panel %d, game mode %d, popup %d; ",
                  static_cast<int>(mt_input_panel()), game_mode(cpu), popup_id(cpu));
    return buf + hero_text(cpu, hero(cpu));
}

// The field: panel 0, game mode 0, no popup, a hero.
inline bool field(mt_cpu *cpu) {
    return mt_input_panel() == 0 && game_mode(cpu) == 0 && popup_id(cpu) == -1 &&
           hero(cpu).addr != 0;
}

inline void run_tasks(Run &r, mt_cpu *cpu) {
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

inline void then(Run &r, std::string what, double timeout,
                 std::function<bool(mt_cpu *, double)> step) {
    r.tasks.push_back({std::move(what), timeout, std::move(step)});
}
inline void then_do(Run &r, std::string what, std::function<void(mt_cpu *)> fn) {
    then(r, std::move(what), 1, [fn](mt_cpu *cpu, double) {
        fn(cpu);
        return true;
    });
}
inline void then_check(Run &r, std::string what, Pred p) {
    then_do(r, what, [what, p](mt_cpu *cpu) { check(p(cpu), what + " (" + where(cpu) + ")"); });
}
inline void then_wait(Run &r, double s) {
    then(r, "wait " + std::to_string(s) + " s", s + 1,
         [s](mt_cpu *, double el) { return el >= s; });
}
// `b` held `hold_s` seconds, then `after` seconds with nothing held.
inline void then_press(Run &r, SDL_GamepadButton b, double hold_s = 0.1, double after = 0.4) {
    then(r, std::string("press ") + SDL_GetGamepadStringForButton(b), hold_s + after + 1,
         [&r, b, hold_s, after](mt_cpu *, double el) {
             hold(r, b, el < hold_s);
             return el >= hold_s + after;
         });
}
// Waits until `p` has held for `stable` seconds.
inline void then_wait_for(Run &r, std::string what, double timeout, Pred p, double stable = 0) {
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
inline void then_wait_field(Run &r, double timeout = 20) {
    then_wait_for(r, "the field", timeout, field, 0.5);
}

// Walks to x (the d-pad held that way, let go within `tol`). Where the hero stops short (a
// step, a ledge), A jumps with the direction still held.
inline void then_walk_to(Run &r, int x, int tol = 4) {
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
inline void then_climb(Run &r, int y) {
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
inline void then_portal(Run &r, int area, int map) {
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
inline void then_talk(Run &r, std::string what, Pred done, double timeout = 90) {
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
inline void then_fight(Run &r, std::string what, i32 want, Pred done, double timeout,
                       Comeback back) {
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
inline void then_save(Run &r) {
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
inline bool focused(const Run &r, const char *action) {
    const fe::FocusTarget *f = r.focus.focused();
    return r.focus.active() && f != nullptr && f->action == action;
}
inline void then_new_game(Run &r) {
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
inline void then_load_slot0(Run &r) {
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

inline void then_finish(Run &r) {
    then_do(r, "finish", [&r](mt_cpu *) {
        release_all(r);
        check(r.input.live() == 0, "no touch is left down");
        r.done = true;
    });
}

// The save files' map and chapter (docs/formats/save.md: theif +0x41C area, +0x41D map;
// quest +0 chapter).
inline std::vector<std::uint8_t> read_save(const Run &r, const char *name) {
    std::ifstream in(r.data / "Documents" / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// --- the tutorial (#127) ----------------------------------------------------------------------

inline Pred main_quest(u32 n, i32 state) {
    return [n, state](mt_cpu *cpu) { return quest_state(cpu, hero(cpu), 0, n) == state; };
}
inline Pred main_quests(u32 done_n, u32 active_n) {
    return [done_n, active_n](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return quest_state(cpu, h, 0, done_n) == 2 && quest_state(cpu, h, 0, active_n) == 1;
    };
}

// From a new game (then_new_game) through the intro, the tutorial's quests 0-6 on maps 2.8
// and 2.9 (talk, jump, climb, a portal, a snail), the left portal to Henesys (2.0, chapter 1)
// and elder Stan, who gives Elder's Advice (main quest 7). Each step checks the quest it
// completes.
inline void then_tutorial(Run &r) {
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
}

// --- the boot ----------------------------------------------------------------------------------

inline int boot(Run &r, const std::vector<std::uint8_t> &slice) {
    mt_cf_config cf = mt_cf_config_default();
    mt_cf_config_from_env(&cf); // MAPLETHIEF_LANGUAGE
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    mt_stubs_set_data_dir(r.data.string().c_str());
    mt_diag_config diag = mt_diag_config_default();
    diag.trace = r.trace.c_str();
    diag.log = on_log;
    mt_diag_configure(&diag);
    mt_gles_config gl{};
    gl.present = on_present;
    gl.command = r.gl_command;
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
    r.outcome = outcome;
    r.result = *res;
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

inline std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace game_driver

#endif // MAPLETHIEF_TESTS_GAME_DRIVER_H
