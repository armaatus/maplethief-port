// The driver of the real-asset game tests that play with a virtual gamepad (#69, #127, #76):
// the recompiled game boots from work/ as MapleThief does with --fixed-step 1 (the virtual
// clock, 1 ms per run-loop turn) on a data directory the caller gives, and from the run loop's
// turn hook the test drives the frontend's own input layer: an SDL3 virtual Xbox 360 pad
// (virtual_pad.h) -> Gamepads -> Actions -> Focus (the UIKit screens' focus, focus.h) or, through
// MenuInput (menu repeat and the confirm guard, menu_input.h; both off unless a flow turns them
// on), the runtime's actions (maplethief/input.h). It reads the game from guest memory
// (guest_state.h) and the runtime's log. Shared by menu_nav_real_test.cpp (the menus, the tutorial,
// the shops) and play_real_test.cpp (the first maps for 15 game-minutes, #76).
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
#include "assists.h"
#include "audio_out.h"
#include "clock.h"
#include "data_location.h"
#include "focus.h"
#include "gamepad.h"
#include "guest_state.h"
#include "input.h"
#include "keypad_layout.h"
#include "list_keys.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "maplethief/widescreen.h"
#include "menu_input.h"
#include "pad_pointer.h"
#include "present.h"
#include "prompts.h"
#include "replay.h"
#include "save_tools.h"
#include "scene_skip.h"
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
    // The gamepad pointer (#185, pad_pointer.h): settings.pad_pointer (off unless a flow or
    // MAPLETHIEF_SETTINGS turns it on), as game.cpp's turn.
    fe::PadPointer pointer{&input, &actions};
    fe::Focus focus{&input};
    // What the focus passes on to the game (#147): menu repeat and the confirm guard, both off
    // unless a flow turns them on (as the game's defaults).
    fe::MenuInput menu;
    fe::GlyphTracker glyphs; // the pad's glyph set (glyphs.h)
    fe::HintTracker hints;   // the port's button prompts and markers (prompts.h, #146)
    // The Assists the frontend runs every turn (#90, assists.h): turbo_attack and auto_potion
    // from `settings` (all off unless a flow or MAPLETHIEF_SETTINGS turns them on, which boot()
    // applies over them), and ui_animations' UIKit scale.
    fe::Settings settings;
    fe::Assists assists;
    fe::SceneSkip scene_skip; // skip_intros (#174, scene_skip.h), from `settings` as the Assists
    // false: the flow sets `settings` itself and MAPLETHIEF_SETTINGS is not applied (#90's own
    // modes, which compare a feature off and on).
    bool settings_from_env = true;
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
    // Run at every GL present after `screen` took the frame (its gl_pixels), if set.
    std::function<void(mt_cpu *)> on_frame;
    // The game's ticks: CMTimer's lastFrame (k_last_frame) changes once per tick.
    std::int64_t last_frame = 0;
    u64 ticks = 0;
    u64 gl_frames = 0; // the GL frames the game presented
    // An observer of every GL call (mt_gles_config.command), if set: for debugging a frame.
    void (*gl_command)(void *user, mt_cpu *cpu, const mt_gles_cmd *cmd) = nullptr;
    // The virtual clock's step a turn: 1 ms (--fixed-step 1). #151's speed measurement steps
    // 1.001 ms, so that the game's 1 ms run-loop timer fires at every turn (see game_speed in
    // play_real_test.cpp).
    u64 step_ns = 1000000;
    // Run after the process loads and before it runs, if set: a frontend change to the guest's
    // memory that the boot reads (#151's game speed table).
    std::function<void(mt_cpu *)> before_run;
    // Widescreen (#86, ADR 0090): the GL layer's width for maplethief/widescreen.h (0: off, the
    // default); the turn lets the field go wide (the driver shows no key pad).
    std::uint32_t widescreen = 0;
    // Widescreen with the touch key pad (#168): each turn, as game.cpp's turn hook, the key pad's
    // groups anchored to the wide view's edges (keypad_widescreen_turn) and Input's side margin.
    bool wide_keypad = false;
    // With wide_keypad: the groups at the view's edges (widescreen_hud = edges), or at their 3:2
    // places (narrow).
    bool wide_anchored = true;
    // The outcome of the run, for a flow's last checks (boot() fills them).
    std::uint32_t outcome = 0;
    mt_process_result result{};

    Run() {
        focus.set_forward(fe::MenuInput::sink, &menu);
        menu.set_actions(&actions);
        gamepads.set_hold_gate(fe::PadPointer::hold_gate, &pointer);
        fe::install_popup_menu_scroll(); // as game.cpp (#166)
        fe::install_shop_list_keys();    // as game.cpp (ADR 0102)
        fe::install_grid_keys();
    }
    // MT_SURVEY_SHOTS (#146): a shot whenever the screen's state changes (survey_shots).
    std::string survey_key;
    double survey_at = -1;
    int survey_n = 0;
    unsigned presses = 0; // the pad's button presses (survey_shots)
    // Window shots (#146, window_shots): one per screen state, the screen with what the port
    // draws over it in the window; the states that showed no marker and no prompt; those
    // with a marker of prompts.h (a list cursor's frame, #164).
    std::string window_key;
    double window_at = -1;
    std::vector<std::string> window_seen, window_bare, window_marked;

    // The regression run (#91, ADR 0104, replay.h). Recording: every change of the pad (and its
    // attach) goes into `record` at the turn's step. Replaying: `replay`'s pad events are played
    // at their steps in place of tasks. Either way: the checkpoints `plan` names are taken at the
    // end of their turns into `checkpoints` (the composited screen's hash, `audio`'s hash, the GL
    // frame, `state`'s line), and the run ends at the end of turn `end_step`.
    fe::Replay *record = nullptr;
    const fe::Replay *replay = nullptr;
    std::unique_ptr<fe::ReplayCursor> cursor;
    fe::CheckpointPlan plan;
    fe::CheckpointLog checkpoints;
    // Set: the mixer is rendered (and hashed) after every turn up to the virtual clock's time,
    // as MapleThief --fixed-step --audio-capture renders it.
    std::unique_ptr<fe::AudioHash> audio;
    std::function<std::string(mt_cpu *)> state;
    bool checkpoint_shots = false; // each checkpoint's screen also to OUT_DIR/check_NAME.png
    u64 end_step = UINT64_MAX;
    // The turn's step: turns from the first (0), one fixed step of virtual time each.
    u64 step() const { return turns == 0 ? 0 : turns - 1; }
};
inline Run *g_run = nullptr;
inline void run_tasks(Run &r, mt_cpu *cpu);
inline void play_replay(Run &r);
inline void take_checkpoints(Run &r, mt_cpu *cpu);
inline void survey_shots(Run &r, mt_cpu *cpu);
inline void window_shots(Run &r, mt_cpu *cpu);
inline void window_summary(const Run &r);

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
        r.pointer.handle(e);
        r.gamepads.handle(e);
        r.glyphs.handle(e, r.gamepads.active_id());
        r.hints.handle(e);
        r.presses += e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1 : 0;
    }
}

