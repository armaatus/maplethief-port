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
// MODE is a flow (main_menu, terms, early, seed, create, select, delete, game) or "explore", which
// runs the STEPs "T:press BUTTON [HOLD]", "T:stick AXIS VALUE HOLD", "T:shot NAME" and
// "T:state" (SDL's gamepad names: a, b, x, y, back, start, leftshoulder, dpup, ...; axes leftx,
// lefty) for finding a way through the game. Shots (OUT_DIR/NAME.png) are for looking only: they
// stay in the test output directory and are never committed. The flows are in run_mode below,
// and platform/menu_nav_test.cmake runs the character flows in order on one data directory.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;

// Guest facts (docs/controls/keypad.md, docs/re/program-flow.md).
constexpr u32 k_data_mgr = 0x5BAC1;   // CDataMgr::GetInstance
constexpr u32 k_game_mode = 0x11AA00; // 0 field, 1 popup menu, 2 shop, 3 map, 4 NPC chat, 0xB cash
constexpr u32 k_popup_id = 0x108C64;  // the open popup (-1 none)

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

i32 game_mode(const mt_cpu *cpu) { return static_cast<i32>(rd(cpu, k_game_mode)); }
i32 popup_id(const mt_cpu *cpu) { return static_cast<i32>(rd(cpu, k_popup_id)); }

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
};
Run *g_run = nullptr;

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
        double v1 = 0, v2 = 0;
        const int n =
            std::sscanf(step.c_str() + colon + 1, "%31s %63s %lf %lf", what, name, &v1, &v2);
        const std::string w = what;
        if (w == "press" && n >= 2) {
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
            press(r, t, b, n >= 3 ? v1 : 0.1);
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

// A on Play at 0.05 s, while the engine is still in CStateTitle (#122): the focus drops it
// (#129), so no character select opens over the empty robinImages and the run goes on. Then the
// pad's Play works as on a fresh install (seed).
void early(Run &r) {
    at(r, 0.0, [&r](mt_cpu *) {
        r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
        check(r.pad->ok(), "an Xbox 360 virtual pad is attached at the first turn " + r.pad->error);
    });
    at(r, 0.04, [&r](mt_cpu *cpu) {
        check(fe::focus_engine_at_title(cpu) &&
                  focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "at 0.04 s the engine is in CStateTitle, the focus on Play: " + focus_text(r));
    });
    between(r, 0.0, 0.6, [&r](mt_cpu *cpu, std::size_t from) {
        const fe::Focus::Stats &s = r.focus.stats();
        check(!fe::focus_engine_at_title(cpu) && s.early == 1 && s.taps == 0 &&
                  actions_since(r, from).empty(),
              "A at 0.05 s is dropped: no button action, the engine at its main menu by 0.6 s");
    });
    press(r, 0.05, A);
    press(r, 3.0, B);
    press(r, 3.5, A);
    at(r, 5.0, [&r](mt_cpu *) {
        check(r.focus.stats().taps == 1 && file_size(r, "theif0") == 1124,
              "A at 3.5 s is Play: a new game writes slot 0");
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
    } else if (r.mode == "early") {
        early(r);
    } else if (r.mode == "create") {
        create(r);
    } else if (r.mode == "select") {
        select(r);
    } else if (r.mode == "delete") {
        delete_flow(r);
    } else if (r.mode == "game") {
        game(r);
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
    diag.trace = "uikit,focus";
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
    std::printf("usage: menu_nav_real_test "
                "main_menu|terms|early|seed|create|select|delete|game|explore DATA_DIR OUT_DIR "
                "[STEP...]\n");
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
