// The gamepad in the real game (#67, ADR 0048). The recompiled game boots from work/ as
// MapleThief does with --fixed-step 1 (the virtual clock, 1 ms per run-loop turn) from a fresh
// data directory; from the run loop's turn hook this test drives the frontend's own input
// layer: SDL3 virtual joysticks (tests/cpp/virtual_pad.h) -> Gamepads -> Actions -> the
// runtime's actions (maplethief/input.h), and touches through Input::touch, the path of every
// touch (ADR 0047). It reads the hero from guest memory: SkyCanvas 0x11A950 -> the current
// state +0x0C (CStateGame) -> the hero +0x30 (CHero): x +0x04, y +0x08, state +0x10, HP +0x94,
// MP +0x98, the other-items inventory +0xD8 (item id +0x90, count +0x100), the shortcuts
// +0xE0 / +0xE4 (docs/formats/save.md, docs/controls/keypad.md). Local only: it needs work/gen
// (python -m recomp all), the slice and the extracted bundle, and exits 77 (skipped) without
// them.
//
// The way into the first map, as tests/golden/device_screens.json's "fresh" run: a touch on
// the promo banner's close button (3 s) and on Play (4.5 s); then the pad's A (OK, -5 in
// menus) for the intro's five speech boxes (30-46 s) and MacArthur's four OKs (51-54 s).
//
//   gamepad_real_test play OUT_DIR      (input.gamepad_play)
//     Then, for an Xbox 360, a PS4 and a PS5 pad, hot-plugged one after the other (each
//     unplugged before the next): walk right and left (the stick, the d-pad: hero x), jump (A:
//     hero y), attack (X: the hero's state), a skill (LB, quick slot 3: Double Step, MP) and
//     the potions (Y and B, quick slots 1 and 2: the Red / Blue Potion counts, HP and MP). The
//     new character has no skill, so the test gives it Double Step the way the game does: job
//     tier 1 (hero +0x8C), the skill list rebuilt by the game's own 0x7FD69 (list, branch 0,
//     tier 1), the skill's level 1 and quick slot 3 = (2 skill, 3).
//
//   gamepad_real_test hold touch|pad|touch-always|pad-auto OUT_FILE
//     (input.gamepad_hold: touch and pad, then compared; input.keypad_hide: all four)
//     Then holds left 1 s, attack 1 s, right 1.5 s with attack held 0.5 s of it, either as
//     touches on the key pad (Input::touch: the d-pad's left and right arms, the attack key)
//     or as the virtual pad (d-pad left, X, the stick right), on the same turns. Writes the
//     hero's x, y and state at every game tick of that stretch to OUT_FILE, and a hash of every
//     GL frame the game presents from the first map on to OUT_FILE.gl: the hero files must be
//     identical (platform/gamepad_test.cmake, platform/keypad_hide_test.cmake compare them).
//     With "-always" the key pad is hidden from the start (KeypadHider, hide_keypad = always:
//     the touches land on the hidden pad); with "-auto" it hides at the pad's first press (#71).
//
//   gamepad_real_test keypad OUT_DIR   (input.keypad_hide, #71, ADR 0052)
//     The key pad's hiding in the real game, with the frontend's KeypadHider for each of auto,
//     always and never hearing the same input (Input's activity listener), auto's opacity on
//     the game pad: always is hidden from the first turn and never shows, never never hides;
//     auto stays shown through the touches into the game, hides after the pad's first A, shows
//     after an SDL mouse click on a dummy-driver window, hides after a pad press, and waits
//     for a held mouse button before hiding. In the first map the screen composited with the
//     pad hidden differs from the one composited (in the same turn) with it shown only inside
//     panel 0's rects (docs/controls/keypad.md), and somewhere inside the d-pad and the attack
//     key. The composited screens go to OUT_DIR.
//
//   gamepad_real_test rumble OUT_DIR    (input.rumble, #72, ADR 0053)
//     With a virtual Xbox 360 pad whose motors record SDL's calls, and the frontend's Rumble
//     installed (the vibrate and game-event hooks): the hero is hit through the game's own
//     damage path, CHero::TakeDamage 0x65B55 (7 HP, as a monster's touch calls it), and gains
//     a level through the real exp path, CHero::setExp 0x6654D with the EXP the level needs
//     (CHero::ExpToLevelUp 0x65919), once each with rumble off and once each with it on at 60 %.
//     Off: HP and level change and the motors get no call. On: one rumble per event, the hit's
//     the game's own vibration (its option is on by default) merged with the damage event, the
//     level-up's the level-up effect, each at 60 %.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
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
#include "gamepad.h"
#include "input.h"
#include "keypad_hide.h"
#include "maplethief/audio.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/events.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "present.h"
#include "rumble.h"
#include "scaling.h"
#include "virtual_pad.h"

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
constexpr u32 k_take_damage = 0x65B55;    // CHero::TakeDamage(hero, damage, from_left)
constexpr u32 k_set_exp = 0x6654D;        // CHero::setExp(hero, gained)
constexpr u32 k_exp_to_level = 0x65919;   // CHero::ExpToLevelUp(hero): the level's EXP
constexpr i32 k_red_potion = 3;           // item_etc ids
constexpr i32 k_blue_potion = 6;
constexpr i32 k_double_step = 3; // skill.dat: Rogue, tier 1, active, MP 6 at level 1

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