inline int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.t = now - r.first;
    mt_widescreen_turn(cpu, 1); // a no-op unless r.widescreen
    if (r.wide_keypad) {
        r.input.set_side_margin(fe::keypad_widescreen_turn(
            fe::keypad_wide(mt_widescreen_view(), mt_widescreen_width(), r.wide_anchored)));
    }
    pump(r);
    r.gamepads.turn(now);
    r.actions.turn();
    r.focus.turn(cpu, now);
    r.pointer.set_mode(r.settings.pad_pointer);
    r.pointer.set_bounds(fe::pointer_bounds(mt_widescreen_view()));
    r.pointer.turn(now, r.focus.stats().uses);
    r.menu.turn(cpu, now);
    r.hints.turn(cpu, r.focus, mt_input_panel(), true, true);
    while (r.next_step < r.steps.size() && r.t >= r.steps[r.next_step].at) {
        r.steps[r.next_step++].run(cpu);
        pump(r); // the pad's change reaches the action layer in this turn
    }
    play_replay(r); // where the steps and the tasks change the pad (#91)
    run_tasks(r, cpu);
    r.assists.turn(cpu, r.settings, r.actions, r.t);
    r.scene_skip.turn(cpu, r.settings, r.t);
    mt_uikit_set_animation_scale(fe::ui_animation_scale(r.settings.ui_animations));
    std::int64_t frame = 0;
    std::memcpy(&frame, cpu->mem + k_last_frame, sizeof frame);
    if (frame != r.last_frame) {
        r.last_frame = frame;
        r.ticks++;
    }
    if (r.on_turn) {
        r.on_turn(cpu);
    }
    survey_shots(r, cpu);
    window_shots(r, cpu);
    take_checkpoints(r, cpu);
    if (r.step() >= r.end_step) {
        r.done = true;
    }
    r.clock.advance(r.step_ns);
    if (r.audio) {
        r.audio->render_to(r.clock.elapsed_ns());
    }
    return r.done ? 1 : 0;
}

inline int on_present(void *, mt_cpu *cpu, const mt_gles_frame *frame) {
    g_run->screen.take_gl_frame(*frame);
    g_run->gl_frames++;
    if (g_run->on_frame) {
        g_run->on_frame(cpu);
    }
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
inline bool write_shot(Run &r, mt_cpu *cpu, const std::string &name) {
    if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
        return false;
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
    return true;
}
inline void shot(Run &r, double t, const std::string &name) {
    at(r, t, [&r, name](mt_cpu *cpu) { write_shot(r, cpu, name); });
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
    const std::uintmax_t n = fs::file_size(r.data / "saves" / name, ec);
    return ec ? 0 : n;
}

// A checkpoint: remembers the log position at `t`, runs `fn(cpu, from)` at `t2`.
inline void between(Run &r, double t, double t2, std::function<void(mt_cpu *, std::size_t)> fn) {
    auto from = std::make_shared<std::size_t>(0);
    at(r, t, [&r, from](mt_cpu *) { *from = r.log.size(); });
    at(r, t2, [fn, from](mt_cpu *cpu) { fn(cpu, *from); });
}

// The tests' pad: an Xbox 360. Recording (#91), its attach and every change it makes go into
// r.record at the turn's step.
inline void make_pad(Run &r) {
    r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
    check(r.pad->ok(), "an Xbox 360 virtual pad is attached " + r.pad->error);
    if (r.record != nullptr) {
        r.record->add({r.step(), fe::ReplayKind::kPadAttach, 0, 0, "xbox360"});
        r.pad->on_change = [&r](bool axis, int index, int value) {
            r.record->add({r.step(), axis ? fe::ReplayKind::kPadAxis : fe::ReplayKind::kPadButton,
                           index, value, ""});
        };
    }
}
inline void attach_pad(Run &r) {
    at(r, 0.5, [&r](mt_cpu *) { make_pad(r); });
}

// --- the regression run (#91, ADR 0104) ----------------------------------------------------------

// Plays the replay's pad events due at this turn's step as a recording run made them: an attach
// where the steps run (then a pump), the buttons and axes where the tasks run, then a pump (which
// run_tasks makes every turn once the pad is there).
inline void play_replay(Run &r) {
    if (r.replay == nullptr) {
        return;
    }
    if (!r.cursor) {
        r.cursor = std::make_unique<fe::ReplayCursor>(r.replay);
    }
    while (const fe::ReplayEvent *e = r.cursor->next(r.step())) {
        switch (e->kind) {
        case fe::ReplayKind::kPadAttach:
            make_pad(r);
            pump(r);
            break;
        case fe::ReplayKind::kPadButton:
            if (r.pad != nullptr) {
                r.pad->button(static_cast<SDL_GamepadButton>(e->index), e->value != 0);
            }
            break;
        case fe::ReplayKind::kPadAxis:
            if (r.pad != nullptr) {
                r.pad->axis(static_cast<SDL_GamepadAxis>(e->index), static_cast<Sint16>(e->value));
            }
            break;
        default:
            break;
        }
    }
    if (r.pad != nullptr && !r.done) {
        pump(r);
    }
}

// The checkpoints due at the end of this turn: the composited screen (as --shot writes it), the
// audio so far, the GL frame and the state line.
inline void take_checkpoints(Run &r, mt_cpu *cpu) {
    for (const std::string &name : r.plan.due(r.step())) {
        fe::Checkpoint c;
        c.name = name;
        c.step = r.step();
        if (r.screen.compose(cpu, mt_cf_absolute_time())) {
            c.frame = fe::frame_hash(r.screen.pixels().data(), r.screen.width(), r.screen.height());
        }
        if (r.audio) {
            c.audio = r.audio->hash();
            c.audio_frames = r.audio->frames();
        }
        c.gl_frame = static_cast<std::uint32_t>(r.gl_frames);
        c.state = r.state ? r.state(cpu) : std::string();
        r.checkpoints.add(c);
        if (r.checkpoint_shots) {
            write_shot(r, cpu, "check_" + name);
        }
        std::printf("checkpoint %s\n", fe::format_checkpoint(c).c_str());
        std::fflush(stdout);
    }
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
        window_summary(r);
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
// (CDataMgr::SaveSaveData, SaveQuestData), then OK, and back to the field. `saved`, if set,
// runs while the game shows "saved" (popup 2, the game paused): the files are written, and the
// hero is as they hold it.
inline void then_save(Run &r, std::function<void(mt_cpu *)> saved = nullptr) {
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
    if (saved) {
        then_do(r, "the save is written", saved);
    }
    then_press(r, A);
    then_press(r, B);
    then_press(r, B);
    then_wait_field(r);
}

// The main menu to the game: B closes the promo banner where it shows (a fresh install), A on
// Play; with a save, A on the first slot (the focus goes on to Start), A.
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
    then_press(r, A); // the slot is chosen and the focus goes on to Start (ADR 0102)
    then_wait_for(r, "the focus on Start", 5, [&r](mt_cpu *) { return focused(r, "clickStart:"); });
    // The 1.5 s the five presses to Start took before ADR 0102: the bots' fights after the
    // load follow the game's random numbers, and so the run's clock.
    then_wait(r, 1.5);
    then_press(r, A);
    then_wait_for(r, "the game", 30, field, 1.0);
}

inline void then_finish(Run &r) {
    then_do(r, "finish", [&r](mt_cpu *) {
        release_all(r);
        check(r.input.live() == 0, "no touch is left down");
        window_summary(r);
        r.done = true;
    });
}

// The save files' map and chapter (docs/formats/save.md: theif +0x41C area, +0x41D map;
// quest +0 chapter).
inline std::vector<std::uint8_t> read_save(const Run &r, const char *name) {
    std::ifstream in(r.data / "saves" / name, std::ios::binary);
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

// A stage of the way reached (#91: the regression recording takes a checkpoint there).
using Mark = std::function<void(const std::string &)>;

// From a new game (then_new_game) through the intro and the tutorial's quests 0-4 on maps 2.8
// and 2.9 (talk, jump, climb, a portal) to the first fight: the snail of quest 5. `mark` (if
// set) is told "intro", "first_map", "map_2_9" and "combat" as each is reached.
inline void then_tutorial_to_snail(Run &r, const Mark &mark = {}) {
    const auto stage = [&mark](const char *name) {
        if (mark) {
            mark(name);
        }
    };
    then_wait_for(r, "the intro (map 2.10)", 60, [](mt_cpu *cpu) { return hero(cpu).on(2, 10); });
    stage("intro");
    // The intro's speech boxes (panel 3) and MacArthur's first dialog (panel 5): A each.
    then(r, "the intro: A through its boxes", 90, [&r](mt_cpu *cpu, double el) {
        const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
        hold(r, A, box && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_wait_field(r);
    then_check(r, "the intro ends in the field on map 2.8, quest 0 (Talking to NPCs) active",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 8) && main_quest(0, 1)(cpu); });
    stage("first_map");
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
    stage("map_2_9");
    then_walk_to(r, 216);
    then_talk(r, "MacArthur: quest 4 done, 5 (attack a monster) given", main_quests(4, 5));
    then_walk_to(r, 178, 3); // the ladder at x 175
    then_climb(r, 319);
    then_fight(r, "the snail (quest 5)", 1,
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 5) >= 1; }, 60,
               {2, 8, 0, 2, 9});
    then_check(r, "the snail is defeated: quest 5's requirement 1/1",
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 5) >= 1; });
    stage("combat");
}