u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

struct Hero {
    u32 addr = 0;
    i32 x = 0, y = 0, flags = 0, state = 0, hp = 0, mp = 0, level = 0, exp = 0;
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
    h.level = static_cast<i32>(rd(cpu, h.addr + 0x80));
    h.exp = static_cast<i32>(rd(cpu, h.addr + 0x90));
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
    fe::VirtualClock clock;
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Rumble rumble{&gamepads}; // installed in the rumble mode only
    bool rumble_pad = false;      // the pad of to_field has motors
    std::vector<Step> steps;
    std::size_t next_step = 0;
    u64 turns = 0;
    double first = 0, t = 0;
    bool done = false;
    // The hero at every tick from `sampling` on.
    bool sampling = false;
    i64 last_tick = 0;
    std::vector<Sample> samples;
    std::unique_ptr<virtual_pad::Pad> pad;
    // The key pad's hiding (#71): hiders hearing Input's activity; the first one's opacity goes
    // on the game pad every turn.
    std::vector<std::unique_ptr<fe::KeypadHider>> hiders;
    // The GL frames: the last one presented, and a hash of each one while sampling.
    fe::Screen screen{1};
    std::vector<std::string> gl_hashes;
    // keypad: a dummy-driver window for SDL mouse events (960x640: the screen at 2x).
    SDL_Window *window = nullptr;
    fe::Placement placement;
    fs::path out_dir;
};
Run *g_run = nullptr;

void hear(void *user, fe::InputSource source) {
    for (const auto &h : static_cast<Run *>(user)->hiders) {
        h->on_activity(source);
    }
}

// The game's presents: the frame kept for compositing, its hash noted while sampling.
int on_present(void *, mt_cpu *, const mt_gles_frame *frame) {
    Run &r = *g_run;
    if (!r.screen.take_gl_frame(*frame) || !r.sampling) {
        return 0;
    }
    u64 h = 1469598103934665603ull; // FNV-1a
    for (const std::uint8_t b : r.screen.gl_pixels()) {
        h = (h ^ b) * 1099511628211ull;
    }
    char line[96];
    std::snprintf(line, sizeof line, "%.3f %u %016llx", r.t, frame->index,
                  static_cast<unsigned long long>(h));
    r.gl_hashes.emplace_back(line);
    return 0;
}

void pump(Run &r) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        r.input.handle(e);
        r.gamepads.handle(e);
    }
}