// After the snail: back to MacArthur on 2.8 (quest 5 done, 6 given), the left portal to Henesys
// (2.0, chapter 1) and elder Stan, who gives Elder's Advice (main quest 7).
inline void then_tutorial_from_snail(Run &r) {
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

// From a new game (then_new_game) through the intro, the tutorial's quests 0-6 on maps 2.8
// and 2.9 (talk, jump, climb, a portal, a snail), the left portal to Henesys (2.0, chapter 1)
// and elder Stan, who gives Elder's Advice (main quest 7). Each step checks the quest it
// completes.
inline void then_tutorial(Run &r) {
    then_tutorial_to_snail(r);
    then_tutorial_from_snail(r);
}

// --- the regression recording (#91, ADR 0104) --------------------------------------------------

// Recording: a named checkpoint at the end of this turn (a task: it takes the turn).
inline void then_checkpoint(Run &r, const std::string &name) {
    then(r, "checkpoint " + name, 1, [&r, name](mt_cpu *, double) {
        if (r.record != nullptr) {
            r.record->add({r.step(), fe::ReplayKind::kCheck, 0, 0, name});
        }
        r.plan.add(r.step(), name);
        return true;
    });
}
// Recording: the run ends `after` seconds from now, with a checkpoint "end".
inline void then_end(Run &r, double after) {
    then_wait(r, after);
    then(r, "the end", 1, [&r](mt_cpu *, double) {
        release_all(r);
        if (r.record != nullptr) {
            r.record->add({r.step(), fe::ReplayKind::kCheck, 0, 0, "end"});
            r.record->add({r.step(), fe::ReplayKind::kEnd, 0, 0, ""});
        }
        r.plan.add(r.step(), "end");
        r.end_step = r.step();
        return true;
    });
}

// --- the boot ----------------------------------------------------------------------------------

inline int boot(Run &r, const std::vector<std::uint8_t> &slice) {
    mt_cf_config cf = mt_cf_config_default();
    mt_cf_config_from_env(&cf); // MAPLETHIEF_LANGUAGE
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    // The data folder as the frontend lays it out (data_location.h, ADR 0127): an old layout
    // migrated, the game's files and gamecenter.json in saves/.
    {
        const fe::DataLocation loc = fe::data_location_at(r.data.string());
        for (const std::string &line : fe::migrate_data_layout(loc)) {
            std::printf("%s\n", line.c_str());
        }
        fe::configure_save_location(r.settings, loc);
    }
    mt_diag_config diag = mt_diag_config_default();
    diag.trace = r.trace.c_str();
    diag.log = on_log;
    mt_diag_configure(&diag);
    mt_gles_config gl{};
    gl.present = on_present;
    gl.command = r.gl_command;
    mt_gles_configure(&gl);
    mt_widescreen_configure(r.widescreen);
    mt_input_configure(nullptr);
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_import_config_from_env(&cfg.imports);
    cfg.imports.log = on_log;
    cfg.app_main = mt_uikit_app_main;
    cfg.app_user = &ui;
    mt_hooks_install_quest_page_fix(1); // in every mode, as the game (#167)
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
    // MAPLETHIEF_SETTINGS (#90): a whole suite run with a feature on, as the game applies it.
    if (const char *list = std::getenv("MAPLETHIEF_SETTINGS");
        list != nullptr && r.settings_from_env) {
        std::string why;
        const bool ok = fe::apply_setting_list(&r.settings, list, &why);
        check(ok, "MAPLETHIEF_SETTINGS applies" + (ok ? std::string() : ": " + why));
        std::printf("MAPLETHIEF_SETTINGS: %s\n", list);
    }
    mt_uikit_set_animation_scale(fe::ui_animation_scale(r.settings.ui_animations));
    fe::install_auto_loot(r.settings.auto_loot); // as game.cpp (#175); a flow may change it live
    if (r.before_run) {
        r.before_run(mt_process_cpu(p));
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
    mt_widescreen_configure(0);
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    mt_saves_set_documents_dir(nullptr);
    mt_stubs_set_gamecenter_path(nullptr);
    mt_stubs_set_data_dir(nullptr);
    r.clock.uninstall();
    mt_uikit_set_animation_scale(1);
    fe::install_auto_loot(false);
    return 0;
}

// MT_SURVEY_SHOTS (#146, for looking): a shot 0.4 s after the screen's state changes (the key
// pad's panel, the game mode, the popup, the popup menu's tab, the focused control), named by
// that state, to OUT_DIR/survey_NNN_....png.
inline void survey_shots(Run &r, mt_cpu *cpu) {
    static const bool on = std::getenv("MT_SURVEY_SHOTS") != nullptr;
    if (!on) {
        return;
    }
    const std::int32_t panel = mt_input_panel();
    std::string key = "p" + std::to_string(panel) + "_m" + std::to_string(game_mode(cpu)) + "_pop" +
                      std::to_string(popup_id(cpu));
    if (panel == 1) {
        key += "_tab" + std::to_string(menu_tab(cpu)) + "_" + std::to_string(menu_sub(cpu));
    }
    if (panel != 0 && panel != 3) { // a menu, a popup or a screen: after every press too
        key += "_k" + std::to_string(r.presses);
    }
    if (const fe::FocusTarget *f = r.focus.focused(); f != nullptr && r.focus.active()) {
        key += "_" + f->action;
    }
    std::replace(key.begin(), key.end(), ':', '_');
    if (key != r.survey_key) {
        r.survey_key = key;
        r.survey_at = r.t + 0.4;
    }
    if (r.survey_at >= 0 && r.t >= r.survey_at) {
        r.survey_at = -1;
        char n[16];
        std::snprintf(n, sizeof n, "%03d", r.survey_n++);
        write_shot(r, cpu, "survey_" + std::string(n) + "_" + r.survey_key);
    }
}

// The window's image of the screen now (#146): the composited screen at 2 pixels a point with
// what the port draws over it in the window (the focus highlight, the markers and the button
// prompts of the device in use: prompts.h's draw_marker and draw_prompts, as the presenter and
// HintLayer draw them), to OUT_DIR/name.png. Returns the number of markers and prompts drawn
// (-1 when the screen could not be composed).
inline int write_window_shot(Run &r, mt_cpu *cpu, const std::string &name) {
    if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
        return -1;
    }
    constexpr int k = 2;
    const int w = static_cast<int>(r.screen.width()), h = static_cast<int>(r.screen.height());
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * k) * static_cast<std::size_t>(h * k) *
                                 4);
    const std::uint8_t *src = r.screen.pixels().data();
    for (int y = 0; y < h * k; y++) {
        for (int x = 0; x < w * k; x++) {
            std::memcpy(&px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w * k) +
                             static_cast<std::size_t>(x)) *
                            4],
                        src + (static_cast<std::size_t>(y / k) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(x / k)) *
                                  4,
                        4);
        }
    }
    fe::Canvas canvas(px.data(), static_cast<std::uint32_t>(w * k),
                      static_cast<std::uint32_t>(h * k), k);
    int n = 0;
    if (const fe::FocusTarget *f = fe::focus_highlight_target(r.focus, r.hints.device())) {
        fe::draw_marker(canvas, fe::Rect{f->x, f->y, f->w, f->h});
        n++;
    }
    for (const fe::Rect &m : r.hints.hints().markers) {
        fe::draw_marker(canvas, m);
        n++;
    }
    const fe::PromptStyle style{r.hints.device(), r.glyphs.type(), r.gamepads.face_buttons()};
    n += fe::draw_prompts(canvas, r.hints.hints().prompts, style, r.actions.bindings());
    SDL_Surface *s =
        SDL_CreateSurfaceFrom(w * k, h * k, SDL_PIXELFORMAT_ABGR8888, px.data(), w * k * 4);
    if (s != nullptr) {
        SDL_SavePNG(s, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(s);
    }
    return n;
}

// Window shots (#146): 0.4 s after the screen's state settles (the key pad's panel, the game
// mode, the popup, the popup menu's tab and list, the focused control), a window shot of each new
// state, OUT_DIR/window_NN_<state>.png, with a line saying what the port drew. The field (panel
// 0) is not a menu, and nothing is drawn before a device was used. A state with no marker and
// no prompt goes into window_bare (window_summary fails on it).
inline void window_shots(Run &r, mt_cpu *cpu) {
    const std::int32_t panel = mt_input_panel();
    if (panel == 0 || r.hints.device() == fe::PromptDevice::kNone) {
        r.window_key.clear();
        r.window_at = -1;
        return;
    }
    std::string key = "p" + std::to_string(panel) + "_m" + std::to_string(game_mode(cpu)) + "_pop" +
                      std::to_string(popup_id(cpu));
    if (panel == 1) {
        key += "_tab" + std::to_string(menu_tab(cpu)) + "_" + std::to_string(menu_sub(cpu));
    }
    if (const fe::FocusTarget *f = r.focus.focused(); f != nullptr && r.focus.active()) {
        key += "_" + f->target + "_" + f->action;
    }
    std::replace(key.begin(), key.end(), ':', '_');
    if (key != r.window_key) {
        r.window_key = key;
        r.window_at = r.t + 0.4;
    }
    if (r.window_at < 0 || r.t < r.window_at) {
        return;
    }
    r.window_at = -1;
    if (std::find(r.window_seen.begin(), r.window_seen.end(), key) != r.window_seen.end()) {
        return;
    }
    r.window_seen.push_back(key);
    char n[16];
    std::snprintf(n, sizeof n, "%02zu", r.window_seen.size());
    const int drawn = write_window_shot(r, cpu, "window_" + std::string(n) + "_" + key);
    std::printf("window shot %s at %.2f s: %s: %zu prompts, %zu markers%s (%d drawn)\n", n, r.t,
                key.c_str(), r.hints.hints().prompts.size(), r.hints.hints().markers.size(),
                fe::focus_highlight_target(r.focus, r.hints.device()) != nullptr
                    ? ", the focus highlight"
                    : "",
                drawn);
    if (drawn == 0) {
        r.window_bare.push_back(key);
    }
    if (!r.hints.hints().markers.empty()) {
        r.window_marked.push_back(key);
    }
}

// The screen states the run showed with a gamepad, each with a focus marker or a prompt in its
// window shot (#146): fails for each that had neither.
inline void window_summary(const Run &r) {
    std::string bare;
    for (const std::string &k : r.window_bare) {
        bare += " " + k;
    }
    check(r.window_bare.empty(), "window shots: " + std::to_string(r.window_seen.size()) +
                                     " screen states, each with a focus marker or a button prompt" +
                                     (bare.empty() ? std::string() : "; without:" + bare));
}

inline std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace game_driver

#endif // MAPLETHIEF_TESTS_GAME_DRIVER_H