// After every run-loop turn, as the frontend's turn hook: SDL's events into Input and
// Gamepads, the synthetic touches, then the script's steps that are due, the hero at a new
// tick, and 1 ms more of virtual time.
int turn(void *, mt_cpu *cpu, double now, double) {
    Run &r = *g_run;
    if (r.turns++ == 0) {
        r.first = now;
    }
    r.t = now - r.first;
    pump(r);
    r.actions.turn();
    for (const auto &h : r.hiders) {
        h->turn(now, r.input.live());
    }
    if (!r.hiders.empty()) {
        fe::apply_keypad_opacity(r.hiders.front()->opacity(now));
    }
    r.rumble.turn();
    while (r.next_step < r.steps.size() && r.t >= r.steps[r.next_step].at) {
        r.steps[r.next_step++].run(cpu);
        pump(r); // the pad's change reaches the action layer in this turn
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

// A button of the attached pad: down at t, up `hold` seconds later.
void press(Run &r, double t, SDL_GamepadButton b, double hold = 0.1) {
    at(r, t, [&r, b](mt_cpu *) { r.pad->button(b, true); });
    at(r, t + hold, [&r, b](mt_cpu *) { r.pad->button(b, false); });
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

// Into the first map: the banner's close button and Play by touch, then the pad's A (OK) for
// the five speech boxes and MacArthur's four OKs. An Xbox 360 pad is attached at 0.5 s.
void to_field(Run &r) {
    at(r, 0.5, [&r](mt_cpu *) {
        r.pad =
            std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360, false, r.rumble_pad);
        check(r.pad->ok(), "an Xbox 360 virtual pad is attached " + r.pad->error);
    });
    tap(r, 3.0, 462, 227);
    tap(r, 4.5, 106, 165);
    for (const double t : {30.0, 34.0, 38.0, 42.0, 46.0, 51.0, 52.0, 53.0, 54.0}) {
        press(r, t, SDL_GAMEPAD_BUTTON_SOUTH);
    }
    at(r, 57.0, [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.addr != 0 && mt_input_panel() == 0 && mt_input_pad_live() != 0,
              "the pad's A went through the intro's speech boxes and MacArthur's dialog: the "
              "first map, the key pad's panel 0 live, the hero at " +
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

// One pad, from `t0`: 13 s of play. `trigger_attack`: attack with RT instead of X.
void play_pad(Run &r, double t0, virtual_pad::Kind kind, bool mapping, bool trigger_attack) {
    const std::string name =
        std::string(virtual_pad::kind_name(kind)) + (mapping ? " (mapped)" : "");
    at(r, t0, [&r, kind, mapping, name](mt_cpu *) {
        r.pad.reset(); // unplugged
        r.pad = std::make_unique<virtual_pad::Pad>(kind, mapping);
        pump(r);
        check(r.pad->ok() && SDL_GetGamepadTypeForID(r.pad->id) == virtual_pad::expected_type(kind),
              name + ": attached, SDL's gamepad type " +
                  SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(r.pad->id)) + " " +
                  r.pad->error);
    });
    // Walk right with the stick, 1 s.
    at(r, t0 + 0.5, [&r](mt_cpu *) { r.pad->axis(SDL_GAMEPAD_AXIS_LEFTX, 32767); });
    at(r, t0 + 1.5, [&r](mt_cpu *) { r.pad->axis(SDL_GAMEPAD_AXIS_LEFTX, 0); });
    at(r, t0 + 2.0, [&r, name, t0](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 + 0.4, t0 + 2.0);
        const bool moved = !v.empty() && v.back().hero.x - v.front().hero.x >= 50;
        check(moved && saw_state(v, 3) && r.gamepads.active() != nullptr &&
                  SDL_GetGamepadID(r.gamepads.active()) == r.pad->id,
              name + ": the left stick right for 1 s walks right (state 3): x " +
                  (v.empty() ? "?"
                             : std::to_string(v.front().hero.x) + " -> " +
                                   std::to_string(v.back().hero.x)) +
                  "; it is the active pad");
    });
    // Walk left with the d-pad, 1 s.
    at(r, t0 + 2.5, [&r](mt_cpu *) { r.pad->button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true); });
    at(r, t0 + 3.5, [&r](mt_cpu *) { r.pad->button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, false); });
    at(r, t0 + 4.0, [&r, name, t0](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 + 2.4, t0 + 4.0);
        const bool moved = !v.empty() && v.front().hero.x - v.back().hero.x >= 50;
        check(moved && saw_state(v, 3), name + ": the d-pad left for 1 s walks left: x " +
                                            (v.empty() ? "?"
                                                       : std::to_string(v.front().hero.x) + " -> " +
                                                             std::to_string(v.back().hero.x)));
    });
    // Jump: A.
    press(r, t0 + 4.5, SDL_GAMEPAD_BUTTON_SOUTH);
    at(r, t0 + 5.5, [&r, name, t0](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 + 4.4, t0 + 5.5);
        i32 top = v.empty() ? 0 : v.front().hero.y;
        for (const Sample &s : v) {
            top = std::min(top, s.hero.y);
        }
        const bool jumped =
            !v.empty() && top <= v.front().hero.y - 20 && v.back().hero.y == v.front().hero.y;
        check(jumped && saw_state(v, 5),
              name + ": A jumps (state 5): y " +
                  (v.empty() ? "?"
                             : std::to_string(v.front().hero.y) + " up to " + std::to_string(top) +
                                   " and back"));
    });
    // Attack: X, or RT past half way.
    if (trigger_attack) {
        at(r, t0 + 6.0, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.8f); });
        at(r, t0 + 6.1, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f); });
    } else {
        press(r, t0 + 6.0, SDL_GAMEPAD_BUTTON_WEST);
    }
    at(r, t0 + 7.0, [&r, name, t0, trigger_attack](mt_cpu *) {
        const std::vector<Sample> v = between(r, t0 + 5.9, t0 + 7.0);
        std::string states;
        for (const Sample &s : v) {
            states += " " + std::to_string(s.hero.state);
        }
        // State 6, or 7 when something stands within reach in front of the hero (0x7617C
        // finds a target and the swing turns into the close one).
        check(saw_state(v, 6) || saw_state(v, 7),
              name + ": " + (trigger_attack ? "RT" : "X") +
                  " attacks (the hero's state 6 or 7, per tick:" + states + ")");
    });
    // A skill: LB, quick slot 3 (Double Step: 6 MP, state 11).
    const auto mp0 = std::make_shared<i32>(0);
    at(r, t0 + 7.4, [mp0](mt_cpu *cpu) { *mp0 = hero(cpu).mp; });
    press(r, t0 + 7.5, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    at(r, t0 + 8.5, [&r, name, t0, mp0](mt_cpu *cpu) {
        const std::vector<Sample> v = between(r, t0 + 7.4, t0 + 8.5);
        const i32 mp = hero(cpu).mp;
        check(saw_state(v, 11) && mp == *mp0 - 6,
              name + ": LB (quick slot 3) uses Double Step (state 11): MP " + std::to_string(*mp0) +
                  " -> " + std::to_string(mp));
    });
    // The potions: B (quick slot 2, Blue Potion: MP) and Y (quick slot 1, Red Potion: HP, after
    // the test takes 7 HP, standing in for a monster).
    const auto blue0 = std::make_shared<i32>(0);
    const auto red0 = std::make_shared<i32>(0);
    at(r, t0 + 8.9, [blue0, red0](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        *blue0 = item_count(cpu, h.addr, k_blue_potion);
        *red0 = item_count(cpu, h.addr, k_red_potion);
    });
    press(r, t0 + 9.0, SDL_GAMEPAD_BUTTON_EAST);
    at(r, t0 + 10.0, [name, blue0](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 blue = item_count(cpu, h.addr, k_blue_potion);
        check(blue == *blue0 - 1 && h.mp == 10,
              name + ": B (quick slot 2) drinks a Blue Potion: " + std::to_string(*blue0) + " -> " +
                  std::to_string(blue) + ", MP " + std::to_string(h.mp));
        wr(cpu, h.addr + 0x94, 3); // HP 3
    });
    press(r, t0 + 10.5, SDL_GAMEPAD_BUTTON_NORTH);
    at(r, t0 + 11.5, [name, red0](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 red = item_count(cpu, h.addr, k_red_potion);
        check(red == *red0 - 1 && h.hp == 10,
              name + ": Y (quick slot 1) drinks a Red Potion: " + std::to_string(*red0) + " -> " +
                  std::to_string(red) + ", HP 3 -> " + std::to_string(h.hp));
    });
}

void play(Run &r) {
    to_field(r);
    at(r, 57.5, [](mt_cpu *cpu) { give_double_step(cpu); });
    play_pad(r, 58.0, virtual_pad::Kind::kXbox360, false, false);
    play_pad(r, 71.0, virtual_pad::Kind::kPs4, false, true);
    play_pad(r, 84.0, virtual_pad::Kind::kPs5, true, false);
    at(r, 97.0, [&r](mt_cpu *) {
        r.pad.reset();
        pump(r);
        check(r.gamepads.count() == 0 && r.gamepads.active() == nullptr,
              "the last pad unplugged: no gamepad open, none active");
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

// The game pad's host opacity now (1 before the game has a pad).
float pad_opacity() { return mt_uikit_host_opacity(mt_input_pad_panel_view(0)); }

void hold(Run &r, bool touch) {
    to_field(r);
    if (!r.hiders.empty()) {
        // The key pad is hidden over the whole stretch: hide_keypad = always, or auto after
        // the pad's A in the intro.
        for (const double t : {k_hold_from, k_hold_to - 0.01}) {
            at(r, t, [&r, t](mt_cpu *) {
                check(r.hiders.front()->hidden() && mt_input_pad_panel_view(0) != 0 &&
                          pad_opacity() == 0.0f,
                      r.variant + ": the key pad is hidden at " + std::to_string(t) +
                          " s (hide_keypad " + fe::keypad_mode_name(r.hiders.front()->mode()) +
                          ", the game pad's host opacity " + std::to_string(pad_opacity()) + ")");
            });
        }
    }
    // Two fingers (virtual fingers 1 and 2 of Input::touch), or the pad's d-pad, X and stick.
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
        press(r, 58.0, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1.0);
        press(r, 59.5, SDL_GAMEPAD_BUTTON_WEST, 1.0);
        at(r, 61.0, [&r](mt_cpu *) { r.pad->axis(SDL_GAMEPAD_AXIS_LEFTX, 32767); });
        press(r, 61.5, SDL_GAMEPAD_BUTTON_WEST, 0.5);
        at(r, 62.5, [&r](mt_cpu *) { r.pad->axis(SDL_GAMEPAD_AXIS_LEFTX, 0); });
    }
    at(r, k_hold_to, [&r](mt_cpu *) {
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
        r.done = true;
    });
}

// --- keypad ---------------------------------------------------------------------------------

// Panel 0's elements in screen points (docs/controls/keypad.md, "Panel 0: the game pad"): the
// d-pad, jump, attack, the four short keys, Map, Bag, Cash.
struct Box {
    int x, y, w, h;
    bool has(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
constexpr Box k_dpad{0, 180, 168, 136};
constexpr Box k_attack_key{405, 277, 70, 34};
constexpr Box k_pad_boxes[] = {
    k_dpad,
    {335, 277, 70, 34},
    k_attack_key,
    {446, 224, 34, 31},
    {446, 178, 34, 31},
    {446, 132, 34, 31},
    {446, 86, 34, 31},
    {0, 0, 52, 52},
    {383, 0, 50, 52},
    {433, 0, 47, 52},
};

// The screen composited now with the game pad at `opacity` (then back to the hider's).
std::vector<std::uint8_t> composite(Run &r, mt_cpu *cpu, float opacity) {
    const double now = mt_cf_absolute_time();
    fe::apply_keypad_opacity(opacity);
    const bool ok = r.screen.compose(cpu, now);
    fe::apply_keypad_opacity(r.hiders.front()->opacity(now));
    if (!ok) {
        check(false, "the screen is composited");
    }
    return r.screen.pixels();
}

void save(const Run &r, const std::vector<std::uint8_t> &px, const char *name) {
    SDL_Surface *s = SDL_CreateSurfaceFrom(480, 320, SDL_PIXELFORMAT_ABGR8888,
                                           const_cast<std::uint8_t *>(px.data()), 480 * 4);
    const std::string path = (r.out_dir / name).string();
    check(s != nullptr && SDL_SavePNG(s, path.c_str()), std::string("written: ") + path);
    SDL_DestroySurface(s);
}

// Where two screens differ: the pixels outside panel 0's boxes, inside the d-pad, inside the
// attack key, in all.
struct Diff {
    int outside = 0, dpad = 0, attack = 0, all = 0;
};
Diff diff(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
    Diff d;
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 480; x++) {
            const std::size_t i = (static_cast<std::size_t>(y) * 480 + x) * 4;
            if (std::memcmp(&a[i], &b[i], 4) == 0) {
                continue;
            }
            d.all++;
            d.dpad += k_dpad.has(x, y) ? 1 : 0;
            d.attack += k_attack_key.has(x, y) ? 1 : 0;
            bool in = false;
            for (const Box &box : k_pad_boxes) {
                in = in || box.has(x, y);
            }
            d.outside += in ? 0 : 1;
        }
    }
    return d;
}

// A left button down or up at screen point (x, y): an SDL mouse event at the window point that
// shows it, polled into Input at the next turn like any other.
void mouse(Run &r, bool down, float x, float y) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(r.window);
    e.button.which = 1;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = 2 * x;
    e.button.y = 2 * y;
    check(SDL_PushEvent(&e), std::string("a mouse button ") + (down ? "down" : "up") +
                                 " is pushed at (" + std::to_string(x) + ", " + std::to_string(y) +
                                 ")");
}

std::string states(const Run &r, double now) {
    std::string s;
    for (const auto &h : r.hiders) {
        s += std::string(s.empty() ? "" : ", ") + fe::keypad_mode_name(h->mode()) + " " +
             std::to_string(h->opacity(now));
    }
    return s + "; the game pad's host opacity " + std::to_string(pad_opacity());
}

// Checks the hiders' opacities at `t`: auto's as given, always's 0, never's 1, and auto's on
// the game pad (once the game has one).
void expect(Run &r, double t, float auto_opacity, const std::string &why) {
    at(r, t, [&r, auto_opacity, why](mt_cpu *) {
        const double now = mt_cf_absolute_time();
        const bool ok = r.hiders[0]->opacity(now) == auto_opacity &&
                        r.hiders[1]->opacity(now) == 0.0f && r.hiders[2]->opacity(now) == 1.0f &&
                        (mt_input_pad_panel_view(0) == 0 || pad_opacity() == auto_opacity);
        check(ok, why + " (" + std::to_string(r.t) + " s: " + states(r, now) + ")");
    });
}

// The sky over the first map's middle: a touch there does nothing.
constexpr float k_sky[2] = {240, 40};

void keypad(Run &r) {
    to_field(r);
    expect(r, 0.0, 1.0f, "the first turn: always is hidden from the start, auto and never shown");
    expect(r, 29.9, 1.0f, "the touches on the banner and Play (scripted taps) leave auto shown");
    expect(r, 30.5, 0.0f, "the pad's first A (30 s) hides auto");
    at(r, 57.2, [&r](mt_cpu *cpu) {
        const std::vector<std::uint8_t> shown = composite(r, cpu, 1.0f);
        const std::vector<std::uint8_t> hidden = composite(r, cpu, 0.0f);
        save(r, shown, "first_map_shown.png");
        save(r, hidden, "first_map_hidden.png");
        const Diff d = diff(shown, hidden);
        check(d.outside == 0 && d.dpad > 1000 && d.attack > 500,
              "the first map hidden vs shown: " + std::to_string(d.all) +
                  " pixels differ, none outside panel 0's rects (" + std::to_string(d.outside) +
                  "), " + std::to_string(d.dpad) + " in the d-pad, " + std::to_string(d.attack) +
                  " in the attack key");
        check(composite(r, cpu, r.hiders[0]->opacity(mt_cf_absolute_time())) == hidden,
              "the screen as auto composites it now is the hidden one");
    });
    // A mouse click shows it (and is delivered: a touch in the sky does nothing).
    at(r, 58.0, [&r](mt_cpu *) { mouse(r, true, k_sky[0], k_sky[1]); });
    at(r, 58.1, [&r](mt_cpu *) { mouse(r, false, k_sky[0], k_sky[1]); });
    expect(r, 58.25, 1.0f, "an SDL mouse click (58.0 s) shows auto again, faded in by 58.25 s");
    at(r, 58.3, [&r](mt_cpu *cpu) {
        check(composite(r, cpu, 1.0f) ==
                  composite(r, cpu, r.hiders[0]->opacity(mt_cf_absolute_time())),
              "shown again: the screen is the one with the pad at full opacity");
    });
    // A pad press hides it again.
    press(r, 59.0, SDL_GAMEPAD_BUTTON_SOUTH);
    expect(r, 59.3, 0.0f, "the pad's A (59.0 s) hides auto again");
    // The mouse held down while the pad is pressed: shown until it lets go.
    at(r, 60.0, [&r](mt_cpu *) { mouse(r, true, k_sky[0], k_sky[1]); });
    expect(r, 60.25, 1.0f, "the mouse button down (60.0 s) shows auto");
    press(r, 60.3, SDL_GAMEPAD_BUTTON_SOUTH);
    expect(r, 60.7, 1.0f, "the pad's A (60.3 s) while the mouse button is down: auto waits");
    at(r, 60.8, [&r](mt_cpu *) { mouse(r, false, k_sky[0], k_sky[1]); });
    expect(r, 61.1, 0.0f, "the mouse button up (60.8 s): auto hides then");
    at(r, 61.5, [&r](mt_cpu *) {
        const fe::KeypadHider &a = *r.hiders[0], &al = *r.hiders[1], &nv = *r.hiders[2];
        check(a.hides() == 3 && a.shows() == 2,
              "auto hid 3 times and showed 2 times: " + std::to_string(a.hides()) + ", " +
                  std::to_string(a.shows()));
        check(al.hides() + al.shows() == 0 && nv.hides() + nv.shows() == 0,
              "always and never never changed");
        r.done = true;
    });
}

// --- rumble -----------------------------------------------------------------------------------

// The calls to the motors that start a rumble (SDL's own stops, when a rumble's time is up,
// are (0, 0)).
std::vector<virtual_pad::RumbleCall> rumbles(const Run &r) {
    std::vector<virtual_pad::RumbleCall> out;
    for (const virtual_pad::RumbleCall &c : r.pad->rumbles) {
        if (c.low != 0 || c.high != 0) {
            out.push_back(c);
        }
    }
    return out;
}

Uint16 amplitude(float a, u32 strength) {
    return static_cast<Uint16>(std::lround(a * static_cast<float>(strength) / 100.0f * 65535.0f));
}

std::string calls_text(const std::vector<virtual_pad::RumbleCall> &v) {
    std::string out = std::to_string(v.size()) + " call(s)";
    for (const virtual_pad::RumbleCall &c : v) {
        out += " (" + std::to_string(c.low) + ", " + std::to_string(c.high) + ")";
    }
    return out;
}

constexpr u32 k_strength = 60;

// A hit and a level-up through the game's own code with rumble off (from t0) and on (t0 + 4).
void rumble_events(Run &r, double t0, bool on) {
    const std::string mode = on ? "rumble on (60 %)" : "rumble off";
    struct Before {
        Hero hero;
        std::size_t calls = 0;
        mt_game_events_stats events{};
        u32 system_sounds = 0;
    };
    const auto before = std::make_shared<Before>();
    at(r, t0, [&r, on, before](mt_cpu *cpu) {
        r.rumble.configure(on, k_strength);
        before->hero = hero(cpu);
        before->calls = rumbles(r).size();
        before->events = mt_game_events_get_stats();
        before->system_sounds = mt_audio_get_stats().system_sounds;
        // A monster's touch: CHero::CheckMobContact 0x65F05 calls this with the mob's attack.
        call(cpu, k_take_damage, {before->hero.addr, 7, 0});
    });
    at(r, t0 + 0.5, [&r, on, mode, before](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const mt_game_events_stats ev = mt_game_events_get_stats();
        const std::vector<virtual_pad::RumbleCall> v = rumbles(r);
        const std::vector<virtual_pad::RumbleCall> fresh(v.begin() + before->calls, v.end());
        const bool vibrated = mt_audio_get_stats().system_sounds == before->system_sounds + 1;
        check(h.hp == before->hero.hp - 7 && ev.hero_damage == before->events.hero_damage + 1 &&
                  vibrated,
              mode + ": CHero::TakeDamage(7): HP " + std::to_string(before->hero.hp) + " -> " +
                  std::to_string(h.hp) + ", one damage event, the game's own vibration played");
        if (on) {
            const Uint16 low =
                amplitude(std::max(fe::kRumbleVibrate.low, fe::kRumbleDamage.low), k_strength);
            const Uint16 high =
                amplitude(std::max(fe::kRumbleVibrate.high, fe::kRumbleDamage.high), k_strength);
            check(fresh.size() == 1 && fresh[0].low == low && fresh[0].high == high,
                  mode + ": the hit rumbles the pad once, the vibration and the damage merged (" +
                      std::to_string(low) + ", " + std::to_string(high) +
                      "): " + calls_text(fresh));
        } else {
            check(fresh.empty(), mode + ": the hit does not rumble: " + calls_text(fresh));
        }
        // The level's EXP, the real exp path: one level gained.
        const i32 need = static_cast<i32>(call(cpu, k_exp_to_level, {h.addr}));
        before->hero = h;
        before->calls = v.size();
        before->events = ev;
        call(cpu, k_set_exp, {h.addr, static_cast<u32>(need - h.exp)});
    });
    at(r, t0 + 1.5, [&r, on, mode, before](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const mt_game_events_stats ev = mt_game_events_get_stats();
        const std::vector<virtual_pad::RumbleCall> v = rumbles(r);
        const std::vector<virtual_pad::RumbleCall> fresh(v.begin() + before->calls, v.end());
        check(h.level == before->hero.level + 1 && ev.level_ups == before->events.level_ups + 1 &&
                  ev.hero_damage == before->events.hero_damage,
              mode + ": CHero::setExp with the level's EXP: level " +
                  std::to_string(before->hero.level) + " -> " + std::to_string(h.level) +
                  ", one level-up event, HP " + std::to_string(h.hp));
        if (on) {
            const Uint16 low = amplitude(fe::kRumbleLevelUp.low, k_strength);
            const Uint16 high = amplitude(fe::kRumbleLevelUp.high, k_strength);
            check(fresh.size() == 1 && fresh[0].low == low && fresh[0].high == high,
                  mode + ": the level-up rumbles the pad once (" + std::to_string(low) + ", " +
                      std::to_string(high) + "): " + calls_text(fresh));
        } else {
            check(fresh.empty(), mode + ": the level-up does not rumble: " + calls_text(fresh));
        }
    });
}

void rumble(Run &r) {
    r.rumble_pad = true;
    // Off until the field: the intro's scenes vibrate too (docs/re/audio.md), which this test
    // does not count.
    r.rumble.configure(false, k_strength);
    r.rumble.install();
    to_field(r);
    at(r, 57.5, [&r](mt_cpu *) {
        check(r.pad->ok() && r.gamepads.active() != nullptr &&
                  SDL_GetGamepadID(r.gamepads.active()) == r.pad->id && rumbles(r).empty(),
              "the virtual pad with motors is the active pad; no rumble while off (" +
                  calls_text(rumbles(r)) + ")");
    });
    rumble_events(r, 58.0, false);
    rumble_events(r, 61.0, true);
    at(r, 64.0, [&r](mt_cpu *) {
        const fe::RumbleStats &st = r.rumble.stats();
        check(st.sent == 2 && st.failed == 0 && st.no_pad == 0,
              "Rumble: " + std::to_string(st.requests) + " requests, " + std::to_string(st.sent) +
                  " sent, " + std::to_string(st.ignored) + " ignored while off");
        r.rumble.uninstall();
        r.rumble.stop();
        r.done = true;
    });
}

int boot(Run &r, const std::vector<std::uint8_t> &slice, const fs::path &data) {
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    fs::remove_all(data);
    fs::create_directories(data);
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
    std::printf(
        "usage: gamepad_real_test play|rumble OUT_DIR | hold touch|pad|touch-always|pad-auto "
        "OUT_FILE | keypad OUT_DIR\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string variant;
    fs::path out;
    if ((mode == "play" || mode == "rumble") && argc == 3) {
        out = argv[2];
    } else if (mode == "hold" && argc == 4) {
        variant = argv[2];
        out = argv[3];
        if (variant != "touch" && variant != "pad" && variant != "touch-always" &&
            variant != "pad-auto") {
            return usage();
        }
    } else if (mode == "keypad" && argc == 3) {
        out = argv[2];
    } else {
        return usage();
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    // The data directory: next to the output, emptied, so that Play starts a new game.
    const fs::path data = (mode == "hold" ? out.parent_path() : out) / ("data-" + mode + variant);
    if (mode != "hold") {
        fs::create_directories(out);
    } else if (out.has_parent_path()) {
        fs::create_directories(out.parent_path());
    }
    // The pads' events reach the test without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // keypad: SDL's mouse events need a window (the dummy driver: headless).
    if (mode == "keypad") {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD | (mode == "keypad" ? SDL_INIT_VIDEO : 0u))) {
        std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Run r;
    r.mode = mode;
    r.variant = variant;
    r.out_dir = out;
    g_run = &r;
    if (mode == "keypad") {
        r.window = SDL_CreateWindow("keypad", 960, 640, SDL_WINDOW_HIDDEN);
        if (r.window == nullptr) {
            std::printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
            return 1;
        }
        r.placement = fe::place(fe::ScaleMode::kInteger, 960, 640, 480, 320);
        r.input.set_window(r.window, &r.placement);
        for (const fe::KeypadMode m :
             {fe::KeypadMode::kAuto, fe::KeypadMode::kAlways, fe::KeypadMode::kNever}) {
            r.hiders.push_back(std::make_unique<fe::KeypadHider>(m));
        }
    } else if (mode == "hold" && variant.find('-') != std::string::npos) {
        r.hiders.push_back(std::make_unique<fe::KeypadHider>(
            variant == "touch-always" ? fe::KeypadMode::kAlways : fe::KeypadMode::kAuto));
    }
    if (!r.hiders.empty()) {
        r.input.set_activity_listener(hear, &r);
    }
    if (mode == "play") {
        play(r);
    } else if (mode == "rumble") {
        rumble(r);
    } else if (mode == "keypad") {
        keypad(r);
    } else {
        hold(r, variant.rfind("touch", 0) == 0);
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE), data);
    if (mode == "hold") {
        // The hero at every tick of the held stretch: the file the two runs must share.
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        for (const Sample &s : between(r, k_hold_from, k_hold_to)) {
            char line[160];
            std::snprintf(line, sizeof line, "%.3f %s\n", s.t, hero_text(s.hero).c_str());
            f << line;
        }
        check(static_cast<bool>(f), "the per-tick hero written to " + out.string());
        std::ofstream gf(out.string() + ".gl", std::ios::binary | std::ios::trunc);
        for (const std::string &line : r.gl_hashes) {
            gf << line << "\n";
        }
        check(static_cast<bool>(gf) && !r.gl_hashes.empty(),
              "the hashes of the " + std::to_string(r.gl_hashes.size()) +
                  " GL frames from the first map on written to " + out.string() + ".gl");
    }
    if (r.window != nullptr) {
        SDL_DestroyWindow(r.window);
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
