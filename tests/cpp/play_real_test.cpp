// The first maps played for 15 game-minutes (#76). The recompiled game boots from work/ as
// MapleThief does with --fixed-step 1 (the virtual clock, ADR 0044) on a fresh data directory,
// and a small bot plays it through the frontend's own input layer with a virtual Xbox 360 pad
// (game_driver.h: Gamepads -> Actions -> the runtime's actions), reading the hero, the mobs and
// the drops from guest memory (guest_state.h; read only) and the game events of the observed
// functions (ADR 0053). Local only: it needs work/gen (python -m recomp all), the slice and the
// extracted bundle, and exits 77 (skipped) without them.
//
//   play_real_test MODE OUT_DIR [TICKS]      (first_map: CTest play.first_map; save_quit, then
//                                             save_relaunch on the same OUT_DIR: CTests
//                                             play.save_quit, play.save_relaunch, #78; the
//                                             backup after the save, #148;
//                                             cash_shop_meso, cash_shop_off: CTests
//                                             play.cash_shop_meso, play.cash_shop_off, #156;
//                                             confirm_guard: CTest play.confirm_guard, #147;
//                                             autosave_*: CTests play.autosave_*, #150 (and
//                                             autosave_exit, #179);
//                                             widescreen_off, then widescreen_on with
//                                             MT_WIDESCREEN_BASE: CTests play.widescreen_off,
//                                             play.widescreen_on, #86; widescreen_pad_off,
//                                             then widescreen_pad_on with
//                                             MT_WIDESCREEN_PAD_BASE: CTests
//                                             play.widescreen_pad_off, play.widescreen_pad_on,
//                                             #168; widescreen_filters: CTest
//                                             play.widescreen_filters, #170; minimap: CTest
//                                             play.minimap, #176; hud_labels: CTest
//                                             play.hud_labels, ADR 0101; minimap_wide: CTest
//                                             play.minimap_wide, the corner;
//                                             regress_record, regress with MT_REPLAY: the
//                                             regression replay, #91, ADR 0104, CTest
//                                             play.regress through scripts/regress;
//                                             save_state: an in-process save and restore at
//                                             two points, #203, ADR 0105, CTest
//                                             play.save_state;
//                                             save_state_census: the host states without a
//                                             serializer at that safe point, #202, CTest
//                                             play.save_state_census;
//                                             quick_state: F5 and F9 through the
//                                             frontend's QuickStates, #204, CTest
//                                             play.quick_state;
//                                             hud_extras: CTest play.hud_extras, #191,
//                                             ADR 0109;
//                                             mechanics: CTest play.mechanics, #214,
//                                             docs/re/mechanics.md)
//
// The way: Play on a fresh install creates the character (slot 0, no name asked); the intro;
// the tutorial on maps 2.8 and 2.9 (walk, talk, jump, climb a rope and a ladder, a portal, the
// snail); Henesys (2.0) and Stan, who gives Elder's Advice; the map screen and its views; then
// the first hunting field, 2.3, until TICKS game ticks (default 18,000: 15 game-minutes at the
// default 50 ms) have passed since the hero first stood in the field. On 2.3 the bot walks to
// the nearest mob, attacks it, jumps now and then, walks over the drops (the game picks them up
// on contact), drinks potions when HP is low, and after a fall goes back from the town.
//
// Checks, at the end and once a game-minute:
//   - the run ends from the script: no trap, no guest crash, no uncaught exception;
//   - the unimplemented report is empty (no import or selector without an implementation), no
//     GL misuse and no unsupported or invalid draw, no error line in the runtime's log;
//   - the game keeps ticking and presenting (at least 0.9 GL frames per tick, and 90 % of the
//     ticks a minute should have) and the screen is not black;
//   - the hero walked, jumped and attacked, mobs died, damage numbers showed, drops lay on the
//     ground and were picked up (meso or items gained), the hero took hits, and gained at least
//     one level by fighting (CHero::LevelUp's observed entry and the level).
// OUT_DIR gets the log (play.log), a summary line per game-minute in stdout, and a composited
// shot per game-minute and of each map view (minute_NN.png, map_*.png) for looking at by eye.
// They stay in the test output directory and are never committed.
//
// Extending it (#77 chapter 1, #146, #147, #86): a new mode is a function that queues tasks on
// the Run (then_* in game_driver.h, then_hunt and the monitor here) and its checks, listed in
// k_modes; the monitor and common_checks apply to every mode. See docs/testing/first-map-run.md.
//
// Chapter 1 (#77, docs/testing/chapter-1-quests.md): the modes chapter_1_a, _b, _c play its
// quests in parts with the quest bot (quest_bot.h: going between maps over world_map.h's
// platform graph, NPC chats, shops, the equip tab, farming). A part after the first starts
// from the save an earlier part wrote: MT_FROM_SAVE=<that part's OUT_DIR> copies its data
// directory, and the part loads slot 0 from the main menu.
#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#if defined(MT_HAVE_GENERATED_CODE)

#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

#include "autosave.h"
#include "exp_percent.h"
#include "foundation/plist.h"
#include "game_driver.h"
#include "game_speed.h"
#include "gfx_backend.h"
#include "hud_labels.h"
#include "lifecycle.h"
#include "live_settings.h"
#include "maplethief/audio.h"
#include "maplethief/cashshop.h"
#include "maplethief/events.h"
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/hooks.h"
#include "maplethief/objc/host.h"
#include "maplethief/saves.h"
#include "maplethief/snapshot.h"
#include "maplethief/texpack.h"
#include "maplethief/textpack.h"
#include "maplethief/widescreen.h"
#include "minimap.h"
#include "present.h"
#include "price_line.h"
#include "quest_bot.h"
#include "quick_state.h"
#include "save_state_input.h"
#include "save_tools.h"
#include "settings.h"

namespace {

using namespace game_driver;

constexpr u64 k_default_ticks = 18000; // 15 game-minutes at 50 ms a tick
constexpr u64 k_minute_ticks = 1200;   // a game-minute at 50 ms

// What the monitor has seen (every turn while the hero is in the field).
struct Play {
    u64 play_ticks = 0; // the goal: game ticks since the hero first stood in the field
    bool started = false;
    bool anywhere = false; // a mode from a save: the play starts in any field
    u64 start_ticks = 0, start_gl = 0;
    double start_t = 0;
    // Per game-minute.
    u64 minute = 0, minute_ticks = 0, minute_gl = 0;
    double minute_t = 0;
    double black_recheck = -1; // when to look again at a black minute shot
    // The hero.
    Hero first{}, last{};
    std::set<std::pair<i32, i32>> maps;
    i32 last_state = -1, min_x = INT32_MAX, max_x = INT32_MIN;
    u64 jumps = 0, attacks = 0, walks = 0;
    // The world.
    std::map<u32, i32> mob_hp; // the last HP of each mob seen
    u64 kills = 0;
    // Drops: a drop is new when its address was not in the list the turn before (the game
    // reuses the memory of drops gone), picked up when its state turns 2.
    std::map<u32, i32> drop_state; // the drops of the last turn: address -> state
    u64 drops_seen = 0, drops_taken = 0;
    i32 last_items = -1, red = 0, blue = 0; // and the Red and Blue Potions
    u64 damage_numbers_max = 0, damage_number_turns = 0;
    i32 meso_gained = 0, items_gained = 0;
    // From the hunt on (2.3).
    bool hunting = false;
    i32 hunt_level = 0;
    mt_game_events_stats hunt_events{};
    i32 falls = 0;
    // Shots of the first kill and pick-up on the hunting field (for looking at).
    double kill_shot = -1, pickup_shot = -1;
    bool kill_shot_done = false, pickup_shot_done = false;
    // The map screen's views: a hash of each one's composited screen.
    std::set<u64> map_views;
};
Play g_play;

// The total count of every other item (+0xD8) the hero carries.
i32 items_total(const mt_cpu *cpu, const Hero &h) {
    i32 n = 0;
    for (i32 i = 0; i < 48; i++) {
        const EtcItem e = etc_item(cpu, h, i);
        if (e.id >= 0) {
            n += e.count;
        }
    }
    return n;
}

// The composited screen now, to OUT/name.png; whether it is not black. `hash` gets its FNV-1a.
bool save_shot(Run &r, mt_cpu *cpu, const std::string &name, u64 *hash = nullptr) {
    if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
        return false;
    }
    const std::vector<std::uint8_t> &px = r.screen.pixels();
    if (hash != nullptr) {
        *hash = 1469598103934665603ull;
        for (const std::uint8_t b : px) {
            *hash = (*hash ^ b) * 1099511628211ull;
        }
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom(
        static_cast<int>(r.screen.width()), static_cast<int>(r.screen.height()),
        SDL_PIXELFORMAT_ABGR8888, const_cast<std::uint8_t *>(px.data()),
        static_cast<int>(r.screen.width() * 4));
    if (s != nullptr) {
        SDL_SavePNG(s, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(s);
    }
    return !fe::is_black(px.data(), r.screen.width(), r.screen.height(), r.screen.width() * 4);
}

std::string play_text(const Play &p) {
    char buf[360];
    std::snprintf(
        buf, sizeof buf,
        "%llu ticks; level %d EXP %d HP %d/%d MP %d/%d meso %d potions %d/%d; walks %llu "
        "jumps %llu "
        "attacks %llu; kills %llu; drops %llu seen %llu picked up; meso +%d items +%d; "
        "damage numbers up to %llu; falls %d",
        static_cast<unsigned long long>(p.play_ticks), p.last.level, p.last.exp, p.last.hp,
        p.last.max_hp, p.last.mp, p.last.max_mp, p.last.meso, p.red, p.blue,
        static_cast<unsigned long long>(p.walks), static_cast<unsigned long long>(p.jumps),
        static_cast<unsigned long long>(p.attacks), static_cast<unsigned long long>(p.kills),
        static_cast<unsigned long long>(p.drops_seen),
        static_cast<unsigned long long>(p.drops_taken), p.meso_gained, p.items_gained,
        static_cast<unsigned long long>(p.damage_numbers_max), p.falls);
    return buf;
}

// The monitor: every turn, what the hero and the world do.
void monitor(Run &r, mt_cpu *cpu) {
    Play &p = g_play;
    const Hero h = hero(cpu);
    if (!p.started) {
        if (!(field(cpu) && (h.on(2, 8) || p.anywhere))) {
            return;
        }
        p.started = true;
        p.start_ticks = p.minute_ticks = r.ticks;
        p.start_gl = p.minute_gl = r.gl_frames;
        p.start_t = p.minute_t = r.t;
        p.first = p.last = h;
        std::printf("play: the hero stands in the field of map %d.%d at %.2f s (tick %llu): %s\n",
                    h.area, h.map, r.t, static_cast<unsigned long long>(r.ticks),
                    hero_text(cpu, h).c_str());
        std::fflush(stdout);
    }
    p.play_ticks = r.ticks - p.start_ticks;
    if (h.addr == 0) {
        return;
    }
    // Gains: meso and items, while the hero stays (a load or a death can change it).
    const i32 items = items_total(cpu, h);
    if (p.hunting && h.addr == p.last.addr && h.on(p.last.area, p.last.map)) {
        p.meso_gained += std::max(0, h.meso - p.last.meso);
        p.items_gained += std::max(0, items - p.last_items);
    }
    p.last_items = items;
    p.red = item_count(cpu, h, 3);
    p.blue = item_count(cpu, h, 6);
    if (h.state != p.last_state) {
        p.walks += h.state == 3 ? 1 : 0;
        p.jumps += h.state == 5 ? 1 : 0;
        p.attacks += h.state == 6 || h.state == 7 ? 1 : 0;
        p.last_state = h.state;
    }
    p.maps.insert({h.area, h.map});
    if (p.hunting) {
        p.min_x = std::min(p.min_x, h.x);
        p.max_x = std::max(p.max_x, h.x);
    }
    for (const Mob &m : mobs(cpu)) {
        const auto it = p.mob_hp.find(m.addr);
        if (it != p.mob_hp.end() && it->second > 0 && m.hp <= 0) {
            p.kills++;
            if (p.hunting && p.kill_shot < 0) {
                p.kill_shot = r.t + 0.15; // the drops falling, the last damage number
            }
            if (std::getenv("MT_DROP_TRACE") != nullptr) {
                std::printf("kill at %.2f: mob %d at %d,%d\n", r.t, m.id, m.x, m.y);
            }
        }
        p.mob_hp[m.addr] = m.hp;
    }
    std::map<u32, i32> now;
    for (const Drop &d : drops(cpu)) {
        const auto it = p.drop_state.find(d.addr);
        if (it == p.drop_state.end()) {
            p.drops_seen++;
            if (std::getenv("MT_DROP_TRACE") != nullptr) {
                std::printf("drop at %.2f: %X id %d at %d,%d state %d life %d\n", r.t, d.addr, d.id,
                            d.x, d.y, d.state, d.life);
            }
        }
        if (d.state == 2 && (it == p.drop_state.end() || it->second != 2)) {
            p.drops_taken++;
            if (p.hunting && p.pickup_shot < 0) {
                p.pickup_shot = r.t + 0.05; // the drop flying to the hero
            }
        }
        now[d.addr] = d.state;
    }
    p.drop_state.swap(now);
    if (!p.kill_shot_done && p.kill_shot >= 0 && r.t >= p.kill_shot) {
        p.kill_shot_done = true;
        save_shot(r, cpu, "first_kill");
    }
    if (!p.pickup_shot_done && p.pickup_shot >= 0 && r.t >= p.pickup_shot) {
        p.pickup_shot_done = true;
        save_shot(r, cpu, "first_pickup");
    }
    const u64 dn = damage_numbers(cpu);
    p.damage_numbers_max = std::max(p.damage_numbers_max, dn);
    p.damage_number_turns += dn != 0 ? 1 : 0;
    p.last = h;
    if (p.black_recheck >= 0 && r.t >= p.black_recheck) {
        p.black_recheck = -1;
        char name[40];
        std::snprintf(name, sizeof name, "minute_%02llu_again",
                      static_cast<unsigned long long>(p.minute));
        check(save_shot(r, cpu, name), "minute " + std::to_string(p.minute) +
                                           ": the screen is not black (3 s after a black shot)");
    }
    // A game-minute.
    if (r.ticks - p.minute_ticks >= k_minute_ticks) {
        p.minute++;
        const u64 ticks = r.ticks - p.minute_ticks, gl = r.gl_frames - p.minute_gl;
        const double secs = r.t - p.minute_t;
        char name[32];
        std::snprintf(name, sizeof name, "minute_%02llu",
                      static_cast<unsigned long long>(p.minute));
        const bool lit = save_shot(r, cpu, name);
        std::printf("minute %llu at %.2f s (%s, map %d.%d): %llu ticks in %.1f s, %llu GL frames; "
                    "%s\n",
                    static_cast<unsigned long long>(p.minute), r.t, name, h.area, h.map,
                    static_cast<unsigned long long>(ticks), secs,
                    static_cast<unsigned long long>(gl), play_text(p).c_str());
        check(static_cast<double>(gl) >= 0.9 * static_cast<double>(ticks) &&
                  static_cast<double>(ticks) >= 0.9 * secs * 20.0,
              "minute " + std::to_string(p.minute) + ": the game ticks (" + std::to_string(ticks) +
                  " in " + std::to_string(secs) + " s) and presents a GL frame a tick (" +
                  std::to_string(gl) + ")");
        // A black shot can be a fade (a fall, a portal, a map loading; #77): looked at again
        // 3 s later, and only a screen still black then fails.
        if (lit) {
            check(true, "minute " + std::to_string(p.minute) + ": the screen is not black");
        } else {
            std::printf("minute %llu: the screen is black (a fade?): again in 3 s\n",
                        static_cast<unsigned long long>(p.minute));
            p.black_recheck = r.t + 3.0;
        }
        const mt_report_stats rep = mt_report_get_stats();
        check(rep.imports == 0 && rep.selectors == 0,
              "minute " + std::to_string(p.minute) +
                  ": nothing unimplemented was called so far (imports " +
                  std::to_string(rep.imports) + ", selectors " + std::to_string(rep.selectors) +
                  ")");
        p.minute_ticks = r.ticks;
        p.minute_gl = r.gl_frames;
        p.minute_t = r.t;
    }
}

// --- the hunt ---------------------------------------------------------------------------------

// Hunts on the hero's map until `done`. Each turn, in this order:
//   - a Red Potion (Y, quick slot 1) when HP is under a third, a Blue Potion (B, slot 2) when MP
//     is under half: the d-pad let go first, since a held walk key blocks every other press
//     (docs/ANALYSIS.md, Input);
//   - the nearest mob on the hero's floor (whatever is in reach first: it would knock the hero
//     back), walked to and attacked with X once the hero faces it;
//   - with no mob near, the nearest drop lying on the floor that the inventory takes: walked
//     onto, and the game picks it up on contact;
//   - else on, turning where the hero stops; a jump (A, with the direction held: the game's
//     jump chord) every 6 s on the way.
// A direction held without the hero moving is let go for a tick and pressed again (a press the
// game dropped, e.g. during a map's fade-in), and every other time with a jump (a step).
// After a fall: the death popup's Return to town (OK), then from the town to `back.portal_x`
// and up through the portal to the hunting map (any other popup: OK).
void then_hunt(Run &r, std::string what, Pred done, double timeout, Comeback back) {
    struct S {
        int facing = 1, wander = 1, last_x = INT32_MIN, stuck = 0;
        double attack = -10, drink = -10, turn = -10, moved = 0, jump = 0, again = -1;
        SDL_GamepadButton potion = Y;
        bool away = false;
    };
    auto s = std::make_shared<S>();
    // Holds direction `go` (-1, 0, 1), pressing it again when the hero does not move.
    auto steer = [&r, s](int go, double el, const Hero &h) {
        if (h.x != s->last_x) {
            s->last_x = h.x;
            s->moved = el;
            s->stuck = 0;
        }
        const bool again = el < s->again;
        if (go != 0 && !again && el - s->moved > 0.5) {
            s->again = el + 0.12; // let go for two ticks or so
            s->moved = el + 0.12;
            s->stuck++;
        }
        if (go == 0) {
            s->moved = el;
        }
        hold(r, LEFT, go < 0 && el >= s->again);
        hold(r, RIGHT, go > 0 && el >= s->again);
        return el >= s->again && go != 0 && s->stuck % 2 == 1 && el - s->again < 0.15;
    };
    then(r, "hunt: " + what, timeout, [&r, done, back, s, steer](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (std::getenv("MT_HUNT_TRACE") != nullptr && std::fmod(el, 0.5) < 0.001) {
            std::string held;
            for (const SDL_GamepadButton b : r.held) {
                held += std::string(" ") + SDL_GetGamepadStringForButton(b);
            }
            std::printf("hunt %.2f: %s; held%s\n", el, where(cpu).c_str(), held.c_str());
        }
        if (done(cpu)) {
            release_all(r);
            std::printf("hunt over at %.2f s: %s\n", r.t, hero_text(cpu, h).c_str());
            return true;
        }
        if (s->away || h.hp <= 0 || !field(cpu)) { // fallen (or a popup): to town and back
            if (!s->away) {
                s->away = true;
                if (h.hp <= 0) {
                    g_play.falls++;
                }
                release_all(r);
                std::printf("hunt: away from the field at %.2f s (%s)\n", r.t, where(cpu).c_str());
            }
            if (h.on(back.area, back.map) && field(cpu) && h.hp > 0) {
                release_all(r);
                s->away = false;
                std::printf("hunt: back at %.2f s (%s)\n", r.t, where(cpu).c_str());
                return false;
            }
            if (!field(cpu)) { // the death popup: OK is Return to town
                release_all(r);
                hold(r, A, popup_id(cpu) != -1 && std::fmod(el, 1.0) < 0.1);
                return false;
            }
            hold(r, A, false);
            if (h.on(back.town_area, back.town_map)) {
                const int d = back.portal_x - h.x;
                const int go = std::abs(d) <= 4 ? 0 : (d > 0 ? 1 : -1);
                hold(r, A, steer(go, el, h));
                hold(r, UP, go == 0 && std::fmod(el, 1.5) < 0.4);
            }
            return false;
        }
        // A potion: the d-pad and X let go, then the quick slot's button.
        if (el - s->drink > 1.5 && (h.hp * 3 < h.max_hp || h.mp * 2 < h.max_mp)) {
            s->drink = el;
            s->potion = h.hp * 3 < h.max_hp ? Y : B;
        }
        if (el - s->drink < 0.3) {
            const double d = el - s->drink;
            hold(r, LEFT, false);
            hold(r, RIGHT, false);
            hold(r, X, false);
            hold(r, A, false);
            hold(r, s->potion, d >= 0.1 && d < 0.2);
            return false;
        }
        hold(r, Y, false);
        hold(r, B, false);
        const std::vector<Mob> all = mobs(cpu);
        const Mob *target = nullptr;
        for (const int pass : {0, 1}) { // in reach first, then any on the floor
            for (const Mob &m : all) {
                const int dist = std::abs(m.x - h.x);
                if (m.hp <= 0 || std::abs(m.y - h.y) > 6 || (pass == 0 && dist > 50) ||
                    dist > 400) {
                    continue;
                }
                if (target == nullptr || dist < std::abs(target->x - h.x)) {
                    target = &m;
                }
            }
            if (target != nullptr) {
                break;
            }
        }
        int aim = INT32_MIN;
        if (target != nullptr) {
            aim = target->x;
        } else {
            for (const Drop &d : drops(cpu)) {
                if (d.state == 0 && !d.refused && std::abs(d.y - h.y) <= 12 &&
                    std::abs(d.x - h.x) <= 300 &&
                    (aim == INT32_MIN || std::abs(d.x - h.x) < std::abs(aim - h.x))) {
                    aim = d.x;
                }
            }
        }
        int go = 0;
        if (aim == INT32_MIN) { // on, turning where the hero stays stuck
            if (s->stuck >= 2) {
                s->wander = -s->wander;
                s->stuck = 0;
            }
            go = s->wander;
        } else {
            const int dx = aim - h.x;
            if (target == nullptr) { // a drop: onto it
                go = std::abs(dx) > 3 ? (dx > 0 ? 1 : -1) : 0;
            } else if (std::abs(dx) > 30) {
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
        const bool step = steer(go != 0 ? go : (turning ? s->facing : 0), el, h);
        if (target != nullptr && go == 0 && !turning && el - s->attack > 0.45) {
            s->attack = el;
        }
        hold(r, X, go == 0 && el - s->attack < 0.1);
        // A jump every 6 s on the way, and the step over what stops the hero.
        if (go != 0 && el - s->jump > 6.0) {
            s->jump = el;
        }
        hold(r, A, step || (go != 0 && el - s->jump < 0.15));
        return false;
    });
}

// --- the map screen ---------------------------------------------------------------------------

// The map (View: '*', game mode 3, the key pad's panel 8), each of its views with a shot, then
// closed (B: -16) back to the field.
void map_view(Run &r, mt_cpu *cpu, const std::string &name, const std::string &what) {
    u64 h = 0;
    const bool lit = save_shot(r, cpu, name, &h);
    g_play.map_views.insert(h);
    check(game_mode(cpu) == 3 && lit, what + " (" + name + ")");
}
void then_map_screen(Run &r) {
    then_wait_field(r);
    then_press(r, SELECT);
    then_wait_for(
        r, "the map screen (game mode 3, panel 8)", 5,
        [](mt_cpu *cpu) { return game_mode(cpu) == 3 && mt_input_panel() == 8; }, 0.5);
    then_do(r, "the map's first view",
            [&r](mt_cpu *cpu) { map_view(r, cpu, "map_view_a", "the map screen is drawn"); });
    then_press(r, SELECT, 0.1, 0.8); // '*': the other view
    then_do(r, "the map's second view",
            [&r](mt_cpu *cpu) { map_view(r, cpu, "map_view_b", "View switches the map's view"); });
    then_press(r, A, 0.1, 0.8); // -5: Mini
    then_do(r, "the map's mini view",
            [&r](mt_cpu *cpu) { map_view(r, cpu, "map_view_mini", "A shows the mini map"); });
    then_press(r, B, 0.1, 0.8);
    then_wait_for(r, "the map screen closed", 5, field, 0.3);
    then_check(r, "B closes the map screen: the field again", field);
}

// MT_GL_DUMP=FIRST-LAST: every draw of GL frames FIRST..LAST (the frame index the next present
// gets) with its vertices, the bound texture and the colour, to stdout. For finding a glitch.
struct GlDump {
    u32 first = 0, last = 0, texture = 0, vsize = 0, vtype = 0, vstride = 0, vptr = 0;
    bool tex_on = false;
    u32 color[4] = {};
};
GlDump g_dump;
void gl_dump(void *, mt_cpu *cpu, const mt_gles_cmd *c) {
    const u32 frame = static_cast<u32>(g_run->gl_frames) + 1;
    switch (c->op) {
    case MT_GLES_BindTexture:
        g_dump.texture = c->args[1];
        break;
    case MT_GLES_Enable:
    case MT_GLES_Disable:
        if (c->args[0] == 0x0DE1) { // GL_TEXTURE_2D
            g_dump.tex_on = c->op == MT_GLES_Enable;
        }
        break;
    case MT_GLES_Color4x:
        std::memcpy(g_dump.color, c->args, sizeof g_dump.color);
        break;
    case MT_GLES_VertexPointer:
        g_dump.vsize = c->args[0];
        g_dump.vtype = c->args[1];
        g_dump.vstride = c->args[2];
        g_dump.vptr = c->args[3];
        break;
    case MT_GLES_DrawArrays: {
        if (frame < g_dump.first || frame > g_dump.last) {
            break;
        }
        std::string v;
        const u32 n = std::min(c->args[2], 8u);
        for (u32 i = 0; i < n && g_dump.vtype == 0x1402; i++) { // GL_SHORT
            const u32 stride = g_dump.vstride != 0 ? g_dump.vstride : g_dump.vsize * 2;
            const u32 a = g_dump.vptr + (c->args[1] + i) * stride;
            v += " (" + std::to_string(rd16(cpu, a)) + "," + std::to_string(rd16(cpu, a + 2)) + ")";
        }
        std::printf("gl %u: lr %X draw mode %u n %u tex %u%s colour %X %X %X %X:%s\n", frame,
                    cpu->r[14], c->args[0], c->args[2], g_dump.texture,
                    g_dump.tex_on ? "" : " (off)", g_dump.color[0], g_dump.color[1],
                    g_dump.color[2], g_dump.color[3], v.c_str());
        break;
    }
    default:
        break;
    }
}

// --- the flows --------------------------------------------------------------------------------

// Queued after the shot of Henesys when set (the widescreen flows' menus, #169).
std::function<void(Run &)> g_in_henesys;

void first_map(Run &r, u64 goal) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_do(r, "a shot of Henesys", [&r](mt_cpu *cpu) { save_shot(r, cpu, "henesys"); });
    if (g_in_henesys) {
        g_in_henesys(r);
    }
    // Down the steps to portal200 (x 40): the Western Hill, 2.3.
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_map_screen(r);
    then_do(r, "the hunt begins", [](mt_cpu *cpu) {
        g_play.hunting = true;
        g_play.hunt_level = hero(cpu).level;
        g_play.hunt_events = mt_game_events_get_stats();
        std::printf("hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(r, "map 2.3 until " + std::to_string(goal) + " ticks of play",
              [goal](mt_cpu *) { return g_play.started && g_play.play_ticks >= goal; },
              static_cast<double>(goal) * 0.05 * 1.5 + 60, {2, 0, 42, 2, 3});
    then_do(r, "the last shot", [&r](mt_cpu *cpu) {
        check(save_shot(r, cpu, "end"), "the screen at the end is not black (end)");
    });
    then_finish(r);
}

// Whether a runtime log line reports an error. The trace channels' lines (uikit, focus, input,
// events) are the game's own actions; the rest is the runtime's: the boot, its notes, and what
// went wrong.
bool error_line(const std::string &line) {
    static const std::regex bad(
        R"(\b(error|fail|failed|unimplemented|not implemented|unsupported|invalid|misuse|)"
        R"(exception|fault|trap|crash|abort|assert|overflow|out of|beyond|cannot|can't|unknown|)"
        R"(bad)\b)",
        std::regex::icase);
    for (const char *channel : {"uikit: ", "focus: ", "input: ", "events: "}) {
        if (line.rfind(channel, 0) == 0) {
            return false;
        }
    }
    return std::regex_search(line, bad);
}

// The end of the run, the checks every mode makes: the run ended from the script, the play
// lasted, nothing is unimplemented, GL saw no misuse and the log no error. Writes play.log.
void common_checks(Run &r, u64 goal) {
    const Play &p = g_play;
    std::printf("play: %s\n", play_text(p).c_str());
    check(r.outcome == MT_PROCESS_STOPPED && r.done && r.result.trap_kind == 0,
          "the run ended from the script, no trap");
    check(p.started && p.play_ticks >= goal,
          std::to_string(p.play_ticks) + " game ticks of play from the first field (" +
              std::to_string(goal) + " wanted: " +
              std::to_string(static_cast<double>(goal) * 50 / 60000.0) + " game-minutes at 50 ms)");
    char report[4096];
    mt_report_format(report, sizeof report);
    const mt_report_stats rep = mt_report_get_stats();
    check(rep.imports == 0 && rep.selectors == 0,
          std::string("the unimplemented report: ") + report);
    const mt_gles_stats gl = mt_gles_get_stats();
    const mt_gfx_stats gfx = mt_gfx_get_stats();
    check(gl.misuses == 0 && gfx.total.unsupported == 0 && gfx.total.invalid == 0,
          "GL: " + std::to_string(gl.frames) + " frames, " + std::to_string(gl.draws_drawn) +
              " draws, " + std::to_string(gl.misuses) + " misuses, " +
              std::to_string(gfx.total.unsupported) + " unsupported and " +
              std::to_string(gfx.total.invalid) + " invalid draws");
    std::map<std::string, int> errors;
    for (const std::string &line : r.log) {
        if (error_line(line)) {
            errors[std::regex_replace(line, std::regex("0x[0-9A-Fa-f]+|[0-9]+"), "#")]++;
        }
    }
    std::string text;
    for (const auto &[line, n] : errors) {
        text += "\n  " + std::to_string(n) + "x " + line;
    }
    check(errors.empty(), "no error lines in the runtime's log (" + std::to_string(r.log.size()) +
                              " lines)" + text);
    std::ofstream log(r.out / "play.log", std::ios::binary | std::ios::trunc);
    for (const std::string &line : r.log) {
        log << line << "\n";
    }
}

// first_map's own checks: the maps on the way, and the hunt's walks, fights, drops and levels.
void first_map_checks(Run &, u64 goal) {
    const Play &p = g_play;
    std::string maps;
    for (const auto &[a, m] : p.maps) {
        maps += " " + std::to_string(a) + "." + std::to_string(m);
    }
    check(p.maps.count({2, 8}) && p.maps.count({2, 9}) && p.maps.count({2, 0}) &&
              p.maps.count({2, 3}),
          "the hero was on maps 2.8, 2.9 (the tutorial), 2.0 (Henesys) and 2.3:" + maps);
    const mt_game_events_stats ev = mt_game_events_get_stats();
    check(p.walks > 0 && p.jumps >= goal / 1800 && p.attacks >= goal / 600 &&
              p.max_x - p.min_x >= 300,
          "the hero walked (" + std::to_string(p.walks) + " times), jumped (" +
              std::to_string(p.jumps) + "), attacked (" + std::to_string(p.attacks) +
              ") and went across the hunting field (x " + std::to_string(p.min_x) + " to " +
              std::to_string(p.max_x) + ")");
    check(p.kills >= goal / 900 && p.damage_numbers_max > 0,
          std::to_string(p.kills) + " mobs defeated, damage numbers on screen (up to " +
              std::to_string(p.damage_numbers_max) + " at once)");
    check(ev.hero_damage > p.hunt_events.hero_damage,
          "mobs hit the hero: " + std::to_string(ev.hero_damage - p.hunt_events.hero_damage) +
              " hits on the hunting field (CHero::TakeDamage)");
    check(p.drops_seen >= p.kills / 2 && p.drops_taken >= p.drops_seen / 4 && p.meso_gained > 0 &&
              p.items_gained > 0,
          std::to_string(p.drops_seen) + " drops on the ground, " + std::to_string(p.drops_taken) +
              " picked up: meso +" + std::to_string(p.meso_gained) + ", items +" +
              std::to_string(p.items_gained));
    check(ev.level_ups > p.hunt_events.level_ups && p.last.level > p.hunt_level,
          "the hero gained a level by fighting: level " + std::to_string(p.hunt_level) + " -> " +
              std::to_string(p.last.level) + " on the hunting field, " +
              std::to_string(ev.level_ups - p.hunt_events.level_ups) +
              " CHero::LevelUp entries there (" + std::to_string(ev.level_ups) + " in all)");
    check(p.map_views.size() >= 2,
          "the map screen showed " + std::to_string(p.map_views.size()) + " different views");
}

// --- save, quit and relaunch (#78) ------------------------------------------------------------
//
// Two processes on one data directory (CTests play.save_quit, then play.save_relaunch):
//   save_quit       a new game, the tutorial (levels, EXP, meso, items and quests change), Save
//                   on Stan's ledge in Henesys; the hero's state and the save files are recorded
//                   in OUT_DIR while the game shows "saved"; then the process ends, with no
//                   shutdown of its own (the game has nothing more to write). With the save
//                   backups on (#148), the save left one backup of slot 0 with those bytes.
//   save_relaunch   before the boot, the save files are byte for byte what the first process
//                   saved; Play, the first slot, Start: the hero is in the state recorded
//                   (level, EXP, meso, HP, MP, job, skill points and levels, every inventory
//                   slot and worn item, the map and place, the chapter and every quest); then
//                   Save again writes the same bytes (the heap bytes the game never writes
//                   excepted, docs/formats/save.md).

// The hero's state that a save keeps (docs/formats/save.md: theif's fields are the hero's,
// quest's the quest manager's), one "key=value" a fact, from guest memory. x and y apart: the
// position may settle a pixel after a load.
std::map<std::string, std::string> saved_state(mt_cpu *cpu) {
    std::map<std::string, std::string> s;
    const Hero h = hero(cpu);
    const auto put = [&s](const std::string &k, i32 v) { s[k] = std::to_string(v); };
    put("level", h.level);
    put("exp", h.exp);
    put("meso", h.meso);
    put("hp", h.hp);
    put("mp", h.mp);
    put("max_hp", h.max_hp);
    put("max_mp", h.max_mp);
    put("branch", rdi(cpu, h.addr + 0x88));
    put("job_tier", rdi(cpu, h.addr + 0x8C));
    put("sp", rd16(cpu, h.addr + 0xBC));
    s["map"] = std::to_string(h.area) + "." + std::to_string(h.map);
    put("chapter", h.chapter);
    put("x", h.x);
    put("y", h.y);
    for (u32 sort = 0; sort < 2; sort++) {
        std::string q;
        for (u32 n = 0; n < 100; n++) {
            q += std::to_string(quest_state(cpu, h, sort, n));
        }
        s[sort == 0 ? "main_quests" : "side_quests"] = q;
    }
    std::string active;
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        active += " " + std::to_string(a.sort) + "." + std::to_string(a.num);
        for (const Requirement &q : a.reqs) {
            active += "[" + std::to_string(q.kind) + ":" + std::to_string(q.target) + " " +
                      std::to_string(q.progress) + "/" + std::to_string(q.needed) + "]";
        }
    }
    s["active_quests"] = active;
    for (i32 i = 0; i < 48; i++) {
        const EtcItem e = etc_item(cpu, h, i);
        s["etc_" + std::to_string(i)] = std::to_string(e.id) + " x" + std::to_string(e.count);
    }
    // The equipment records: item_equip id +0x90, rolled +0xD4, slots left +0xCC, scrolls used
    // +0xD0, the six stat bonuses +0xD8..+0xEC; the inventory (+0xD4, 48) and the worn (+0xDC, 5).
    const auto equip = [cpu](u32 item) {
        if (item == 0) {
            return std::string("-");
        }
        std::string e = std::to_string(rdi(cpu, item + 0x90));
        for (const u32 off : {0xD4u, 0xCCu, 0xD0u, 0xD8u, 0xDCu, 0xE0u, 0xE4u, 0xE8u, 0xECu}) {
            e += " " + std::to_string(rdi(cpu, item + off));
        }
        return e;
    };
    for (const auto &[list, n, name] :
         {std::tuple{0xD4u, 48u, "equip_"}, std::tuple{0xDCu, 5u, "worn_"}}) {
        const u32 items = rd(cpu, h.addr + list);
        for (u32 i = 0; i < n; i++) {
            s[name + std::to_string(i)] = equip(items != 0 ? rd(cpu, items + 4 * i) : 0);
        }
    }
    // The skill levels by skill slot ([hero +0x114] + 0xC + 8i).
    std::string skills;
    const u32 slots = rd(cpu, h.addr + 0x114);
    for (u32 i = 0; slots != 0 && i < 13; i++) {
        skills += " " + std::to_string(rdi(cpu, slots + 0xC + 8 * i));
    }
    s["skill_levels"] = skills;
    return s;
}

const fs::path k_state_file = "saved_hero.txt";

void write_state(const fs::path &file, const std::map<std::string, std::string> &s) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto &[k, v] : s) {
        out << k << "=" << v << "\n";
    }
}
std::map<std::string, std::string> read_state(const fs::path &file) {
    std::map<std::string, std::string> s;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            s[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return s;
}

// The bytes SaveSaveData / SaveQuestData never write (heap contents, docs/formats/save.md).
bool unwritten(const char *name, std::size_t i) {
    if (std::string(name) == "theif0") {
        return (i >= 0x009 && i <= 0x00C) || (i >= 0x315 && i <= 0x324);
    }
    return i >= 0x12A4 && i <= 0x12AC;
}

void save_quit(Run &r, u64) {
    // The backups after each save, installed as the frontend does (#148, ADR 0078).
    fe::configure_save_location(fe::Settings{}, fe::data_location_at(r.data.string()));
    fe::install_save_backups();
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    // A Red and a Blue Potion drunk (quick slots 1 and 2): the inventory is no new character's.
    then_press(r, Y, 0.1, 1.0);
    then_press(r, B, 0.1, 1.0);
    then_check(r, "a Red and a Blue Potion drunk: 4 of each left", [](mt_cpu *cpu) {
        return item_count(cpu, hero(cpu), 3) == 4 && item_count(cpu, hero(cpu), 6) == 4;
    });
    then_save(r, [&r](mt_cpu *cpu) {
        const auto s = saved_state(cpu);
        write_state(r.out / k_state_file, s);
        for (const char *name : {"theif0", "quest0"}) {
            std::error_code ec;
            fs::copy_file(r.data / "saves" / name, r.out / (std::string("saved_") + name),
                          fs::copy_options::overwrite_existing, ec);
            check(!ec, std::string("the save's ") + name + " is recorded");
        }
        check(s.at("map") == "2.0" && read_save(r, "theif0").size() == 1124 &&
                  read_save(r, "quest0").size() == 4781,
              "saved in Henesys (2.0): theif0 and quest0 written; " + hero_text(cpu, hero(cpu)));
    });
    then_finish(r);
}

void save_quit_checks(Run &r, u64) {
    const auto s = read_state(r.out / k_state_file);
    check(!s.empty() && std::stoi(s.at("level")) > 1 && s.at("chapter") == "1",
          "the hero's state is recorded past the tutorial (level " +
              (s.empty() ? std::string("?") : s.at("level")) + ", chapter 1)");
    // Nothing temporary is left beside the saves (ADR 0074).
    bool leftovers = false;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(r.data / "saves", ec)) {
        leftovers |= e.path().string().find(MT_SAVES_TMP_SUFFIX) != std::string::npos;
    }
    check(!leftovers, "no temporary save file is left in saves/");
    // The game's save went through the hooked CDataMgr::SaveQuestData (#148): slot 0 has a
    // backup holding the bytes saved.
    const std::vector<fe::BackupInfo> backups = fe::list_backups(fe::save_location());
    bool same = backups.size() == 1 && backups[0].kind == "0";
    for (const char *name : {"theif0", "quest0"}) {
        same = same && read_file(fs::path(backups[0].path) / name) ==
                           read_file(r.out / (std::string("saved_") + name));
    }
    check(same, "the save made one backup of slot 0 with the bytes saved (" +
                    std::to_string(backups.size()) + " backups)");
}

void save_relaunch(Run &r, u64) {
    // Before the boot: what the first process saved is on the disk, byte for byte.
    for (const char *name : {"theif0", "quest0"}) {
        const std::vector<std::uint8_t> saved = read_file(r.out / (std::string("saved_") + name));
        check(!saved.empty() && read_save(r, name) == saved,
              std::string("after the relaunch saves/") + name + " holds the bytes saved (" +
                  std::to_string(saved.size()) + ")");
    }
    attach_pad(r);
    // The monitor's play starts in the field after the load (it waits for map 2.8 otherwise).
    r.on_turn = [&r](mt_cpu *cpu) {
        Play &p = g_play;
        if (!p.started && field(cpu)) {
            p.started = true;
            p.start_ticks = p.minute_ticks = r.ticks;
            p.start_gl = p.minute_gl = r.gl_frames;
            p.start_t = p.minute_t = r.t;
            p.first = p.last = hero(cpu);
        }
        monitor(r, cpu);
    };
    then_load_slot0(r);
    then_do(r, "the hero after the relaunch", [&r](mt_cpu *cpu) {
        const auto want = read_state(r.out / k_state_file);
        auto got = saved_state(cpu);
        std::string diff;
        for (const auto &[k, v] : want) {
            const auto it = got.find(k);
            const std::string g = it != got.end() ? it->second : "<none>";
            const bool near = (k == "x" || k == "y") && it != got.end() &&
                              std::abs(std::stoi(g) - std::stoi(v)) <= 2;
            if (g != v && !near) {
                diff += "\n  " + k + ": saved " + v + ", loaded " + g;
            }
        }
        check(!want.empty() && got.size() == want.size() && diff.empty(),
              "the relaunched game loads the hero as saved: " + std::to_string(want.size()) +
                  " facts (level " + got["level"] + ", EXP " + got["exp"] + ", meso " +
                  got["meso"] + ", map " + got["map"] + ")" + diff);
    });
    // Saved again at once: the same bytes as the first process saved.
    then_save(r, [&r](mt_cpu *) {
        for (const char *name : {"theif0", "quest0"}) {
            const std::vector<std::uint8_t> first =
                read_file(r.out / (std::string("saved_") + name));
            const std::vector<std::uint8_t> again = read_save(r, name);
            std::string diff;
            for (std::size_t i = 0; i < std::max(first.size(), again.size()); i++) {
                const int a = i < first.size() ? first[i] : -1,
                          b = i < again.size() ? again[i] : -1;
                if (a != b && !unwritten(name, i)) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " +0x%03zX %d->%d", i, a, b);
                    diff += buf;
                }
            }
            check(first.size() == again.size() && diff.empty(),
                  std::string("saving after the relaunch writes the same ") + name +
                      " (the never-written heap bytes aside)" + diff);
        }
    });
    then_finish(r);
}

void save_relaunch_checks(Run &, u64) {}

// --- autosave (#150, autosave.h, docs/re/autosave.md) --------------------------------------------
//
// The frontend's turn hook (game.cpp on_turn) runs the Autosaver right before Lifecycle::deliver
// sends applicationWillResignActive: or applicationWillTerminate:. These modes do the same from
// the driver's turn hook, with a Lifecycle fed scripted SDL events as --minimise and --quit feed
// game.cpp's: the same code decides, saves and delivers.
//   autosave_scene   a new game: a minimise in the intro's scene (game mode 9) and a quit on the
//                    tutorial's map 2.8 (a no-save map) save nothing; the files stay as they were
//   autosave_map     from a save (MT_FROM_SAVE) moved to map MT_AUTOSAVE_MAP (one of the 16 maps
//                    where the game cannot save): A through any scene on the way in, then a quit
//                    in the field saves nothing
//   autosave_quit    from a save in Henesys: a minimise with popup 13 open saves nothing; then a
//                    Red Potion drunk and a few steps, and the quit saves (through the backup
//                    hook too) before applicationWillTerminate:
//   autosave_reload  a second process on autosave_quit's data: the files are as autosaved, and
//                    slot 0 loads the hero with that progress

void from_save(Run &r); // below, with chapter 1's parts

struct AutoRun {
    fe::Lifecycle lifecycle{true};
    fe::Autosaver saver;
};
AutoRun g_auto;

// A scripted lifecycle event, then the turn hook's order: the autosave, the messages. Whether it
// saved.
bool leave(Run &r, mt_cpu *cpu, std::uint32_t type) {
    SDL_Event e{};
    e.type = type;
    g_auto.lifecycle.handle(e, true);
    const bool saved =
        g_auto.saver.before_deliver(cpu, fe::Autosave::kOnQuit, g_auto.lifecycle, r.turns, r.t);
    g_auto.lifecycle.deliver(cpu, r.t);
    return saved;
}

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> slot0_files(const Run &r) {
    return {read_save(r, "theif0"), read_save(r, "quest0")};
}

// The quit's end: applicationWillTerminate: was the last message (the game deleted its state),
// so the run ends at once and the monitor reads nothing more.
void end_after_quit(Run &r) {
    const std::vector<fe::AppMessageRecord> sent = g_auto.lifecycle.delivered();
    check(!sent.empty() && sent.back().message == fe::AppMessage::kWillTerminate,
          "applicationWillTerminate: was delivered after the autosave");
    release_all(r);
    r.on_turn = nullptr;
    r.done = true;
}

// A quit that must not save: the check fails on `want`, the files stay as they are.
void quit_without_save(Run &r, mt_cpu *cpu, fe::AutosaveGuard want, const std::string &where) {
    const auto before = slot0_files(r);
    const fe::AutosaveCheck c = fe::check_autosave(cpu);
    const bool saved = leave(r, cpu, SDL_EVENT_QUIT);
    check(!saved && c.guard == want && g_auto.saver.stats().last == want &&
              slot0_files(r) == before && before.first.size() == 1124,
          "a quit " + where + " saves nothing: " + c.describe() + " (want " +
              fe::autosave_guard_name(want) + "); theif0 and quest0 unchanged");
    end_after_quit(r);
}

// The hero stands still in the field (the autosave's hero guard passes).
bool settled(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    return field(cpu) && h.state == 2 && (h.flags & 0xB) == 0;
}

void autosave_scene(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_wait_for(
        r, "the intro's scene (map 2.10, game mode 9)", 60,
        [](mt_cpu *cpu) { return hero(cpu).on(2, 10) && game_mode(cpu) == 9; }, 0.5);
    then_do(r, "a minimise during the scene", [&r](mt_cpu *cpu) {
        const auto before = slot0_files(r);
        const fe::AutosaveCheck c = fe::check_autosave(cpu);
        const bool saved = leave(r, cpu, SDL_EVENT_WINDOW_MINIMIZED);
        check(!saved && c.guard == fe::AutosaveGuard::kScene && slot0_files(r) == before &&
                  before.first.size() == 1124,
              "a minimise in the intro's scene saves nothing: " + c.describe() +
                  "; the new character's theif0 and quest0 unchanged");
        leave(r, cpu, SDL_EVENT_WINDOW_RESTORED); // applicationDidBecomeActive:
    });
    then(r, "the intro: A through its boxes", 90, [&r](mt_cpu *cpu, double el) {
        const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
        hold(r, A, box && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_wait_for(
        r, "the hero stands on the tutorial's map 2.8", 20,
        [](mt_cpu *cpu) { return settled(cpu) && hero(cpu).on(2, 8); }, 0.5);
    then_do(r, "a quit on map 2.8", [&r](mt_cpu *cpu) {
        quit_without_save(r, cpu, fe::AutosaveGuard::kNoSaveMap, "on the tutorial's map 2.8");
    });
}
void autosave_scene_checks(Run &, u64) {}

// MT_AUTOSAVE_MAP=<area>.<map>: slot 0's theif0 is moved there (area 0x41C, map 0x41D, x and y
// -1: the map's default place, docs/formats/save.md) before the boot.
int g_map_area = -1, g_map_map = -1;
int autosave_map_setup(Run &r) {
    const char *env = std::getenv("MT_AUTOSAVE_MAP");
    if (env == nullptr || std::sscanf(env, "%d.%d", &g_map_area, &g_map_map) != 2 ||
        fe::find_no_save_map(g_map_area, g_map_map) == nullptr) {
        std::printf("FAIL: MT_AUTOSAVE_MAP must name one of the 16 no-save maps (area.map)\n");
        return 1;
    }
    std::vector<std::uint8_t> theif = read_save(r, "theif0");
    if (theif.size() != 1124) {
        std::printf("FAIL: MT_FROM_SAVE gave no theif0 (%zu bytes)\n", theif.size());
        return 1;
    }
    theif[0x41C] = static_cast<std::uint8_t>(g_map_area);
    theif[0x41D] = static_cast<std::uint8_t>(g_map_map);
    std::fill(theif.begin() + 0x41E, theif.begin() + 0x422, std::uint8_t{0xFF});
    std::ofstream out(r.data / "saves" / "theif0", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(theif.data()),
              static_cast<std::streamsize>(theif.size()));
    std::printf("theif0 moved to map %d.%d (%s)\n", g_map_area, g_map_map,
                fe::find_no_save_map(g_map_area, g_map_map)->kind);
    // Balog's map (10.5) runs scripts that only chapter 3 has (script/chapter3/BalogMap.out,
    // Sabi3.out): a chapter-1 hero there stops the game, as no real save can be. quest0's
    // chapter (byte 0) goes to 3 for it; every other no-save map loads in chapter 1.
    if (g_map_area == 10) {
        std::vector<std::uint8_t> quest = read_save(r, "quest0");
        if (quest.size() != 4781) {
            return 1;
        }
        quest[0] = 3;
        std::ofstream q(r.data / "saves" / "quest0", std::ios::binary | std::ios::trunc);
        q.write(reinterpret_cast<const char *>(quest.data()),
                static_cast<std::streamsize>(quest.size()));
        std::printf("quest0's chapter set to 3 (Balog's scripts)\n");
        if (!q) {
            return 1;
        }
    }
    return out ? 0 : 1;
}

// then_load_slot0 up to Start (not the wait for the field: a map may start with a scene).
void then_start_slot0(Run &r) {
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
}

void autosave_map(Run &r, u64) {
    g_play.anywhere = true;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    // Slot 0 up to Start; then A through whatever scene the map starts with.
    then_start_slot0(r);
    auto scenes = std::make_shared<int>(0);
    auto since = std::make_shared<double>(-1);
    const std::string where = std::to_string(g_map_area) + "." + std::to_string(g_map_map);
    then(r, "the field of map " + where + " (A through any scene)", 180,
         [&r, scenes, since](mt_cpu *cpu, double el) {
             // With skip_intros on (#174) the skip taps the scene's boxes: the flow's own A
             // presses would add taps, and change the frames the boxes wait (on 10.5, the phase
             // of Balrog's attack after the scene).
             const bool box =
                 (mt_input_panel() == 3 && !r.settings.skip_intros) || mt_input_panel() == 5;
             hold(r, A, box && std::fmod(el, 1.0) < 0.1);
             if (game_mode(cpu) == 9 && hero(cpu).on(g_map_area, g_map_map) && *scenes == 0) {
                 const fe::AutosaveCheck c = fe::check_autosave(cpu);
                 check(c.guard == fe::AutosaveGuard::kScene,
                       "the scene on the way in: no autosave (" + c.describe() + ")");
                 (*scenes)++;
             }
             if (!(field(cpu) && hero(cpu).on(g_map_area, g_map_map))) {
                 *since = -1;
                 return false;
             }
             *since = *since < 0 ? el : *since;
             return el - *since >= 1.0;
         });
    // The earlier guards (a hit, a popup) can hold for a moment; the map's holds throughout.
    then_wait_for(r, "the map's guard", 20, [](mt_cpu *cpu) {
        return fe::check_autosave(cpu).guard == fe::AutosaveGuard::kNoSaveMap;
    });
    then_do(r, "a quit on map " + where, [&r, where](mt_cpu *cpu) {
        quit_without_save(r, cpu, fe::AutosaveGuard::kNoSaveMap,
                          std::string("on the ") +
                              fe::find_no_save_map(g_map_area, g_map_map)->kind + " map " + where);
    });
}
void autosave_map_checks(Run &, u64) {}

const fs::path k_loaded_file = "loaded_hero.txt";

void autosave_quit(Run &r, u64) {
    fe::configure_save_location(fe::Settings{}, fe::data_location_at(r.data.string()));
    fe::install_save_backups();
    from_save(r);
    auto loaded = std::make_shared<std::map<std::string, std::string>>();
    auto red = std::make_shared<i32>(-1);
    then_do(r, "the hero as loaded", [&r, loaded, red](mt_cpu *cpu) {
        *loaded = saved_state(cpu);
        *red = item_count(cpu, hero(cpu), 3);
        write_state(r.out / k_loaded_file, *loaded);
        const fe::AutosaveCheck c = fe::check_autosave(cpu);
        check(c.ok() && c.slot == 0 && !fe::find_no_save_map(c.area, c.map),
              "standing in the field of a plain map, every guard passes: " + c.describe());
    });
    // The popup menu (mode 1) is where the game's own Save is; its "Do you want to save?" is a
    // popup.
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_check(r, "in the popup menu every guard passes (the game saves from here)",
               [](mt_cpu *cpu) { return fe::check_autosave(cpu).ok(); });
    then_press(r, LEFT);
    then_wait_for(r, "the option tab", 5, [](mt_cpu *cpu) { return menu_tab(cpu) == 5; });
    then_press(r, A);
    then_wait_for(r, "save? (popup 13)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 13; });
    then_do(r, "a minimise with the popup open", [&r](mt_cpu *cpu) {
        const auto before = slot0_files(r);
        const fe::AutosaveCheck c = fe::check_autosave(cpu);
        const bool saved = leave(r, cpu, SDL_EVENT_WINDOW_MINIMIZED);
        check(!saved && c.guard == fe::AutosaveGuard::kPopup && slot0_files(r) == before,
              "a minimise with popup 13 open saves nothing: " + c.describe() +
                  "; theif0 and quest0 unchanged");
        leave(r, cpu, SDL_EVENT_WINDOW_RESTORED);
    });
    then_press(r, B); // No
    then_press(r, B);
    then_press(r, B);
    then_wait_field(r);
    // Progress since the save: a Red Potion (quick slot 1) and a few steps left.
    then_press(r, Y, 0.1, 1.0);
    then_check(r, "a Red Potion drunk since the save", [red](mt_cpu *cpu) {
        return *red > 0 && item_count(cpu, hero(cpu), 3) == *red - 1;
    });
    then_press(r, LEFT, 0.4, 1.0);
    then_wait_for(r, "the hero stands", 10, settled, 0.5);
    then_do(r, "the quit", [&r, loaded](mt_cpu *cpu) {
        const auto before = slot0_files(r);
        const std::size_t backups = fe::list_backups(fe::save_location()).size();
        const auto s = saved_state(cpu);
        write_state(r.out / k_state_file, s);
        const bool saved = leave(r, cpu, SDL_EVENT_QUIT);
        const auto after = slot0_files(r);
        check(saved && g_auto.saver.stats().saves == 1 && after != before &&
                  after.first.size() == 1124 && after.second.size() == 4781,
              "the quit autosaved slot 0 before applicationWillTerminate: (" +
                  std::to_string(g_auto.saver.stats().skips) + " skips before it)");
        check(s != *loaded && s.at("etc_0") != loaded->at("etc_0"),
              "the state saved has the progress: Red Potions " + loaded->at("etc_0") + " -> " +
                  s.at("etc_0") + ", x " + loaded->at("x") + " -> " + s.at("x"));
        for (const auto &[name, bytes] :
             {std::pair{"theif0", after.first}, std::pair{"quest0", after.second}}) {
            std::ofstream out(r.out / (std::string("saved_") + name),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        const std::vector<fe::BackupInfo> list = fe::list_backups(fe::save_location());
        check(list.size() == backups + 1 && list[0].kind == "0" &&
                  read_file(fs::path(list[0].path) / "theif0") == after.first,
              "the autosave went through the game's SaveQuestData: a new backup of slot 0 with "
              "the bytes saved (" +
                  std::to_string(list.size()) + " backups)");
        end_after_quit(r);
    });
}
void autosave_quit_checks(Run &r, u64) {
    bool leftovers = false;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(r.data / "saves", ec)) {
        leftovers |= e.path().string().find(MT_SAVES_TMP_SUFFIX) != std::string::npos;
    }
    check(!leftovers, "no temporary save file is left in saves/ (the atomic replacement)");
}

// autosave_exit (#179): from a save in Henesys, a Red Potion drunk, then the popup menu's option
// tab, its exit (item 3, popup 14) and Yes: Game_ExitToMainMenu's hook saves slot 0 before the
// game leaves for the main menu, which never saves. MT_AUTOSAVE_MODE=off: the same exit saves
// nothing.
bool exit_yes_picked(mt_cpu *cpu) {
    const fe::GuestRead32 read = fe::guest_reader(cpu);
    std::uint32_t widget = 0, choice = 1;
    return read(fe::kAutosavePopupWidget, &widget) && read(widget + 0x14, &choice) && choice == 0;
}

void autosave_exit(Run &r, u64) {
    const char *mode_env = std::getenv("MT_AUTOSAVE_MODE");
    static fe::Autosave mode = fe::Autosave::kOnQuit;
    mode = mode_env != nullptr && std::string(mode_env) == "off" ? fe::Autosave::kOff
                                                                 : fe::Autosave::kOnQuit;
    g_auto.saver.install_exit_to_menu(&mode, [&r] { return r.t; });
    from_save(r);
    auto before =
        std::make_shared<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>>();
    auto red = std::make_shared<i32>(-1);
    then_do(r, "the hero as loaded", [red](mt_cpu *cpu) { *red = item_count(cpu, hero(cpu), 3); });
    then_press(r, Y, 0.1, 1.0); // a Red Potion: progress since the save
    then_check(r, "a Red Potion drunk since the save", [red](mt_cpu *cpu) {
        return *red > 0 && item_count(cpu, hero(cpu), 3) == *red - 1;
    });
    then_wait_for(r, "the hero stands", 10, settled, 0.5);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_press(r, LEFT);
    then_wait_for(r, "the option tab", 5, [](mt_cpu *cpu) { return menu_tab(cpu) == 5; });
    for (int i = 0; i < 3; i++) { // save, ..., exit: item 3
        then_press(r, DOWN, 0.1, 0.6);
    }
    then_press(r, A);
    then_wait_for(r, "exit? (popup 14)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 14; });
    then_do(r, "the files before the exit", [&r, before](mt_cpu *cpu) {
        *before = slot0_files(r);
        const fe::AutosaveCheck c = fe::check_autosave(cpu, fe::kAutosaveExitPopup);
        check(c.ok() && fe::check_autosave(cpu).guard == fe::AutosaveGuard::kPopup,
              "with popup 14 open only the exit lets the popup through: " + c.describe());
    });
    then_wait_for(r, "Yes picked (the popup's first choice)", 3, exit_yes_picked);
    then_press(r, A);
    then_wait_for(r, "the main menu (the game left)", 20, [](mt_cpu *cpu) {
        return fe::check_autosave(cpu).guard == fe::AutosaveGuard::kNotInGame;
    });
    then_do(r, "the exit's save", [&r, before](mt_cpu *) {
        const auto after = slot0_files(r);
        const bool on = mode == fe::Autosave::kOnQuit;
        if (on) {
            check(g_auto.saver.stats().saves == 1 && after != *before &&
                      after.first.size() == 1124 && after.second.size() == 4781,
                  "Yes on popup 14 autosaved slot 0 before the main menu (theif0 and quest0 "
                  "rewritten)");
        } else {
            check(g_auto.saver.stats().saves + g_auto.saver.stats().skips == 0 && after == *before,
                  "with autosave = off the exit to the main menu saves nothing, as the game");
        }
        release_all(r);
        r.on_turn = nullptr;
        r.done = true;
    });
}
void autosave_exit_checks(Run &, u64) {}

void autosave_reload(Run &r, u64) {
    for (const char *name : {"theif0", "quest0"}) {
        const std::vector<std::uint8_t> saved = read_file(r.out / (std::string("saved_") + name));
        check(!saved.empty() && read_save(r, name) == saved,
              std::string("after the relaunch saves/") + name + " holds the autosaved bytes");
    }
    from_save(r);
    then_do(r, "the hero after the relaunch", [&r](mt_cpu *cpu) {
        const auto want = read_state(r.out / k_state_file);
        auto before = read_state(r.out / k_loaded_file);
        auto got = saved_state(cpu);
        std::string diff;
        for (const auto &[k, v] : want) {
            const auto it = got.find(k);
            const std::string g = it != got.end() ? it->second : "<none>";
            const bool near = (k == "x" || k == "y") && it != got.end() &&
                              std::abs(std::stoi(g) - std::stoi(v)) <= 2;
            if (g != v && !near) {
                diff += "\n  " + k + ": autosaved " + v + ", loaded " + g;
            }
        }
        check(!want.empty() && got.size() == want.size() && diff.empty() &&
                  got["etc_0"] != before["etc_0"],
              "slot 0 loads the hero as autosaved, with the progress since the game's own save (" +
                  std::to_string(want.size()) + " facts; Red Potions " + before["etc_0"] + " -> " +
                  got["etc_0"] + ", map " + got["map"] + ")" + diff);
    });
    then_finish(r);
}
void autosave_reload_checks(Run &, u64) {}

// --- a config with speed byte 3 (#78) -----------------------------------------------------------
//
// config's byte 1 picks the tick length from {65, 50, 40} ms unchecked (ApplyGameSpeed
// 0x37C41, table 0x108C58): 3 read the word after the table, -1, a tick on every 1 ms poll.
// The port reads it as 2 (mt_saves_sanitise, ADR 0074): the game boots on such a config and
// ticks every 40 ms, 25 ticks a second of the virtual clock; the file is left as it is.

struct SpeedRun {
    double from_t = -1;
    u64 from_ticks = 0;
};
SpeedRun g_speed;

const std::string k_bad_config{3, 3, 0, 1, 1, 3};

void config_speed(Run &r, u64) {
    fs::create_directories(r.data / "saves");
    std::ofstream(r.data / "saves" / "config", std::ios::binary) << k_bad_config;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        Play &p = g_play;
        if (!p.started) { // no field here: the play is the main menu's time
            p.started = true;
            p.start_ticks = r.ticks;
        }
        p.play_ticks = r.ticks - p.start_ticks;
        if (g_speed.from_t < 0 && r.t >= 3.0) { // the boot and the menu's first frames done
            g_speed.from_t = r.t;
            g_speed.from_ticks = r.ticks;
        }
        (void)cpu;
    };
    then_wait_for(
        r, "the main menu (the focus on Play)", 20, [&r](mt_cpu *) { return r.focus.active(); },
        0.5);
    then_wait(r, 8.0);
    then_do(r, "the tick rate", [&r](mt_cpu *) {
        const double secs = r.t - g_speed.from_t;
        const double rate = static_cast<double>(r.ticks - g_speed.from_ticks) / secs;
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "a config with speed byte 3 ticks at a valid speed: %.1f ticks a second "
                      "over %.1f s (40 ms: 25; the unclamped -1: hundreds)",
                      rate, secs);
        check(secs > 5 && rate > 23.0 && rate < 27.0, buf);
    });
    then_finish(r);
}

void config_speed_checks(Run &r, u64) {
    check(logged(r, 0, "config holds values past the game's tables"),
          "the runtime logged that it read config's speed as a valid one");
    check(read_save(r, "config") ==
              std::vector<std::uint8_t>(k_bad_config.begin(), k_bad_config.end()),
          "the config file itself is left as it was");
}

// --- game_speed (#151, ADR 0087) ----------------------------------------------------------------
//
// The port's speed table (game_speed.h) on the virtual clock: the boot with game_speed = 33
// patched in before LoadConfig, then the settings menu's live changes through 65, 50, 40 and 25
// and back to game (the config's own Speed option: 50 on a new config). Each speed's tick rate
// is measured over 6 s of the main menu from CMTimer's lastFrame (the guest's whole ms at each
// tick): within 1 % of 1000 / its length. The game's config keeps its own Speed byte (1) in
// memory and on disk, never a port-only length.
//
// The virtual clock steps 1.001 ms a turn here, not 1 ms. The game re-arms its 1 ms run-loop
// timer for (the callout's time + 0.001 s); with 1 ms turns that fire date ties with the next
// turn's time, and CFAbsoluteTime's rounding makes the timer miss about every other turn, so the
// gate is polled every 1 or 2 ms and each tick runs 0 or 1 ms late: about 0.75 ms a tick, 1.5 %
// at 50 ms and 3 % at 25 ms, for the game's own speeds as for the port's. That is the fixed
// step's artefact (on the device the timer fires about every millisecond and GetTimeMs' whole
// milliseconds absorb a late poll); at 1.001 ms the timer fires at every turn and a tick is late
// a millisecond only once every thousand turns (0.1 %), so the measurement is of the speed.

struct SpeedPhase {
    fe::GameSpeed speed;
    double want; // ticks a second
};
const SpeedPhase k_speed_phases[] = {
    {fe::GameSpeed::k33, 1000.0 / 33}, {fe::GameSpeed::k65, 1000.0 / 65},
    {fe::GameSpeed::k50, 1000.0 / 50}, {fe::GameSpeed::k40, 1000.0 / 40},
    {fe::GameSpeed::k25, 1000.0 / 25}, {fe::GameSpeed::kGame, 1000.0 / 50},
};

// CDataMgr's config (the pointer at 0x11A98C): its Speed byte, or -1 before the game has it.
int config_speed_byte(mt_cpu *cpu) {
    const u32 config = rd(cpu, fe::kConfigPointer);
    return config == 0 ? -1 : cpu->mem[config + 1];
}

struct SpeedMeasure {
    std::int64_t from_frame = 0;
    u64 from_ticks = 0;
    std::vector<int> config_bytes; // the config's Speed byte seen at each phase's end
};
SpeedMeasure g_measure;

void game_speed(Run &r, u64) {
    r.step_ns = 1001000;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *) {
        Play &p = g_play;
        if (!p.started) { // no field here: the play is the main menu's time
            p.started = true;
            p.start_ticks = r.ticks;
        }
        p.play_ticks = r.ticks - p.start_ticks;
    };
    r.before_run = [](mt_cpu *cpu) {
        check(fe::patch_game_speed(cpu, fe::GameSpeed::k33),
              "game_speed 33 is patched into the table before the boot reads the config");
    };
    then_wait_for(
        r, "the main menu (the focus on Play)", 20, [&r](mt_cpu *) { return r.focus.active(); },
        0.5);
    for (const SpeedPhase &ph : k_speed_phases) {
        const fe::GameSpeed speed = ph.speed;
        const double want = ph.want;
        if (speed != fe::GameSpeed::k33) { // the boot's is in place already
            then_do(r, std::string("game_speed ") + fe::game_speed_name(speed),
                    [speed](mt_cpu *cpu) {
                        const std::int64_t ms = fe::apply_game_speed(cpu, speed);
                        check(ms == (speed == fe::GameSpeed::kGame ? 50 : fe::game_speed_ms(speed)),
                              std::string("game_speed ") + fe::game_speed_name(speed) +
                                  " is applied at once: CMTimer's interval " + std::to_string(ms) +
                                  " ms");
                    });
        }
        then_wait(r, 0.5); // the tick under way at the change
        then_do(r, "measure from here", [&r](mt_cpu *) {
            g_measure.from_frame = r.last_frame;
            g_measure.from_ticks = r.ticks;
        });
        then_wait(r, 6.0);
        then_do(r, std::string("the tick rate at ") + fe::game_speed_name(speed),
                [&r, speed, want](mt_cpu *cpu) {
                    const double ms = static_cast<double>(r.last_frame - g_measure.from_frame);
                    const double rate =
                        ms > 0 ? static_cast<double>(r.ticks - g_measure.from_ticks) * 1000.0 / ms
                               : 0;
                    char buf[200];
                    std::snprintf(buf, sizeof buf,
                                  "game_speed %s: %.3f ticks a second over %.0f ms of the guest's "
                                  "clock, %.3f wanted (%+.2f %%; 1 %% allowed)",
                                  fe::game_speed_name(speed), rate, ms, want,
                                  100.0 * (rate / want - 1));
                    check(ms > 5000 && std::abs(rate / want - 1) <= 0.01, buf);
                    g_measure.config_bytes.push_back(config_speed_byte(cpu));
                });
    }
    then_finish(r);
}

void game_speed_checks(Run &r, u64) {
    bool kept = !g_measure.config_bytes.empty();
    for (const int b : g_measure.config_bytes) {
        kept = kept && b == 1;
    }
    check(kept, "the game's config keeps its own Speed byte (1, a new config's) in memory at "
                "every speed");
    const std::vector<std::uint8_t> config = read_save(r, "config");
    check(config.size() == 6 && config[1] <= 2,
          "the config file on disk holds one of the game's own Speed values (" +
              (config.size() > 1 ? std::to_string(config[1]) : std::string("none")) + ")");
}

// --- the offline cash shop (#156, ADR 0072) ------------------------------------------------------
//
// Two runs from a new game, past the tutorial (Henesys, level 5, about 600 meso):
//   cash_shop_meso  cash_shop = meso. LT opens the cash shop; the price line's quotes are there
//                   before any tap; EXP Explosion 40% (the controller focus on its tile) is
//                   bought: the meso go down by the price shown, the EXP goes up, crestore holds
//                   no receipt after the fake server's op 3, theif0 is saved with the new meso.
//                   SP Marble SP+1 (5,000) is more than the hero has: the game's setMeso refuses
//                   it and nothing changes. Then the hero falls (the test calls the game's
//                   CHero::TakeDamage: no mob is needed), the POW! Package is bought on the death
//                   popup (X) at the price shown: revived, 30 Power Elixirs, riprestore1 empty;
//                   the next fall at the level quotes it dearer. Return to town ends it.
//   cash_shop_off   cash_shop = off (ADR 0034): the same taps are cancelled: no meso, EXP or
//                   item changes, no receipt list, no connection to the receipt server.
// The revives' resale floor (30 Power Elixirs, 9,000 meso) is set to 0 for these runs: a level-5
// hero could never pay it, and the floor is a knob (ADR 0072). Everything else is the default.

// CHero::TakeDamage(hero, damage, from_left) 0x65B55: the game's own hit, enough to fall.
void fall(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 args[3] = {h.addr, static_cast<u32>(h.max_hp * 2 + 1000), 0u};
    mt_call_guest(cpu, MT_EVENTS_FN_TAKE_DAMAGE, args, 3, MT_EVENTS_FN_TAKE_DAMAGE);
    std::memcpy(cpu->r, saved, sizeof saved);
}

// A trigger pulled all the way for 0.15 s, then let go for `after` seconds.
void then_trigger(Run &r, SDL_GamepadAxis a, double after = 0.5) {
    then(r, std::string("pull ") + SDL_GetGamepadStringForAxis(a), after + 1.2,
         [&r, a, after](mt_cpu *, double el) {
             r.pad->trigger(a, el < 0.15 ? 1.0f : 0.0f);
             return el >= 0.15 + after;
         });
}

// Moves the controller focus to the control whose action is `action`: a direction press at a
// time toward it (0.1 s down, 0.3 s up), until it is focused.
void then_focus(Run &r, const std::string &action) {
    then(r, "the focus on " + action, 10, [&r, action](mt_cpu *, double el) {
        const fe::FocusTarget *f = r.focus.focused();
        const fe::FocusTarget *want = nullptr;
        for (const fe::FocusTarget &t : r.focus.targets()) {
            want = t.action == action ? &t : want;
        }
        const double phase = std::fmod(el, 0.4);
        if (f != nullptr && f->action == action) {
            release_all(r);
            return phase >= 0.1;
        }
        SDL_GamepadButton b = RIGHT;
        if (f != nullptr && want != nullptr) {
            const float dx = (want->x + want->w / 2) - (f->x + f->w / 2);
            const float dy = (want->y + want->h / 2) - (f->y + f->h / 2);
            b = std::abs(dx) >= std::abs(dy) ? (dx > 0 ? RIGHT : LEFT) : (dy > 0 ? DOWN : UP);
        }
        for (const SDL_GamepadButton d : {LEFT, RIGHT, UP, DOWN}) {
            hold(r, d, d == b && phase < 0.1);
        }
        return false;
    });
}

// The receipt list `name` in saves/: its entry count (i16), -1 if the file is absent.
int receipts(const Run &r, const char *name) {
    const std::vector<std::uint8_t> b = read_save(r, name);
    return b.size() >= 2 ? static_cast<std::int16_t>(b[0] | b[1] << 8) : -1;
}

// theif0's meso (docs/formats/save.md: i32 at 0x011).
i32 saved_meso(const Run &r) {
    const std::vector<std::uint8_t> b = read_save(r, "theif0");
    i32 v = INT32_MIN;
    if (b.size() >= 0x15) {
        std::memcpy(&v, b.data() + 0x11, 4);
    }
    return v;
}

std::int64_t view_quote(const mt_cash_view &v, u32 product) {
    for (u32 i = 0; i < v.count; i++) {
        if (v.quotes[i].product == product) {
            return v.quotes[i].price;
        }
    }
    return -2;
}

std::string lines_text(const mt_cash_view &v) {
    std::string s;
    for (const std::string &l : fe::price_lines(v)) {
        s += (s.empty() ? "" : " | ") + l;
    }
    return s;
}

struct CashRun {
    bool meso = false;
    i32 meso0 = 0, level0 = 0, exp0 = 0, elixirs0 = 0;
    std::int64_t price = 0, revive_price = 0;
    std::size_t log0 = 0;
    mt_cash_stats stats0{};
    mt_stubs_server_stats server0{};
};
CashRun g_cash;

void then_cash_shop_open(Run &r) {
    then_wait_field(r);
    then_trigger(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    then_wait_for(
        r, "the cash shop (panel 9, game mode 0xB, the focus)", 10,
        [&r](mt_cpu *cpu) {
            return mt_input_panel() == 9 && game_mode(cpu) == 0xB && r.focus.active();
        },
        0.5);
}

void cash_shop_run(Run &r, bool meso) {
    g_cash = CashRun{};
    g_cash.meso = meso;
    mt_cash_knobs knobs = *mt_cash_default_knobs();
    knobs.resale[MT_CASH_POW] = knobs.resale[MT_CASH_COMEBACK] = 0;
    mt_cash_shop_set_knobs(&knobs);
    mt_cash_shop_install(meso ? MT_CASH_SHOP_MESO : MT_CASH_SHOP_OFF);
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_cash_shop_open(r);
    then_do(r, "the cash shop's prices, before any tap", [&r, meso](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        g_cash.meso0 = h.meso;
        g_cash.level0 = h.level;
        g_cash.exp0 = h.exp;
        g_cash.log0 = r.log.size();
        g_cash.stats0 = mt_cash_shop_get_stats();
        g_cash.server0 = mt_stubs_get_server_stats();
        const mt_cash_view v = mt_cash_shop_view();
        g_cash.price = view_quote(v, MT_CASH_EXP40);
        if (meso) {
            check(v.screen == MT_CASH_SCREEN_SHOP && v.count == 8 && g_cash.price > 0 &&
                      g_cash.price == mt_cash_shop_quote(MT_CASH_EXP40) &&
                      view_quote(v, MT_CASH_MESOPACK6) == -1 && v.meso == h.meso,
                  "the price line quotes the shop at level " + std::to_string(h.level) + " with " +
                      std::to_string(h.meso) + " meso: " + lines_text(v));
            check(lines_text(v).find("EXP Explosion 40% " + fe::meso_text(g_cash.price)) !=
                      std::string::npos,
                  "the price line shows EXP Explosion 40%'s price");
        } else {
            check(v.screen == MT_CASH_SCREEN_NONE && fe::price_lines(v).empty(),
                  "cash_shop = off: no price line");
        }
        std::printf("cash shop: %s; %s\n", hero_text(cpu, h).c_str(), lines_text(v).c_str());
    });
    then_focus(r, "releaseExp40Button:");
    then_press(r, A, 0.1, 0.2);
    if (meso) {
        then_wait_for(r, "Purchase complete! (popup 0x26C)", 30,
                      [](mt_cpu *cpu) { return popup_id(cpu) == 0x26C; });
        then_do(r, "EXP Explosion 40% bought for meso", [&r](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            const mt_cash_stats st = mt_cash_shop_get_stats();
            const mt_stubs_server_stats sv = mt_stubs_get_server_stats();
            check(h.meso == g_cash.meso0 - g_cash.price && st.sold == g_cash.stats0.sold + 1 &&
                      st.last_price == g_cash.price,
                  "the meso went down by the price shown: " + std::to_string(g_cash.meso0) + " - " +
                      std::to_string(g_cash.price) + " = " + std::to_string(h.meso));
            check(h.level > g_cash.level0 || h.exp > g_cash.exp0,
                  "the EXP was granted: level " + std::to_string(g_cash.level0) + " EXP " +
                      std::to_string(g_cash.exp0) + " -> level " + std::to_string(h.level) +
                      " EXP " + std::to_string(h.exp));
            check(sv.accepted == g_cash.server0.accepted + 1 &&
                      sv.confirmed == g_cash.server0.confirmed + 1 &&
                      logged(r, g_cash.log0,
                             "fake server: receipt (op 2) for "
                             "com.nexonmobile.maplestory.thief.exppack1"),
                  "the fake receipt server confirmed the synthetic receipt (op 2 -> op 3)");
            check(receipts(r, "crestore") == 0, "crestore holds no receipt after the grant (" +
                                                    std::to_string(receipts(r, "crestore")) + ")");
            check(saved_meso(r) == h.meso, "theif0 is saved with the meso after the purchase (" +
                                               std::to_string(saved_meso(r)) + ")");
            char path[1100];
            check(mt_cash_shop_counters_path(0, path, sizeof path) == 0 &&
                      fs::is_regular_file(path),
                  "the cash shop's counters file of slot 0 is written");
        });
        then_press(r, A); // OK: back to the shop
        then_wait_for(
            r, "the cash shop again", 10,
            [&r](mt_cpu *cpu) {
                return mt_input_panel() == 9 && game_mode(cpu) == 0xB && r.focus.active();
            },
            0.5);
        // SP Marble SP+1: up and right of EXP 40; 5,000 is more than the hero has.
        then_do(r, "before SP Marble", [](mt_cpu *cpu) {
            g_cash.meso0 = hero(cpu).meso;
            g_cash.stats0 = mt_cash_shop_get_stats();
        });
        then_focus(r, "releaseMeso10000Button:"); // the SP Marble (its image: 10,000 meso)
        then_press(r, A, 0.1, 0.2);
        then_wait_for(
            r, "the payment refused, the shop back", 15,
            [&r](mt_cpu *cpu) {
                return mt_cash_shop_get_stats().refused > g_cash.stats0.refused &&
                       mt_input_panel() == 9 && game_mode(cpu) == 0xB && r.focus.active();
            },
            1.0);
        then_do(r, "SP Marble refused: not enough meso", [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            check(h.meso == g_cash.meso0 && mt_cash_shop_get_stats().sold == g_cash.stats0.sold &&
                      mt_cash_shop_counters().bought[MT_CASH_SP_MARBLE] == 0,
                  "SP Marble (" + std::to_string(mt_cash_shop_quote(MT_CASH_SP_MARBLE)) +
                      " meso) with " + std::to_string(h.meso) +
                      " meso: setMeso refuses, nothing is sold");
        });
    } else {
        then_wait_for(
            r, "the payment cancelled, the shop back", 15,
            [&r](mt_cpu *cpu) {
                return logged(r, g_cash.log0, "the payment is cancelled") &&
                       mt_input_panel() == 9 && game_mode(cpu) == 0xB && r.focus.active();
            },
            1.0);
        then_do(r, "EXP Explosion 40% cancelled", [&r](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            check(h.meso == g_cash.meso0 && h.level == g_cash.level0 && h.exp == g_cash.exp0,
                  "cash_shop = off: the tap is cancelled, meso, level and EXP unchanged (" +
                      hero_text(cpu, h) + ")");
            check(receipts(r, "crestore") <= 0, "no receipt in crestore");
        });
    }
    then_press(r, B);
    then_wait_field(r);
    // A fall and the death popup.
    then_do(r, "the hero falls (CHero::TakeDamage)", [](mt_cpu *cpu) {
        g_cash.meso0 = hero(cpu).meso;
        g_cash.elixirs0 = item_count(cpu, hero(cpu), 10);
        g_cash.stats0 = mt_cash_shop_get_stats();
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup (0x68)", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_do(r, "the revive's price, before any tap", [meso](mt_cpu *) {
        const mt_cash_view v = mt_cash_shop_view();
        g_cash.revive_price = view_quote(v, MT_CASH_POW);
        if (meso) {
            check(v.screen == MT_CASH_SCREEN_DEATH_POPUP && g_cash.revive_price > 0 &&
                      mt_cash_shop_counters().deaths == 1,
                  "the price line quotes the revives on the death popup: " + lines_text(v));
        } else {
            check(v.screen == MT_CASH_SCREEN_NONE, "cash_shop = off: no price line");
        }
        std::printf("death popup: %s\n", lines_text(v).c_str());
    });
    then_press(r, X, 0.1, 0.2); // -25: the POW! Package
    if (meso) {
        then_wait_for(
            r, "revived in place", 30,
            [](mt_cpu *cpu) {
                const Hero h = hero(cpu);
                return field(cpu) && h.hp == h.max_hp && h.hp > 0;
            },
            1.0);
        then_do(r, "the POW! Package bought for meso", [&r](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            const i32 elixirs = item_count(cpu, h, 10);
            check(h.meso == g_cash.meso0 - g_cash.revive_price && elixirs == g_cash.elixirs0 + 30,
                  "revived for the price shown (" + std::to_string(g_cash.revive_price) +
                      " meso: " + std::to_string(g_cash.meso0) + " -> " + std::to_string(h.meso) +
                      ") with 30 Power Elixirs (" + std::to_string(g_cash.elixirs0) + " -> " +
                      std::to_string(elixirs) + ")");
            check(receipts(r, "riprestore1") == 0, "riprestore1 holds no receipt after the grant");
            const mt_cash_counters c = mt_cash_shop_counters();
            check(c.deaths == 1 && c.paid_revives == 1, "the counters: 1 fall, 1 paid revive");
        });
        then_do(r, "the hero falls again", [](mt_cpu *cpu) { fall(cpu); });
        then_wait_for(
            r, "the death popup again", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
        then_do(r, "the second fall at this level is dearer", [](mt_cpu *) {
            const mt_cash_view v = mt_cash_shop_view();
            const std::int64_t again = view_quote(v, MT_CASH_POW);
            check(again > g_cash.revive_price && v.quotes[0].steps == 2,
                  "the POW! Package after a paid revive and a second fall: " +
                      std::to_string(g_cash.revive_price) + " -> " + std::to_string(again) +
                      " meso (" + lines_text(v) + ")");
        });
    } else {
        then_wait(r, 3.0);
        then_do(r, "the revive cancelled", [&r](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            check(h.meso == g_cash.meso0 && item_count(cpu, h, 10) == g_cash.elixirs0 &&
                      receipts(r, "riprestore1") <= 0 && h.hp <= 0,
                  "cash_shop = off: the revive is cancelled, the hero still down (" + where(cpu) +
                      ")");
            check(mt_stubs_get_server_stats().accepted == g_cash.server0.accepted,
                  "no connection to the receipt server");
        });
    }
    // Return to town (OK, then OK on its confirmation).
    then(r, "return to town", 40, [&r](mt_cpu *cpu, double el) {
        hold(r, A, popup_id(cpu) != -1 && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && hero(cpu).hp > 0;
    });
    then_finish(r);
}

void cash_shop_meso(Run &r, u64) { cash_shop_run(r, true); }
void cash_shop_off(Run &r, u64) { cash_shop_run(r, false); }
void cash_shop_checks(Run &, u64) { mt_cash_shop_uninstall(); }

// --- the popup confirm guard (#147, menu_input.h) ---------------------------------------------
//
// confirm_guard: a new game through the tutorial (the guard off, as the defaults), then in
// Henesys three falls (the game's CHero::TakeDamage, as cash_shop_run does) with the pad's A
// mashed: jump in the field, OK on the death popup 0x68, where OK is Return to town.
//   1. confirm_guard off: A mashed from before the fall to 0.28 s after the popup opened answers
//      the popup (the problem #147 fixes).
//   2. on: the same mash leaves the popup open, the presses dropped by the guard; one A after it
//      is Return to town.
//   3. on: X (attack in the field; revive on the popup) held from the field through the fall, A
//      at about 0.45 s and 0.75 s after the popup opened: dropped while X is down, no revive. X
//      let go, A returns to town.
// Then the hunt on 2.3 until the hero levels up by fighting, with A mashed for 1.5 s after it:
// the game opens no popup on a level-up (only a white flash, docs/ANALYSIS.md), so A stays jump.

constexpr i32 k_death_popup = 0x68;

// What a fall with A mashed saw.
struct Mash {
    double opened = -1;        // seconds into the task when popup 0x68 opened
    std::uint64_t guarded = 0; // MenuInput's dropped presses before it
    std::uint64_t held = 0;    // of them, for a button held from the field
};

// The hero falls 0.5 s into the task while A is mashed (0.05 s down, 0.05 s up), on until
// `after` seconds after the death popup opened; `held` (if not INVALID) is held instead of the
// mash.
void then_mash_fall(Run &r, std::string what, double after, std::shared_ptr<Mash> m,
                    SDL_GamepadButton held = SDL_GAMEPAD_BUTTON_INVALID) {
    auto fell = std::make_shared<bool>(false);
    then(r, std::move(what), 30, [&r, after, m, held, fell](mt_cpu *cpu, double el) {
        if (!*fell) {
            m->guarded = r.menu.stats().guarded;
            m->held = r.menu.stats().held;
        }
        if (held != SDL_GAMEPAD_BUTTON_INVALID) {
            hold(r, held, true);
        }
        if (!*fell && el >= 0.5) {
            *fell = true;
            fall(cpu);
        }
        if (m->opened < 0 && popup_id(cpu) == k_death_popup) {
            m->opened = el;
            std::printf("the death popup opened at %.3f s, %.3f s after the fall\n", r.t, el - 0.5);
        }
        if (m->opened >= 0 && el - m->opened >= after) {
            hold(r, A, false);
            return true;
        }
        hold(r, A, held == SDL_GAMEPAD_BUTTON_INVALID && std::fmod(el, 0.1) < 0.05);
        return false;
    });
}

// OK on whatever popup is open (once a second) until the hero stands in the field again.
void then_return_to_town(Run &r) {
    then(r, "return to town", 40, [&r](mt_cpu *cpu, double el) {
        hold(r, A, popup_id(cpu) != -1 && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && hero(cpu).hp > 0;
    });
    then_wait_field(r);
}

void confirm_guard(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);

    // 1. Off: the mash answers the popup.
    auto off = std::make_shared<Mash>();
    then_mash_fall(r, "confirm_guard off: A mashed through a fall", 0.28, off);
    then_wait(r, 0.3);
    then_do(r, "the death popup answered", [&r, off](mt_cpu *cpu) {
        check(off->opened >= 0 && popup_id(cpu) != k_death_popup &&
                  r.menu.stats().guarded == off->guarded,
              "confirm_guard off: A mashed into the death popup answers it, nothing dropped (" +
                  where(cpu) + ")");
    });
    then_return_to_town(r);

    // 2. On: the same mash leaves it open.
    then_do(r, "confirm_guard on", [&r](mt_cpu *) { r.menu.set_guard(true); });
    auto on = std::make_shared<Mash>();
    then_mash_fall(r, "confirm_guard on: A mashed through a fall", 0.28, on);
    then_wait(r, 0.3);
    then_do(r, "the death popup still open", [&r, on](mt_cpu *cpu) {
        const std::uint64_t dropped = r.menu.stats().guarded - on->guarded;
        check(on->opened >= 0 && popup_id(cpu) == k_death_popup && hero(cpu).hp <= 0 &&
                  dropped >= 2,
              "confirm_guard on: the death popup is still open after A was mashed into it, " +
                  std::to_string(dropped) + " presses dropped (" + where(cpu) + ")");
    });
    then_press(r, A, 0.1, 0.5);
    then_check(r, "past the guard, A answers it (Return to town)",
               [](mt_cpu *cpu) { return popup_id(cpu) != k_death_popup; });
    then_return_to_town(r);

    // 3. On, X held from the field: A dropped while it is down.
    auto x = std::make_shared<Mash>();
    then_mash_fall(r, "confirm_guard on: X held through a fall", 0.45, x, X);
    then_press(r, A, 0.05, 0.25); // 0.45 s after the popup opened
    then_press(r, A, 0.05, 0.25); // 0.75 s
    then_do(r, "the death popup still open with X held", [&r, x](mt_cpu *cpu) {
        const std::uint64_t dropped = r.menu.stats().guarded - x->guarded;
        check(x->opened >= 0 && popup_id(cpu) == k_death_popup && hero(cpu).hp <= 0 && dropped >= 2,
              "confirm_guard on: A 0.45 s and 0.75 s after the popup opened, with X still "
              "held from the field: dropped (" +
                  std::to_string(dropped) + "), the popup open, no revive (" + where(cpu) + ")");
        check(r.menu.stats().held - x->held == dropped,
              "each for a button still held from the field (X)");
    });
    then_do(r, "X let go", [&r](mt_cpu *) { hold(r, X, false); });
    then_wait(r, 0.3);
    then_check(r, "X let go: its release is the field's attack, the popup still open",
               [](mt_cpu *cpu) { return popup_id(cpu) == k_death_popup; });
    then_press(r, A, 0.1, 0.5);
    then_check(r, "then A answers it", [](mt_cpu *cpu) { return popup_id(cpu) != k_death_popup; });
    then_return_to_town(r);

    // A level-up by fighting on 2.3, A mashed after it.
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    auto levels = std::make_shared<std::uint64_t>(0);
    then_do(r, "the hunt to a level-up", [levels](mt_cpu *cpu) {
        *levels = mt_game_events_get_stats().level_ups;
        std::printf("hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(r, "map 2.3 until a level-up",
              [levels](mt_cpu *) { return mt_game_events_get_stats().level_ups > *levels; }, 600,
              {2, 0, 42, 2, 3});
    auto popups = std::make_shared<int>(0);
    auto level = std::make_shared<i32>(0);
    then(r, "A mashed after the level-up", 3, [&r, popups, level](mt_cpu *cpu, double el) {
        if (*level == 0) {
            *level = hero(cpu).level;
        }
        *popups += popup_id(cpu) != -1 ? 1 : 0;
        hold(r, A, el < 1.5 && std::fmod(el, 0.1) < 0.05);
        return el >= 2.0;
    });
    then_do(r, "a level-up opens no popup", [popups, level](mt_cpu *cpu) {
        check(*popups == 0 && field(cpu),
              "the level-up (to " + std::to_string(*level) +
                  ") opened no popup: A mashed after it stayed jump in the field (" + where(cpu) +
                  ")");
    });
    then_finish(r);
}
void confirm_guard_checks(Run &r, u64) {
    const fe::MenuInput::Stats &s = r.menu.stats();
    std::printf("menu input: %llu popups seen, %llu presses dropped, %llu forwarded\n",
                static_cast<unsigned long long>(s.popups),
                static_cast<unsigned long long>(s.guarded),
                static_cast<unsigned long long>(s.forwarded));
}

// --- chapter 1 (#77) -------------------------------------------------------------------------

Pred defeated(i32 sort, i32 n, i32 count) {
    return
        [sort, n, count](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), sort, n) >= count; };
}
Pred level_at_least(i32 level) {
    return [level](mt_cpu *cpu) { return hero(cpu).level >= level; };
}
Pred both(Pred a, Pred b) {
    return [a, b](mt_cpu *cpu) { return a(cpu) && b(cpu); };
}

// Part A: a new game through the tutorial, then Elder's Advice (main 7), Find the Dark Lord (8)
// and the Job Advancement Evaluation (21), and the save.
void chapter_1_a(Run &r, u64) {
    using namespace quest_bot;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_stock(r, "Red Potions from Mr. Wang (Etc20, Henesys)", 2, 0, "Etc20", 3, 20, 100);
    then_farm(r, "5 Red Snails on 2.3 (Elder's Advice)", 2, 3, {2}, defeated(0, 7, 5), 900);
    then_farm(r, "level 10 on 2.3", 2, 3, {}, level_at_least(10), 2400);
    then_talk_to(r, "Stan: Elder's Advice (main 7) done, Find the Dark Lord (8) given", 2, 0,
                 "Stan", 1, main_quests(7, 8));
    then_stock(r, "Red Potions", 2, 0, "Etc20", 3, 40, 200);
    then_talk_to(r, "Darklord: Find the Dark Lord done, Job Advancement Evaluation (21) given", 0,
                 0, "Darklord", 1, main_quests(8, 21));
    then_farm(r, "10 Stumps on 0.4 (Job Advancement Evaluation)", 0, 4, {4}, defeated(0, 21, 10),
              1800);
    then_talk_to(r, "Darklord: the Job Advancement done (the Thief), Depart for Perion (9) given",
                 0, 0, "Darklord", 2, main_quests(21, 9));
    then_save(r);
    then_finish(r);
}
void chapter_1_a_checks(Run &, u64) {}

// The start of a part that goes on from an earlier part's save (MT_FROM_SAVE): the main menu,
// slot 0, the field.
void from_save(Run &r) {
    g_play.anywhere = true;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_load_slot0(r);
    then_do(r, "the save loaded",
            [](mt_cpu *cpu) { std::printf("loaded: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
}

// Part B: the side quests of Henesys and Kerning City at levels 11-20. Human Longings (side
// 3: 8 Blue Snail Shells), Pia's Play Costume (2: 5 Tree Branches), JM's Music 1 and 2 (4: Ming
// to JM; 5: 15 Squishy Liquids), Alex's Worry (21, level 15: to Ayan in Perion), Show Me What
// You Got (6: 30 Orange Mushroom Caps), Father's Revenge (30: 30 Pigs).
void chapter_1_b(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_equip(r, "the Fruit Knife (the Job Advancement's reward)", 24, 1);
    then_check(r, "the hero wields the Fruit Knife",
               [](mt_cpu *cpu) { return worn(cpu, 1) == 24; });
    then_stock(r, "Red Potions", 2, 0, "Etc20", 3, 60, 200);
    then_talk_to(r, "Humanoid: Human Longings (side 3) given", 2, 0, "Humanoid0", -1,
                 quest_is(1, 3, 1));
    then_talk_to(r, "Pia: Pia's Play Costume (side 2) given", 2, 0, "Pia", -1, quest_is(1, 2, 1));
    then_talk_to(r, "Ming: JM's Music 1 (side 4) given", 2, 0, "Ming", 2, quest_is(1, 4, 1));
    then_farm(r, "8 Blue Snail Shells and 5 Tree Branches on 2.3", 2, 3, {1, 4},
              both(ready(1, 3), ready(1, 2)), 1800);
    then_talk_to(r, "Humanoid: Human Longings done", 2, 0, "Humanoid0", -1, quest_is(1, 3, 2));
    then_talk_to(r, "Pia: Pia's Play Costume done", 2, 0, "Pia", -1, quest_is(1, 2, 2));
    then_talk_to(r, "JM: JM's Music 1 done", 0, 0, "JM", 1, quest_is(1, 4, 2));
    then_talk_to(r, "JM: JM's Music 2 (side 5) given", 0, 0, "JM", 2, quest_is(1, 5, 1));
    then_farm(r, "15 Squishy Liquids from the Slimes on 0.4", 0, 4, {3}, ready(1, 5), 1800);
    then_talk_to(r, "JM: JM's Music 2 done", 0, 0, "JM", 2, quest_is(1, 5, 2));
    then_farm(r, "level 15 on 0.4", 0, 4, {}, level_at_least(15), 3600);
    then_talk_to(r, "Alex: Alex's Worry (side 21) given", 0, 0, "Alex", 1, quest_is(1, 21, 1));
    then_talk_to(r, "Dark Lord: Show Me What You Got (side 6) given", 0, 0, "Darklord", 4,
                 quest_is(1, 6, 1));
    then_farm(r, "30 Orange Mushroom Caps on 0.1", 0, 1, {6}, ready(1, 6), 3600);
    then_talk_to(r, "Dark Lord: Show Me What You Got done", 0, 0, "Darklord", 4, quest_is(1, 6, 2));
    then_talk_to(r, "JM: Father's Revenge (side 30) given", 0, 0, "JM", 3, quest_is(1, 30, 1));
    then_farm(r, "30 Pigs on 2.2", 2, 2, {5}, ready(1, 30), 3600);
    then_talk_to(r, "JM: Father's Revenge done", 0, 0, "JM", 3, quest_is(1, 30, 2));
    then_save(r);
    then_finish(r);
}
void chapter_1_b_checks(Run &, u64) {}

// Part C: Perion (1.0). Depart for Perion (main 9) is done by Perion's own map script
// (PerionMap.out) when the hero first enters 1.0, which gives Missing Master (10). In the
// chain that already happens in part B: Henesys's portal201 to 2.2 stays shut until main 9 is
// done, so B's way to the Pigs goes through area 1 and Perion. Ayan gives Missing Father (side
// 22) and her necklace.
void chapter_1_c(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_go(r, "Perion (1.0)", {1, 0, 376, 399, 30}, 600);
    then_wait_for(r,
                  "Perion's map script (on the first visit): Depart for Perion (9) done, Missing "
                  "Master (10) given",
                  60, main_quests(9, 10), 0.5);
    then_talk_to(r, "Ayan: Missing Father (side 22) given, her necklace", 1, 0, "Iyan", 1,
                 quest_is(1, 22, 1));
    then_check(r, "Ayan's necklace (item 108) in the bag", has_item(108, 1));
    then_save(r);
    then_finish(r);
}
void chapter_1_c_checks(Run &, u64) {}

// Part D (not in CTest yet; #77's follow-up): Missing Master (10: 30 Leaves from the Dark
// Stumps), Missing Master 2 (11: Bruce), Bruce's Research (12: 30 Blue Mushroom Caps; 13: Stan),
// Elder Stan's Assumption (14: Roy), Roy's Advice (15: 20 Axe Stumps, 20 Dark Stumps; 16: on to
// the Ant Tunnel); Missing Father (side 23: the necklace to Bruce; side 22 done). The level-15
// hero of part B cannot beat the Dark Stumps (level 29) on 1.2: in 3,600 game-s it fell 271
// times. It needs skills, gear and levels first.
void chapter_1_d(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_farm(r, "30 Leaves from the Dark Stumps on 1.2 (Missing Master)", 1, 2, {9}, ready(0, 10),
              3600);
    then_talk_to(r, "Manzie: Missing Master (10) done, Missing Master 2 (11) given", 1, 0,
                 "Manzie2", -1, main_quests(10, 11));
    then_talk_to(r, "Bruce: Missing Master 2 (11) done, Bruce's Research (12) given", 2, 0, "Bruce",
                 1, main_quests(11, 12));
    then_talk_to(r, "Bruce: the necklace, Missing Father (side 23) done", 2, 0, "Bruce", 2,
                 quest_is(1, 23, 2));
    then_farm(r, "30 Blue Mushroom Caps on 2.2 (Bruce's Research)", 2, 2, {14}, ready(0, 12), 3600);
    then_talk_to(r, "Bruce: Bruce's Research (12) done, 13 given", 2, 0, "Bruce", 1,
                 main_quests(12, 13));
    then_talk_to(r, "Stan: Bruce's Research (13) done, Elder Stan's Assumption (14) given", 2, 0,
                 "Stan", 3, main_quests(13, 14));
    then_talk_to(r, "Roy: Elder Stan's Assumption (14) done, Roy's Advice (15) given", 1, 0, "Loy",
                 1, main_quests(14, 15));
    then_farm(
        r, "20 Axe Stumps on 1.3 (Roy's Advice)", 1, 3, {12},
        [](mt_cpu *cpu) { return quest_bot::defeats(cpu, 0, 15, 12) >= 20; }, 3600);
    then_farm(r, "20 Dark Stumps on 1.2 (Roy's Advice)", 1, 2, {9}, ready(0, 15), 3600);
    then_talk_to(r, "Roy: Roy's Advice (15) done, Roy's Advice 2 (16) given", 1, 0, "Loy", 2,
                 main_quests(15, 16));
    then_talk_to(r, "Ayan: Missing Father (side 22) done", 1, 0, "Iyan", 1, quest_is(1, 22, 2));
    then_save(r);
    then_finish(r);
}
void chapter_1_d_checks(Run &, u64) {}

// --- the settings menu's Game options and Import saves (#89, ADR 0076) ---------------------------
//
// From play.save_quit's save (MT_FROM_SAVE): on the main menu the key pad has no panel, so the
// menu's Import saves may run there; in the field (Henesys, slot 0) it is refused. Then the
// menu's Game options as game.cpp runs it: GameOptionsOpener presses the field's Menu action,
// waits for the popup menu and touches its option tab (popup_menu_tab_point), one turn at a
// time. The popup menu must show the option tab (CDataMgr +0xB0 = 5). Started again with the
// popup menu open it refuses (not the field). B closes the popup menu.

std::shared_ptr<fe::GameOptionsOpener> g_options;

void game_options(Run &r, u64) {
    g_play.anywhere = true;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        if (g_options) {
            g_options->turn(r.t);
        }
    };
    g_options = std::make_shared<fe::GameOptionsOpener>(
        [](std::uint32_t action, bool down) {
            if (down) {
                mt_input_press(action);
            } else {
                mt_input_release(action);
            }
        },
        [] { return mt_input_panel(); },
        [&r](std::uint32_t phase, float x, float y) {
            r.input.touch(fe::InputSource::kGamepad, 0x40, phase, x, y);
        });
    then_wait_for(
        r, "the main menu (the focus on Play)", 20, [&r](mt_cpu *) { return r.focus.active(); },
        0.5);
    then_check(r, "the main menu: no key pad panel, the menu's Import saves may run",
               [](mt_cpu *) { return mt_input_panel() < 0; });
    then_check(r, "the main menu: Game options is not offered (not the field)",
               [](mt_cpu *) { return !g_options->available(); });
    then_load_slot0(r);
    then_check(r, "the field: the menu's Import saves is refused (a character is loaded)",
               [&r](mt_cpu *) {
                   fe::SaveLocation loc;
                   loc.data_dir = r.data.string();
                   loc.documents_dir = (r.data / "saves").string();
                   return fe::import_saves_action(loc, mt_input_panel(), "x")
                              .rfind("Import from the title", 0) == 0;
               });
    then_check(r, "the field: Game options starts", [](mt_cpu *) { return g_options->start(); });
    then(r, "the opener presses Menu and touches the option tab", 10,
         [](mt_cpu *, double) { return !g_options->busy(); });
    then_check(r, "the opener opened the popup menu", [](mt_cpu *) {
        return g_options->outcome() == fe::GameOptionsOpener::Outcome::kOpened;
    });
    then_wait_for(
        r, "the popup menu on its option tab", 5,
        [](mt_cpu *cpu) { return game_mode(cpu) == 1 && menu_tab(cpu) == 5; }, 0.5);
    then_do(r, "a shot of the option tab", [&r](mt_cpu *cpu) {
        check(game_mode(cpu) == 1 && menu_tab(cpu) == 5,
              "the game's option tab is open (game mode 1, CDataMgr +0xB0 = 5)");
        save_shot(r, cpu, "game_options");
    });
    then_check(r, "with the popup menu open, Game options refuses (not the field)",
               [](mt_cpu *) { return !g_options->start(); });
    then_press(r, B, 0.1, 1.0);
    then_wait_for(r, "the field again", 5, field, 0.5);
    then_finish(r);
}

void game_options_checks(Run &, u64) { g_options.reset(); }

// --- the popup menu's tabs (#164) ------------------------------------------------------------
//
// popup_menu_tabs goes on from part C's save (a Rogue with four skills, Missing Master and two
// side quests) and walks the popup menu's skill, quest and option tabs with the pad's d-pad,
// where the engine draws no cursor: each list shows (the skill and side quest lists only once
// the focus touched their tab, focus.h) and the port's frame sits on the cursor's row
// (prompts.h), in the hints and in each tab's window shot.

// The marker after a press: one, at `want` (screen points: the row as the list widget places
// it, clipped to the list).
void then_marker(Run &r, std::string what, fe::Rect want) {
    then_do(r, what, [&r, what, want](mt_cpu *cpu) {
        const std::vector<fe::Rect> &m = r.hints.hints().markers;
        char got[96] = "none";
        if (!m.empty()) {
            std::snprintf(got, sizeof got, "(%g, %g, %g, %g)", static_cast<double>(m[0].x),
                          static_cast<double>(m[0].y), static_cast<double>(m[0].w),
                          static_cast<double>(m[0].h));
        }
        check(m.size() == 1 && m[0].x == want.x && m[0].y == want.y && m[0].w == want.w &&
                  m[0].h == want.h,
              what + ": the frame at " + got + " (tab " + std::to_string(menu_tab(cpu)) +
                  ", open " + std::to_string(menu_sub(cpu)) + ")");
    });
}

// OK (A) on the quest tab's cursor row opens that quest's page, and B closes it (#167): the page
// shows quest (sort, num), CDataMgr +0x530 as PopupMenu_PaintQuestTab 0x4ED75 reads it, which
// the port's hook stores (mt_hooks_install_quest_page_fix; without it the paint faults).
void then_quest_page(Run &r, const std::string &what, i32 sort, i32 num) {
    then_press(r, A, 0.1, 1.0);
    then_do(r, "A on " + what + ": its page", [&r, what, sort, num](mt_cpu *cpu) {
        const u32 m = data_mgr(cpu);
        const u32 q = rd(cpu, m + 0x530);
        check(game_mode(cpu) == 1 && menu_tab(cpu) == 4 && rd(cpu, m + 0xAC) == 1 && q != 0 &&
                  rdi(cpu, q + 0x04) == sort && rdi(cpu, q + 0x08) == num,
              "A on " + what + " opens its page (+0xAC " + std::to_string(rd(cpu, m + 0xAC)) +
                  ", quest " + std::to_string(rdi(cpu, q + 0x04)) + "/" +
                  std::to_string(rdi(cpu, q + 0x08)) + ")");
        std::string name = "quest_page_" + std::to_string(sort) + "_" + std::to_string(num);
        save_shot(r, cpu, name);
    });
    then_press(r, B, 0.1, 1.0);
    then_check(r, "B closes " + what + "'s page, back on the quest list", [](mt_cpu *cpu) {
        return game_mode(cpu) == 1 && menu_tab(cpu) == 4 && menu_sub(cpu) == 4 &&
               rd(cpu, data_mgr(cpu) + 0xAC) == 0;
    });
}

// The iOS way to a quest's page (#167): a double tap on its row (TouchWidget_HandleTouch
// 0xA18E5, 300 ms) goes to the touch handler 0x502D9, which stores the quest in +0x530 itself.
void then_double_tap(Run &r, float x, float y) {
    auto step = std::make_shared<int>(0);
    then(r,
         "a double tap at (" + std::to_string(static_cast<int>(x)) + ", " +
             std::to_string(static_cast<int>(y)) + ")",
         3, [&r, x, y, step](mt_cpu *, double el) {
             // down 0.0, up 0.05, down 0.15, up 0.2: both taps inside 300 ms.
             const double at[] = {0.0, 0.05, 0.15, 0.2};
             const std::uint32_t phase[] = {MT_UIKIT_TOUCH_BEGAN, MT_UIKIT_TOUCH_ENDED,
                                            MT_UIKIT_TOUCH_BEGAN, MT_UIKIT_TOUCH_ENDED};
             while (*step < 4 && el >= at[*step]) {
                 r.input.touch(fe::InputSource::kGamepad, 0x41, phase[*step], x, y);
                 ++*step;
             }
             return *step == 4 && el >= 1.0;
         });
}

void popup_menu_tabs(Run &r, u64) {
    from_save(r);
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    for (int i = 0; i < 3; i++) { // status, item, equip, skill: left / right open it
        then_press(r, RIGHT, 0.1, 0.6);
    }
    then_check(
        r, "the skill tab open, its list shown (the focus's touch on the tab)", [](mt_cpu *cpu) {
            return menu_tab(cpu) == 3 && menu_sub(cpu) == 3 && !fe::popup_menu_list_hidden(cpu, 3);
        });
    // Skills: rows of 38 points from (97, 70), the skill (210 wide) and its + (34) framed.
    then_marker(r, "skills, row 1 (Lucky Seven)", fe::Rect{97, 70, 254, 35});
    then_press(r, DOWN, 0.1, 0.6);
    then_press(r, DOWN, 0.1, 0.6);
    then_marker(r, "skills, row 3 (Nimble Body)", fe::Rect{97, 146, 254, 35});
    then_press(r, RIGHT, 0.1, 0.6);
    then_check(r, "the quest tab open, its list shown", [](mt_cpu *cpu) {
        return menu_tab(cpu) == 4 && menu_sub(cpu) == 4 && !fe::popup_menu_list_hidden(cpu, 4);
    });
    // Quests: the main quest (placed alone), then the side quests, 33 points apart from
    // (102, 150).
    then_marker(r, "quests, the main quest (Missing Master)", fe::Rect{102, 80, 276, 35});
    then_quest_page(r, "the main quest (Missing Master)", 0, 10);
    then_marker(r, "quests, the main quest again", fe::Rect{102, 80, 276, 35});
    then_press(r, DOWN, 0.1, 0.6);
    then_marker(r, "quests, side quest 1 (Alex's Worry)", fe::Rect{102, 150, 262, 32});
    then_quest_page(r, "side quest 1 (Alex's Worry)", 1, 21);
    then_press(r, DOWN, 0.1, 0.6);
    then_marker(r, "quests, side quest 2 (Missing Father)", fe::Rect{102, 183, 262, 32});
    then_quest_page(r, "side quest 2 (Missing Father)", 1, 22);
    then_marker(r, "quests, side quest 2 again", fe::Rect{102, 183, 262, 32});
    // Counted before the double tap below: after a touch on a row the focus touches the tab
    // again (the option tab never needs one).
    then_do(r, "the lists the focus showed", [&r](mt_cpu *) {
        check(r.focus.stats().menu_tabs == 2, "the focus showed 2 lists by a touch on their tab (" +
                                                  std::to_string(r.focus.stats().menu_tabs) + ")");
    });
    then_double_tap(r, 240, 97); // the main quest's row
    then_check(r, "a double tap on the main quest's row opens its page (the iOS way)",
               [](mt_cpu *cpu) {
                   const u32 m = data_mgr(cpu);
                   const u32 q = rd(cpu, m + 0x530);
                   return game_mode(cpu) == 1 && rd(cpu, m + 0xAC) == 1 && q != 0 &&
                          rdi(cpu, q + 0x04) == 0 && rdi(cpu, q + 0x08) == 10;
               });
    then_press(r, B, 0.1, 1.0);
    then_check(r, "B closes the touched page", [](mt_cpu *cpu) {
        return game_mode(cpu) == 1 && menu_tab(cpu) == 4 && rd(cpu, data_mgr(cpu) + 0xAC) == 0;
    });
    then_press(r, RIGHT, 0.1, 0.6);
    // Options: Save, Option, Help, Main menu, 48 points apart from (145, 60).
    then_marker(r, "options, Save", fe::Rect{145, 60, 200, 40});
    then_press(r, DOWN, 0.1, 0.6);
    then_marker(r, "options, Option", fe::Rect{145, 108, 200, 40});
    then_do(r, "the quest pages' hook", [](mt_cpu *) {
        const mt_hook_stats st = mt_hook_get_stats(MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY);
        check(mt_hook_enabled(MT_HOOK_FN_POPUP_MENU_QUEST_TAB_ON_KEY) == 1 && st.handled > 0,
              "PopupMenu_QuestTabOnKey went through the quest page fix (" +
                  std::to_string(st.handled) + " keys)");
        // #166's list scroll wraps the same function, outside the fix (ADR 0069's chain).
        check(fe::popup_menu_scroll_installed(),
              "the list scroll wraps the quest tab's keys too (#166 x #167)");
    });
    // B on a tab's list closes the popup menu.
    then_press(r, B);
    then_wait_field(r);
    then_finish(r);
}
// popup_menu_down goes on from part C's save and opens the item and equipment tabs with the
// d-pad's down, which Menu_OnKey takes as "open this tab" without the touch that shows the list:
// the item list was empty and the equipment tab showed the doll alone, nothing worn or carried.
// The focus now makes that touch (popup_menu_list_hidden covers tabs 1 and 2): each list is
// shown, its shot (OUT_DIR/down_items.png, down_equipment.png, kept local) has its cells.
void then_down_opens(Run &r, i32 tab, const char *name) {
    then_press(r, DOWN, 0.1, 0.8);
    then_do(r, std::string("down on the ") + name + " tab", [&r, tab, name](mt_cpu *cpu) {
        check(game_mode(cpu) == 1 && menu_tab(cpu) == tab && menu_sub(cpu) == tab,
              std::string("down opened the ") + name + " tab (tab " +
                  std::to_string(menu_tab(cpu)) + ", open " + std::to_string(menu_sub(cpu)) + ")");
        check(!fe::popup_menu_list_hidden(cpu, tab),
              std::string("the ") + name + " list is shown (the focus's touch on the tab)");
        save_shot(r, cpu, std::string("down_") + name);
    });
    then_press(r, B, 0.1, 0.8);
    then_check(r, std::string("B closes the ") + name + " tab",
               [tab](mt_cpu *cpu) { return menu_tab(cpu) == tab && menu_sub(cpu) == 0; });
}

void popup_menu_down(Run &r, u64) {
    from_save(r);
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_press(r, RIGHT, 0.1, 0.6);
    then_down_opens(r, 1, "items");
    then_press(r, RIGHT, 0.1, 0.6);
    then_down_opens(r, 2, "equipment");
    then_do(r, "the lists the focus showed", [&r](mt_cpu *) {
        check(r.focus.stats().menu_tabs == 2, "the focus showed 2 lists by a touch on their tab (" +
                                                  std::to_string(r.focus.stats().menu_tabs) + ")");
    });
    then_press(r, B);
    then_wait_field(r);
    then_finish(r);
}
void popup_menu_down_checks(Run &, u64) {}

// popup_menu_shoulders goes on from part A's save and steps through the popup menu's tabs with
// RB and back with LB. On the tab bar the game's '6' / '4' only select the item and equipment
// tabs (a blank panel), and inside them it ignores those keys: the focus touches the tab
// (focus.h), so each step lands on its tab open, the item and equipment lists shown, a shot of
// each (OUT_DIR/shoulder_items.png, shoulder_equipment.png, kept local).
void then_shoulder_to(Run &r, SDL_GamepadButton b, i32 tab, const char *name) {
    then_press(r, b, 0.1, 0.8);
    then_do(r, std::string(b == RB ? "RB" : "LB") + " to the " + name + " tab",
            [&r, b, tab, name](mt_cpu *cpu) {
                const bool listed =
                    tab != 1 && tab != 2 ? true : !fe::popup_menu_list_hidden(cpu, tab);
                check(game_mode(cpu) == 1 && menu_tab(cpu) == tab &&
                          (tab == 0 || menu_sub(cpu) == tab) && listed,
                      std::string(b == RB ? "RB" : "LB") + " opened the " + name +
                          " tab, its list shown (tab " + std::to_string(menu_tab(cpu)) + ", open " +
                          std::to_string(menu_sub(cpu)) + ")");
                if (tab == 1 || tab == 2) {
                    save_shot(r, cpu, std::string("shoulder_") + name);
                }
            });
}

void popup_menu_shoulders(Run &r, u64) {
    from_save(r);
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_shoulder_to(r, RB, 1, "items");
    then_shoulder_to(r, RB, 2, "equipment");
    then_shoulder_to(r, RB, 3, "skill");
    then_shoulder_to(r, LB, 2, "equipment");
    then_shoulder_to(r, LB, 1, "items");
    then_shoulder_to(r, LB, 0, "status");
    then_do(r, "the tabs the focus touched", [&r](mt_cpu *) {
        check(r.focus.stats().menu_tabs >= 5, "the focus opened 5 tabs by a touch (" +
                                                  std::to_string(r.focus.stats().menu_tabs) + ")");
    });
    then_press(r, B);
    then_wait_field(r);
    then_finish(r);
}
void popup_menu_shoulders_checks(Run &, u64) {}

void popup_menu_tabs_checks(Run &r, u64) {
    // Each tab's window shot (window_shots: the state's first settled frame) has the frame.
    for (const char *tab : {"_tab3_3", "_tab4_4", "_tab5_5"}) {
        bool marked = false;
        for (const std::string &k : r.window_marked) {
            marked = marked || k.find(tab) != std::string::npos;
        }
        check(marked, std::string("the window shot of the popup menu's ") + (tab + 1) +
                          " state has the port's frame");
    }
}

// popup_menu_scroll (#166) goes on from part C's save made a tier-4 Assassin (theif0's branch
// 1, job tier 4: 13 skills, the keys show 5 rows) and walks the skill list with the d-pad down
// to the 13th skill and back up: after each step the port's frame is on the cursor's row
// (the keys' first row + CDataMgr +0xD4), the whole row inside the list (y 50-250), with a
// window shot of each step (OUT_DIR/scroll_*.png, kept local). A touch drag of the list moves
// it by the finger's delta from where the keys left it, as on the device, and the next key
// brings the cursor's row back.

int popup_menu_scroll_setup(Run &r) {
    std::vector<std::uint8_t> theif = read_save(r, "theif0");
    if (theif.size() != 1124) {
        std::printf("FAIL: MT_FROM_SAVE gave no theif0 (%zu bytes)\n", theif.size());
        return 1;
    }
    theif[0x000] = 1; // branch: Assassin
    theif[0x001] = 4; // job tier 4: all 13 skills
    std::ofstream out(r.data / "saves" / "theif0", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(theif.data()),
              static_cast<std::streamsize>(theif.size()));
    std::printf("theif0 made a tier-4 Assassin\n");
    return 0;
}

// The skill list's widget (0xF14A8) and the keys' first row.
u32 skill_widget(mt_cpu *cpu) { return rd(cpu, rd(cpu, 0xF14A8)); }
i32 keys_first(mt_cpu *cpu) {
    const u32 scroll = rd(cpu, data_mgr(cpu) + 0x10C);
    return scroll != 0 ? static_cast<i32>(rd(cpu, scroll + 4)) : -1;
}
i32 skill_drag(mt_cpu *cpu) { return static_cast<i32>(rd(cpu, skill_widget(cpu) + 0x6C)); }

// After a step: the frame on skill `row`, wholly inside the list, and a window shot.
void then_skill_framed(Run &r, i32 row, const std::string &shot) {
    then_do(r, "skill " + std::to_string(row + 1) + " framed", [&r, row, shot](mt_cpu *cpu) {
        const u32 mgr = data_mgr(cpu);
        const i32 at = keys_first(cpu) + static_cast<i32>(rd(cpu, mgr + 0xD4));
        const std::vector<fe::Rect> &m = r.hints.hints().markers;
        // Rows of 38 points from y 70 (the list's offset), moved by the drag.
        const float y = static_cast<float>(70 + 38 * row + skill_drag(cpu));
        char got[160];
        std::snprintf(got, sizeof got, "first row %d, cursor %d, drag %d, frame %s",
                      keys_first(cpu), static_cast<i32>(rd(cpu, mgr + 0xD4)), skill_drag(cpu),
                      m.empty() ? "none"
                                : (std::to_string(static_cast<int>(m[0].x)) + "," +
                                   std::to_string(static_cast<int>(m[0].y)) + " " +
                                   std::to_string(static_cast<int>(m[0].w)) + "x" +
                                   std::to_string(static_cast<int>(m[0].h)))
                                      .c_str());
        check(at == row && m.size() == 1 && m[0].y == y && m[0].h == 35 && m[0].y >= 50 &&
                  m[0].y + m[0].h <= 250,
              "skill " + std::to_string(row + 1) + ": the cursor's row framed on screen (" + got +
                  ")");
        write_window_shot(r, cpu, shot);
    });
}

// A touch drag of the list from (150, y0) to (150, y1) in ten moves 60 ms apart (a game tick
// is 50 ms), a last move without a delta, then the finger lifted; then the list settles.
void then_drag_list(Run &r, float y0, float y1) {
    auto n = std::make_shared<int>(0);
    then(r, "a touch drag of the list", 3, [&r, n, y0, y1](mt_cpu *, double el) {
        if (el < 0.06 * *n) {
            return false;
        }
        const int i = (*n)++;
        // Nine tenths in nine moves, then to a point short of y1, then the last point: the
        // last move's 1 point gives no fling (TouchWidget_HandleTouch: speed 30 x the last
        // move's delta; 0xA20D1 moves the list by speed / 75 a step).
        const float d = y1 - y0;
        const float y = i <= 9    ? y0 + d * static_cast<float>(i) / 10.0f
                        : i == 10 ? y1 - (d < 0 ? -1.0f : 1.0f)
                                  : y1;
        const std::uint32_t phase = i == 0    ? MT_UIKIT_TOUCH_BEGAN
                                    : i <= 11 ? MT_UIKIT_TOUCH_MOVED
                                              : MT_UIKIT_TOUCH_ENDED;
        r.input.touch(fe::InputSource::kScripted, 0, phase, 150, y);
        return i >= 12;
    });
    then_wait(r, 1.0);
}

// The drag a touch drag gave, with the hooks on or off (shared by the steps).
struct DragSeen {
    i32 before = 0, off = 0, on = 0;
};

void popup_menu_scroll(Run &r, u64) {
    from_save(r);
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    for (int i = 0; i < 3; i++) {
        then_press(r, RIGHT, 0.1, 0.6);
    }
    then_check(r, "the skill tab open, its list shown, 13 skills", [](mt_cpu *cpu) {
        const u32 w = skill_widget(cpu);
        std::printf("skill list: %u cells, rect y %d h %d, content h %d, touch area y %d h %d\n",
                    rd(cpu, w + 8), static_cast<i32>(rd(cpu, w + 0x20)),
                    static_cast<i32>(rd(cpu, w + 0x28)), static_cast<i32>(rd(cpu, w + 0x38)),
                    static_cast<i32>(rd(cpu, w + 0x40)), static_cast<i32>(rd(cpu, w + 0x48)));
        return menu_tab(cpu) == 3 && menu_sub(cpu) == 3 && !fe::popup_menu_list_hidden(cpu, 3) &&
               rd(cpu, w + 8) == 26;
    });
    // A touch drag up by 80 points before any key, with the hooks off (the original), and again
    // with them on: the same drag.
    auto seen = std::make_shared<DragSeen>();
    then_do(r, "the hooks off", [](mt_cpu *) { fe::install_popup_menu_scroll(false); });
    then_drag_list(r, 200, 120);
    then_do(r, "the drag with the hooks off", [seen](mt_cpu *cpu) { seen->off = skill_drag(cpu); });
    then_drag_list(r, 120, 240); // back to the top (the engine's clamp: 0)
    then_do(r, "the hooks on", [](mt_cpu *) { fe::install_popup_menu_scroll(true); });
    then_check(r, "the list at the top again", [](mt_cpu *cpu) { return skill_drag(cpu) == 0; });
    then_drag_list(r, 200, 120);
    then_do(r, "the drag with the hooks on", [&r, seen](mt_cpu *cpu) {
        seen->on = skill_drag(cpu);
        check(seen->off == -80 && seen->on == seen->off,
              "a touch drag moves the list as the original does (drag " + std::to_string(seen->on) +
                  ", " + std::to_string(seen->off) + " with the hooks off)");
        write_window_shot(r, cpu, "scroll_dragged_top");
    });
    then_drag_list(r, 120, 240);
    then_check(r, "the list at the top again", [](mt_cpu *cpu) { return skill_drag(cpu) == 0; });
    then_skill_framed(r, 0, "scroll_down_01");
    for (i32 row = 1; row < 13; row++) {
        then_press(r, DOWN, 0.1, 0.4);
        char shot[32];
        std::snprintf(shot, sizeof shot, "scroll_down_%02d", row + 1);
        then_skill_framed(r, row, shot);
    }
    then_press(r, DOWN, 0.1, 0.4); // the last skill: the cursor stays
    then_skill_framed(r, 12, "scroll_down_13_again");
    // The same drag the other way from where the keys left the list: it moves by as much.
    then_do(r, "the drag before", [seen](mt_cpu *cpu) { seen->before = skill_drag(cpu); });
    then_drag_list(r, 120, 200);
    then_do(r, "the touch drag after the keys", [&r, seen](mt_cpu *cpu) {
        check(skill_drag(cpu) == seen->before - seen->off,
              "a touch drag after the keys moves the list from there by as much (drag " +
                  std::to_string(seen->before) + " -> " + std::to_string(skill_drag(cpu)) + ")");
        write_window_shot(r, cpu, "scroll_dragged");
    });
    // Up again: the first key brings the cursor's row back into the list.
    for (i32 row = 11; row >= 0; row--) {
        then_press(r, UP, 0.1, 0.4);
        char shot[32];
        std::snprintf(shot, sizeof shot, "scroll_up_%02d", row + 1);
        then_skill_framed(r, row, shot);
    }
    then_do(r, "the hooks", [](mt_cpu *) {
        const fe::PopupMenuScrollStats s = fe::popup_menu_scroll_stats();
        std::printf("popup menu scroll: %llu keys in the skill and quest tabs, %llu followed\n",
                    static_cast<unsigned long long>(s.keys),
                    static_cast<unsigned long long>(s.moved));
        check(s.keys == 25 && s.moved == 18, "the key handlers' hooks ran and the list followed");
    });
    then_press(r, B);
    then_wait_field(r);
    then_finish(r);
}
void popup_menu_scroll_checks(Run &, u64) {}

// --- texture packs (#153, ADR 0081) ----------------------------------------------------------
//
// texture_dump plays first_map's flow (the intro, the tutorial maps, Henesys, the field 2.3 for
// TICKS ticks) with a dump into OUT_DIR/dump; texture_pack plays the same flow, which the
// virtual clock makes the same uploads, with the pack in MT_TEXTURE_PACK (made from that dump by
// python3 -m mtport.texpack build) and checks that it replaced every one of them.

// 0, or 77 (skipped) with the reason printed.
int texture_dump_setup(Run &r) {
    const fs::path dir = r.out / "dump";
    check(mt_texpack_dump_open(dir.string().c_str()) == 0, "the dump opens in " + dir.string());
    return 0;
}

void texture_dump_checks(Run &r, u64) {
    const mt_texpack_stats s = mt_texpack_get_stats();
    mt_texpack_dump_close();
    std::size_t files = 0;
    for (const auto &e : fs::recursive_directory_iterator(r.out / "dump")) {
        files += e.is_regular_file() && e.path().extension() == ".png" ? 1u : 0u;
    }
    std::printf("texture_dump: %llu textures and %llu UIKit images written, %zu PNG files\n",
                static_cast<unsigned long long>(s.dumped),
                static_cast<unsigned long long>(s.dumped_ui), files);
    check(s.dumped > 0 && s.dump_errors == 0, std::to_string(s.dumped) + " textures dumped, " +
                                                  std::to_string(s.dump_errors) + " not written");
    check(s.dumped_ui > 0, std::to_string(s.dumped_ui) + " UIKit images dumped");
    check(files == s.dumped + s.dumped_ui, "one PNG per texture and image dumped");
}

int texture_pack_setup(Run &) {
    const char *pack = std::getenv("MT_TEXTURE_PACK");
    if (pack == nullptr || !fs::is_directory(pack)) {
        std::printf("skipped: MT_TEXTURE_PACK names no pack (play.texture_dump, then "
                    "python3 -m mtport.texpack build)\n");
        return 77;
    }
    const int files = mt_texpack_open(pack);
    std::printf("texture_pack: %s: %d files\n", pack, files);
    check(files > 0, "the pack opens with files");
    return 0;
}

void texture_pack_checks(Run &, u64) {
    const mt_texpack_stats s = mt_texpack_get_stats();
    const mt_gfx_stats g = mt_gfx_get_stats();
    mt_texpack_close();
    std::printf(
        "texture_pack: %llu uploads, %llu replaced, %llu without a file, %llu rejected; "
        "%llu UIKit images, %llu replaced, %llu rejected; mt_gfx: %llu replacements, "
        "%u of %u textures alive replaced, %.1f MB of texels\n",
        static_cast<unsigned long long>(s.uploads), static_cast<unsigned long long>(s.replaced),
        static_cast<unsigned long long>(s.missing), static_cast<unsigned long long>(s.rejected),
        static_cast<unsigned long long>(s.ui_images),
        static_cast<unsigned long long>(s.ui_replaced),
        static_cast<unsigned long long>(s.ui_rejected),
        static_cast<unsigned long long>(g.replacements), g.replaced_textures, g.textures,
        static_cast<double>(g.texture_bytes) / 1e6);
    check(s.uploads > 0 && s.replaced == s.uploads,
          "the pack replaced " + std::to_string(s.replaced) + " of " + std::to_string(s.uploads) +
              " uploads (100% wanted)");
    check(s.missing == 0 && s.rejected == 0, std::to_string(s.missing) + " without a file, " +
                                                 std::to_string(s.rejected) + " rejected");
    check(g.replacements == s.replaced && g.rejected_replacements == 0,
          "mt_gfx took every replacement");
    check(s.ui_images > 0 && s.ui_replaced == s.ui_images && s.ui_rejected == 0,
          "the pack replaced " + std::to_string(s.ui_replaced) + " of " +
              std::to_string(s.ui_images) + " UIKit images");
}

// --- widescreen (#86, ADR 0090) ----------------------------------------------------------------
//
// widescreen_off and widescreen_on play first_map's flow with the same pad: the tutorial's maps
// 2.8 and 2.9 (30 tiles, 480 points: 3:2 either way), Henesys 2.0 and the field 2.3 (51 and 60
// tiles: 568 points wide with widescreen on), then TICKS ticks of the hunt on 2.3. Each writes the
// game mode, the hero's state and every mob's at each game tick to OUT_DIR/state.txt.
// widescreen_on runs with the 568-point GL layer and compares its file line by line with the off
// run's (MT_WIDESCREEN_BASE=<widescreen_off's OUT_DIR>; skipped without it): the wider view
// changes no gameplay. It also checks the view width per map and writes the window's picture
// (the GL layer beside the composited screen, as the Presenter draws it) once per map, to
// OUT_DIR/wide_<area>_<map>.png, for looking at (never committed).
//
// Widescreen menus (#169): both runs also open, on Stan's ledge in Henesys, the popup menu (with
// a touch on its item tab: the GL touch widgets), the map screen, the cash shop and Mr. Wang's
// shop, each with a wide shot (OUT_DIR/menu_<screen>.png, on only), a shot of the composited
// screen (menu_<screen>_screen.png) and a hash of part of it in OUT_DIR/menus.txt. widescreen_on
// compares the popup menu's (its window's opaque top bar, (78, 32, 320, 20)) with the off run's:
// the screen is drawn at 480 points in the wide view's centre, where the touches land.

struct WideRun {
    std::ofstream state;
    u64 last_tick = ~0ull;
    u64 lines = 0;
    std::map<std::string, std::set<u32>> views; // "area.map" -> the view widths seen
    std::map<std::string, u64> ticks_on;        // "area.map" -> ticks seen there
};
WideRun g_wide;

// The window's picture: the GL layer's first `view` points centred in a 568-point picture, the
// 480-point composited screen over its middle (Presenter::draw's wings), to OUT_DIR/name.png.
// `pad` (#168): the key pad's groups as the frontend places them, and the strips of its keys
// moved into the wings (keypad_wings_composite) over the wings.
// The window's wide picture now (568 x 320 at 1x: the GL frame, the screen image over its middle
// and, with `pad`, the key pad's strips in the wings) into `out`; its view's width in `view_out`.
// False when it cannot be composed (or the render scale is not 1).
bool wide_pixels(Run &r, mt_cpu *cpu, bool pad, std::vector<std::uint8_t> *out, u32 *view_out) {
    if (pad) {
        fe::apply_keypad_layout();
    }
    const double now = mt_cf_absolute_time();
    if (!r.screen.compose(cpu, now) || r.screen.gl_pixels().empty()) {
        return false;
    }
    const u32 gw = r.screen.gl_width(), gh = r.screen.gl_height();
    const u32 view = std::min(r.screen.gl_view(), gw);
    constexpr u32 w = MT_WIDESCREEN_WIDE, h = MT_WIDESCREEN_HEIGHT;
    if (gh != h) {
        return false; // render scale 1 only
    }
    std::vector<std::uint8_t> &px = *out;
    px.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 3; i < px.size(); i += 4) {
        px[i] = 255;
    }
    const u32 left = (w - view) / 2;
    for (u32 y = 0; y < h; y++) {
        std::memcpy(&px[(static_cast<std::size_t>(y) * w + left) * 4],
                    &r.screen.gl_pixels()[static_cast<std::size_t>(y) * gw * 4],
                    static_cast<std::size_t>(view) * 4);
        std::memcpy(&px[(static_cast<std::size_t>(y) * w + (w - 480) / 2) * 4],
                    &r.screen.pixels()[static_cast<std::size_t>(y) * r.screen.width() * 4],
                    static_cast<std::size_t>(480) * 4);
    }
    std::vector<std::uint8_t> strips;
    constexpr u32 wing = (w - 480) / 2;
    if (pad && fe::keypad_wings_composite(cpu, now, wing, 1, &strips)) {
        // Premultiplied over: the left strip on columns 0 ... 43, the right one on 524 ... 567.
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < 2 * wing; x++) {
                const std::uint8_t *s = &strips[(static_cast<std::size_t>(y) * 2 * wing + x) * 4];
                const u32 px_x = x < wing ? x : 480 + x;
                std::uint8_t *d = &px[(static_cast<std::size_t>(y) * w + px_x) * 4];
                for (int c = 0; c < 3; c++) {
                    d[c] = static_cast<std::uint8_t>(s[c] + (d[c] * (255 - s[3]) + 127) / 255);
                }
            }
        }
    }
    *view_out = view;
    return true;
}

// wide_pixels to OUT_DIR/name.png. `over`, if set, draws on the 568 x 320 picture (RGBA8) before
// it is written (the minimap's corner).
void wide_shot(Run &r, mt_cpu *cpu, const std::string &name, bool pad = false,
               const std::function<void(std::vector<std::uint8_t> &)> &over = {}) {
    constexpr u32 w = MT_WIDESCREEN_WIDE, h = MT_WIDESCREEN_HEIGHT;
    std::vector<std::uint8_t> px;
    u32 view = 0;
    if (!wide_pixels(r, cpu, pad, &px, &view)) {
        return;
    }
    if (over) {
        over(px);
    }
    SDL_Surface *surf =
        SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_ABGR8888,
                              px.data(), static_cast<int>(w * 4));
    if (surf != nullptr) {
        SDL_SavePNG(surf, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(surf);
    }
    std::printf("wide shot %s at %.2f s (view %u)\n", name.c_str(), r.t, view);
}

// Once a game tick: the line of state.txt, the view width on the hero's map, a wide shot after
// 100 ticks on each map.
void wide_tick(Run &r, mt_cpu *cpu) {
    if (r.ticks == g_wide.last_tick) {
        return;
    }
    g_wide.last_tick = r.ticks;
    const Hero h = hero(cpu);
    if (h.addr == 0) {
        return;
    }
    std::string line = std::to_string(r.ticks) + " mode " + std::to_string(game_mode(cpu)) +
                       " hero " + std::to_string(h.area) + "." + std::to_string(h.map) + " " +
                       std::to_string(h.x) + "," + std::to_string(h.y) + " f" +
                       std::to_string(h.flags) + " s" + std::to_string(h.state) + " hp" +
                       std::to_string(h.hp) + " mp" + std::to_string(h.mp) + " lv" +
                       std::to_string(h.level) + " exp" + std::to_string(h.exp) + " meso" +
                       std::to_string(h.meso) + " mobs";
    for (const Mob &m : mobs(cpu)) {
        line += " " + std::to_string(m.id) + "@" + std::to_string(m.x) + "," + std::to_string(m.y) +
                ":" + std::to_string(m.hp);
    }
    line += " drops " + std::to_string(drops(cpu).size());
    g_wide.state << line << '\n';
    g_wide.lines++;
    const std::string map = std::to_string(h.area) + "." + std::to_string(h.map);
    if (game_mode(cpu) == 0) {
        g_wide.views[map].insert(mt_widescreen_view());
    }
    if (++g_wide.ticks_on[map] == 100 && r.widescreen != 0) {
        wide_shot(r, cpu, "wide_" + std::to_string(h.area) + "_" + std::to_string(h.map));
    }
}

// The composited screen's FNV-1a over (x, y, w, h), screen points at render scale 1.
u64 screen_hash(Run &r, mt_cpu *cpu, u32 x, u32 y, u32 w, u32 h) {
    u64 hash = 1469598103934665603ull;
    if (!r.screen.compose(cpu, mt_cf_absolute_time()) || r.screen.width() < x + w ||
        r.screen.height() < y + h) {
        return 0;
    }
    const std::vector<std::uint8_t> &px = r.screen.pixels();
    for (u32 row = y; row < y + h; row++) {
        for (u32 i = x * 4; i < (x + w) * 4; i++) {
            hash = (hash ^ px[static_cast<std::size_t>(row) * r.screen.width() * 4 + i]) *
                   1099511628211ull;
        }
    }
    return hash;
}

// The menus' lines of OUT_DIR/menus.txt: "<screen> mode <m> view <E> hash <h>".
struct MenuRun {
    std::ofstream file;
    std::vector<std::string> lines;
    bool touched = false; // the item tab answered the touch
};
MenuRun g_menus;

void menu_note(Run &r, mt_cpu *cpu, const std::string &name, u64 hash) {
    if (r.widescreen != 0) {
        wide_shot(r, cpu, "menu_" + name);
    }
    save_shot(r, cpu, "menu_" + name + "_screen");
    const std::string line =
        name + " mode " + std::to_string(game_mode(cpu)) + " hash " + std::to_string(hash);
    g_menus.lines.push_back(line + " view " + std::to_string(mt_widescreen_view()));
    g_menus.file << line << '\n';
    std::printf("menu %s: view %u\n", line.c_str(), static_cast<unsigned>(mt_widescreen_view()));
}

// B, a press a second, until the field shows (a tab, then the popup menu; a shop; the map).
void then_back_to_field(Run &r, std::string what) {
    then(r, std::move(what), 10, [&r](mt_cpu *cpu, double el) {
        const bool down = std::fmod(el, 1.0) < 0.1;
        if (field(cpu) && !down) {
            hold(r, B, false);
            return true;
        }
        hold(r, B, down);
        return false;
    });
}

void then_touch(Run &r, std::string what, float x, float y) {
    auto began = std::make_shared<bool>(false);
    then(r, std::move(what), 2, [x, y, began](mt_cpu *, double el) {
        if (el < 0.1) {
            if (!*began) {
                mt_uikit_touch(7, MT_UIKIT_TOUCH_BEGAN, x, y);
                *began = true;
            }
            return false;
        }
        mt_uikit_touch(7, MT_UIKIT_TOUCH_ENDED, x, y);
        return true;
    });
}

void widescreen_menus(Run &r) {
    // The popup menu (START): its window at (74, 28, 332, 264), the tab bar's widget at
    // (70, 260, 335, 35) with the tabs 54 points apart from x 75: the item tab's middle (156, 277).
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(
        r, "the popup menu", 5,
        [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; }, 0.5);
    then_do(r, "the popup menu, a shot", [&r](mt_cpu *cpu) {
        menu_note(r, cpu, "popup", screen_hash(r, cpu, 78, 32, 320, 20));
    });
    then_touch(r, "a touch on the popup menu's item tab (156, 277)", 156, 277);
    then_wait_for(
        r, "the item tab, opened by the touch", 3,
        [](mt_cpu *cpu) { return game_mode(cpu) == 1 && menu_tab(cpu) == 1; }, 0.5);
    then_do(r, "the item tab, a shot", [&r](mt_cpu *cpu) {
        g_menus.touched = menu_tab(cpu) == 1 && menu_sub(cpu) == 1;
        menu_note(r, cpu, "popup_item", screen_hash(r, cpu, 78, 32, 320, 20));
    });
    then_back_to_field(r, "the popup menu closed");
    // The map screen (SELECT).
    then_press(r, SELECT);
    then_wait_for(
        r, "the map screen", 5,
        [](mt_cpu *cpu) { return game_mode(cpu) == 3 && mt_input_panel() == 8; }, 0.5);
    then_do(r, "the map screen, a shot",
            [&r](mt_cpu *cpu) { menu_note(r, cpu, "map", screen_hash(r, cpu, 0, 0, 480, 320)); });
    then_back_to_field(r, "the map screen closed");
    // The cash shop (LT): UIKit over the wide field.
    then_trigger(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    then_wait_for(
        r, "the cash shop", 10,
        [](mt_cpu *cpu) { return mt_input_panel() == 9 && game_mode(cpu) == 0xB; }, 0.5);
    then_do(r, "the cash shop, a shot", [&r](mt_cpu *cpu) {
        menu_note(r, cpu, "cash_shop", screen_hash(r, cpu, 0, 0, 480, 320));
    });
    then_back_to_field(r, "the cash shop closed");
    // Mr. Wang's shop (Etc20, x 440 on Stan's ledge).
    then_walk_to(r, 440);
    then(r, "Mr. Wang's shop", 6, [&r](mt_cpu *cpu, double el) {
        if (quest_bot::in_shop(cpu) && std::fmod(el, 0.5) >= 0.1) {
            hold(r, UP, false);
            return true;
        }
        hold(r, UP, std::fmod(el, 0.5) < 0.1);
        return false;
    });
    then_wait_for(r, "the shop settled", 3, quest_bot::in_shop, 0.5);
    then_do(r, "the shop, a shot",
            [&r](mt_cpu *cpu) { menu_note(r, cpu, "shop", screen_hash(r, cpu, 0, 0, 480, 320)); });
    then_back_to_field(r, "the shop closed");
}

void widescreen_flow(Run &r, u64 goal) {
    g_menus.file.open(r.out / "menus.txt", std::ios::binary | std::ios::trunc);
    g_in_henesys = widescreen_menus;
    first_map(r, goal);
    std::function<void(mt_cpu *)> inner = r.on_turn;
    r.on_turn = [&r, inner](mt_cpu *cpu) {
        inner(cpu);
        wide_tick(r, cpu);
    };
}

int widescreen_off_setup(Run &r) {
    g_wide.state.open(r.out / "state.txt", std::ios::binary | std::ios::trunc);
    return g_wide.state ? 0 : 1;
}

int widescreen_on_setup(Run &r) {
    const char *base = std::getenv("MT_WIDESCREEN_BASE");
    if (base == nullptr || !fs::is_regular_file(fs::path(base) / "state.txt")) {
        std::printf("skipped: MT_WIDESCREEN_BASE names no widescreen_off run (play.widescreen_off "
                    "first)\n");
        return 77;
    }
    r.widescreen = MT_WIDESCREEN_WIDE;
    return widescreen_off_setup(r);
}

std::string views_text() {
    std::string text;
    for (const auto &[map, set] : g_wide.views) {
        text += " " + map + ":";
        for (const u32 v : set) {
            text += " " + std::to_string(v);
        }
    }
    return text;
}

// The menus' screens were shown (and the touch hit the item tab): in both runs.
void menus_checks() {
    g_menus.file.close();
    check(g_menus.lines.size() == 5, std::to_string(g_menus.lines.size()) +
                                         " of 5 menu screens shown (popup, its item tab, map, "
                                         "cash shop, shop)");
    check(g_menus.touched, "a touch on the popup menu's item tab (156, 277) opened it");
}

void widescreen_off_checks(Run &, u64) {
    g_wide.state.close();
    menus_checks();
    const mt_widescreen_stats st = mt_widescreen_get_stats();
    std::printf("widescreen_off: %llu state lines; views%s\n",
                static_cast<unsigned long long>(g_wide.lines), views_text().c_str());
    check(g_wide.lines > 0, "the state was written at every tick");
    check(st.changes == 0 && st.wide_turns == 0 && st.backdrops == 0,
          "widescreen off: the view never left 480 points");
}

void widescreen_on_checks(Run &r, u64) {
    g_wide.state.close();
    menus_checks();
    const mt_widescreen_menu_stats ms = mt_widescreen_get_menu_stats();
    std::printf("widescreen_on: menus: %llu screens drawn in the wide view's centre, %llu dims "
                "stretched\n",
                static_cast<unsigned long long>(ms.screens),
                static_cast<unsigned long long>(ms.dims));
    for (const std::string &l : g_menus.lines) {
        check(l.find(" view " + std::to_string(MT_WIDESCREEN_WIDE)) != std::string::npos,
              "menus: the field stays wide behind the screen: " + l);
    }
    check(ms.screens > 0 && ms.dims > 0,
          "menus: the popup menu, the map screen and the shop drawn in the centre, the popup "
          "menu's dim stretched");
    // The popup menu window's top bar (opaque; the rest lets the field through): the same pixels
    // as the off run's, in the screen image.
    {
        std::ifstream off(fs::path(std::getenv("MT_WIDESCREEN_BASE")) / "menus.txt",
                          std::ios::binary);
        std::map<std::string, std::string> base;
        for (std::string l; std::getline(off, l);) {
            base[l.substr(0, l.find(' '))] = l;
        }
        for (const std::string &l : g_menus.lines) {
            const std::string name = l.substr(0, l.find(' '));
            if (name != "popup" && name != "popup_item") {
                continue;
            }
            const std::string line = l.substr(0, l.find(" view "));
            check(base[name] == line, "menus: the " + name +
                                          " window's pixels in the screen image equal the off "
                                          "run's (" +
                                          line + " / " + base[name] + ")");
        }
    }
    const mt_widescreen_stats st = mt_widescreen_get_stats();
    std::printf(
        "widescreen_on: %llu state lines; %llu turns, %llu wide, %llu changes, %llu camera "
        "hook calls, %llu backdrops stretched; views%s\n",
        static_cast<unsigned long long>(g_wide.lines), static_cast<unsigned long long>(st.turns),
        static_cast<unsigned long long>(st.wide_turns), static_cast<unsigned long long>(st.changes),
        static_cast<unsigned long long>(st.camera_calls),
        static_cast<unsigned long long>(st.backdrops), views_text().c_str());
    const auto only = [](const char *map, u32 width) {
        const auto it = g_wide.views.find(map);
        return it != g_wide.views.end() && it->second == std::set<u32>{width};
    };
    const auto has = [](const char *map, u32 width) {
        const auto it = g_wide.views.find(map);
        return it != g_wide.views.end() && it->second.count(width) != 0;
    };
    check(only("2.8", 480) && only("2.9", 480),
          "the tutorial's maps (30 tiles, 480 points) stay 3:2 in the field");
    check(has("2.0", MT_WIDESCREEN_WIDE) && has("2.3", MT_WIDESCREEN_WIDE),
          "Henesys (2.0) and the field 2.3 are drawn 568 points wide");
    check(st.backdrops > 0 && st.camera_calls > 0, "the backdrop is stretched, the camera hooked");
    // The gameplay: the same state at every tick as the off run's.
    std::ifstream on(r.out / "state.txt", std::ios::binary);
    std::ifstream off(fs::path(std::getenv("MT_WIDESCREEN_BASE")) / "state.txt", std::ios::binary);
    std::string a, b;
    u64 n = 0;
    bool same = true;
    std::string first;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(on, a));
        const bool gb = static_cast<bool>(std::getline(off, b));
        if (!ga && !gb) {
            break;
        }
        n++;
        if (ga != gb || a != b) {
            same = false;
            first = "line " + std::to_string(n) + ":\n  on:  " + (ga ? a : "(end)") +
                    "\n  off: " + (gb ? b : "(end)");
            break;
        }
    }
    check(same && n > 0, "the hero's and the mobs' state at every one of " + std::to_string(n) +
                             " ticks equals the off run's" + (same ? "" : ": " + first));
}

// screen_shake (#158, ADR 0093, docs/re/screen-shake.md): the intro (CAniManager::makeAni1)
// shakes the camera three times (steps 135, 274 and 359: 10, 15 and 5 screen points). With
// MT_SCREEN_SHAKE=P (percent, default 100) the hook scales them. Each turn the current
// CStateGame's shake fields are read: on (+0x47E), the game's amplitude (+0x488, which the hook
// puts back after each Paint), and the offset the last Paint drew (+0x48C, +0x490). The game's
// rand() state (0x11A510) after the intro is printed: the hook draws the same random numbers
// whatever P is, so two runs of different P print the same state.
struct ShakeSeen {
    u32 percent = 100;
    u64 turns_on = 0;
    i32 max_amp = 0, max_off = 0;
    u32 rand_after = 0;
} g_shake;
int screen_shake_setup(Run &) {
    if (const char *env = std::getenv("MT_SCREEN_SHAKE"); env != nullptr && *env != 0) {
        g_shake.percent = static_cast<u32>(std::strtoul(env, nullptr, 10));
    }
    mt_hooks_install_screen_shake(g_shake.percent);
    std::printf("screen_shake %u%%\n", g_shake.percent);
    return 0;
}
void screen_shake(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        const u32 canvas = rd(cpu, k_canvas);
        const u32 st = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0;
        if (st == 0 || game_mode(cpu) != 9 || cpu->mem[st + 0x47E] == 0) {
            return;
        }
        g_shake.turns_on++;
        g_shake.max_amp = std::max(g_shake.max_amp, rdi(cpu, st + 0x488));
        g_shake.max_off = std::max(
            {g_shake.max_off, std::abs(rdi(cpu, st + 0x48C)), std::abs(rdi(cpu, st + 0x490))});
    };
    then_new_game(r);
    then_wait_for(
        r, "the intro's scene (map 2.10, game mode 9)", 60,
        [](mt_cpu *cpu) { return hero(cpu).on(2, 10) && game_mode(cpu) == 9; }, 0.5);
    then(r, "the intro: A through its boxes", 90, [&r](mt_cpu *cpu, double el) {
        const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
        hold(r, A, box && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_do(r, "the rand() state after the intro", [](mt_cpu *cpu) {
        g_shake.rand_after = rd(cpu, 0x11A510);
        std::printf("screen_shake %u%%: %llu scene turns with the shake on, the game's amplitude "
                    "up to %d, offsets up to %d; %llu Paints scaled; rand() state after the "
                    "intro 0x%08X\n",
                    g_shake.percent, static_cast<unsigned long long>(g_shake.turns_on),
                    g_shake.max_amp, g_shake.max_off,
                    static_cast<unsigned long long>(mt_hooks_screen_shake_scaled()),
                    g_shake.rand_after);
    });
    then_wait(r, 0.5); // a few ticks of play in the field (common_checks)
    then_finish(r);
}
void screen_shake_checks(Run &, u64) {
    const u32 p = std::min<u32>(g_shake.percent, 100);
    check(g_shake.turns_on > 0 && g_shake.max_amp == 15,
          "the intro shook (" + std::to_string(g_shake.turns_on) +
              " turns), the game's amplitude up to 15 (" + std::to_string(g_shake.max_amp) + ")");
    // rand[0, a] - a / 2 with a = 15 * P / 100 (rounded): at most a - a / 2.
    const i32 a = static_cast<i32>((15 * p + 50) / 100);
    check(g_shake.max_off <= a - a / 2,
          "the offsets stay within the scaled amplitude: " + std::to_string(g_shake.max_off) +
              " <= " + std::to_string(a - a / 2));
    check(p == 0 ? g_shake.max_off == 0 : g_shake.max_off > 0,
          p == 0 ? "no offset at 0%" : "the camera shook");
    check(p >= 100 ? mt_hooks_screen_shake_scaled() == 0 : mt_hooks_screen_shake_scaled() > 0,
          "the hook scaled " + std::to_string(mt_hooks_screen_shake_scaled()) + " Paints");
}

// --- flashes (#186, ADR 0093 addendum, docs/re/flashes.md) -----------------------------------
//
// flashes: a new game into the tutorial until the hero's first level-up (CHero::LevelUp's game
// event, MacArthur's quest 1), through its dialog, and 60 game ticks of the field after it, in
// which the level-up's effect 12 draws its white and its black frame. Every GL frame from the
// level-up on is hashed (FNV-1a) and measured: the share of its pixels that are white (every
// channel 240 or more; the game's white is 254, glColor4x 0xFF00) and black (15 or less). A
// frame with half its pixels or more of one of them is a full-screen white or black frame (the
// HUD and the hero's level-up text stay on top of the flash). At the end the
// level-up's tick, the last tick and the game's rand() state (0x11A510) are written with the
// frames to OUT_DIR/flashes.txt. MT_FLASHES=P installs the hooks at P percent (unset: never
// installed, today's game); MT_FLASHES_BASE=<the unset run's OUT_DIR> compares with it:
//   - every run: the same level-up tick, last tick, rand() state and frames' ticks as the base;
//   - unset: the level-up drew a full-screen white frame and a full-screen black frame;
//   - 100: every frame byte for byte the base's (the hooks are registered, and off);
//   - below 100: no full-screen white or black frame; the hooks scaled the flash fills (to alpha
//     0 at 0 %), and every frame without one is byte for byte the base's.
struct FlashFrame {
    u64 tick = 0, hash = 0;
    u32 white = 0, black = 0; // per mille of the frame's pixels
    u64 sum = 0;              // of the pixels' red, green and blue
    u32 marked = 0;           // flash fills the hooks drew in it
};
struct FlashRun {
    int percent = -1; // -1: the hooks not installed
    u64 level_up_tick = 0, field_tick = 0, end_tick = 0;
    bool open = false, closed = false;
    u32 rand_after = 0;
    u64 marked_seen = 0;
    std::vector<FlashFrame> frames;
} g_flash;
constexpr u64 k_flash_ticks = 60;
constexpr u32 k_flash_share = 500; // per mille: a full-screen white or black frame

bool flash_frame(const FlashFrame &f) {
    return f.white >= k_flash_share || f.black >= k_flash_share;
}
int flashes_setup(Run &r) {
    if (const char *env = std::getenv("MT_FLASHES"); env != nullptr && *env != 0) {
        g_flash.percent = static_cast<int>(std::strtoul(env, nullptr, 10));
        mt_hooks_install_flashes(static_cast<u32>(g_flash.percent));
        std::printf("flashes %d%%\n", g_flash.percent);
    } else {
        std::printf("flashes: the hooks not installed (the original)\n");
    }
    r.trace = "uikit,focus,events,hooks";
    return 0;
}
void flashes(Run &r, u64) {
    attach_pad(r);
    r.on_frame = [&r](mt_cpu *) {
        if (!g_flash.open || g_flash.closed) {
            return;
        }
        const std::vector<std::uint8_t> &px = r.screen.gl_pixels();
        FlashFrame f;
        f.tick = r.ticks;
        f.hash = 1469598103934665603ull;
        u64 white = 0, black = 0;
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            f.sum += px[i] + px[i + 1] + px[i + 2];
            white += px[i] >= 240 && px[i + 1] >= 240 && px[i + 2] >= 240 ? 1 : 0;
            black += px[i] <= 15 && px[i + 1] <= 15 && px[i + 2] <= 15 ? 1 : 0;
        }
        for (const std::uint8_t b : px) {
            f.hash = (f.hash ^ b) * 1099511628211ull;
        }
        const u64 n = std::max<u64>(1, px.size() / 4);
        f.white = static_cast<u32>(white * 1000 / n);
        f.black = static_cast<u32>(black * 1000 / n);
        const u64 marked = mt_hooks_flash_stats().marked;
        f.marked = static_cast<u32>(marked - g_flash.marked_seen);
        g_flash.marked_seen = marked;
        g_flash.frames.push_back(f);
    };
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        if (!g_flash.open && mt_game_events_get_stats().level_ups > 0) {
            g_flash.open = true;
            g_flash.level_up_tick = r.ticks;
            g_flash.marked_seen = mt_hooks_flash_stats().marked; // the fills before it
            std::printf("flashes: the first level-up at tick %llu (%s)\n",
                        static_cast<unsigned long long>(r.ticks),
                        hero_text(cpu, hero(cpu)).c_str());
        }
        if (g_flash.open && g_flash.field_tick == 0 && game_mode(cpu) == 0 && field(cpu)) {
            g_flash.field_tick = r.ticks; // the level-up's dialog closed: its effect plays
        }
        if (g_flash.field_tick != 0 && !g_flash.closed &&
            r.ticks >= g_flash.field_tick + k_flash_ticks) {
            g_flash.closed = true;
            g_flash.end_tick = r.ticks;
            g_flash.rand_after = rd(cpu, 0x11A510);
            release_all(r);
            r.done = true; // the rest of the tutorial is not needed
        }
    };
    then_new_game(r);
    then_tutorial(r);
    then_finish(r); // only if the tutorial had no level-up: the checks fail
}
void flashes_checks(Run &r, u64) {
    const FlashRun &g = g_flash;
    const mt_flash_stats st = mt_hooks_flash_stats();
    u32 whites = 0, blacks = 0;
    {
        std::ofstream out(r.out / "flashes.txt", std::ios::binary | std::ios::trunc);
        out << "level_up_tick " << g.level_up_tick << "\nend_tick " << g.end_tick << "\nrand "
            << g.rand_after << "\n";
        for (const FlashFrame &f : g.frames) {
            out << f.tick << ' ' << f.hash << ' ' << f.white << ' ' << f.black << ' ' << f.sum
                << ' ' << f.marked << "\n";
            whites += f.white >= k_flash_share ? 1 : 0;
            blacks += f.black >= k_flash_share ? 1 : 0;
        }
    }
    std::printf("flashes %d%%: level-up at tick %llu, end at tick %llu, rand() state 0x%08X; "
                "%zu frames, %u full-screen white, %u full-screen black; the hooks: %llu flash "
                "fills, %llu scaled, %llu at alpha 0\n",
                g.percent, static_cast<unsigned long long>(g.level_up_tick),
                static_cast<unsigned long long>(g.end_tick), g.rand_after, g.frames.size(), whites,
                blacks, static_cast<unsigned long long>(st.marked),
                static_cast<unsigned long long>(st.scaled),
                static_cast<unsigned long long>(st.hidden));
    check(g.closed && !g.frames.empty(), "the tutorial levelled the hero up, and " +
                                             std::to_string(k_flash_ticks) +
                                             " ticks of frames after it were measured");
    const bool hooked = g.percent >= 0 && g.percent < 100;
    if (!hooked) {
        check(whites >= 1 && blacks >= 1, "the level-up drew a full-screen white frame (" +
                                              std::to_string(whites) + ") and black frame (" +
                                              std::to_string(blacks) + ")");
        check(st.marked == 0, "the hooks are off: no flash fill marked");
    } else {
        check(whites == 0 && blacks == 0, "no full-screen white or black frame (" +
                                              std::to_string(whites) + " white, " +
                                              std::to_string(blacks) + " black)");
        check(st.marked >= 2 && (g.percent == 0 ? st.hidden == st.marked : st.scaled == st.marked),
              "the hooks took the level-up's flash fills");
    }
    const char *base_dir = std::getenv("MT_FLASHES_BASE");
    if (base_dir == nullptr || *base_dir == 0) {
        return;
    }
    std::ifstream in(fs::path(base_dir) / "flashes.txt", std::ios::binary);
    std::string key;
    u64 base_level_up = 0, base_end = 0;
    u32 base_rand = 0;
    in >> key >> base_level_up >> key >> base_end >> key >> base_rand;
    std::vector<FlashFrame> base;
    FlashFrame f;
    while (in >> f.tick >> f.hash >> f.white >> f.black >> f.sum >> f.marked) {
        base.push_back(f);
    }
    check(!base.empty(), std::string("the base run's frames are in ") + base_dir);
    check(g.level_up_tick == base_level_up && g.end_tick == base_end && g.rand_after == base_rand,
          "the level-up's tick, the last tick and the rand() state are the base run's (" +
              std::to_string(g.level_up_tick) + ", " + std::to_string(g.end_tick) + ", " +
              std::to_string(g.rand_after) + " against " + std::to_string(base_level_up) + ", " +
              std::to_string(base_end) + ", " + std::to_string(base_rand) + ")");
    bool same_ticks = base.size() == g.frames.size();
    std::size_t same = 0, flashed = 0, flash_marked = 0, before = 0, before_same = 0;
    bool fill_seen = false;
    for (std::size_t i = 0; same_ticks && i < base.size(); i++) {
        same_ticks = base[i].tick == g.frames[i].tick;
        same += base[i].hash == g.frames[i].hash ? 1 : 0;
        flashed += flash_frame(base[i]) ? 1 : 0;
        flash_marked += flash_frame(base[i]) && g.frames[i].marked != 0 ? 1 : 0;
        fill_seen = fill_seen || g.frames[i].marked != 0;
        before += fill_seen ? 0 : 1;
        before_same += !fill_seen && base[i].hash == g.frames[i].hash ? 1 : 0;
    }
    // The game never clears the screen, and some of the field's pixels are blended over the
    // last frame's: the frames after a flash still hold a trace of it (docs/re/flashes.md), so
    // after the first flash fill they are compared by their sums, which must come back to the
    // base run's within a thousandth of a level per channel on average.
    const u64 channels = 3ull * r.screen.gl_width() * r.screen.gl_height();
    u64 last_delta = 0;
    if (same_ticks && !base.empty()) {
        const u64 a = base.back().sum, b = g.frames.back().sum;
        last_delta = a > b ? a - b : b - a;
    }
    check(same_ticks, "the same frames at the same ticks as the base run (" +
                          std::to_string(g.frames.size()) + " and " + std::to_string(base.size()) +
                          ")");
    if (!hooked) {
        check(same == base.size(), "every frame is byte for byte the base run's (" +
                                       std::to_string(same) + " of " + std::to_string(base.size()) +
                                       ")");
    } else {
        check(flash_marked == flashed, "the hooks drew a flash fill in each of the base run's " +
                                           std::to_string(flashed) + " flash frames");
        check(before > 0 && before_same == before,
              "every frame before the first flash fill is byte for byte the base run's (" +
                  std::to_string(before_same) + " of " + std::to_string(before) + ")");
        check(last_delta * 1000 <= channels,
              "the last frame's channel sum is the base run's within " +
                  std::to_string(channels / 1000) + " (" + std::to_string(last_delta) +
                  "); frames byte for byte the base run's: " + std::to_string(same) + " of " +
                  std::to_string(base.size()));
    }
}

// --- widescreen with the touch key pad (#168) ---------------------------------------------------
//
// widescreen_pad_off and widescreen_pad_on: the way of first_map into the field 2.3 (60 tiles),
// then the key pad's left, attack and right keys held by touch for 6 s, as input.keypad_layout
// holds them (left 1 s, attack 1 s, right 1.5 s with attack 0.5 s of it), on the same turns.
// With widescreen off the touches are on the keys' original places; with it on (the field 568
// points wide, the key pad shown and anchored to the view's edges) where the keys are drawn now:
// the left arm in the left wing, the attack key partly in the right one. The hero's x, y, state,
// flags, HP and MP at each game tick of the hold go to OUT_DIR/hold.txt; widescreen_pad_on
// compares its file with the off run's (MT_WIDESCREEN_PAD_BASE=<widescreen_pad_off's OUT_DIR>)
// line by line. widescreen_pad_on also writes the window's wide picture with the key pad on 2.0
// and 2.3, before and during the hold (OUT_DIR/pad_*.png, local only, never committed).

struct WidePadRun {
    std::ofstream hold;
    u64 lines = 0;
    i32 min_x = INT32_MAX, max_x = INT32_MIN;
    bool walked = false, attacked = false;
    u32 hold_view = 0;
};
WidePadRun g_wpad;

// The key pad's touch points of the hold (docs/controls/keypad.md's mask table) where the
// frontend draws them now: the original places moved with their group (keypad_view_transform).
fe::KeypadPoint pad_point(fe::KeypadPoint p) {
    fe::KeypadGroup g{};
    if (!fe::keypad_group_of_frame(fe::KeypadRect{p.x, p.y, 0, 0}, &g)) {
        return p;
    }
    return fe::keypad_view_transform(g, fe::installed_keypad_layout()[g],
                                     fe::installed_keypad_wide())
        .apply(p);
}

void then_pad_hold(Run &r) {
    struct Ev {
        double t;
        u32 finger, phase;
        fe::KeypadPoint at;
    };
    constexpr fe::KeypadPoint left{35, 257}, right{132, 257}, attack{440, 290};
    static const Ev k_events[] = {
        {0.5, 1, MT_UIKIT_TOUCH_BEGAN, left},   {1.5, 1, MT_UIKIT_TOUCH_ENDED, left},
        {2.0, 2, MT_UIKIT_TOUCH_BEGAN, attack}, {3.0, 2, MT_UIKIT_TOUCH_ENDED, attack},
        {3.5, 1, MT_UIKIT_TOUCH_BEGAN, right},  {4.0, 2, MT_UIKIT_TOUCH_BEGAN, attack},
        {4.5, 2, MT_UIKIT_TOUCH_ENDED, attack}, {5.0, 1, MT_UIKIT_TOUCH_ENDED, right},
    };
    struct S {
        std::size_t next = 0;
        u64 start_tick = 0, last_tick = ~0ull;
        bool begun = false;
    };
    auto s = std::make_shared<S>();
    then(r, "the key pad's left, attack and right held by touch", 10,
         [&r, s](mt_cpu *cpu, double el) {
             if (!s->begun) {
                 s->begun = true;
                 s->start_tick = r.ticks;
                 g_wpad.hold_view = mt_widescreen_view();
                 std::printf("hold from %s; view %u, the key pad's offset %g\n",
                             hero_text(cpu, hero(cpu)).c_str(), g_wpad.hold_view,
                             static_cast<double>(fe::installed_keypad_wide().off));
             }
             while (s->next < std::size(k_events) && el >= k_events[s->next].t) {
                 const Ev &e = k_events[s->next++];
                 const fe::KeypadPoint p = pad_point(e.at);
                 check(r.input.touch(fe::InputSource::kFinger, e.finger, e.phase, p.x, p.y),
                       "the hold's touch at (" + std::to_string(p.x) + ", " + std::to_string(p.y) +
                           ") goes to the game");
             }
             if (r.ticks != s->last_tick && el < 6.0) {
                 s->last_tick = r.ticks;
                 const Hero h = hero(cpu);
                 g_wpad.hold << (r.ticks - s->start_tick) << " " << h.x << "," << h.y << " s"
                             << h.state << " f" << h.flags << " hp" << h.hp << " mp" << h.mp
                             << '\n';
                 g_wpad.lines++;
                 g_wpad.min_x = std::min(g_wpad.min_x, h.x);
                 g_wpad.max_x = std::max(g_wpad.max_x, h.x);
                 g_wpad.walked = g_wpad.walked || h.state == 3;
                 g_wpad.attacked = g_wpad.attacked || h.state == 6 || h.state == 7;
             }
             if (r.widescreen != 0 && el >= 4.2 && el < 4.2 + 0.002) {
                 wide_shot(r, cpu, "pad_2_3_hold", true); // right and attack held
             }
             return el >= 6.5;
         });
}

void widescreen_pad(Run &r, u64) {
    r.wide_keypad = true;
    fe::install_keypad_layout(fe::KeypadLayout{});
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait(r, 2);
    then_do(r, "Henesys with the key pad", [&r](mt_cpu *cpu) {
        if (r.widescreen != 0) {
            check(mt_widescreen_view() == MT_WIDESCREEN_WIDE,
                  "Henesys (2.0) is drawn 568 points wide with the key pad shown");
            wide_shot(r, cpu, "pad_2_0", true);
        }
    });
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_wait_field(r);
    then_wait(r, 2);
    then_do(r, "2.3 with the key pad", [&r](mt_cpu *cpu) {
        check(hero(cpu).on(2, 3), "the hero is on the field 2.3");
        if (r.widescreen != 0) {
            check(mt_widescreen_view() == MT_WIDESCREEN_WIDE &&
                      fe::installed_keypad_wide().off == 44.0f && r.input.side_margin() == 44.0f,
                  "2.3 is drawn 568 points wide with the key pad shown, its groups 44 points out");
            wide_shot(r, cpu, "pad_2_3", true);
        }
    });
    then_pad_hold(r);
    then_finish(r);
}

int widescreen_pad_off_setup(Run &r) {
    g_wpad.hold.open(r.out / "hold.txt", std::ios::binary | std::ios::trunc);
    return g_wpad.hold ? 0 : 1;
}

int widescreen_pad_on_setup(Run &r) {
    const char *base = std::getenv("MT_WIDESCREEN_PAD_BASE");
    if (base == nullptr || !fs::is_regular_file(fs::path(base) / "hold.txt")) {
        std::printf("skipped: MT_WIDESCREEN_PAD_BASE names no widescreen_pad_off run "
                    "(play.widescreen_pad_off first)\n");
        return 77;
    }
    r.widescreen = MT_WIDESCREEN_WIDE;
    return widescreen_pad_off_setup(r);
}

void widescreen_pad_checks(Run &r, u64) {
    g_wpad.hold.close();
    const fe::KeypadLayoutStats st = fe::keypad_layout_stats();
    std::printf("%s: %llu hold lines, hero x %d..%d, the view %u; the mask hook %llu reads (%llu "
                "moved)\n",
                r.mode.c_str(), static_cast<unsigned long long>(g_wpad.lines), g_wpad.min_x,
                g_wpad.max_x, g_wpad.hold_view, static_cast<unsigned long long>(st.hits),
                static_cast<unsigned long long>(st.remapped));
    check(g_wpad.lines > 0 && g_wpad.max_x - g_wpad.min_x >= 50 && g_wpad.walked && g_wpad.attacked,
          "the held keys walked the hero (x " + std::to_string(g_wpad.min_x) + ".." +
              std::to_string(g_wpad.max_x) + ") and attacked");
    if (r.widescreen == 0) {
        check(st.hits == 0 && g_wpad.hold_view == 480u,
              "widescreen off: 480 points, the mask hook never ran");
        return;
    }
    check(g_wpad.hold_view == MT_WIDESCREEN_WIDE && st.hits > 0 && st.remapped > 0,
          "widescreen on: the hold at 568 points, the mask read through the hook");
    std::ifstream on(r.out / "hold.txt", std::ios::binary);
    std::ifstream off(fs::path(std::getenv("MT_WIDESCREEN_PAD_BASE")) / "hold.txt",
                      std::ios::binary);
    std::string a, b;
    u64 n = 0;
    bool same = true;
    std::string first;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(on, a));
        const bool gb = static_cast<bool>(std::getline(off, b));
        if (!ga && !gb) {
            break;
        }
        n++;
        if (ga != gb || a != b) {
            same = false;
            first = "line " + std::to_string(n) + ":\n  on:  " + (ga ? a : "(end)") +
                    "\n  off: " + (gb ? b : "(end)");
            break;
        }
    }
    check(same && n > 0, "the hero at every one of " + std::to_string(n) +
                             " ticks of the hold equals the off run's" +
                             (same ? "" : ": " + first));
}

// --- widescreen's wings through the present pass (#170) ------------------------------------------
//
// widescreen_filters: widescreen_pad's way into the field 2.3 with widescreen on and the key pad
// at the view's edges. There the real frame (the GL layer, the composited screen, the key pad's
// strips) goes through the real Presenter on an offscreen SDL GPU renderer at 1920 x 1080 with
// each filter (none, xbr, crt, lcd), the colour-blind filter off and on (deuteranopia). For each:
//   - check_readback covers the whole wide picture: 0 mismatches;
//   - the window is byte for byte the frame drawn as one picture: wings and screen stitched on the
//     CPU, the strips over them and the grade on the GPU as the Presenter does, then the same pass
//     over one 568-point image (no seam at the wings' edges);
//   - #86's way (the wings beside the image through SDL's shader, ungraded, the image alone
//     through the pass) is drawn too, and its difference from the one picture printed: the seam
//     #170 removes.
// The window shots go to OUT_DIR/filters_<filter>_<colour>.png (and _before.png for #86's way),
// local only, never committed. Needs a GPU (skipped, 77, without one).

struct FilterRun {
    int shots = 0, skipped = 0;
};
FilterRun g_filters;

struct Rgba {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
};

Rgba read_output(SDL_Renderer *ren) {
    Rgba o;
    SDL_Surface *read = SDL_RenderReadPixels(ren, nullptr);
    SDL_Surface *s = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    SDL_DestroySurface(read);
    if (s == nullptr) {
        return o;
    }
    o.w = s->w;
    o.h = s->h;
    o.px.resize(static_cast<std::size_t>(o.w) * o.h * 4);
    for (int y = 0; y < o.h; y++) {
        std::memcpy(&o.px[static_cast<std::size_t>(y) * o.w * 4],
                    static_cast<const std::uint8_t *>(s->pixels) +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(s->pitch),
                    static_cast<std::size_t>(o.w) * 4);
    }
    SDL_DestroySurface(s);
    return o;
}

void save_rgba(const Rgba &img, const fs::path &path) {
    SDL_Surface *s = SDL_CreateSurfaceFrom(img.w, img.h, SDL_PIXELFORMAT_ABGR8888,
                                           const_cast<std::uint8_t *>(img.px.data()), img.w * 4);
    if (s != nullptr) {
        SDL_SavePNG(s, path.string().c_str());
        SDL_DestroySurface(s);
    }
}

SDL_Texture *texture_of(SDL_Renderer *ren, const std::uint8_t *px, int w, int h, int stride,
                        SDL_BlendMode blend) {
    SDL_Texture *t =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t != nullptr) {
        SDL_UpdateTexture(t, nullptr, px, stride);
        SDL_SetTextureBlendMode(t, blend);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

// `src` (w x h) through the colour-blind pass into a new target of its size.
SDL_Texture *graded_copy(SDL_Renderer *ren, SDL_Texture *src, int w, int h,
                         fe::ColorblindFilter cb) {
    SDL_Texture *g =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
    fe::ColorblindPass pass;
    if (g == nullptr || !pass.open(ren)) {
        return g;
    }
    SDL_SetTextureBlendMode(g, SDL_BLENDMODE_NONE);
    SDL_Texture *before = SDL_GetRenderTarget(ren);
    pass.set_uniforms(cb, static_cast<u32>(w), static_cast<u32>(h));
    SDL_SetRenderTarget(ren, g);
    SDL_SetGPURenderState(ren, pass.state());
    SDL_RenderTexture(ren, src, nullptr, nullptr);
    SDL_SetGPURenderState(ren, nullptr);
    SDL_SetRenderTarget(ren, before);
    pass.close();
    return g;
}

// `src` (w x h texels) into `r` through the stage (with its sizes) or SDL's shader.
void draw_through(SDL_Renderer *ren, SDL_Texture *src, int w, int h, const fe::Rect &r,
                  const fe::PresentStage &st, SDL_ScaleMode mode) {
    SDL_SetTextureScaleMode(src, mode);
    const SDL_FRect dst{r.x, r.y, r.w, r.h};
    if (st.state != nullptr && st.sizes) {
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        const fe::PresentSizes sizes{{fw, fh, 1 / fw, 1 / fh},
                                     {dst.w, dst.h, 1 / dst.w, 1 / dst.h}};
        SDL_SetGPURenderStateFragmentUniforms(st.state, 0, &sizes, sizeof sizes);
    }
    if (st.state != nullptr) {
        SDL_SetGPURenderState(ren, st.state);
    }
    SDL_RenderTexture(ren, src, nullptr, &dst);
    SDL_SetGPURenderState(ren, nullptr);
}

void filter_shots(Run &r, mt_cpu *cpu) {
    fe::apply_keypad_layout();
    const double now = mt_cf_absolute_time();
    if (!r.screen.compose(cpu, now) || r.screen.gl_pixels().empty() ||
        r.screen.gl_height() != MT_WIDESCREEN_HEIGHT) {
        check(false, "the frame on 2.3 composes at render scale 1");
        return;
    }
    const std::vector<std::uint8_t> gl = r.screen.gl_pixels();
    const std::vector<std::uint8_t> screen = r.screen.pixels();
    const int gw = static_cast<int>(r.screen.gl_width());
    const u32 view = std::min(r.screen.gl_view(), r.screen.gl_width());
    check(view == MT_WIDESCREEN_WIDE, "2.3's GL frame is 568 points wide");
    constexpr u32 wing = (MT_WIDESCREEN_WIDE - MT_WIDESCREEN_NARROW) / 2;
    std::vector<std::uint8_t> strips;
    const bool pad = fe::keypad_wings_composite(cpu, now, wing, 1, &strips);
    check(pad, "the key pad's keys are drawn in the wings");
    SDL_GPUDevice *dev = mt_gfx_gpu_create_device();
    if (dev == nullptr) {
        std::printf("widescreen_filters: skipped: no GPU device (%s)\n", SDL_GetError());
        g_filters.skipped++;
        return;
    }
    SDL_Renderer *ren = SDL_CreateGPURenderer(dev, nullptr);
    constexpr int ow = 1920, oh = 1080;
    SDL_Texture *target = ren != nullptr ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                                             SDL_TEXTUREACCESS_TARGET, ow, oh)
                                         : nullptr;
    if (target == nullptr) {
        check(false, std::string("an offscreen SDL GPU renderer: ") + SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyGPUDevice(dev);
        return;
    }
    // The frame stitched on the CPU: the GL layer's first 568 points, the screen over the middle.
    constexpr int pw = MT_WIDESCREEN_WIDE, ph = MT_WIDESCREEN_HEIGHT;
    std::vector<std::uint8_t> whole(static_cast<std::size_t>(pw) * ph * 4);
    for (int y = 0; y < ph; y++) {
        std::memcpy(&whole[static_cast<std::size_t>(y) * pw * 4],
                    &gl[static_cast<std::size_t>(y) * gw * 4], static_cast<std::size_t>(pw) * 4);
        std::memcpy(&whole[(static_cast<std::size_t>(y) * pw + wing) * 4],
                    &screen[static_cast<std::size_t>(y) * 480 * 4], 480 * 4);
    }
    for (const fe::ShaderFilter f : {fe::ShaderFilter::kNone, fe::ShaderFilter::kXbr,
                                     fe::ShaderFilter::kCrt, fe::ShaderFilter::kLcd}) {
        for (const fe::ColorblindFilter cb :
             {fe::ColorblindFilter::kOff, fe::ColorblindFilter::kDeuteranopia}) {
            const std::string name = std::string("filters_") + fe::shader_filter_name(f) + "_" +
                                     fe::colorblind_filter_name(cb);
            fe::Presenter p;
            fe::PresentPass pass;
            SDL_SetRenderTarget(ren, target);
            check(p.start(ren, 480, 320), name + ": the Presenter starts");
            p.set_scaling(fe::ScaleMode::kInteger, fe::Color{});
            p.set_wide(MT_WIDESCREEN_WIDE);
            p.set_colorblind(cb);
            if (f != fe::ShaderFilter::kNone) {
                check(pass.open_filter(ren, f), name + ": the filter's pass opens");
                p.set_stage(pass.stage());
            }
            p.upload_wings(gl.data(), r.screen.gl_width(), r.screen.gl_height(), view);
            p.upload_pad_wings(strips.data(), 2 * wing, ph);
            p.upload(screen.data(), 480 * 4);
            check(p.draw() && p.picture_drawn(), name + ": the wide picture is composed");
            std::string report;
            const long bad = p.check_readback(screen.data(), 480 * 4, &report);
            std::printf("%s: read-back: %s\n", name.c_str(), report.c_str());
            check(bad == 0 &&
                      report.find("wings checked against the GL frame") != std::string::npos,
                  name + ": check_readback covers the whole wide picture, 0 mismatched");
            const Rgba shot = read_output(ren);
            save_rgba(shot, r.out / (name + ".png"));
            const fe::Rect rect = p.picture_rect();
            const fe::Filter pf = p.placement().filter;
            const SDL_ScaleMode sm = pf == fe::Filter::kLinear     ? SDL_SCALEMODE_LINEAR
                                     : pf == fe::Filter::kPixelArt ? SDL_SCALEMODE_PIXELART
                                                                   : SDL_SCALEMODE_NEAREST;
            const fe::PresentStage st = p.stage();
            SDL_Texture *strip_tex =
                texture_of(ren, strips.data(), 2 * wing, ph, static_cast<int>(2 * wing * 4),
                           SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            // The one picture.
            SDL_Texture *flat = texture_of(ren, whole.data(), pw, ph, pw * 4, SDL_BLENDMODE_NONE);
            SDL_Texture *one =
                SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, pw, ph);
            SDL_SetTextureBlendMode(one, SDL_BLENDMODE_NONE);
            SDL_SetRenderTarget(ren, one);
            SDL_RenderTexture(ren, flat, nullptr, nullptr);
            const SDL_FRect sl{0, 0, wing, ph}, sr{wing, 0, wing, ph};
            const SDL_FRect dl{0, 0, wing, ph}, dr{pw - wing, 0, wing, ph};
            SDL_RenderTexture(ren, strip_tex, &sl, &dl);
            SDL_RenderTexture(ren, strip_tex, &sr, &dr);
            SDL_Texture *one_graded =
                cb != fe::ColorblindFilter::kOff ? graded_copy(ren, one, pw, ph, cb) : nullptr;
            SDL_SetRenderTarget(ren, target);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            draw_through(ren, one_graded != nullptr ? one_graded : one, pw, ph, rect, st, sm);
            const Rgba ref = read_output(ren);
            // #86's way: the wings beside the image (SDL's shader, ungraded), the image alone
            // through the pass, the strips over the wings.
            SDL_Texture *gl_tex = texture_of(ren, gl.data(), gw, ph, gw * 4, SDL_BLENDMODE_NONE);
            SDL_Texture *scr =
                texture_of(ren, screen.data(), 480, 320, 480 * 4, SDL_BLENDMODE_NONE);
            SDL_Texture *scr_graded =
                cb != fe::ColorblindFilter::kOff ? graded_copy(ren, scr, 480, 320, cb) : nullptr;
            SDL_SetRenderTarget(ren, target);
            SDL_RenderClear(ren);
            SDL_SetTextureScaleMode(gl_tex, sm);
            const SDL_FRect gsrc{0, 0, static_cast<float>(view), ph};
            const SDL_FRect gdst{rect.x, rect.y, rect.w, rect.h};
            SDL_RenderTexture(ren, gl_tex, &gsrc, &gdst);
            draw_through(ren, scr_graded != nullptr ? scr_graded : scr, 480, 320,
                         p.placement().rect, st, sm);
            const fe::Rect &in = p.placement().rect;
            const SDL_FRect pl{rect.x, rect.y, in.x - rect.x, rect.h};
            const SDL_FRect pr{in.x + in.w, rect.y, rect.x + rect.w - in.x - in.w, rect.h};
            SDL_SetTextureScaleMode(strip_tex, sm);
            SDL_RenderTexture(ren, strip_tex, &sl, &pl);
            SDL_RenderTexture(ren, strip_tex, &sr, &pr);
            const Rgba before = read_output(ren);
            save_rgba(before, r.out / (name + "_before.png"));
            // Differences from the one picture: all pixels, and those within 6 output pixels of
            // the wings' edges (the seam).
            const auto diff = [&](const Rgba &a, long *seam, int *worst) {
                long n = 0;
                *seam = 0;
                *worst = 0;
                const int e0 = static_cast<int>(in.x), e1 = static_cast<int>(in.x + in.w);
                for (int y = 0; y < a.h && a.h == ref.h; y++) {
                    for (int x = 0; x < a.w; x++) {
                        const std::size_t i = (static_cast<std::size_t>(y) * a.w + x) * 4;
                        int d = 0;
                        for (int c = 0; c < 3; c++) {
                            d = std::max(d, std::abs(a.px[i + c] - ref.px[i + c]));
                        }
                        if (d != 0) {
                            n++;
                            *worst = std::max(*worst, d);
                            if (std::abs(x - e0) <= 6 || std::abs(x - e1) <= 6) {
                                (*seam)++;
                            }
                        }
                    }
                }
                return n;
            };
            long seam_now = 0, seam_before = 0;
            int worst_now = 0, worst_before = 0;
            const long now_n = diff(shot, &seam_now, &worst_now);
            const long before_n = diff(before, &seam_before, &worst_before);
            std::printf("%s: against the one picture: now %ld pixels differ (%ld at the wings' "
                        "edges, max %d); #86's way %ld (%ld at the edges, max %d)\n",
                        name.c_str(), now_n, seam_now, worst_now, before_n, seam_before,
                        worst_before);
            check(shot.w == ow && ref.w == ow && now_n == 0,
                  name + ": the window is the frame drawn as one picture, byte for byte");
            g_filters.shots++;
            for (SDL_Texture *t : {strip_tex, flat, one, one_graded, gl_tex, scr, scr_graded}) {
                if (t != nullptr) {
                    SDL_DestroyTexture(t);
                }
            }
            p.stop();
            pass.close();
        }
    }
    SDL_DestroyTexture(target);
    SDL_DestroyRenderer(ren);
    SDL_DestroyGPUDevice(dev);
}

void widescreen_filters(Run &r, u64) {
    r.wide_keypad = true;
    fe::install_keypad_layout(fe::KeypadLayout{});
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait(r, 2);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_wait_field(r);
    then_wait(r, 2);
    then_do(r, "2.3 through every filter", [&r](mt_cpu *cpu) {
        check(hero(cpu).on(2, 3) && mt_widescreen_view() == MT_WIDESCREEN_WIDE,
              "the hero is on 2.3, drawn 568 points wide");
        filter_shots(r, cpu);
    });
    then_finish(r);
}

int widescreen_filters_setup(Run &r) {
    r.widescreen = MT_WIDESCREEN_WIDE;
    return 0;
}

void widescreen_filters_checks(Run &, u64) {
    std::printf("widescreen_filters: %d window shots checked, %d skipped\n", g_filters.shots,
                g_filters.skipped);
    check(g_filters.skipped != 0 || g_filters.shots == 8,
          "every filter, the colour-blind filter off and on");
}

// --- Quality of life (#90, assists.h, ADR 0092) -------------------------------------------------
//
// auto_potion: a new game to the tutorial's first map (2.8, no mobs), the hero's HP set to a
// fifth of its max in guest memory (the test's only write). Off (the default): nothing drunk
// in 3 s. auto_potion = 50: quick slot 1's Red Potion is drunk once through its key, HP rises,
// and no second one follows above the threshold. With the popup menu open and HP low again,
// nothing; once it closes, the next potion.
//
// turbo_attack: the same map. Attack (X) held 3 s with turbo off is one attack (the game fires
// once per press); with it on, several, from the same hold. Let go, then a popup menu with X
// held (OK there): no repeat reaches it.
//
// ui_animations: ui_animations = instant from the boot: no UIKit animation runs on the title
// menu, the character select or after, and a new game reaches the intro as with normal.

// From a new game through the intro to the field of map 2.8 (MacArthur's first dialog done).
void then_intro_field(Run &r) {
    then_new_game(r);
    then_wait_for(r, "the intro (map 2.10)", 60, [](mt_cpu *cpu) { return hero(cpu).on(2, 10); });
    then(r, "the intro: A through its boxes", 90, [&r](mt_cpu *cpu, double el) {
        const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
        hold(r, A, box && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && hero(cpu).on(2, 8) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_wait_field(r);
}

void set_hero_hp(mt_cpu *cpu, i32 hp) {
    const Hero h = hero(cpu);
    if (h.addr != 0) {
        std::memcpy(cpu->mem + h.addr + 0x94, &hp, sizeof hp);
    }
}

// B until the field shows again (the popup menu closed).
void then_close_to_field(Run &r) {
    then(r, "B to the field", 10, [&r](mt_cpu *cpu, double el) {
        hold(r, B, !field(cpu) && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && el > 0.5;
    });
    then_wait_field(r);
}

struct PotionRun {
    i32 red = -1, low = 0, hp = 0;
    u64 potions = 0;
};

void auto_potion(Run &r, u64) {
    attach_pad(r);
    r.settings_from_env = false; // off and on compared here
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_intro_field(r);
    auto s = std::make_shared<PotionRun>();
    then_do(r, "the quick slots, HP to a fifth", [&r, s](mt_cpu *cpu) {
        const fe::AssistView v = fe::read_assist_view(fe::guest_reader(cpu));
        check(v.field && v.alive && v.potion_slot == 1 && v.potion_id == 3 && v.potion_hp == 50 &&
                  v.potion_count == 5,
              "the field: the HP potion is on quick slot 1 (slot " + std::to_string(v.potion_slot) +
                  ", item " + std::to_string(v.potion_id) + ", HP +" + std::to_string(v.potion_hp) +
                  ", " + std::to_string(v.potion_count) + " of them; Red Potion, +50, 5 wanted)");
        check(r.settings.auto_potion == fe::AutoPotion::kOff, "auto_potion is off by default");
        const Hero h = hero(cpu);
        s->red = item_count(cpu, h, 3);
        s->low = std::max(1, h.max_hp / 5);
        set_hero_hp(cpu, s->low);
        std::printf("auto_potion: HP %d of %d, %d Red Potions\n", s->low, h.max_hp, s->red);
    });
    // 1. Off: nothing.
    then_wait(r, 3);
    then_do(r, "auto_potion off", [&r, s](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(item_count(cpu, h, 3) == s->red && r.assists.stats().potions == 0 &&
                  h.hp < h.max_hp / 2,
              "auto_potion off: 3 s at HP " + std::to_string(h.hp) + " of " +
                  std::to_string(h.max_hp) + ", no potion drunk (" +
                  std::to_string(item_count(cpu, h, 3)) + " Red Potions)");
        r.settings.auto_potion = fe::AutoPotion::k50;
    });
    // 2. On at 50 %: one Red Potion through quick slot 1.
    then_wait_for(r, "auto_potion = 50: a Red Potion drunk", 5,
                  [s](mt_cpu *cpu) { return item_count(cpu, hero(cpu), 3) == s->red - 1; });
    then_wait(r, 2);
    then_do(r, "auto_potion on", [&r, s](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(
            r.assists.stats().potions == 1 && r.assists.stats().last_slot == 1 &&
                item_count(cpu, h, 3) == s->red - 1 && h.hp >= std::min(h.max_hp, s->low + 50) - 2,
            "auto_potion = 50: one Red Potion (quick slot 1) drunk, HP " + std::to_string(s->low) +
                " -> " + std::to_string(h.hp) + " of " + std::to_string(h.max_hp) +
                ", none after it (" + std::to_string(r.assists.stats().potions) + " presses)");
        s->red = item_count(cpu, h, 3);
    });
    // 3. The popup menu open: nothing until it closes.
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_do(r, "HP low in the popup menu", [s](mt_cpu *cpu) { set_hero_hp(cpu, s->low); });
    then_wait(r, 2);
    then_do(r, "auto_potion in the popup menu", [&r, s](mt_cpu *cpu) {
        check(item_count(cpu, hero(cpu), 3) == s->red && r.assists.stats().potions == 1,
              "auto_potion: nothing drunk while the popup menu is open (" + where(cpu) + ")");
    });
    then_close_to_field(r);
    then_wait_for(r, "the field again: the next Red Potion", 5,
                  [s](mt_cpu *cpu) { return item_count(cpu, hero(cpu), 3) == s->red - 1; });
    then_check(r, "back in the field: the second potion drunk",
               [&r](mt_cpu *) { return r.assists.stats().potions == 2; });
    then_finish(r);
}

void auto_potion_checks(Run &, u64) {}

// Attacks: the hero's state going into 6 or 7 (an attack) since `from`, counted by the monitor.
struct AttackRun {
    i32 last = -1;
    u64 attacks = 0, attack_ticks = 0;
};
std::shared_ptr<AttackRun> g_attack;

// X held `seconds` from the field, standing; the attacks it gave into *out.
void then_hold_attack(Run &r, const std::string &what, double seconds, u64 *out) {
    then_do(r, what + ": X down", [&r](mt_cpu *) {
        g_attack->attacks = 0;
        g_attack->attack_ticks = 0;
        hold(r, X, true);
    });
    then_wait(r, seconds);
    then_do(r, what + ": X up", [&r, out, what](mt_cpu *) {
        hold(r, X, false);
        *out = g_attack->attacks;
        std::printf("turbo_attack: %s: %llu attacks, %llu ticks attacking\n", what.c_str(),
                    static_cast<unsigned long long>(g_attack->attacks),
                    static_cast<unsigned long long>(g_attack->attack_ticks));
    });
    then_wait(r, 1.5);
}

struct TurboRun {
    u64 off = 0, on = 0;
    u64 turbo0 = 0;
};

void turbo_attack(Run &r, u64) {
    attach_pad(r);
    r.settings_from_env = false; // off and on compared here
    g_attack = std::make_shared<AttackRun>();
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        const Hero h = hero(cpu);
        const bool attacking = h.state == 6 || h.state == 7;
        if (h.state != g_attack->last) {
            g_attack->attacks += attacking ? 1 : 0;
            g_attack->last = h.state;
        }
        static std::int64_t frame = 0;
        if (attacking && r.last_frame != frame) {
            g_attack->attack_ticks++;
        }
        frame = r.last_frame;
    };
    then_intro_field(r);
    auto s = std::make_shared<TurboRun>();
    then_check(r, "turbo_attack is off by default",
               [&r](mt_cpu *) { return !r.settings.turbo_attack; });
    // 1. Off: one attack per press.
    then_hold_attack(r, "turbo off", 3.0, &s->off);
    then_do(r, "turbo off", [&r, s](mt_cpu *) {
        check(s->off == 1 && r.assists.stats().turbo_presses == 0,
              "turbo_attack off: X held 3 s attacks " + std::to_string(s->off) +
                  " time(s) (1 wanted: the game fires once per press)");
        r.settings.turbo_attack = true;
    });
    // 2. On: the same hold attacks again and again.
    then_hold_attack(r, "turbo on", 3.0, &s->on);
    then_do(r, "turbo on", [&r, s](mt_cpu *) {
        check(s->on >= 4 && r.assists.stats().turbo_presses >= 4,
              "turbo_attack on: X held 3 s attacks " + std::to_string(s->on) + " times (" +
                  std::to_string(r.assists.stats().turbo_presses) + " repeats; at least 4 wanted)");
        check(mt_input_held(MT_ACTION_ATTACK) == 0, "X let go: attack is not held");
        s->turbo0 = r.assists.stats().turbo_presses;
    });
    // 3. The popup menu with X held: attack is OK there; turbo repeats nothing.
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_do(r, "X down in the popup menu", [&r](mt_cpu *) { hold(r, X, true); });
    then_wait(r, 2);
    then_do(r, "X up in the popup menu", [&r, s](mt_cpu *cpu) {
        hold(r, X, false);
        check(r.assists.stats().turbo_presses == s->turbo0,
              "turbo_attack: no repeat with X held in the popup menu (" + where(cpu) + ")");
    });
    then_close_to_field(r);
    then_finish(r);
}

void turbo_attack_checks(Run &, u64) {}

struct AnimRun {
    fe::UiAnimations want = fe::UiAnimations::kNormal;
    u32 max_menu = 0, max_game = 0;
    double last_menu = -1; // the last time an animation ran before the game
    u64 turns = 0;
};
AnimRun g_anim;

// ui_animations_<normal|fast|instant>: the setting from the boot; the UIKit animations seen
// running on the title menu and the character select (no key pad panel yet) and in the game,
// and when the last one before the game ran.
void ui_animations(Run &r, fe::UiAnimations want) {
    attach_pad(r);
    r.settings_from_env = false; // off and on compared here
    g_anim = AnimRun{};
    g_anim.want = want;
    r.settings.ui_animations = want;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        const u32 n = mt_uikit_animation_count(mt_cf_absolute_time());
        if (mt_input_panel() < 0) {
            g_anim.max_menu = std::max(g_anim.max_menu, n);
            g_anim.last_menu = n > 0 ? r.t : g_anim.last_menu;
        } else {
            g_anim.max_game = std::max(g_anim.max_game, n);
        }
        g_anim.turns++;
    };
    then_check(
        r,
        std::string("the runtime's animation scale is ui_animations = ") +
            fe::ui_animations_name(want) + "'s",
        [want](mt_cpu *) { return mt_uikit_animation_scale() == fe::ui_animation_scale(want); });
    then_intro_field(r);
    then_finish(r);
}
void ui_animations_normal(Run &r, u64) { ui_animations(r, fe::UiAnimations::kNormal); }
void ui_animations_fast(Run &r, u64) { ui_animations(r, fe::UiAnimations::kFast); }
void ui_animations_instant(Run &r, u64) { ui_animations(r, fe::UiAnimations::kInstant); }

void ui_animations_checks(Run &, u64) {
    const AnimRun &a = g_anim;
    std::printf("ui_animations = %s: at most %u animations running before the game (the last "
                "at %.2f s), %u in the game, over %llu turns\n",
                fe::ui_animations_name(a.want), a.max_menu, a.last_menu, a.max_game,
                static_cast<unsigned long long>(a.turns));
    if (a.want == fe::UiAnimations::kInstant) {
        check(a.max_menu == 0 && a.max_game == 0,
              "ui_animations = instant: no UIKit animation ran (" + std::to_string(a.max_menu) +
                  " before the game, " + std::to_string(a.max_game) + " in it)");
    } else {
        check(a.max_menu > 0, std::string("ui_animations = ") + fe::ui_animations_name(a.want) +
                                  ": the title menu animates (" + std::to_string(a.max_menu) +
                                  " at once)");
    }
    check(a.turns > 0, "a new game reached the field of map 2.8 (then_intro_field)");
}

// --- Fast text (#173, docs/re/qol.md "Fast text") ----------------------------------------------
//
// chat_text: a new game through the intro (map 2.10: the scene's speech boxes, then MacArthur's
// first dialog on map 2.8), A pressed as soon as each box shows and the game takes keys. Read
// every tick: the NPC dialogue box (CStateGame +0x5C, a CChatBox: page +0x10, the page's
// wrapped lines +0x18 and their count +0x1C, set whole by CChatBox::FirstPage 0x34CD1 and
// CChatBox::NextPage 0x34A05) and the scene's speech box (CStateGame +0x1E9 on, its lines
// +0x4D0, the first shown +0x4C8, the count +0x4D4). CStateGame::OnKeyPressed 0x8D4C9 drops
// every key in game modes 6-8 (loading) and while the screen fades (CStateGame +0xB4, 4 Paint
// frames, CStateGame::Paint 0x87501); a dialog that opens as a map arrives shows during that
// fade-in, so its first press waits for it. Checks: each box's text is whole on the tick it
// shows and never changes while it shows (no letter-by-letter reveal), each A is taken at once
// (the next box within the input's latency), and the only wait before a press is the fade.

std::string guest_cstr(const mt_cpu *cpu, u32 addr, std::size_t max = 512) {
    std::string s;
    while (addr != 0 && addr < cpu->mem_size && s.size() < max && cpu->mem[addr] != 0) {
        s.push_back(static_cast<char>(cpu->mem[addr++]));
    }
    return s;
}

struct ChatBoxView {
    std::string kind; // "npc", "speech", or empty: no box
    i32 page = -1;    // the NPC page, or the speech box's first line
    i32 lines = 0;    // the lines shown
    std::string text; // the shown lines, each ended by '|'
    bool same_box(const ChatBoxView &o) const { return kind == o.kind && page == o.page; }
};

ChatBoxView chat_box_view(const mt_cpu *cpu) {
    ChatBoxView v;
    const u32 state = hero(cpu).addr != 0 ? game_state(cpu) : 0;
    if (state == 0) {
        return v;
    }
    if (game_mode(cpu) == 4) {
        const u32 chat = rd(cpu, state + 0x5C);
        if (chat == 0) {
            return v;
        }
        v.kind = "npc";
        v.page = rdi(cpu, chat + 0x10);
        const u32 lines = rd(cpu, chat + 0x18);
        v.lines = std::clamp(rdi(cpu, chat + 0x1C), 0, 64);
        for (i32 i = 0; lines != 0 && i < v.lines; i++) {
            v.text += guest_cstr(cpu, rd(cpu, lines + 4 * static_cast<u32>(i))) + "|";
        }
    } else if (state + 0x1E9 < cpu->mem_size && cpu->mem[state + 0x1E9] != 0) {
        v.kind = "speech";
        v.page = rdi(cpu, state + 0x4C8);
        const i32 count = std::clamp(rdi(cpu, state + 0x4D4), 0, 1000);
        const u32 lines = rd(cpu, state + 0x4D0);
        for (i32 i = v.page; lines != 0 && i >= 0 && i < count && i < v.page + 2; i++) {
            v.text += guest_cstr(cpu, rd(cpu, lines + 4 * static_cast<u32>(i))) + "|";
            v.lines++;
        }
    }
    return v;
}

// Whether CStateGame::OnKeyPressed takes a key now: not loading (game modes 6-8), no fade
// (CStateGame +0xB4).
bool keys_taken(const mt_cpu *cpu) {
    const u32 state = game_state(cpu);
    const i32 mode = game_mode(cpu);
    return state != 0 && state + 0xB4 < cpu->mem_size && cpu->mem[state + 0xB4] == 0 &&
           (mode < 6 || mode > 8);
}

struct ChatRun {
    ChatBoxView box;
    u64 tick = 0, shown_at = 0, press_at = 0;
    bool want_press = false, pressed_this_box = false, down = false;
    u64 npc = 0, speech = 0, empty = 0, changed = 0, refused = 0, fallback = 0;
    std::map<u64, u64> from_press, fade_wait; // ticks -> boxes
    double no_box = -1;
};
ChatRun g_chat;

void chat_text(Run &r, u64) {
    attach_pad(r);
    g_chat = ChatRun{};
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_wait_for(r, "the intro (map 2.10)", 60, [](mt_cpu *cpu) { return hero(cpu).on(2, 10); });
    then(r, "the intro: A on each box as soon as it shows", 90, [&r](mt_cpu *cpu, double el) {
        ChatRun &s = g_chat;
        if (r.ticks == s.tick) {
            return false;
        }
        s.tick = r.ticks;
        const ChatBoxView v = chat_box_view(cpu);
        if (!v.same_box(s.box)) {
            if (!s.box.kind.empty() && s.pressed_this_box) {
                s.from_press[s.tick - s.press_at]++;
            }
            s.box = v;
            s.pressed_this_box = false;
            if (!v.kind.empty()) {
                (v.kind == "npc" ? s.npc : s.speech)++;
                s.shown_at = s.tick;
                s.want_press = true;
                bool blank = v.lines == 0;
                for (std::size_t a = 0, b = 0; (b = v.text.find('|', a)) != std::string::npos;
                     a = b + 1) {
                    blank = blank || b == a;
                }
                s.empty += blank ? 1 : 0;
                std::printf("chat_text: tick %llu: %s box %d shown, %d whole lines (%zu chars)\n",
                            static_cast<unsigned long long>(s.tick), v.kind.c_str(), v.page,
                            v.lines, v.text.size() - static_cast<std::size_t>(v.lines));
            }
        } else if (!v.kind.empty() && v.text != s.box.text) {
            s.changed++;
            std::printf("chat_text: tick %llu: %s box %d's text changed while shown\n",
                        static_cast<unsigned long long>(s.tick), v.kind.c_str(), v.page);
            s.box = v;
        }
        // A: down for one tick, up for at least one, so each box gets its own press.
        if (s.down) {
            hold(r, A, false);
            s.down = false;
        } else if (s.want_press && keys_taken(cpu)) {
            hold(r, A, true);
            s.down = true;
            s.want_press = false;
            if (!v.kind.empty() && !s.pressed_this_box) {
                s.fade_wait[s.tick - s.shown_at]++;
            }
            s.pressed_this_box = !v.kind.empty();
            s.press_at = s.tick;
            std::printf("chat_text: tick %llu: A down\n", static_cast<unsigned long long>(s.tick));
        } else if (!v.kind.empty() && s.pressed_this_box && s.tick - s.press_at > 40) {
            s.refused++; // OK not taken within 2 s: again
            s.want_press = true;
        }
        // Between boxes (the scene moving its actors) A does nothing; a panel waiting with no
        // box gets A once a second, as then_intro_field does.
        const bool panel = mt_input_panel() == 3 || mt_input_panel() == 5;
        if (v.kind.empty() && panel) {
            s.no_box = s.no_box < 0 ? el : s.no_box;
            if (el - s.no_box > 2.0 && !s.down && !s.want_press) {
                s.fallback++;
                s.want_press = true;
                s.no_box = el;
            }
        } else {
            s.no_box = -1;
        }
        return field(cpu) && hero(cpu).on(2, 8) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
    });
    then_wait_field(r);
    then_finish(r);
}

void chat_text_checks(Run &, u64) {
    const ChatRun &s = g_chat;
    const auto hist = [](const std::map<u64, u64> &m) {
        std::string out;
        u64 max = 0;
        for (const auto &[ticks, n] : m) {
            out += " " + std::to_string(ticks) + " tick(s) x" + std::to_string(n);
            max = std::max(max, ticks);
        }
        return std::pair{out, max};
    };
    const auto [press_text, press_max] = hist(s.from_press);
    const auto [fade_text, fade_max] = hist(s.fade_wait);
    std::printf("chat_text: %llu NPC pages, %llu speech boxes; shown to A (the fade):%s; A to "
                "the next box:%s; %llu presses with no box\n",
                static_cast<unsigned long long>(s.npc), static_cast<unsigned long long>(s.speech),
                fade_text.c_str(), press_text.c_str(), static_cast<unsigned long long>(s.fallback));
    check(s.npc >= 2 && s.speech >= 1, "the intro shows NPC dialogue pages and speech boxes (" +
                                           std::to_string(s.npc) + " pages, " +
                                           std::to_string(s.speech) + " speech boxes)");
    check(s.empty == 0, "every box has its whole text on the tick it shows (" +
                            std::to_string(s.empty) + " with an empty line)");
    check(s.changed == 0, "no box's text changes while it shows: no letter-by-letter reveal (" +
                              std::to_string(s.changed) + " changes)");
    check(s.refused == 0 && press_max <= 2,
          "each A is taken at once: the next box within " + std::to_string(press_max) +
              " tick(s) of the press (2 wanted: the input's own latency), " +
              std::to_string(s.refused) + " presses not taken");
    check(fade_max <= 5, "the only wait before a box takes A is the map's fade-in: at most " +
                             std::to_string(fade_max) + " tick(s) (5 wanted)");
}

// --- Text packs (#189, ADR 0108, maplethief/textpack.h) ----------------------------------------
//
// text_pack: a pack made at run time from your own copy, in OUT_DIR/pack (never committed):
// every Scripts.strings value with "Henesys" as "Hénésys" and "Potion" as "Pötion", each also
// starting "Hénésys " so the intro's first box shows it; and every item_etc name with "Potion"
// as "Pötion" (Tables.strings). It opens before the boot. A new game through the intro and
// the tutorial, then the popup menu's item list with the tutorial's potions and a touch on the
// first, whose page shows its name, with a shot (OUT_DIR/text_pack_items.png). Checks: every NPC
// and speech box line is whole UTF-8 (Text_Wrap's steps never split "é"), the boxes show "Hénésys",
// the game draws "Hénésys" and "Pötion" (the draw observer: what reaches
// -[NSString drawAtPoint:withFont:]) and never a Hangul syllable (UTF-8 read as EUC-KR).

struct TextPackRun {
    int entries = -1;
    u64 boxes = 0, lines = 0, broken = 0, with_name = 0, tick = ~0ull;
    std::string last_text;
    std::set<std::string> drawn;
    u64 drawn_name = 0, drawn_potion = 0, hangul = 0, potion_before = 0, potion_list = 0;
};
TextPackRun g_tp;

std::string utf8_of(const std::u16string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Whole UTF-8 (no lead without its continuations, no stray continuation); counts Hangul
// syllables (U+AC00..U+D7A3) in *hangul.
bool whole_utf8(const std::string &s, u64 *hangul = nullptr) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80                ? 1
                              : c >= 0xC2 && c < 0xE0 ? 2
                              : c >= 0xE0 && c < 0xF0 ? 3
                              : c >= 0xF0 && c < 0xF5 ? 4
                                                      : 0;
        if (n == 0 || i + n > s.size()) {
            return false;
        }
        u32 cp = n == 1 ? c : c & (0x7F >> n);
        for (std::size_t k = 1; k < n; k++) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (hangul != nullptr && cp >= 0xAC00 && cp <= 0xD7A3) {
            (*hangul)++;
        }
        i += n;
    }
    return true;
}

std::string replace_all(std::string s, const std::string &from, const std::string &to) {
    for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) {
        s.replace(at, from.size(), to);
    }
    return s;
}

std::string strings_quote(const std::string &s) {
    std::string out = "\"";
    for (const char c : s) {
        out += c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : std::string(1, c);
    }
    return out + "\"";
}

const std::string k_name_utf8 = "H\xC3\xA9n\xC3\xA9sys"; // Hénésys
const std::string k_potion_utf8 = "P\xC3\xB6tion";       // Pötion

int text_pack_setup(Run &r) {
    const fs::path bundle = MT_BUNDLE_DIR;
    const fs::path pack = r.out / "pack";
    fs::create_directories(pack);
    // Scripts.strings: the bundle's table (a binary plist) with the names changed.
    const std::vector<std::uint8_t> raw = read_file((bundle / "en.lproj/Scripts.strings").string());
    mt_ns::PlistValue table;
    std::string why;
    if (!mt_ns::plist_parse(raw.data(), raw.size(), &table, &why) ||
        table.kind != mt_ns::PlistValue::dict) {
        std::printf("FAIL: Scripts.strings: %s\n", why.c_str());
        return 1;
    }
    std::ofstream scripts(pack / "Scripts.strings", std::ios::binary);
    u64 named = 0;
    for (const auto &[key, value] : table.entries) {
        if (value.kind != mt_ns::PlistValue::string) {
            continue;
        }
        std::string v = utf8_of(value.s);
        named += v.find("Henesys") != std::string::npos ? 1 : 0;
        v = replace_all(replace_all(v, "Henesys", k_name_utf8), "Potion", k_potion_utf8);
        scripts << strings_quote(utf8_of(key)) << " = " << strings_quote(k_name_utf8 + " " + v)
                << ";\n";
    }
    scripts.close();
    // Tables.strings: item_etc's names (64 bytes at offset 1 of each 218-byte row).
    const std::vector<std::uint8_t> items =
        read_file((bundle / "English/data/item_etc.dat").string());
    std::ofstream tables(pack / "Tables.strings", std::ios::binary);
    u64 potions = 0;
    for (std::size_t row = 0; items.size() >= 6 && 6 + (row + 1) * 218 <= items.size(); row++) {
        const char *name = reinterpret_cast<const char *>(items.data() + 6 + row * 218 + 1);
        const std::string n(name, strnlen(name, 64));
        if (n.find("Potion") != std::string::npos) {
            tables << strings_quote("item_etc." + std::to_string(row) + ".name") << " = "
                   << strings_quote(replace_all(n, "Potion", k_potion_utf8)) << ";\n";
            potions++;
        }
    }
    tables.close();
    std::ofstream(pack / "textpack.strings", std::ios::binary)
        << "\"format\" = \"1\";\n\"name\" = \"play.text_pack\";\n\"language\" = \"en-x-test\";\n";
    g_tp = TextPackRun{};
    g_tp.entries = mt_textpack_open(pack.string().c_str());
    std::printf("text_pack: %s: %d entries (%llu values naming Henesys, %llu potions)\n",
                pack.string().c_str(), g_tp.entries, static_cast<unsigned long long>(named),
                static_cast<unsigned long long>(potions));
    check(g_tp.entries > 1000 && potions > 0, "the pack opens with the scripts and the potions");
    mt_textpack_set_draw_observer(
        [](const char *utf8, void *) {
            TextPackRun &s = g_tp;
            const std::string t = utf8;
            if (s.drawn.insert(t).second) {
                whole_utf8(t, &s.hangul);
                s.drawn_name += t.find(k_name_utf8) != std::string::npos ? 1 : 0;
                s.drawn_potion += t.find(k_potion_utf8) != std::string::npos ? 1 : 0;
            }
        },
        nullptr);
    return 0;
}

void text_pack_turn(mt_cpu *cpu) {
    TextPackRun &s = g_tp;
    const ChatBoxView v = chat_box_view(cpu);
    if (v.kind.empty() || v.text == s.last_text) {
        return;
    }
    s.last_text = v.text;
    s.boxes++;
    s.with_name += v.text.find(k_name_utf8) != std::string::npos ? 1 : 0;
    for (std::size_t a = 0, b = 0; (b = v.text.find('|', a)) != std::string::npos; a = b + 1) {
        const std::string line = v.text.substr(a, b - a);
        s.lines++;
        if (!whole_utf8(line)) {
            s.broken++;
            std::printf("text_pack: a %s box line is not whole UTF-8: \"%s\"\n", v.kind.c_str(),
                        line.c_str());
        }
    }
    if (s.boxes <= 3) {
        std::printf("text_pack: %s box %d: %s\n", v.kind.c_str(), v.page, v.text.c_str());
    }
}

void text_pack_play(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        text_pack_turn(cpu);
    };
    // The intro and the tutorial (MacArthur's and Stan's dialogs), which leaves Red and Blue
    // Potions in the inventory.
    then_new_game(r);
    then_tutorial(r);
    // The popup menu (START) on its first tab, then RIGHT to the item tab (1).
    then_press(r, START);
    then_wait_for(
        r, "the popup menu", 5,
        [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; }, 0.5);
    then(r, "RIGHT to the item tab", 10, [&r](mt_cpu *cpu, double el) {
        hold(r, RIGHT, menu_tab(cpu) != 1 && std::fmod(el, 1.0) < 0.1);
        return menu_tab(cpu) == 1 && el > 1.0;
    });
    // DOWN opens the tab's list (popup_menu_down): its cells draw the items' names.
    then_do(r, "the potions drawn before the list",
            [](mt_cpu *) { g_tp.potion_before = g_tp.drawn_potion; });
    then_press(r, DOWN, 0.1, 0.8);
    then_do(r, "the item list", [](mt_cpu *cpu) {
        check(game_mode(cpu) == 1 && menu_tab(cpu) == 1 && menu_sub(cpu) == 1,
              "the item list is open");
    });
    // A touch on its first cell (the Red Potions, (118, 80)): the item's page, with its name.
    then_touch(r, "a touch on the first item", 118, 80);
    then_wait_for(
        r, "the item's page", 2, [](mt_cpu *) { return g_tp.drawn_potion > g_tp.potion_before; },
        0.5);
    then_do(r, "the item's page, a shot", [&r](mt_cpu *cpu) {
        check(save_shot(r, cpu, "text_pack_items"), "the item's page shot");
        g_tp.potion_list = g_tp.drawn_potion - g_tp.potion_before;
    });
    then_close_to_field(r);
    then_finish(r);
}

void text_pack_checks(Run &, u64) {
    const TextPackRun &s = g_tp;
    const mt_textpack_stats st = mt_textpack_get_stats();
    mt_textpack_set_draw_observer(nullptr, nullptr);
    mt_textpack_close();
    for (const std::string &t : s.drawn) {
        if (!whole_utf8(t) || t.find_first_of("\x80\xC3") != std::string::npos) {
            std::printf("text_pack: drawn: \"%s\"\n", replace_all(t, "\n", "\\n").c_str());
        }
    }
    std::printf("text_pack: %llu boxes, %llu lines, %llu with Hénésys; %zu texts drawn, %llu with "
                "Hénésys, %llu with Pötion; %llu lookups, %llu replaced, %u table fields, %u "
                "overflows\n",
                static_cast<unsigned long long>(s.boxes), static_cast<unsigned long long>(s.lines),
                static_cast<unsigned long long>(s.with_name), s.drawn.size(),
                static_cast<unsigned long long>(s.drawn_name),
                static_cast<unsigned long long>(s.drawn_potion),
                static_cast<unsigned long long>(st.lookups),
                static_cast<unsigned long long>(st.replaced), st.fields, st.overflows);
    check(s.boxes >= 3 && s.with_name > 0, "the intro's boxes show the pack's Hénésys (" +
                                               std::to_string(s.with_name) + " of " +
                                               std::to_string(s.boxes) + ")");
    check(s.broken == 0, "every box line is whole UTF-8: Text_Wrap never splits a character (" +
                             std::to_string(s.broken) + " of " + std::to_string(s.lines) +
                             " broken)");
    check(st.fields > 0 && st.replaced > 0, "the pack's table names and script text were used");
    check(s.drawn_name > 0, "the game draws Hénésys");
    check(s.potion_list > 0, "the item list draws Pötion (" + std::to_string(s.potion_list) +
                                 " names on the touched item's page)");
    check(s.hangul == 0, "no drawn text has a Hangul syllable: UTF-8 is never read as EUC-KR (" +
                             std::to_string(s.hangul) + ")");
}

// --- skip_intros (#174, scene_skip.h, ADR 0096, docs/re/qol.md#cutscene-skip) -------------------
//
// A scene watched against the same scene skipped, from the same start: the guest state the scene
// leaves behind is written by both and compared line by line.
//   skip_intros_watched       a new game; the intro (CAniManager::makeAni1, map 2.10) watched,
//                             A through its speech boxes as a player taps them (once a second)
//   skip_intros_skipped       the same with skip_intros = on: no input from the scene's start
//                             to its end; the skip taps the boxes (and runs x8 in real time)
//   skip_intros_boss_watched  slot 0 of play.save_quit's save moved to Balog's map 10.5
//                             (MT_AUTOSAVE_MAP=10.5, as autosave_map): Balog's entrance
//                             (makeAniBalogIn, scene 14) watched
//   skip_intros_boss_skipped  the same skipped
// The state is taken at the first turn after the scene: the intro's warp has loaded map 2.8 (game
// mode out of 8 to 10), Balog's cmd 58 has given the field back on 10.5 (game mode 0). The
// skipped modes compare with the watched run's file (MT_SKIP_WATCHED: its OUT_DIR). "info."
// lines are printed, not compared: the ticks the scene took and the game's rand() state, which
// depend on how long each box waited for its tap in the original too.
struct SceneRun {
    bool skip = false, boss = false;
    bool seen = false;    // the scene's game mode 9 seen
    i32 max_step = -1;    // the scene list's step counter, highest seen
    u64 scene_ticks = 0;  // game ticks in game mode 9
    u64 test_presses = 0; // A presses the flow made during the scene
    u64 last_ticks = 0;
    bool a_down = false;
    std::vector<std::pair<std::string, std::string>> state;
} g_scene;

// The guest state a scene can change (docs/re/qol.md#cutscene-skip: its state commands): the
// map, the hero (place, facing, speed, motion, face, stats, items), the quests, the map's mobs
// (the boss's state: cmd 58's 0x7579D, cmd 60's 0x83BE1), the NPCs (cmds 12-17, 59), what the
// scene's end left in CStateGame.
std::vector<std::pair<std::string, std::string>> scene_state(mt_cpu *cpu) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add = [&out](std::string k, const std::string &v) { out.emplace_back(std::move(k), v); };
    auto num = [](std::int64_t v) { return std::to_string(v); };
    const Hero h = hero(cpu);
    const u32 st = game_state(cpu);
    add("game_mode", num(game_mode(cpu)));
    add("popup", num(popup_id(cpu)));
    add("map", num(h.area) + "." + num(h.map));
    add("chapter", num(h.chapter));
    for (const auto &[name, off] :
         std::initializer_list<std::pair<const char *, u32>>{{"x", 0x04},
                                                             {"y", 0x08},
                                                             {"flags", 0x0C},
                                                             {"action", 0x10},
                                                             {"facing", 0x14},
                                                             {"walk_dx", 0x18},
                                                             {"walk_dy", 0x1C},
                                                             {"speed", 0x20},
                                                             {"vx", 0x74},
                                                             {"vy", 0x78},
                                                             {"level", 0x80},
                                                             {"exp", 0x90},
                                                             {"hp", 0x94},
                                                             {"mp", 0x98},
                                                             {"max_hp", 0x9C},
                                                             {"max_mp", 0xA0},
                                                             {"meso", 0xD0},
                                                             {"face", 0x14C}}) {
        add(std::string("hero.") + name, num(rdi(cpu, h.addr + off)));
    }
    std::string slots;
    for (u32 i = 0; i < 5; i++) {
        slots +=
            num(rdi(cpu, h.addr + 0xE0 + 8 * i)) + ":" + num(rdi(cpu, h.addr + 0xE4 + 8 * i)) + " ";
    }
    add("hero.quick_slots", slots);
    std::string items;
    const u32 inv = h.addr != 0 ? rd(cpu, h.addr + 0xD8) : 0;
    for (u32 i = 0; inv != 0 && i < 48; i++) {
        const u32 item = rd(cpu, inv + 4 * i);
        if (item != 0) {
            items += num(rdi(cpu, item + 0x90)) + "x" + num(rd16(cpu, item + 0x100)) + " ";
        }
    }
    add("hero.items", items);
    for (u32 sort = 0; sort < 2; sort++) {
        std::string q;
        for (u32 n = 0; n < 100; n++) {
            if (const i32 v = quest_state(cpu, h, sort, n); v != 0) {
                q += num(n) + "=" + num(v) + " ";
            }
        }
        add(sort == 0 ? "quests.main" : "quests.side", q);
    }
    std::string active;
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        active += num(a.sort) + "." + num(a.num) + "[";
        for (const Requirement &q : a.reqs) {
            active += num(q.kind) + ":" + num(q.target) + ":" + num(q.progress) + " ";
        }
        active += "] ";
    }
    add("quests.active", active);
    std::string m;
    for (const Mob &mob : mobs(cpu)) {
        m += num(mob.id) + "@" + num(mob.x) + "," + num(mob.y) + " hp " + num(mob.hp) + " s" +
             num(rdi(cpu, mob.addr + 0x10)) + " f" + num(rdi(cpu, mob.addr + 0xF0)) + " t" +
             num(rdi(cpu, mob.addr + 0xF8)) + "; ";
    }
    add("mobs", m);
    if (st != 0) {
        add("state.scene_list", num(rd(cpu, st + 0x38)));
        add("state.next_scene", num(rdi(cpu, st + 0x1E4)));
        add("state.warp", num(rdi(cpu, st + 0x98)) + "." + num(rdi(cpu, st + 0x9C)));
        add("state.flags_24_1e8_3fc", num(cpu->mem[st + 0x24]) + " " + num(cpu->mem[st + 0x1E8]) +
                                          " " + num(cpu->mem[st + 0x3FC]));
    }
    add("info.rand", num(rd(cpu, 0x11A510)));
    add("info.scene_ticks", num(static_cast<std::int64_t>(g_scene.scene_ticks)));
    return out;
}

void skip_scene(Run &r, bool skip, bool boss) {
    attach_pad(r);
    r.settings_from_env = false; // watched and skipped compared here
    r.settings.skip_intros = skip;
    g_scene = SceneRun{};
    g_scene.skip = skip;
    g_scene.boss = boss;
    g_play.anywhere = boss;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        const fe::SceneView v = fe::read_scene_view(fe::guest_reader(cpu));
        if (v.scene) {
            g_scene.seen = true;
            g_scene.max_step = std::max(g_scene.max_step, v.step);
            g_scene.scene_ticks += r.ticks - g_scene.last_ticks;
        }
        g_scene.last_ticks = r.ticks;
    };
    const i32 area = boss ? 10 : 2, scene_map = boss ? 5 : 10, after_map = boss ? 5 : 8;
    if (boss) {
        then_start_slot0(r);
    } else {
        then_new_game(r);
    }
    then_wait_for(r, "the scene (game mode 9)", 90, [area, scene_map](mt_cpu *cpu) {
        return hero(cpu).on(area, scene_map) && game_mode(cpu) == 9;
    });
    then(r, skip ? "the scene skipped: no input" : "the scene watched: A through its boxes", 120,
         [&r, skip, boss, area, after_map](mt_cpu *cpu, double el) {
             const bool down = !skip && mt_input_panel() == 3 && std::fmod(el, 1.0) < 0.1;
             if (down && !g_scene.a_down) {
                 g_scene.test_presses++;
             }
             g_scene.a_down = down;
             hold(r, A, down);
             // The field, or (the intro) MacArthur's first dialog, which the map's script opens
             // once the map has loaded (game modes 6 to 8).
             const i32 mode = game_mode(cpu);
             return g_scene.seen && hero(cpu).on(area, after_map) &&
                    (mode == 0 || (!boss && mode == 4));
         });
    then_do(r, "the state the scene left", [](mt_cpu *cpu) { g_scene.state = scene_state(cpu); });
    if (!boss) {
        // The same A presses in both runs from here: through MacArthur's dialog to the field,
        // quest 0 (Talking to NPCs) given; that state is compared too ("field." lines).
        then(r, "A through MacArthur's dialog", 60, [&r](mt_cpu *cpu, double el) {
            hold(r, A, mt_input_panel() == 5 && std::fmod(el, 1.0) < 0.1);
            return field(cpu) && quest_state(cpu, hero(cpu), 0, 0) >= 1;
        });
        then_wait_field(r);
        then_do(r, "the state in the field after the dialog", [](mt_cpu *cpu) {
            for (auto &[k, v] : scene_state(cpu)) {
                if (k.rfind("info.", 0) != 0) {
                    g_scene.state.emplace_back("field." + k, v);
                }
            }
        });
    }
    then_do(r, "the state written", [&r](mt_cpu *) {
        std::ofstream f(r.out / "scene_state.txt", std::ios::trunc);
        for (const auto &[k, v] : g_scene.state) {
            f << k << " = " << v << "\n";
        }
        const fe::SceneSkip::Stats &k = r.scene_skip.stats();
        std::printf("scene %s: steps up to %d, %llu ticks in the scene, %llu test presses; the "
                    "skip: %llu scenes, %llu taps, %llu ticks, last step %d\n",
                    g_scene.skip ? "skipped" : "watched", g_scene.max_step,
                    static_cast<unsigned long long>(g_scene.scene_ticks),
                    static_cast<unsigned long long>(g_scene.test_presses),
                    static_cast<unsigned long long>(k.scenes),
                    static_cast<unsigned long long>(k.taps),
                    static_cast<unsigned long long>(k.scene_ticks), k.last_step);
    });
    then_wait(r, 0.5); // a few ticks of play after it (common_checks)
    then_finish(r);
}
void skip_intros_watched(Run &r, u64) { skip_scene(r, false, false); }
void skip_intros_skipped(Run &r, u64) { skip_scene(r, true, false); }
void skip_intros_boss_watched(Run &r, u64) { skip_scene(r, false, true); }
void skip_intros_boss_skipped(Run &r, u64) { skip_scene(r, true, true); }

void skip_intros_checks(Run &r, u64) {
    const SceneRun &s = g_scene;
    // The scene's last step: the intro's warp (cmd 28) at 444, Balog's end (cmd 58) at 79.
    const i32 last = s.boss ? 79 : 444;
    check(s.seen && s.max_step >= last, "the scene ran to its last step " + std::to_string(last) +
                                            " (the list's step counter " +
                                            std::to_string(s.max_step) + ")");
    check(!s.state.empty(), "the state after the scene was taken");
    if (!s.skip) {
        check(s.test_presses > 0 && r.scene_skip.stats().scenes == 0,
              "watched: A tapped the boxes (" + std::to_string(s.test_presses) +
                  " presses), no skip ran");
        return;
    }
    // The intro has 4 speech boxes, Balog's entrance 6 (cmd 50); a long box takes a tap per two
    // lines.
    const u64 boxes = s.boss ? 6 : 4;
    const fe::SceneSkip::Stats &k = r.scene_skip.stats();
    check(s.test_presses == 0 && k.scenes == 1 && k.taps >= boxes,
          "skipped: no input from the flow, one scene skipped, " + std::to_string(k.taps) +
              " taps for its " + std::to_string(boxes) + " boxes");
    const char *watched = std::getenv("MT_SKIP_WATCHED");
    if (watched == nullptr || *watched == 0) {
        check(false, "MT_SKIP_WATCHED names the watched run's OUT_DIR");
        return;
    }
    std::ifstream f(fs::path(watched) / "scene_state.txt");
    std::map<std::string, std::string> want;
    for (std::string line; std::getline(f, line);) {
        const std::size_t eq = line.find(" = ");
        if (eq != std::string::npos) {
            want[line.substr(0, eq)] = line.substr(eq + 3);
        }
    }
    check(!want.empty(), std::string("the watched run's state is in ") + watched);
    std::size_t same = 0, compared = 0;
    for (const auto &[key, v] : s.state) {
        const auto it = want.find(key);
        const std::string w = it != want.end() ? it->second : "(missing)";
        if (key.rfind("info.", 0) == 0) {
            std::printf("  %s: watched %s, skipped %s (not compared)\n", key.c_str(), w.c_str(),
                        v.c_str());
            continue;
        }
        compared++;
        if (w == v) {
            same++;
        } else {
            check(false, key + ": watched '" + w + "', skipped '" + v + "'");
        }
    }
    check(same == compared && compared > 0,
          "skipped = watched in " + std::to_string(same) + " of " + std::to_string(compared) +
              " fields (map, hero, items, quests, mobs, the scene's end)");
}

// --- Auto-loot (#175, assists.h, docs/re/qol.md#auto-loot) --------------------------------------
//
// auto_loot: a new game through the tutorial to the Western Hill (2.3), as first_map. Every
// pick-up (a drop's state turning 2) is measured as the game tested it: the gap between the
// hero's box and the drop's (both at +0x40, written by 0x6CE5D in that tick's contact test).
//   1. Off (the default): the bot hunts until a drop lies on the hero's floor out of its box but
//      within the reach. Every pick-up so far had no gap (contact).
//   2. auto_loot on, as the settings menu turns it on (install_auto_loot), nothing held: that
//      drop is picked up from where the hero stands.
//   3. On, the bot hunts on: drops are picked up across a gap, never wider than the reach.
//   4. On, the hero's two inventories full: their free places point at an item the hero has and
//      every stack is at its maximum (+0x100 = +0xFC; the test's only writes, undone after).
//      Drops in reach are refused (+0x10C) and stay on the ground; no item is taken, meso is.

struct LootGap {
    i32 x = 0, y = 0;
};

// The gap between the hero's and a drop's boxes (0 on an axis where they overlap).
LootGap loot_gap(const mt_cpu *cpu, u32 hero_addr, u32 drop) {
    const i32 hl = rdi(cpu, hero_addr + 0x40), ht = rdi(cpu, hero_addr + 0x44);
    const i32 hr = rdi(cpu, hero_addr + 0x48), hb = rdi(cpu, hero_addr + 0x4C);
    const i32 dl = rdi(cpu, drop + 0x40), dt = rdi(cpu, drop + 0x44);
    const i32 dr = rdi(cpu, drop + 0x48), db = rdi(cpu, drop + 0x4C);
    return {std::max({0, dl - hr + 1, hl - dr + 1}), std::max({0, dt - hb + 1, ht - db + 1})};
}

struct LootRun {
    int phase = 0;            // 1 off, 2 on standing, 3 on hunting, 4 on with full inventories
    std::map<u32, i32> state; // the drops of the last turn
    u64 taken[5] = {};
    i32 max_gap_x[5] = {}, max_gap_y[5] = {};
    u64 across[5] = {}; // pick-ups with a gap
    // Phase 2: the drop out of reach of contact, the hero's x, the meso and items before.
    u32 far = 0;
    i32 far_gap = 0, hero_x = 0, meso = 0, items = 0;
    // Phase 4: the free places filled (list address + 4 * index), the refused drops seen, the
    // meso before.
    std::vector<u32> filled;
    std::vector<std::pair<u32, i32>> counts; // each item's count before
    std::set<u32> refused;
    u64 refused_in_reach = 0;
    i32 meso4 = 0, items4 = 0;
};
std::shared_ptr<LootRun> g_loot;

void loot_monitor(mt_cpu *cpu) {
    LootRun &l = *g_loot;
    const Hero h = hero(cpu);
    std::map<u32, i32> now;
    for (const Drop &d : drops(cpu)) {
        const auto it = l.state.find(d.addr);
        if (h.addr != 0 && d.state == 2 && (it == l.state.end() || it->second != 2)) {
            const LootGap g = loot_gap(cpu, h.addr, d.addr);
            l.taken[l.phase]++;
            l.max_gap_x[l.phase] = std::max(l.max_gap_x[l.phase], g.x);
            l.max_gap_y[l.phase] = std::max(l.max_gap_y[l.phase], g.y);
            l.across[l.phase] += g.x > 0 || g.y > 0 ? 1 : 0;
            if (std::getenv("MT_DROP_TRACE") != nullptr || (g.x > 0 && l.across[l.phase] <= 3)) {
                std::printf("auto_loot: phase %d: drop %d at %d,%d taken by the hero at %d,%d: "
                            "gap %d x %d\n",
                            l.phase, d.id, d.x, d.y, h.x, h.y, g.x, g.y);
            }
        }
        if (l.phase == 4 && d.state == 0 && d.refused && l.refused.insert(d.addr).second) {
            const LootGap g = loot_gap(cpu, h.addr, d.addr);
            l.refused_in_reach += g.x > 0 ? 1 : 0;
            std::printf("auto_loot: full: drop %d at %d,%d refused, the hero at %d,%d (gap %d)\n",
                        d.id, d.x, d.y, h.x, h.y, g.x);
        }
        now[d.addr] = d.state;
    }
    l.state.swap(now);
}

// A lying drop on the hero's floor, out of contact but within the reach (its box's gap).
u32 far_drop(mt_cpu *cpu, i32 *gap) {
    const Hero h = hero(cpu);
    if (h.addr == 0 || !field(cpu)) {
        return 0;
    }
    for (const Drop &d : drops(cpu)) {
        if (d.state != 0 || d.refused || std::abs(d.y - h.y) > 6 || d.life < 100) {
            continue;
        }
        const LootGap g = loot_gap(cpu, h.addr, d.addr);
        if (g.y == 0 && g.x > 24 && g.x < fe::kAutoLootReachX - 24) {
            *gap = g.x;
            return d.addr;
        }
    }
    return 0;
}

void auto_loot(Run &r, u64) {
    attach_pad(r);
    r.settings_from_env = false; // off and on compared here
    g_loot = std::make_shared<LootRun>();
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        loot_monitor(cpu);
    };
    then_new_game(r);
    then_tutorial(r);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_wait_field(r);
    const Comeback back = {2, 0, 42, 2, 3};
    // 1. Off: hunt until a drop lies out of contact, within the reach.
    then_do(r, "auto_loot off", [&r](mt_cpu *) {
        check(!r.settings.auto_loot && !fe::auto_loot_installed(),
              "auto_loot is off by default: no hook");
        g_play.hunting = true;
        g_loot->phase = 1;
    });
    then_hunt(
        r, "auto_loot off, until a drop lies near the hero",
        [](mt_cpu *cpu) {
            i32 gap = 0;
            if (g_loot->taken[1] < 3) {
                return false;
            }
            g_loot->far = far_drop(cpu, &gap);
            g_loot->far_gap = gap;
            return g_loot->far != 0;
        },
        600, back);
    then_do(r, "auto_loot on, standing", [&r](mt_cpu *cpu) {
        LootRun &l = *g_loot;
        const Hero h = hero(cpu);
        check(l.taken[1] >= 3 && l.across[1] == 0,
              "auto_loot off: " + std::to_string(l.taken[1]) +
                  " drops picked up, each in contact (the widest gap " +
                  std::to_string(l.max_gap_x[1]) + " x " + std::to_string(l.max_gap_y[1]) + ")");
        l.hero_x = h.x;
        l.meso = h.meso;
        l.items = items_total(cpu, h);
        std::printf("auto_loot: a drop lies %d points from the hero's box at x %d\n", l.far_gap,
                    h.x);
        l.phase = 2;
        r.settings.auto_loot = true;
        fe::install_auto_loot(true); // as apply_live_setting does
    });
    then_wait(r, 1.5);
    then_do(r, "auto_loot on: the near drop", [](mt_cpu *cpu) {
        LootRun &l = *g_loot;
        const Hero h = hero(cpu);
        bool lying = false;
        for (const Drop &d : drops(cpu)) {
            lying = lying || (d.addr == l.far && d.state == 0);
        }
        check(!lying && l.across[2] >= 1 && std::abs(h.x - l.hero_x) <= 8 &&
                  (h.meso > l.meso || items_total(cpu, h) > l.items) &&
                  fe::auto_loot_stats().reached >= 1,
              "auto_loot on: the drop " + std::to_string(l.far_gap) +
                  " points away is picked up with the hero standing (x " +
                  std::to_string(l.hero_x) + " -> " + std::to_string(h.x) + "; meso " +
                  std::to_string(l.meso) + " -> " + std::to_string(h.meso) + ", items " +
                  std::to_string(l.items) + " -> " + std::to_string(items_total(cpu, h)) + "; " +
                  std::to_string(fe::auto_loot_stats().reached) + " reached from a distance)");
        l.phase = 3;
    });
    // 3. On, hunting.
    then_hunt(
        r, "auto_loot on",
        [](mt_cpu *) { return g_loot->across[3] >= 5 && g_loot->taken[3] >= 10; }, 600, back);
    then_do(r, "auto_loot on: the hunt", [](mt_cpu *cpu) {
        LootRun &l = *g_loot;
        check(l.across[3] >= 5 && l.max_gap_x[3] <= fe::kAutoLootReachX &&
                  l.max_gap_y[3] <= fe::kAutoLootReachY,
              "auto_loot on: " + std::to_string(l.taken[3]) + " drops picked up, " +
                  std::to_string(l.across[3]) + " across a gap (the widest " +
                  std::to_string(l.max_gap_x[3]) + " x " + std::to_string(l.max_gap_y[3]) +
                  ", the reach " + std::to_string(fe::kAutoLootReachX) + " x " +
                  std::to_string(fe::kAutoLootReachY) + ")");
        // 4. Both inventories full: each free place points at an item the hero has.
        const Hero h = hero(cpu);
        const u32 items = rd(cpu, h.addr + 0xD8), equips = rd(cpu, h.addr + 0xD4);
        u32 any = 0;
        for (u32 i = 0; i < 48 && any == 0; i++) {
            any = rd(cpu, items + 4 * i);
        }
        check(any != 0 && equips != 0, "the hero carries an item to fill the inventories with");
        for (const u32 list : {items, equips}) {
            for (u32 i = 0; any != 0 && list != 0 && i < 48; i++) {
                if (rd(cpu, list + 4 * i) == 0) {
                    std::memcpy(cpu->mem + list + 4 * i, &any, 4);
                    l.filled.push_back(list + 4 * i);
                }
            }
        }
        for (u32 i = 0; i < 48; i++) {
            const u32 item = rd(cpu, items + 4 * i);
            const bool seen = std::any_of(l.counts.begin(), l.counts.end(),
                                          [item](const auto &c) { return c.first == item; });
            if (item != 0 && !seen) {
                const i32 max = rdi(cpu, item + 0xFC);
                l.counts.emplace_back(item, rdi(cpu, item + 0x100));
                std::memcpy(cpu->mem + item + 0x100, &max, 4);
            }
        }
        l.meso4 = h.meso;
        l.items4 = items_total(cpu, h);
        std::printf("auto_loot: %zu free places filled, %zu stacks full\n", l.filled.size(),
                    l.counts.size());
        l.phase = 4;
    });
    then_hunt(
        r, "auto_loot on, the inventories full",
        [](mt_cpu *) { return g_loot->refused_in_reach >= 2; }, 600, back);
    then_do(r, "auto_loot on: full inventories", [](mt_cpu *cpu) {
        LootRun &l = *g_loot;
        u64 lying = 0;
        for (const Drop &d : drops(cpu)) {
            lying += l.refused.count(d.addr) != 0 && d.state == 0 ? 1 : 0;
        }
        const Hero h = hero(cpu);
        const i32 items = items_total(cpu, h);
        check(l.refused_in_reach >= 2 && lying >= 1 && items <= l.items4 &&
                  (l.taken[4] == 0 || h.meso > l.meso4),
              "auto_loot with full inventories: " + std::to_string(l.refused.size()) +
                  " drops refused (" + std::to_string(l.refused_in_reach) +
                  " out of contact, in reach), " + std::to_string(lying) +
                  " still on the ground; items " + std::to_string(l.items4) + " -> " +
                  std::to_string(items) + ", meso " + std::to_string(l.meso4) + " -> " +
                  std::to_string(h.meso) + " (" + std::to_string(l.taken[4]) + " picked up)");
        const u32 zero = 0;
        for (const u32 at : l.filled) {
            std::memcpy(cpu->mem + at, &zero, 4);
        }
        for (const auto &[item, count] : l.counts) {
            std::memcpy(cpu->mem + item + 0x100, &count, 4);
        }
        l.filled.clear();
        l.counts.clear();
        l.phase = 0;
    });
    then_finish(r);
}

void auto_loot_checks(Run &, u64) {
    const LootRun &l = *g_loot;
    std::printf(
        "auto_loot: picked up off %llu (%llu across a gap), on standing %llu, on %llu "
        "(%llu across, widest %d x %d), full %llu; hook: %llu tests, %llu widened, %llu "
        "reached\n",
        static_cast<unsigned long long>(l.taken[1]), static_cast<unsigned long long>(l.across[1]),
        static_cast<unsigned long long>(l.taken[2]), static_cast<unsigned long long>(l.taken[3]),
        static_cast<unsigned long long>(l.across[3]), l.max_gap_x[3], l.max_gap_y[3],
        static_cast<unsigned long long>(l.taken[4]),
        static_cast<unsigned long long>(fe::auto_loot_stats().tests),
        static_cast<unsigned long long>(fe::auto_loot_stats().widened),
        static_cast<unsigned long long>(fe::auto_loot_stats().reached));
}

// --- the minimap (#176, minimap.h, ADR 0098) -------------------------------------------------
//
// first_map's route (the tutorial, Henesys, the map screen) to the hunting field 2.3 with
// minimap = overlay, then TICKS game ticks (default 600) of the hunt there. Every turn in the
// field the minimap's view (fe::read_minimap on guest memory, as the frontend reads it at each
// present) must show the hero where guest_state reads it; in other game modes (the map screen)
// it must not show. On 2.3: the grid the game loaded equals map/23.att from the bundle (decoded
// by world_map.h) cell for cell, every marker is a portal or NPC of map/23.obj, and the hero's
// dot (at a 3:2 960 x 640 window, overlay, and a 1920 x 1080 one, border: in the left bar) is
// where the hero's position maps to, and moves left and right with him. A shot of the screen
// with the minimap over it every 5 s on 2.3 (minimap_NN.png), for looking at; never committed.
struct MinimapRun {
    u64 field_turns = 0, shown_turns = 0, hero_mismatch = 0, menu_turns = 0, menu_shown = 0;
    u64 on_23 = 0, dot_bad = 0, border_turns = 0, moves = 0, moves_left = 0, moves_right = 0;
    u64 moves_wrong = 0, marker_bad = 0, hunt_start = 0;
    bool grid_checked = false, grid_equal = false;
    std::string grid_note, first_bad;
    std::set<std::pair<i32, i32>> maps;
    std::set<int> dot_xs;
    i32 last_x = INT32_MIN;
    float last_dot = 0;
    int shots = 0;
    double next_shot = 0;
};
std::shared_ptr<MinimapRun> g_minimap;

// Whether `dot` holds map point (x, y) of `l`, clamped into the box (a pixel of slack).
bool dot_holds(const fe::MinimapLayout &l, const fe::Rect &dot, float x, float y) {
    float px = 0, py = 0;
    fe::minimap_point(l, x, y, &px, &py);
    px = std::clamp(px, l.box.x, l.box.x + l.box.w - 1);
    py = std::clamp(py, l.box.y, l.box.y + l.box.h - 1);
    return px >= dot.x - 1 && px <= dot.x + dot.w + 1 && py >= dot.y - 1 && py <= dot.y + dot.h + 1;
}

// The screen at 1x with the minimap (overlay at 480 x 320) over it, to OUT/name.png.
void minimap_shot(Run &r, mt_cpu *cpu, const fe::MinimapView &v, const std::string &name) {
    if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
        return;
    }
    std::vector<std::uint8_t> px = r.screen.pixels();
    const int w = static_cast<int>(r.screen.width()), h = static_cast<int>(r.screen.height());
    const fe::Placement p = fe::place(fe::ScaleMode::kFit, w, h, w, h);
    const fe::MinimapLayout l =
        fe::minimap_layout(fe::Minimap::kOverlay, p, p.rect, v.map_w(), v.map_h());
    const std::vector<std::uint8_t> mini = fe::render_minimap(v, l);
    const int bw = static_cast<int>(l.box.w), bh = static_cast<int>(l.box.h);
    for (int y = 0; !mini.empty() && y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            const int sx = static_cast<int>(l.box.x) + x, sy = static_cast<int>(l.box.y) + y;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
                continue;
            }
            const std::uint8_t *s = &mini[(static_cast<std::size_t>(y) * bw + x) * 4];
            std::uint8_t *d = &px[(static_cast<std::size_t>(sy) * w + sx) * 4];
            for (int c = 0; c < 3; c++) {
                d[c] = static_cast<std::uint8_t>((s[c] * s[3] + d[c] * (255 - s[3]) + 127) / 255);
            }
        }
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4);
    if (surf != nullptr) {
        SDL_SavePNG(surf, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(surf);
    }
}

void minimap_tick(Run &r, mt_cpu *cpu) {
    MinimapRun &m = *g_minimap;
    const fe::MinimapView v = fe::read_minimap(fe::guest_span(cpu));
    const Hero h = hero(cpu);
    if (h.addr != 0 && game_mode(cpu) == 3) {
        m.menu_turns++;
        m.menu_shown += v.shown ? 1 : 0;
    }
    if (!field(cpu)) {
        return;
    }
    m.field_turns++;
    if (!v.shown) {
        return;
    }
    m.shown_turns++;
    m.maps.insert({h.area, h.map});
    if (v.hero_x != h.x || v.hero_y != h.y) {
        m.hero_mismatch++;
    }
    if (!h.on(2, 3)) {
        return;
    }
    m.on_23++;
    if (!m.grid_checked) {
        m.grid_checked = true;
        const world_map::Map file = world_map::load(2, 3);
        m.grid_equal = file.w == v.cols && file.h == v.rows && file.att == v.grid;
        char buf[160];
        std::snprintf(buf, sizeof buf, "the game's grid %d x %d cells, map/23.att %d x %d", v.cols,
                      v.rows, file.w, file.h);
        m.grid_note = buf;
        for (const fe::MinimapMarker &mk : v.markers) {
            bool found = false;
            for (const world_map::Object &o : file.objects) {
                const bool portal = o.kind >= 0 && o.kind <= 2;
                const bool npc = o.kind >= 4 && o.kind <= 59;
                found = found || (o.x == mk.x && o.y == mk.y &&
                                  (mk.kind == fe::MinimapMarker::kPortal ? portal : npc));
            }
            m.marker_bad += found ? 0 : 1;
        }
        std::printf("minimap: 2.3: %s, %zu markers\n", m.grid_note.c_str(), v.markers.size());
    }
    // The dot at a 3:2 window (overlay) and at 16:9 (border, the left bar).
    const fe::Placement p32 = fe::place(fe::ScaleMode::kInteger, 960, 640, 480, 320);
    const fe::MinimapLayout l =
        fe::minimap_layout(r.settings.minimap, p32, p32.rect, v.map_w(), v.map_h());
    const fe::Placement p169 = fe::place(fe::ScaleMode::kInteger, 1920, 1080, 480, 320);
    const fe::MinimapLayout lb =
        fe::minimap_layout(fe::Minimap::kBorder, p169, p169.rect, v.map_w(), v.map_h());
    const float hx = static_cast<float>(h.x), hy = static_cast<float>(h.y - v.cell_h);
    const fe::Rect dot = fe::minimap_hero_dot(v, l);
    const fe::Rect dot_b = fe::minimap_hero_dot(v, lb);
    m.border_turns += lb.in_border && dot_b.x + dot_b.w <= p169.rect.x ? 1 : 0;
    if (!l.shown || !dot_holds(l, dot, hx, hy) || !dot_holds(lb, dot_b, hx, hy)) {
        if (m.dot_bad++ == 0) {
            char buf[200];
            std::snprintf(buf, sizeof buf,
                          "hero (%d, %d): dot (%.0f, %.0f %.0fx%.0f) in box (%.0f, %.0f "
                          "%.0fx%.0f) at scale %.4f",
                          h.x, h.y, static_cast<double>(dot.x), static_cast<double>(dot.y),
                          static_cast<double>(dot.w), static_cast<double>(dot.h),
                          static_cast<double>(l.box.x), static_cast<double>(l.box.y),
                          static_cast<double>(l.box.w), static_cast<double>(l.box.h),
                          static_cast<double>(l.scale));
            m.first_bad = buf;
        }
    }
    m.dot_xs.insert(static_cast<int>(dot.x));
    // A move the dot can show (two pixels at least): the dot went the hero's way.
    if (m.last_x == INT32_MIN) {
        m.last_x = h.x;
        m.last_dot = dot.x;
    } else if (static_cast<float>(std::abs(h.x - m.last_x)) * l.scale >= 2) {
        const bool right = h.x > m.last_x;
        m.moves++;
        (right ? m.moves_right : m.moves_left)++;
        if ((dot.x > m.last_dot) != right || dot.x == m.last_dot) {
            m.moves_wrong++;
        }
        m.last_x = h.x;
        m.last_dot = dot.x;
    }
    if (r.t >= m.next_shot && m.shots < 8) {
        char name[32];
        std::snprintf(name, sizeof name, "minimap_%02d", m.shots++);
        minimap_shot(r, cpu, v, name);
        m.next_shot = r.t + 5;
    }
}

void minimap(Run &r, u64 goal) {
    attach_pad(r);
    g_minimap = std::make_shared<MinimapRun>();
    r.settings.minimap = fe::Minimap::kOverlay;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        minimap_tick(r, cpu);
    };
    then_new_game(r);
    then_tutorial(r);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_map_screen(r);
    then_do(r, "the hunt begins", [&r](mt_cpu *cpu) {
        g_play.hunting = true;
        g_play.hunt_level = hero(cpu).level;
        g_play.hunt_events = mt_game_events_get_stats();
        g_minimap->hunt_start = r.ticks;
        std::printf("minimap: the hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(r, "map 2.3 with the minimap for " + std::to_string(goal) + " ticks",
              [&r, goal](mt_cpu *) { return r.ticks >= g_minimap->hunt_start + goal; },
              static_cast<double>(goal) * 0.05 * 1.5 + 60, {2, 0, 42, 2, 3});
    then_finish(r);
}

void minimap_checks(Run &, u64) {
    const MinimapRun &m = *g_minimap;
    std::string maps;
    for (const auto &[a, mp] : m.maps) {
        maps += " " + std::to_string(a) + "." + std::to_string(mp);
    }
    std::printf("minimap: %llu field turns, %llu shown; maps%s; %llu on 2.3; %llu moves (%llu "
                "left, %llu right), %zu dot x positions; %d shots\n",
                static_cast<unsigned long long>(m.field_turns),
                static_cast<unsigned long long>(m.shown_turns), maps.c_str(),
                static_cast<unsigned long long>(m.on_23), static_cast<unsigned long long>(m.moves),
                static_cast<unsigned long long>(m.moves_left),
                static_cast<unsigned long long>(m.moves_right), m.dot_xs.size(), m.shots);
    check(m.field_turns > 0 && m.shown_turns == m.field_turns,
          "the minimap's view shows in every field turn (" + std::to_string(m.shown_turns) +
              " of " + std::to_string(m.field_turns) + ")");
    check(m.maps.count({2, 8}) == 1 && m.maps.count({2, 0}) == 1 && m.maps.count({2, 3}) == 1,
          "shown on 2.8, Henesys (2.0) and 2.3 (maps" + maps + ")");
    check(m.hero_mismatch == 0, "its hero is guest_state's hero in every turn (" +
                                    std::to_string(m.hero_mismatch) + " differ)");
    check(m.menu_turns > 0 && m.menu_shown == 0,
          "not shown on the map screen (game mode 3: " + std::to_string(m.menu_shown) + " of " +
              std::to_string(m.menu_turns) + " turns)");
    check(m.grid_checked && m.grid_equal,
          "on 2.3 the game's grid equals map/23.att cell for cell (" + m.grid_note + ")");
    check(m.marker_bad == 0, "every marker is a portal or NPC of map/23.obj (" +
                                 std::to_string(m.marker_bad) + " are not)");
    check(m.on_23 > 0 && m.dot_bad == 0,
          "the hero's dot is at the hero's position in all " + std::to_string(m.on_23) +
              " turns on 2.3 (" + std::to_string(m.dot_bad) + " not" +
              (m.first_bad.empty() ? "" : "; first: " + m.first_bad) + ")");
    check(m.border_turns == m.on_23, "border at 16:9: in the left bar beside the picture (" +
                                         std::to_string(m.border_turns) + " of " +
                                         std::to_string(m.on_23) + " turns)");
    check(m.moves_left > 0 && m.moves_right > 0 && m.moves_wrong == 0 && m.dot_xs.size() >= 5,
          "the dot follows the hero: " + std::to_string(m.moves) + " moves (" +
              std::to_string(m.moves_left) + " left, " + std::to_string(m.moves_right) +
              " right), " + std::to_string(m.moves_wrong) + " the wrong way, " +
              std::to_string(m.dot_xs.size()) + " dot positions");
}

// --- the key pad's button labels (ADR 0101, hud_labels.h) ---------------------------------------
//
// hud_labels: widescreen_pad's way to Henesys (2.0) with widescreen on (568 points) and the key
// pad shown, the virtual Xbox pad in use. There, with widescreen's edges, then narrow, then a
// custom layout (each in effect for a few turns):
//   - every key of hud_labels.h's table is a view of the game pad (a subview of panel 0 whose
//     screen frame is the key's rect), and the label's key is where the compositor draws that
//     view now (its frame through its host transform, set by apply_keypad_layout);
//   - with the pad's glyphs and with the keyboard's, all 9 labels draw, each a point or two
//     beside its key, inside the wide picture, over no key and no other label;
//   - the device the labels follow is the gamepad's (the tutorial's presses).
// A wide shot at 2x with the labels per case and device (labels_*.png, for looking at by eye;
// game art, never committed).
struct HudLabelsRun {
    int checked = 0, bad = 0;
    std::string first_bad;
};
HudLabelsRun g_hud;

void hud_bad(const std::string &what) {
    if (g_hud.bad++ == 0) {
        g_hud.first_bad = what;
    }
    std::printf("hud labels: %s\n", what.c_str());
}

std::string rect_text(const fe::KeypadRect &k) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.1f, %.1f %.1fx%.1f)", static_cast<double>(k.x),
                  static_cast<double>(k.y), static_cast<double>(k.w), static_cast<double>(k.h));
    return b;
}

bool rects_overlap(const fe::KeypadRect &a, const fe::KeypadRect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The wide picture at 2x with the labels for `style` over it, to OUT_DIR/name.png.
void labels_shot(Run &r, mt_cpu *cpu, const std::vector<fe::HudLabel> &labels,
                 const fe::PromptStyle &style, const std::string &name) {
    std::vector<std::uint8_t> px1;
    u32 view = 0;
    if (!wide_pixels(r, cpu, true, &px1, &view)) {
        return;
    }
    constexpr int k = 2, w = MT_WIDESCREEN_WIDE * k, h = MT_WIDESCREEN_HEIGHT * k;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            std::memcpy(&px[(static_cast<std::size_t>(y) * w + x) * 4],
                        &px1[(static_cast<std::size_t>(y / k) * MT_WIDESCREEN_WIDE + x / k) * 4],
                        4);
        }
    }
    fe::Canvas canvas(px.data(), w, h, fe::hud_label_unit(k));
    const float off = (MT_WIDESCREEN_WIDE - 480) / 2.0f;
    fe::draw_hud_labels(canvas, k, -off, 480 + off, labels, style, r.actions.bindings());
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4);
    if (surf != nullptr) {
        SDL_SavePNG(surf, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(surf);
    }
}

void check_hud_labels(Run &r, mt_cpu *cpu, const std::string &what) {
    fe::apply_keypad_layout();
    const u32 panel = mt_input_pad_panel_view(0);
    if (panel == 0 || mt_input_panel() != 0) {
        hud_bad(what + ": the game pad is not the panel in use");
        return;
    }
    u32 subs[64];
    const u32 n = std::min<u32>(mt_uikit_subviews(panel, subs, 64), 64);
    const std::vector<fe::HudLabel> labels =
        fe::hud_labels(fe::installed_keypad_layout(), fe::installed_keypad_wide(), 1);
    const fe::HudKey *keys = fe::hud_keys();
    for (std::size_t i = 0; i < fe::kHudKeyCount; i++) {
        const fe::KeypadRect &k = keys[i].rect;
        bool found = false;
        for (u32 j = 0; j < n && !found; j++) {
            float fr[4];
            if (mt_uikit_screen_frame(subs[j], fr) == 0 || std::abs(fr[0] - k.x) > 0.5f ||
                std::abs(fr[1] - k.y) > 0.5f || std::abs(fr[2] - k.w) > 0.5f ||
                std::abs(fr[3] - k.h) > 0.5f) {
                continue;
            }
            found = true;
            float m[6] = {1, 0, 0, 1, 0, 0};
            mt_uikit_host_transform(subs[j], m);
            const fe::KeypadRect drawn{m[0] * fr[0] + m[2] * fr[1] + m[4],
                                       m[1] * fr[0] + m[3] * fr[1] + m[5], fr[2] * m[0],
                                       fr[3] * m[3]};
            const fe::KeypadRect &l = labels[i].key;
            if (std::abs(drawn.x - l.x) > 0.5f || std::abs(drawn.y - l.y) > 0.5f ||
                std::abs(drawn.w - l.w) > 0.5f || std::abs(drawn.h - l.h) > 0.5f) {
                hud_bad(what + ": key " + std::to_string(i) + " drawn at " + rect_text(drawn) +
                        ", its label's key at " + rect_text(l));
            }
        }
        if (!found) {
            hud_bad(what + ": no view of the game pad at key " + std::to_string(i) + "'s rect " +
                    rect_text(k));
        }
    }
    if (r.hints.device() != fe::PromptDevice::kGamepad) {
        hud_bad(what + ": the labels' device is not the gamepad after the pad's presses");
    }
    const float off = (MT_WIDESCREEN_WIDE - 480) / 2.0f, lo = -off, hi = 480 + off;
    const fe::PromptStyle pad{fe::PromptDevice::kGamepad, r.glyphs.type(),
                              r.gamepads.face_buttons()};
    const fe::PromptStyle keys_style{fe::PromptDevice::kKeyboard, SDL_GAMEPAD_TYPE_UNKNOWN,
                                     r.gamepads.face_buttons()};
    for (const fe::PromptStyle &style : {pad, keys_style}) {
        const std::string dev = style.device == fe::PromptDevice::kGamepad ? "pad" : "keys";
        constexpr int k = 2, w = MT_WIDESCREEN_WIDE * k, h = MT_WIDESCREEN_HEIGHT * k;
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
        fe::Canvas canvas(px.data(), w, h, fe::hud_label_unit(k));
        std::vector<fe::HudLabelDrawn> drawn;
        const int count =
            fe::draw_hud_labels(canvas, k, lo, hi, labels, style, r.actions.bindings(), &drawn);
        if (count != static_cast<int>(fe::kHudKeyCount)) {
            hud_bad(what + " (" + dev + "): " + std::to_string(count) + " labels drawn, not 9");
        }
        for (std::size_t a = 0; a < drawn.size(); a++) {
            const fe::KeypadRect &b = drawn[a].box;
            const fe::KeypadRect &key = labels[drawn[a].label].key;
            const float sep = std::max({b.x - (key.x + key.w), key.x - (b.x + b.w),
                                        b.y - (key.y + key.h), key.y - (b.y + b.h)});
            const bool in =
                b.x >= lo - 0.01f && b.x + b.w <= hi + 0.01f && b.y >= 0 && b.y + b.h <= 320.01f;
            if (!in || sep < -0.01f || sep > 2.01f) {
                hud_bad(what + " (" + dev + "): label " + std::to_string(drawn[a].label) + " at " +
                        rect_text(b) + " not beside its key " + rect_text(key));
            }
            for (const fe::HudLabel &other : labels) {
                if (rects_overlap(b, other.key)) {
                    hud_bad(what + " (" + dev + "): label " + std::to_string(drawn[a].label) +
                            " over the key at " + rect_text(other.key));
                }
            }
            for (std::size_t c = a + 1; c < drawn.size(); c++) {
                if (rects_overlap(b, drawn[c].box)) {
                    hud_bad(what + " (" + dev + "): labels " + std::to_string(drawn[a].label) +
                            " and " + std::to_string(drawn[c].label) + " overlap");
                }
            }
        }
        labels_shot(r, cpu, labels, style, "labels_" + what + "_" + dev);
    }
    std::printf("hud labels: %s: offset %g, anchored %d, layout %s; slot 1's key at %s\n",
                what.c_str(), static_cast<double>(fe::installed_keypad_wide().off),
                fe::installed_keypad_wide().anchored ? 1 : 0,
                fe::keypad_layout_text(fe::installed_keypad_layout()).c_str(),
                rect_text(labels[2].key).c_str());
    g_hud.checked++;
}

void hud_labels(Run &r, u64) {
    r.wide_keypad = true;
    fe::install_keypad_layout(fe::KeypadLayout{});
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait(r, 2);
    then_do(r, "the labels with widescreen's edges", [&r](mt_cpu *cpu) {
        check(mt_widescreen_view() == MT_WIDESCREEN_WIDE &&
                  fe::installed_keypad_wide().off == 44.0f && fe::installed_keypad_wide().anchored,
              "Henesys (2.0) is drawn 568 points wide, the key pad at the view's edges");
        check_hud_labels(r, cpu, "edges");
        r.wide_anchored = false; // widescreen_hud = narrow
    });
    then_wait(r, 0.3);
    then_do(r, "the labels with widescreen narrow", [&r](mt_cpu *cpu) {
        check(!fe::installed_keypad_wide().anchored, "narrow: the key pad at its 3:2 places");
        check_hud_labels(r, cpu, "narrow");
        r.wide_anchored = true;
        fe::KeypadLayout custom;
        check(fe::parse_keypad_layout("slots=130,-60,20,70;actions=80,-150,0,100", &custom),
              "the custom layout parses");
        fe::install_keypad_layout(custom);
    });
    then_wait(r, 0.3);
    then_do(r, "the labels with a custom layout", [&r](mt_cpu *cpu) {
        check_hud_labels(r, cpu, "custom");
        fe::install_keypad_layout(fe::KeypadLayout{});
    });
    then_finish(r);
}

int hud_labels_setup(Run &r) {
    r.widescreen = MT_WIDESCREEN_WIDE;
    return 0;
}

// --- the minimap's corner: widescreen with the key pad at the edges (#176, ADR 0098) ----------
//
// minimap_wide: first_map's way into the field 2.3 with widescreen (568 points), the touch key
// pad shown at the view's edges (widescreen_hud = edges) and minimap = overlay, then TICKS game
// ticks (default 600) of the hunt there. Every field turn on the wide view the corner's layout,
// at a 16:9 1920 x 1080 window (3x) and at the 1x wide picture, has the box at the visible
// view's top left a margin in, ending above y = 52 (the game's messages), and the compass moved
// right of it (fe::minimap_move_compass, as game.cpp does at each composite). Every 20th of
// those turns the game pad (panel 0: the compass, Bag, Cash, the d-pad, jump, attack and the
// quick slots, with their host moves) is composited alone across the whole wide view
// (mt_uikit_composite_view): no pixel of it may lie under the box or its frame; once, with the
// compass at its own place, it must (the check sees the compass). A tap on the moved compass
// and one on the box in the left wing, through Input's redirects, each open the map screen
// (game mode 3), B closing it. Shots of the wide picture with the minimap every 5 s on 2.3
// (minimap_wide_NN.png; local only, never committed).
struct MinimapWideRun {
    u64 wide_turns = 0, corner_bad = 0, composites = 0, covered = 0, compass_px = 0;
    u64 bare_covered = 0, hunt_start = 0, map_opens = 0;
    std::string first_bad;
    float dx = 0;
    int shots = 0;
    double next_shot = 0;
    bool bare_checked = false;
};
std::shared_ptr<MinimapWideRun> g_mwide;

// The corner's layouts now: at a 1920 x 1080 window (3x) and at the 1x wide picture.
struct CornerPair {
    fe::MinimapLayout big, one;
    fe::Rect big_visible, one_visible;
};
CornerPair corner_layouts(const fe::MinimapView &v) {
    CornerPair c;
    auto layout = [&v](int w, int h, fe::Rect *visible) {
        fe::Placement image = fe::place(fe::ScaleMode::kInteger, w, h, 568, 320, fe::Insets{}, 1, 1,
                                        MT_WIDESCREEN_WIDE);
        const fe::Rect wide = image.rect;
        image.rect = fe::wide_part(wide, MT_WIDESCREEN_WIDE, 480);
        *visible = fe::minimap_visible(image, wide, MT_WIDESCREEN_WIDE, mt_widescreen_view());
        return fe::minimap_layout(fe::Minimap::kOverlay, fe::MinimapAnchor::kCorner, image, wide,
                                  *visible, v.map_w(), v.map_h());
    };
    c.big = layout(1920, 1080, &c.big_visible);
    c.one = layout(MT_WIDESCREEN_WIDE, MT_WIDESCREEN_HEIGHT, &c.one_visible);
    return c;
}

// The game pad's pixels (alpha > 0) composited alone across the 568-point view inside `r` (1x
// picture pixels), with the compass where it is now.
u64 pad_pixels_in(mt_cpu *cpu, const fe::Rect &r) {
    constexpr u32 w = MT_WIDESCREEN_WIDE, h = MT_WIDESCREEN_HEIGHT;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
    const float x0 = -static_cast<float>(w - 480) / 2;
    if (mt_uikit_composite_view(cpu, mt_cf_absolute_time(), mt_input_pad_panel_view(0), x0,
                                px.data(), w, h, w * 4) != 0) {
        return ~0ull;
    }
    u64 n = 0;
    const int y1 = std::min<int>(h, static_cast<int>(r.y + r.h));
    const int x1 = std::min<int>(w, static_cast<int>(r.x + r.w));
    for (int y = std::max(0, static_cast<int>(r.y)); y < y1; y++) {
        for (int x = std::max(0, static_cast<int>(r.x)); x < x1; x++) {
            n += px[(static_cast<std::size_t>(y) * w + x) * 4 + 3] != 0 ? 1 : 0;
        }
    }
    return n;
}

// The box and its frame (a point: a pixel at 1x).
fe::Rect framed(const fe::MinimapLayout &l) {
    return fe::Rect{l.box.x - 1, l.box.y - 1, l.box.w + 2, l.box.h + 2};
}

void minimap_wide_tick(Run &r, mt_cpu *cpu) {
    MinimapWideRun &m = *g_mwide;
    const fe::MinimapView v = fe::read_minimap(fe::guest_span(cpu));
    const bool on = field(cpu) && v.shown && mt_widescreen_view() == MT_WIDESCREEN_WIDE;
    const CornerPair c = on ? corner_layouts(v) : CornerPair{};
    // As game.cpp's composite: the compass moved while the corner shows, the redirects with it.
    const float dx = on ? c.one.compass_dx : 0.0f;
    r.input.set_redirects(fe::minimap_touch_redirects(on ? c.one : fe::MinimapLayout{}));
    if (dx != m.dx && fe::minimap_move_compass(dx)) {
        m.dx = dx;
    }
    if (!on) {
        return;
    }
    m.wide_turns++;
    const float m3 = 12, m1 = 4;
    const bool ok = c.big.corner && c.one.corner && c.big.box.x == c.big_visible.x + m3 &&
                    c.big.box.y == c.big_visible.y + m3 && c.one.box.x == c.one_visible.x + m1 &&
                    c.one.box.y == c.one_visible.y + m1 &&
                    c.big.box.y + c.big.box.h <= 60 + 52 * 3 && c.one.box.y + c.one.box.h <= 52 &&
                    c.one.compass_dx == c.big.compass_dx && c.one.compass_dx > 0;
    if (!ok && m.corner_bad++ == 0) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "1x box (%g, %g %gx%g), visible x %g; compass +%g",
                      static_cast<double>(c.one.box.x), static_cast<double>(c.one.box.y),
                      static_cast<double>(c.one.box.w), static_cast<double>(c.one.box.h),
                      static_cast<double>(c.one_visible.x), static_cast<double>(c.one.compass_dx));
        m.first_bad = buf;
    }
    if (m.wide_turns % 20 == 1 && m.dx == c.one.compass_dx) {
        fe::apply_keypad_layout();
        const u64 n = pad_pixels_in(cpu, framed(c.one));
        m.composites++;
        m.covered += n != 0 ? 1 : 0;
        // The moved compass is drawn (the composite holds the pad at all).
        const fe::Rect compass{44 + m.dx, 0, fe::kMinimapCompassSide, fe::kMinimapCompassSide};
        m.compass_px = std::max(m.compass_px, pad_pixels_in(cpu, compass));
        if (!m.bare_checked) {
            m.bare_checked = true;
            fe::minimap_move_compass(0);
            m.bare_covered = pad_pixels_in(cpu, framed(c.one));
            fe::minimap_move_compass(m.dx);
        }
    }
    if (hero(cpu).on(2, 3) && r.t >= m.next_shot && m.shots < 8) {
        char name[32];
        std::snprintf(name, sizeof name, "minimap_wide_%02d", m.shots++);
        const fe::MinimapLayout l = c.one;
        wide_shot(r, cpu, name, true, [&v, &l](std::vector<std::uint8_t> &px) {
            constexpr int w = MT_WIDESCREEN_WIDE;
            auto blend = [&px](int x, int y, const std::uint8_t *s) {
                if (x < 0 || y < 0 || x >= w || y >= static_cast<int>(MT_WIDESCREEN_HEIGHT)) {
                    return;
                }
                std::uint8_t *d = &px[(static_cast<std::size_t>(y) * w + x) * 4];
                for (int i = 0; i < 3; i++) {
                    d[i] =
                        static_cast<std::uint8_t>((s[i] * s[3] + d[i] * (255 - s[3]) + 127) / 255);
                }
            };
            const std::uint8_t edge[4] = {fe::kMinimapEdge.r, fe::kMinimapEdge.g,
                                          fe::kMinimapEdge.b, fe::kMinimapEdge.a};
            for (const fe::Rect &f : fe::minimap_frame(l)) {
                for (int y = static_cast<int>(f.y); y < static_cast<int>(f.y + f.h); y++) {
                    for (int x = static_cast<int>(f.x); x < static_cast<int>(f.x + f.w); x++) {
                        blend(x, y, edge);
                    }
                }
            }
            const std::vector<std::uint8_t> mini = fe::render_minimap(v, l);
            const int bw = static_cast<int>(l.box.w), bh = static_cast<int>(l.box.h);
            for (int y = 0; !mini.empty() && y < bh; y++) {
                for (int x = 0; x < bw; x++) {
                    blend(static_cast<int>(l.box.x) + x, static_cast<int>(l.box.y) + y,
                          &mini[(static_cast<std::size_t>(y) * bw + x) * 4]);
                }
            }
        });
        m.next_shot = r.t + 5;
    }
}

// A tap at screen point (x, y) through Input (its redirects), then the map screen, closed by B.
void then_tap_opens_map(Run &r, const std::string &what, std::function<fe::KeypadPoint()> at) {
    auto down = std::make_shared<bool>(false);
    then(r, "tap " + what, 3, [&r, at, down](mt_cpu *, double el) {
        const fe::KeypadPoint p = at();
        if (el < 0.05) {
            return false;
        }
        if (!*down) {
            *down = true;
            check(r.input.touch(fe::InputSource::kFinger, 7, MT_UIKIT_TOUCH_BEGAN, p.x, p.y),
                  "the tap goes to the game");
            return false;
        }
        if (el < 0.2) {
            return false;
        }
        *down = false;
        r.input.touch(fe::InputSource::kFinger, 7, MT_UIKIT_TOUCH_ENDED, p.x, p.y);
        return true;
    });
    then_wait_for(
        r, "the map screen after the tap on " + what, 5,
        [](mt_cpu *cpu) { return game_mode(cpu) == 3 && mt_input_panel() == 8; }, 0.3);
    then_do(r, "count it", [](mt_cpu *) { g_mwide->map_opens++; });
    then_press(r, B, 0.1, 0.8);
    then_wait_for(r, "the map screen closed", 5, field, 0.5);
}

void minimap_wide(Run &r, u64 goal) {
    r.wide_keypad = true;
    fe::install_keypad_layout(fe::KeypadLayout{});
    attach_pad(r);
    g_mwide = std::make_shared<MinimapWideRun>();
    r.settings.minimap = fe::Minimap::kOverlay;
    r.settings.widescreen_hud = fe::WidescreenHud::kEdges;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        minimap_wide_tick(r, cpu);
    };
    then_new_game(r);
    then_tutorial(r);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_wait_field(r);
    then_wait(r, 1);
    then_do(r, "2.3 wide with the key pad", [](mt_cpu *cpu) {
        check(hero(cpu).on(2, 3) && mt_widescreen_view() == MT_WIDESCREEN_WIDE && g_mwide->dx > 0,
              "2.3 is drawn 568 points wide, the compass moved " + std::to_string(g_mwide->dx) +
                  " points right of the corner's box");
    });
    then_tap_opens_map(r, "the moved compass", [] {
        return fe::KeypadPoint{g_mwide->dx + fe::kMinimapCompassSide / 2, 26};
    });
    then_tap_opens_map(r, "the box in the left wing", [] { return fe::KeypadPoint{-30, 20}; });
    then_do(r, "the hunt begins", [&r](mt_cpu *cpu) {
        g_play.hunting = true;
        g_play.hunt_level = hero(cpu).level;
        g_play.hunt_events = mt_game_events_get_stats();
        g_mwide->hunt_start = r.ticks;
    });
    then_hunt(r, "map 2.3 with the minimap's corner for " + std::to_string(goal) + " ticks",
              [&r, goal](mt_cpu *) { return r.ticks >= g_mwide->hunt_start + goal; },
              static_cast<double>(goal) * 0.05 * 1.5 + 60, {2, 0, 42, 2, 3});
    then_finish(r);
}

int minimap_wide_setup(Run &r) {
    r.widescreen = MT_WIDESCREEN_WIDE;
    return 0;
}

void hud_labels_checks(Run &, u64) {
    check(g_hud.checked == 3 && g_hud.bad == 0,
          "the labels sit beside the keys as drawn in edges, narrow and a custom layout (" +
              std::to_string(g_hud.checked) + " of 3 cases, " + std::to_string(g_hud.bad) +
              " problems" + (g_hud.first_bad.empty() ? "" : "; first: " + g_hud.first_bad) + ")");
}

void minimap_wide_checks(Run &, u64) {
    const MinimapWideRun &m = *g_mwide;
    std::printf("minimap_wide: %llu wide field turns, %llu composites (%llu covered), compass "
                "%llu px, %llu px under the box with the compass unmoved, %llu map opens, %d "
                "shots\n",
                static_cast<unsigned long long>(m.wide_turns),
                static_cast<unsigned long long>(m.composites),
                static_cast<unsigned long long>(m.covered),
                static_cast<unsigned long long>(m.compass_px),
                static_cast<unsigned long long>(m.bare_covered),
                static_cast<unsigned long long>(m.map_opens), m.shots);
    check(m.wide_turns > 0 && m.corner_bad == 0,
          "the box at the visible view's top left, above y = 52, the compass right of it, in all " +
              std::to_string(m.wide_turns) + " wide field turns (" + std::to_string(m.corner_bad) +
              " not" + (m.first_bad.empty() ? "" : "; first: " + m.first_bad) + ")");
    check(m.composites > 0 && m.covered == 0 && m.compass_px > 0,
          "no pixel of the game pad (compass, Bag, Cash, d-pad, keys, quick slots) under the box "
          "or its frame in " +
              std::to_string(m.composites) + " composites (" + std::to_string(m.covered) +
              " covered; the moved compass drawn: " + std::to_string(m.compass_px) + " px)");
    check(m.bare_checked && m.bare_covered > 0,
          "with the compass at its own place the box would cover it (" +
              std::to_string(m.bare_covered) + " px)");
    check(m.map_opens == 2, "a tap on the moved compass and one on the box open the map screen (" +
                                std::to_string(m.map_opens) + " of 2)");
}

// --- Controller extras (#172, assists.h, ADR 0094) ---------------------------------------------
//
// auto_run: a new game to the tutorial's first map (2.8). Off (the default): right held, then
// L3 (the auto_run action), then right let go: the hero stops. auto_run on: left held, L3, left
// let go: the hero keeps walking left with nothing held until the map's left edge; right (the
// other direction) ends it; L3 with nothing held latches right (the last direction walked) and
// the hero crosses the map to its right edge (x 0 to 479), stick and d-pad let go; at the
// tutorial's steps A jumps, the latched walk making it a jump to the right. Start opens the popup
// menu: the latch ends there, L3 latches nothing in it, and the hero stands when it closes.
// Then hold_toggle = walk (auto_run off): a tap of left walks until the next tap of left.

constexpr SDL_GamepadButton L3 = SDL_GAMEPAD_BUTTON_LEFT_STICK;

struct AutoRunRun {
    i32 x0 = 0, x1 = 0;       // a stretch's start and a point on it
    i32 min_x = 0, max_x = 0; // the map's walkable ends, as the hero reached them
};

// Waits until the hero's x has not changed for `stable` seconds (a wall, or let go).
void then_until_still(Run &r, const std::string &what, double timeout, double stable = 0.6) {
    auto last = std::make_shared<i32>(INT32_MIN);
    auto since = std::make_shared<double>(-1);
    then(r, what, timeout, [last, since, stable](mt_cpu *cpu, double el) {
        const i32 x = hero(cpu).x;
        if (x != *last) {
            *last = x;
            *since = el;
            return false;
        }
        return el - *since >= stable;
    });
}

void auto_run(Run &r, u64) {
    attach_pad(r);
    r.settings_from_env = false; // off and on compared here
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_intro_field(r);
    auto s = std::make_shared<AutoRunRun>();
    then_check(r, "auto_run and hold_toggle are off by default", [&r](mt_cpu *) {
        return !r.settings.auto_run && r.settings.hold_toggle == fe::HoldToggle::kOff;
    });
    // 1. Off: L3 does nothing; the hero stops when right is let go.
    then_do(r, "auto_run off: right, L3", [&r](mt_cpu *) { hold(r, RIGHT, true); });
    then_wait(r, 0.5);
    then_press(r, L3, 0.1, 0.1);
    then_do(r, "auto_run off: right let go", [&r](mt_cpu *) { hold(r, RIGHT, false); });
    then_until_still(r, "auto_run off: the hero stops", 5);
    then_do(r, "auto_run off: x", [s](mt_cpu *cpu) { s->x0 = hero(cpu).x; });
    then_wait(r, 1.5);
    then_do(r, "auto_run off", [&r, s](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.x == s->x0 && r.assists.stats().latches == 0 && mt_input_held(MT_ACTION_RIGHT) == 0,
              "auto_run off: L3 latches nothing, the hero stands at x " + std::to_string(h.x) +
                  " after right is let go (" + std::to_string(s->x0) + " 1.5 s before)");
        r.settings.auto_run = true;
    });
    // 2. On: left held, L3, left let go: the hero walks left on its own to the map's edge.
    then_do(r, "auto_run on: left, L3", [&r, s](mt_cpu *cpu) {
        s->x0 = hero(cpu).x;
        hold(r, LEFT, true);
    });
    then_wait(r, 0.3);
    then_press(r, L3, 0.1, 0.1);
    then_do(r, "auto_run on: left let go", [&r](mt_cpu *) { hold(r, LEFT, false); });
    then_wait(r, 1.0);
    then_do(r, "auto_run on: walking left", [&r, s](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(r.assists.walk_latch() == MT_ACTION_LEFT && r.actions.held() == 0 &&
                  mt_input_held(MT_ACTION_LEFT) == 1 && h.state == 3,
              "auto_run on: nothing held, the hero walks left (" + where(cpu) + ")");
        s->x1 = h.x;
    });
    then_until_still(r, "auto_run on: the hero reaches the left edge", 60);
    then_do(r, "auto_run on: the left edge", [&r, s](mt_cpu *cpu) {
        s->min_x = hero(cpu).x;
        check(s->min_x < s->x1 && s->x1 < s->x0 && r.assists.walk_latch() == MT_ACTION_LEFT,
              "auto_run on: the hero walked from x " + std::to_string(s->x0) +
                  " to the left edge at " + std::to_string(s->min_x) + " with nothing held");
    });
    // The other direction ends it.
    then_press(r, RIGHT, 0.1, 0.3);
    then_do(r, "auto_run: right ends the latch", [&r](mt_cpu *cpu) {
        check(r.assists.walk_latch() == MT_ACTION_NONE &&
                  r.assists.stats().last_end == fe::LatchEnd::kOther &&
                  mt_input_held(MT_ACTION_LEFT) == 0 && mt_input_held(MT_ACTION_RIGHT) == 0,
              "auto_run: a press of right ends the latched walk (" + where(cpu) + ")");
    });
    then_until_still(r, "auto_run: the hero stands", 5);
    // 3. L3 with nothing held: the last direction walked (right), across the map.
    then_do(r, "auto_run: x before L3", [s](mt_cpu *cpu) { s->x0 = hero(cpu).x; });
    then_press(r, L3, 0.1, 0.1);
    then_wait(r, 1.0);
    then_do(r, "auto_run: walking right", [&r, s](mt_cpu *cpu) {
        check(r.assists.walk_latch() == MT_ACTION_RIGHT && r.actions.held() == 0 &&
                  hero(cpu).x > s->x0,
              "auto_run: L3 alone walks right, the last direction (" + where(cpu) + ")");
    });
    then_until_still(r, "auto_run: the hero walks to the tutorial's step", 90);
    // The step the tutorial teaches jumping on: A (jump, with the latched direction a jump to
    // the right), then on to the right edge; again for each step.
    for (int i = 0; i < 3; i++) {
        then_press(r, A, 0.1, 0.1);
        then_until_still(r, "auto_run: jumped, walking on to the right", 30);
    }
    then_do(r, "auto_run: across the map", [&r, s](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        s->max_x = h.x;
        std::printf("auto_run: map 2.8 crossed from x %d to x %d with nothing held\n", s->min_x,
                    s->max_x);
        check(h.on(2, 8) && s->max_x - s->min_x >= 400 &&
                  r.assists.walk_latch() == MT_ACTION_RIGHT && r.actions.held() == 0,
              "auto_run: the hero crossed map 2.8 from x " + std::to_string(s->min_x) + " to " +
                  std::to_string(s->max_x) + " with the stick and d-pad let go");
    });
    // Auto-running left again (a tap of left ends the latch, L3 takes up left), then the popup
    // menu.
    then_press(r, LEFT, 0.1, 0.1);
    then_press(r, L3, 0.1, 0.6);
    then_check(r, "auto_run: walking left again",
               [&r](mt_cpu *) { return r.assists.walk_latch() == MT_ACTION_LEFT; });
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_do(r, "auto_run: the popup menu ends it", [&r](mt_cpu *cpu) {
        check(r.assists.walk_latch() == MT_ACTION_NONE &&
                  r.assists.stats().last_end == fe::LatchEnd::kField &&
                  mt_input_held(MT_ACTION_LEFT) == 0,
              "auto_run: the latch ends in the popup menu (" + where(cpu) + ")");
    });
    then_press(r, L3, 0.1, 0.3); // menus never latch
    then_check(r, "auto_run: L3 in the popup menu latches nothing",
               [&r](mt_cpu *) { return r.assists.walk_latch() == MT_ACTION_NONE; });
    then_close_to_field(r);
    then_do(r, "the field again: x", [s](mt_cpu *cpu) { s->x0 = hero(cpu).x; });
    then_wait(r, 1.5);
    then_do(r, "auto_run: standing after the popup menu", [&r, s](mt_cpu *cpu) {
        check(hero(cpu).x == s->x0 && r.assists.walk_latch() == MT_ACTION_NONE &&
                  mt_input_held(MT_ACTION_LEFT) == 0,
              "auto_run: back in the field the hero stands (" + where(cpu) + ")");
        r.settings.auto_run = false;
        r.settings.hold_toggle = fe::HoldToggle::kWalk;
    });
    // 4. hold_toggle = walk: a tap of left walks on, the next tap stops.
    then_press(r, LEFT, 0.1, 1.5);
    then_do(r, "hold_toggle: walking left", [&r, s](mt_cpu *cpu) {
        check(r.assists.walk_latch() == MT_ACTION_LEFT && r.actions.held() == 0 &&
                  hero(cpu).x < s->x0 - 20,
              "hold_toggle = walk: a tap of left walks on (from x " + std::to_string(s->x0) + ", " +
                  where(cpu) + ")");
    });
    then_press(r, LEFT, 0.1, 0.3);
    then_until_still(r, "hold_toggle: the hero stops", 5);
    then_do(r, "hold_toggle: x", [s](mt_cpu *cpu) { s->x1 = hero(cpu).x; });
    then_wait(r, 1.0);
    then_do(r, "hold_toggle: stopped", [&r, s](mt_cpu *cpu) {
        check(r.assists.walk_latch() == MT_ACTION_NONE &&
                  r.assists.stats().last_end == fe::LatchEnd::kAgain && hero(cpu).x == s->x1 &&
                  mt_input_held(MT_ACTION_LEFT) == 0,
              "hold_toggle = walk: the second tap of left stops the hero (" + where(cpu) + ")");
    });
    then_finish(r);
}

void auto_run_checks(Run &, u64) {}

// --- the regression run (#91, ADR 0104, docs/testing/regression-replay.md) ---------------------
//
// regress_record: the bot plays from a fresh install through the title, Play, the intro, the
// first map (2.8) and the tutorial to the first fight, the snail on 2.9 (then_tutorial_to_snail),
// recording its pad into a replay (replay.h) with a checkpoint at each stage and every second,
// and the facts each stage must show. MT_REPLAY: where the replay goes (default
// OUT_DIR/recorded.replay); the committed one is tests/regress/title_to_combat.replay.
//
// regress: plays the replay MT_REPLAY back through the same input layer, with no bot (no task
// reads the game to decide anything), takes its checkpoints and checks its expects. Both write
// OUT_DIR/checkpoints.txt and print its digest; scripts/regress runs it and compares digests.
// MT_REGRESS_SHOTS=1 also writes each checkpoint's screen (check_NAME.png, for looking at).
//
// Settings: MAPLETHIEF_SETTINGS (e.g. preset=original) as every mode; besides what boot()
// applies (the Assists, skip_intros, ui_animations, auto_loot), menu_repeat, confirm_guard and
// game_speed as game.cpp applies them.

fe::Replay g_replay;
fs::path g_replay_path;
fe::GameSpeed g_regress_speed = fe::GameSpeed::kGame;

// A checkpoint's state: the hero, the screen's panel, mode and popup, and the mobs the monitor
// saw defeated.
std::string regress_state(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "map=%d.%d x=%d y=%d hp=%d mp=%d level=%d exp=%d meso=%d panel=%d mode=%d "
                  "popup=%d kills=%llu ticks=%llu",
                  h.area, h.map, h.x, h.y, h.hp, h.mp, h.level, h.exp, h.meso,
                  static_cast<int>(mt_input_panel()), game_mode(cpu), popup_id(cpu),
                  static_cast<unsigned long long>(g_play.kills),
                  static_cast<unsigned long long>(g_run != nullptr ? g_run->ticks : 0));
    return buf;
}

// What both modes share: the audio hashed, the state line, the monitor, the settings.
void regress_common(Run &r) {
    r.audio = std::make_unique<fe::AudioHash>();
    r.state = regress_state;
    r.checkpoint_shots = std::getenv("MT_REGRESS_SHOTS") != nullptr;
    r.before_run = [&r](mt_cpu *) {
        r.menu.set_repeat(r.settings.menu_repeat);
        r.menu.set_guard(r.settings.confirm_guard);
        std::printf("regress: menu_repeat %d, confirm_guard %d, game_speed %s\n",
                    r.settings.menu_repeat ? 1 : 0, r.settings.confirm_guard ? 1 : 0,
                    fe::game_speed_name(r.settings.game_speed));
    };
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        if (r.settings.game_speed != g_regress_speed) { // as game.cpp's turn hook
            fe::apply_game_speed(cpu, r.settings.game_speed);
            g_regress_speed = r.settings.game_speed;
        }
    };
}

fs::path replay_path(const Run &r) {
    const char *p = std::getenv("MT_REPLAY");
    return p != nullptr && *p != 0 ? fs::path(p) : r.out / "recorded.replay";
}

int regress_record_setup(Run &r) {
    g_replay_path = replay_path(r);
    r.record = &g_replay;
    g_replay.every = 1000; // a checkpoint every virtual second
    r.plan.set_every(g_replay.every);
    return 0;
}

void regress_record(Run &r, u64) {
    regress_common(r);
    attach_pad(r);
    // The title: the main menu with its promo banner (ADR 0044), still for a second.
    then_wait_for(
        r, "the title (the promo banner)", 15,
        [&r](mt_cpu *) {
            for (const fe::FocusTarget &f : r.focus.targets()) {
                if (f.action == "clickADClose:") {
                    return true;
                }
            }
            return false;
        },
        1.0);
    then_checkpoint(r, "title");
    then_new_game(r);
    then_checkpoint(r, "play");
    then_tutorial_to_snail(r, [&r](const std::string &name) { then_checkpoint(r, name); });
    then_end(r, 2.0);
}

// The facts each stage of the recording shows (the replay's expect lines).
void regress_record_checks(Run &r, u64) {
    g_replay.expects = {
        {"title", "map", "=", "-1.-1"},    {"intro", "map", "=", "2.10"},
        {"first_map", "map", "=", "2.8"},  {"first_map", "panel", "=", "0"},
        {"first_map", "popup", "=", "-1"}, {"map_2_9", "map", "=", "2.9"},
        {"combat", "map", "=", "2.9"},     {"combat", "kills", ">=", "1"},
        {"end", "map", "=", "2.9"},        {"end", "kills", ">=", "1"},
    };
    // The recording run shows them too.
    for (const fe::ReplayExpect &e : g_replay.expects) {
        const fe::Checkpoint *c = r.checkpoints.find(e.check);
        std::map<std::string, std::string> state;
        std::string why = "no checkpoint";
        check(c != nullptr && fe::parse_state(c->state, &state) && fe::expect_holds(e, state, &why),
              "recorded: " + e.check + " shows " + e.key + e.op + e.value + " (" + why + ")");
    }
    std::ofstream out(g_replay_path, std::ios::binary);
    out << fe::format_replay(
        g_replay, {"Recorded by play_real_test regress_record (#91, ADR 0104): a fresh install, "
                   "the title, Play, the intro,",
                   "the first map (2.8), the tutorial to 2.9 and the snail of quest 5. Pad "
                   "events at steps of the virtual",
                   "clock (1 ms a run-loop turn; the game's 50 ms tick is 50 steps). Our own "
                   "data: no game content.",
                   "scripts/regress plays it; docs/testing/regression-replay.md."});
    out.close();
    check(static_cast<bool>(out), "the replay is written to " + g_replay_path.string());
    std::ofstream log(r.out / "checkpoints.txt", std::ios::binary);
    log << r.checkpoints.text();
    std::printf("regress: %zu events, %zu checkpoints, digest %s\n", g_replay.events.size(),
                r.checkpoints.points().size(), r.checkpoints.digest().c_str());
}

int regress_setup(Run &r) {
    g_replay_path = replay_path(r);
    std::ifstream in(g_replay_path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string error;
    if (!in.good() && text.empty()) {
        std::printf("FAIL: MT_REPLAY: cannot read %s\n", g_replay_path.string().c_str());
        return 1;
    }
    if (!fe::parse_replay(text, &g_replay, &error)) {
        std::printf("FAIL: %s: %s\n", g_replay_path.string().c_str(), error.c_str());
        return 1;
    }
    if (g_replay.step_us * 1000 != r.step_ns) {
        std::printf("FAIL: %s: step_us %llu (the driver steps %llu ns)\n",
                    g_replay_path.string().c_str(),
                    static_cast<unsigned long long>(g_replay.step_us),
                    static_cast<unsigned long long>(r.step_ns));
        return 1;
    }
    r.replay = &g_replay;
    r.plan = fe::CheckpointPlan(g_replay);
    r.end_step = g_replay.end_step();
    std::printf("regress: %s: %zu events, %zu named checks, every %llu, ends at step %llu\n",
                g_replay_path.string().c_str(), g_replay.events.size(), r.plan.named(),
                static_cast<unsigned long long>(g_replay.every),
                static_cast<unsigned long long>(r.end_step));
    return 0;
}

void regress(Run &r, u64) { regress_common(r); }

void regress_checks(Run &r, u64) {
    check(r.step() == r.end_step, "the replay ran to its end (step " + std::to_string(r.step()) +
                                      " of " + std::to_string(r.end_step) + ")");
    std::size_t named = 0;
    for (const fe::ReplayEvent &e : g_replay.events) {
        if (e.kind == fe::ReplayKind::kCheck) {
            named++;
            check(r.checkpoints.find(e.name) != nullptr, "checkpoint " + e.name + " was taken");
        }
    }
    for (const fe::ReplayExpect &e : g_replay.expects) {
        const fe::Checkpoint *c = r.checkpoints.find(e.check);
        std::map<std::string, std::string> state;
        std::string why = "no checkpoint";
        check(c != nullptr && fe::parse_state(c->state, &state) && fe::expect_holds(e, state, &why),
              e.check + " shows " + e.key + e.op + e.value + " (" + why + ")");
    }
    std::ofstream log(r.out / "checkpoints.txt", std::ios::binary);
    log << r.checkpoints.text();
    log.close();
    check(static_cast<bool>(log), "checkpoints.txt is written");
    std::printf("regress: %zu checkpoints (%zu named), digest %s\n", r.checkpoints.points().size(),
                named, r.checkpoints.digest().c_str());
}

// --- save states in process (#203, ADR 0105 and its addendum, docs/re/save-states.md) -----------
//
// save_state: the replay MT_REPLAY (tests/regress/title_to_combat.replay) with an in-process
// save and restore at two points: the tutorial's first map (2.8, from step 46,000, the fork
// prototype's #190) and the hunting field 2.9 before the snail fight (from step 73,627: the
// player attacks the snails, one dies and drops, the `combat` check). At each, the first turn
// at or after the point's step where mt_snapshot_safe holds and the pad is let go (the replay
// holds nothing) is the save: mt_snapshot_save into memory in that turn's hook, the harness's
// own counters kept beside it (its turns, ticks, GL frames, the replay's cursor, the audio hash,
// the last GL frame and the monitor: the frontend's, not the game's). 200 game ticks follow with a
// checkpoint after each (the composited screen's hash, the mixer's hash, the state line): the
// uninterrupted run. Then, at the first turn where a restore may run, everything held is let go
// (the pad, the frontend's layers: save_state_input.h), mt_snapshot_restore puts the state back
// (the virtual clock rebased to it), the harness's counters too, and the same 200 ticks run
// again: their checkpoints must equal the first run's. The run then goes on, so the second
// point also checks that the game after a restore plays on as it would have.
// OUT_DIR: point<N>_uninterrupted.txt, point<N>_restored.txt, census.txt (the first point's).
// For finding a difference: MT_SAVE_STATE_DEBUG=1 prints every GL frame's hash ("glframe"),
// MT_SAVE_STATE_DUMP=<step> writes the GL frames of that step (gl_<step>_<n>_WxH.rgba, one per
// run), MT_SAVE_STATE_TRACE=<step> every GL call of 60 steps from there (trace_<pass>.txt).
struct SavePoint {
    const char *name;
    u64 from;
    SavePoint(const char *n, u64 f) : name(n), from(f) {}
    enum class Phase { kSeek, kFirst, kRestore, kSecond, kDone } phase = Phase::kSeek;
    u64 saved_at = 0, restored_at = 0, ticks_seen = 0, last_tick = 0, end = 0;
    std::vector<std::uint8_t> state;
    double save_ms = 0, restore_ms = 0;
    std::string why;
    // The harness at the save.
    u64 turns = 0, ticks = 0, gl_frames = 0, clock_ns = 0;
    std::int64_t last_frame = 0;
    std::unique_ptr<fe::ReplayCursor> cursor;
    std::unique_ptr<fe::AudioHash> audio;
    std::unique_ptr<fe::Screen> screen;
    Play play; // the monitor's (the state line's kills)
    std::size_t first_index = 0, second_index = 0;
    fe::CheckpointLog first, second;
};
constexpr u64 k_save_ticks = 200;
SavePoint g_save_points[] = {{"map_2_8", 46000}, {"hunting_2_9", 73627}};

// Whether the replay's pad holds nothing at the end of step `step` (every button it pressed
// up again; the replay has no axes).
bool replay_pad_free(u64 step) {
    std::map<int, int> buttons;
    for (const fe::ReplayEvent &e : g_replay.events) {
        if (e.step > step) {
            break;
        }
        if (e.kind == fe::ReplayKind::kPadButton) {
            buttons[e.index] = e.value;
        } else if (e.kind == fe::ReplayKind::kPadAxis && e.value != 0) {
            return false;
        }
    }
    for (const auto &[b, v] : buttons) {
        if (v != 0) {
            return false;
        }
    }
    return true;
}

using CensusMap = std::map<std::string, std::pair<u64, u64>>; // type -> (objects, state bytes)
void census_state(void *user, u32, const mt_objc_state_type *type, const void *) {
    auto &e = (*static_cast<CensusMap *>(user))[type->name];
    e.first++;
    e.second += type->size;
}

// What a state holds now: to stdout and OUT_DIR/census.txt.
void snapshot_census(Run &r, mt_cpu *cpu) {
    std::string text;
    char line[256];
    const auto put = [&](const std::string &s) { text += s + "\n"; };
    mt_mem *mem = mt_mem_from_cpu(cpu);
    u32 nseg = 0;
    const mt_segment *segs = mt_mem_segments(mem, &nseg);
    u64 writable = 0;
    for (u32 i = 0; i < nseg; i++) {
        writable += (segs[i].initprot & MT_PROT_WRITE) != 0 ? segs[i].vmsize : 0;
    }
    mt_heap_stats hs{};
    mt_heap_get_stats(mem, &hs);
    put("census at step " + std::to_string(r.step()) + ": " + regress_state(cpu));
    put("guest: writable segments " + std::to_string(writable) + " bytes");
    std::snprintf(line, sizeof line,
                  "guest: heap top 0x%08X (%u bytes handed out), committed 0x%08X, %u live "
                  "blocks, %u live bytes",
                  hs.top, hs.top - MT_GUEST_HEAP_BASE, hs.committed, hs.live_blocks, hs.live_bytes);
    put(line);
    CensusMap census;
    mt_objc *rt = mt_objc_current();
    mt_objc_each_state(rt, census_state, &census);
    u64 total = 0;
    for (const auto &[name, e] : census) {
        std::snprintf(line, sizeof line, "host state %-28s %6llu objects, %8llu bytes of box",
                      name.c_str(), static_cast<unsigned long long>(e.first),
                      static_cast<unsigned long long>(e.second));
        put(line);
        total += e.first;
    }
    put("host states: " + std::to_string(total) + " in " + std::to_string(census.size()) +
        " types");
    const mt_gles_stats gs = mt_gles_get_stats();
    std::snprintf(line, sizeof line, "gl: %u textures, %u level-0 bytes, %u contexts", gs.textures,
                  gs.texture_bytes, gs.contexts);
    put(line);
    const mt_audio_stats as = mt_audio_get_stats();
    std::snprintf(line, sizeof line,
                  "audio: %u buffers (%u filled), %u sources, %u files opened, %u players (%u "
                  "playing)",
                  as.buffers, as.buffers_filled, as.sources, as.files_opened, as.players,
                  as.players_playing);
    put(line);
    std::fputs(text.c_str(), stdout);
    std::ofstream(r.out / "census.txt", std::ios::binary) << text;
}

int save_state_setup(Run &r) {
    if (const int rc = regress_setup(r); rc != 0) {
        return rc;
    }
    r.plan = fe::CheckpointPlan(); // only the windows' checkpoints, one after each tick
    r.end_step = UINT64_MAX;       // the last point's second window ends the run
    return 0;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The save, in the hook of the point's turn: the state into memory, the harness's counters.
void save_point(Run &r, mt_cpu *cpu, SavePoint &p, std::size_t index) {
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    mt_process_image_sha256(mt_process_current(), meta.binary_sha256);
    std::uint8_t *bytes = nullptr;
    std::size_t size = 0;
    char why[512] = {};
    const auto t0 = std::chrono::steady_clock::now();
    if (mt_snapshot_save(cpu, &meta, &bytes, &size, why, sizeof why) != 0) {
        p.why = why;
        p.phase = SavePoint::Phase::kDone;
        r.end_step = r.step();
        return;
    }
    p.save_ms = ms_since(t0);
    p.state.assign(bytes, bytes + size);
    std::free(bytes);
    p.saved_at = r.step();
    p.turns = r.turns;
    p.ticks = r.ticks;
    p.last_frame = r.last_frame;
    p.gl_frames = r.gl_frames;
    p.clock_ns = r.clock.elapsed_ns();
    p.cursor = std::make_unique<fe::ReplayCursor>(*r.cursor);
    p.audio = std::make_unique<fe::AudioHash>(*r.audio);
    p.screen = std::make_unique<fe::Screen>(r.screen);
    p.play = g_play;
    p.last_tick = r.ticks;
    p.first_index = r.checkpoints.points().size();
    r.plan.add(r.step(), std::string(p.name) + ".t0");
    p.phase = SavePoint::Phase::kFirst;
    std::printf("save_state: %s saved at step %llu (%s): %zu bytes in %.0f ms\n", p.name,
                static_cast<unsigned long long>(p.saved_at), regress_state(cpu).c_str(), size,
                p.save_ms);
    if (index == 0) {
        snapshot_census(r, cpu);
    }
}

void guest_rebase(void *user, std::uint64_t unix_us) {
    static_cast<fe::VirtualClock *>(user)->rebase_to(unix_us);
}

// The restore, in a later turn's hook: everything held let go, the state back, the harness's
// counters back. The turn then ends as the save's turn ended (its checkpoint, the clock's step).
bool restore_point(Run &r, mt_cpu *cpu, SavePoint &p) {
    char why[512] = {};
    if (r.pad != nullptr) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            r.pad->button(static_cast<SDL_GamepadButton>(b), false);
        }
        pump(r);
    }
    fe::RestoreInput layers;
    layers.input = &r.input;
    layers.actions = &r.actions;
    layers.gamepads = &r.gamepads;
    layers.pointer = &r.pointer;
    layers.menu = &r.menu;
    layers.assists = &r.assists;
    layers.scene_skip = &r.scene_skip;
    fe::release_for_restore(layers, r.t);
    mt_snapshot_expect ex = mt_snapshot_expect_default();
    std::uint8_t sha[32] = {};
    mt_process_image_sha256(mt_process_current(), sha);
    ex.binary_sha256 = sha;
    ex.rebase_clock = guest_rebase;
    ex.user = &r.clock;
    const u64 at = r.step();
    const auto t0 = std::chrono::steady_clock::now();
    if (mt_snapshot_restore(cpu, p.state.data(), p.state.size(), &ex, nullptr, why, sizeof why) !=
        0) {
        p.why = why;
        return false;
    }
    p.restore_ms = ms_since(t0);
    p.restored_at = at;
    check(r.clock.elapsed_ns() == p.clock_ns, std::string(p.name) +
                                                  ": the clock is back at the save's time (" +
                                                  std::to_string(r.clock.elapsed_ns()) +
                                                  " ns, saved " + std::to_string(p.clock_ns) + ")");
    r.turns = p.turns;
    r.ticks = p.ticks;
    r.last_frame = p.last_frame;
    r.gl_frames = p.gl_frames;
    r.cursor = std::make_unique<fe::ReplayCursor>(*p.cursor);
    r.audio = std::make_unique<fe::AudioHash>(*p.audio);
    r.screen = *p.screen;
    g_play = p.play;
    p.second_index = r.checkpoints.points().size();
    std::printf("save_state: %s restored at step %llu to step %llu in %.0f ms (%s)\n", p.name,
                static_cast<unsigned long long>(at), static_cast<unsigned long long>(p.saved_at),
                p.restore_ms, regress_state(cpu).c_str());
    return true;
}

void save_state(Run &r, u64) {
    regress_common(r);
    if (std::getenv("MT_SAVE_STATE_TRACE") != nullptr) {
        r.gl_command = [](void *, mt_cpu *, const mt_gles_cmd *c) {
            static u64 from = std::strtoull(std::getenv("MT_SAVE_STATE_TRACE"), nullptr, 10);
            static int pass = 0;
            static u64 last = 0;
            static std::FILE *f = nullptr;
            const u64 step = g_run->step();
            if (step < last) {
                pass++;
                if (f != nullptr) {
                    std::fclose(f);
                    f = nullptr;
                }
            }
            last = step;
            if (step < from || step > from + 60) {
                return;
            }
            if (f == nullptr) {
                f = std::fopen(
                    (g_run->out / ("trace_" + std::to_string(pass) + ".txt")).string().c_str(),
                    "w");
            }
            std::fprintf(f, "%llu op %u r %u:", static_cast<unsigned long long>(step), c->op,
                         c->result);
            for (u32 i = 0; i < c->argc && i < MT_GLES_MAX_ARGS; i++) {
                std::fprintf(f, " %08X", c->args[i]);
            }
            std::fprintf(f, "\n");
        };
    }
    if (std::getenv("MT_SAVE_STATE_DEBUG") != nullptr) {
        r.on_frame = [&r](mt_cpu *) {
            const std::vector<std::uint8_t> &px = r.screen.gl_pixels();
            std::printf("glframe step %llu gl %llu hash %s\n",
                        static_cast<unsigned long long>(r.step()),
                        static_cast<unsigned long long>(r.gl_frames),
                        fe::hex64(fe::fnv1a64(px.data(), px.size())).c_str());
            const char *dump = std::getenv("MT_SAVE_STATE_DUMP");
            if (dump != nullptr && std::strtoull(dump, nullptr, 10) == r.step()) {
                static int n = 0;
                std::ofstream(r.out /
                                  ("gl_" + std::to_string(r.step()) + "_" + std::to_string(n++) +
                                   "_" + std::to_string(r.screen.gl_width()) + "x" +
                                   std::to_string(r.screen.gl_height()) + ".rgba"),
                              std::ios::binary)
                    .write(reinterpret_cast<const char *>(px.data()),
                           static_cast<std::streamsize>(px.size()));
            }
        };
    }
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        for (std::size_t i = 0; i < std::size(g_save_points); i++) {
            SavePoint &p = g_save_points[i];
            const u64 step = r.step();
            switch (p.phase) {
            case SavePoint::Phase::kSeek: {
                if (step < p.from) {
                    return;
                }
                char why[256] = {};
                if (mt_snapshot_safe(cpu, why, sizeof why) == 1 && replay_pad_free(step)) {
                    save_point(r, cpu, p, i);
                } else {
                    p.why = why;
                }
                return;
            }
            case SavePoint::Phase::kFirst:
                if (r.ticks != p.last_tick) {
                    p.last_tick = r.ticks;
                    r.plan.add(step, std::string(p.name) + ".t" + std::to_string(++p.ticks_seen));
                    if (p.ticks_seen == k_save_ticks) {
                        p.end = step;
                        p.phase = SavePoint::Phase::kRestore;
                    }
                }
                return;
            case SavePoint::Phase::kRestore:
                if (step <= p.end) {
                    return; // the window's last checkpoint is taken in this turn
                }
                if (restore_point(r, cpu, p)) {
                    p.phase = SavePoint::Phase::kSecond;
                } else if (step > p.end + 20000) {
                    p.phase = SavePoint::Phase::kDone;
                    r.end_step = step;
                }
                return;
            case SavePoint::Phase::kSecond:
                // The plan takes the window's checkpoints again at their steps.
                if (step == p.end) {
                    p.phase = SavePoint::Phase::kDone;
                    if (i + 1 == std::size(g_save_points)) {
                        r.end_step = step;
                    }
                }
                return;
            case SavePoint::Phase::kDone:
                continue;
            }
        }
    };
}

// One point's checks: saved, restored, both windows' checkpoints taken and equal, the window
// play (OUT_DIR/point<N>_uninterrupted.txt and _restored.txt).
void check_save_point(Run &r, SavePoint &p, std::size_t i) {
    const std::vector<fe::Checkpoint> &all = r.checkpoints.points();
    check(p.saved_at != 0, std::string(p.name) + ": a state is saved at a safe point from step " +
                               std::to_string(p.from) + " (at " + std::to_string(p.saved_at) +
                               (p.saved_at != 0 ? std::string(")") : "; " + p.why + ")"));
    check(p.restored_at != 0, std::string(p.name) + ": the state is restored (at " +
                                  std::to_string(p.restored_at) +
                                  (p.restored_at != 0 ? std::string(")") : "; " + p.why + ")"));
    if (p.saved_at == 0 || p.restored_at == 0) {
        return;
    }
    const std::size_t n = k_save_ticks + 1;
    check(p.second_index >= p.first_index + n && all.size() >= p.second_index + n,
          std::string(p.name) + ": both runs took " + std::to_string(n) + " checkpoints");
    if (all.size() < p.second_index + n || p.second_index < p.first_index + n) {
        return;
    }
    for (std::size_t k = 0; k < n; k++) {
        p.first.add(all[p.first_index + k]);
        p.second.add(all[p.second_index + k]);
    }
    const std::string tag = "point" + std::to_string(i + 1);
    std::ofstream(r.out / (tag + "_uninterrupted.txt"), std::ios::binary) << p.first.text();
    std::ofstream(r.out / (tag + "_restored.txt"), std::ios::binary) << p.second.text();
    // The window is play, not a still screen: the frames and the hero change.
    std::set<std::string> frames, places, audio;
    for (const fe::Checkpoint &c : p.first.points()) {
        frames.insert(fe::hex64(c.frame));
        audio.insert(fe::hex64(c.audio));
        std::map<std::string, std::string> s;
        fe::parse_state(c.state, &s);
        places.insert(s["x"] + "," + s["y"]);
    }
    check(frames.size() > k_save_ticks / 4 && places.size() > 10 && audio.size() > 10,
          std::string(p.name) + ": the window is play: " + std::to_string(frames.size()) +
              " distinct frames, " + std::to_string(audio.size()) + " audio hashes, the hero at " +
              std::to_string(places.size()) + " places");
    const fe::LogDifference d = fe::compare_logs(p.first, p.second);
    check(d.same, std::string(p.name) + ": the restored run's " +
                      std::to_string(p.second.points().size()) +
                      " checkpoints equal the uninterrupted run's" +
                      (d.same ? std::string()
                              : " (first difference at " + std::to_string(d.index) + ":\n  " + d.a +
                                    "\n  " + d.b + ")"));
    const fe::Checkpoint &last = p.first.points().back();
    std::map<std::string, std::string> s;
    fe::parse_state(last.state, &s);
    std::printf("save_state: %s: steps %llu..%llu, %zu checkpoints, digest %s (restored %s), "
                "last audio %s/%llu frames, %s, kills %s; save %.0f ms, restore %.0f ms, "
                "%zu bytes\n",
                p.name, static_cast<unsigned long long>(p.first.points().front().step),
                static_cast<unsigned long long>(last.step), p.first.points().size(),
                p.first.digest().c_str(), p.second.digest().c_str(), fe::hex64(last.audio).c_str(),
                static_cast<unsigned long long>(last.audio_frames), s["map"].c_str(),
                s["kills"].c_str(), p.save_ms, p.restore_ms, p.state.size());
}

void save_state_checks(Run &r, u64) {
    for (std::size_t i = 0; i < std::size(g_save_points); i++) {
        check_save_point(r, g_save_points[i], i);
    }
    // The hunting field's window is a fight: a mob defeated within it.
    const SavePoint &hunt = g_save_points[1];
    if (!hunt.first.points().empty()) {
        std::map<std::string, std::string> a, b;
        fe::parse_state(hunt.first.points().front().state, &a);
        fe::parse_state(hunt.first.points().back().state, &b);
        check(a["map"] == "2.9" && std::stoull(b["kills"]) > std::stoull(a["kills"]),
              "the hunting window is on 2.9 and a mob is defeated in it (kills " + a["kills"] +
                  " -> " + b["kills"] + ")");
    }
}

// --- quick-save and quick-load through the frontend (#204, quick_state.h) ----------------------
//
// quick_state: the replay MT_REPLAY with save_states on and the frontend's own path, as game.cpp's
// turn hook runs it: at the hunting field 2.9 (from step 73,627, save_state's second point), the
// first turn where mt_snapshot_safe holds and the replay's pad is free, F5 is pressed and let go
// through the keyboard's default bindings (Actions), and QuickStates takes the quick_save and runs
// it with game_quick_ops: the state written to OUT_DIR/data/states/slot<N>.state through
// mt_saves_replace_file. 200 game ticks follow with a checkpoint after each (save_state's harness).
// Then F9 the same way: quick_load reads the file, checks its header, lets go of the pad and the
// frontend (release_for_restore), restores with the virtual clock rebased, and the same 200 ticks
// run again: their checkpoints (ADR 0104) must equal the first run's. The game's save files
// (saves/theif*, quest*) are hashed before and after each of the two actions: unchanged.
// Before that, at step 40,000 (an NPC chat in the tutorial, game mode 4: no safe point), F5 and
// F9 are refused with the player's reason and write nothing.
fe::QuickStates g_quick;
constexpr u64 k_quick_refused_step = 40000;
std::string g_refused_save, g_refused_load; // the notices there
bool g_refused_wrote = false;               // a state file appeared there
SavePoint g_quick_point{"quick_2_9", 73627};
using SaveHashes = std::map<std::string, std::string>;
SaveHashes g_saves[4]; // before and after the save, before and after the load
std::string g_state_path;

// The game's save files in saves/ (theif<N>, quest<N>) by name, each its bytes' hash.
SaveHashes game_save_hashes(const Run &r) {
    SaveHashes out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(r.data / "saves", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("theif", 0) != 0 && name.rfind("quest", 0) != 0) {
            continue;
        }
        std::ifstream in(e.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
        out[name] = fe::hex64(fe::fnv1a64(bytes.data(), bytes.size()));
    }
    return out;
}

// A key pressed and let go through the keyboard's bindings, as keyboard.h does.
void tap_key(Run &r, SDL_Keycode k) {
    const fe::Control c{fe::ControlKind::kKey, static_cast<int>(k)};
    r.actions.control(fe::InputSource::kKeyboard, c, true);
    r.actions.control(fe::InputSource::kKeyboard, c, false);
}

// game_quick_ops on this run: before the restore, the replay's pad and the frontend let go.
fe::QuickOps run_quick_ops(Run &r, mt_cpu *cpu) {
    return fe::game_quick_ops(
        cpu,
        [&r] {
            if (r.pad != nullptr) {
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
                    r.pad->button(static_cast<SDL_GamepadButton>(b), false);
                }
                pump(r);
            }
            fe::RestoreInput layers;
            layers.input = &r.input;
            layers.actions = &r.actions;
            layers.gamepads = &r.gamepads;
            layers.pointer = &r.pointer;
            layers.menu = &r.menu;
            layers.assists = &r.assists;
            layers.scene_skip = &r.scene_skip;
            fe::release_for_restore(layers, r.t);
        },
        guest_rebase, &r.clock);
}

// F5 at the point's turn: the save through QuickStates; the harness's counters kept.
void quick_save_point(Run &r, mt_cpu *cpu, SavePoint &p) {
    g_saves[0] = game_save_hashes(r);
    tap_key(r, SDLK_F5);
    const fe::QuickRequest q = g_quick.take(r.actions, true);
    const auto t0 = std::chrono::steady_clock::now();
    const fe::QuickResult res = g_quick.run(q, run_quick_ops(r, cpu), r.data.string());
    g_saves[1] = game_save_hashes(r);
    std::printf("quick_state: F5 at step %llu: %s (notice \"%s\")\n",
                static_cast<unsigned long long>(r.step()), res.log.c_str(), res.notice.c_str());
    if (q != fe::QuickRequest::kSave || !res.done) {
        p.why = res.log.empty() ? "F5 made no quick_save request" : res.log;
        p.phase = SavePoint::Phase::kDone;
        r.end_step = r.step();
        return;
    }
    p.save_ms = ms_since(t0);
    const fe::AutosaveCheck c = fe::check_autosave(cpu);
    g_state_path = fe::quick_state_path(r.data.string(), c.slot);
    p.state = fe::read_quick_state(g_state_path).value_or(std::vector<std::uint8_t>{});
    p.saved_at = r.step();
    p.turns = r.turns;
    p.ticks = r.ticks;
    p.last_frame = r.last_frame;
    p.gl_frames = r.gl_frames;
    p.clock_ns = r.clock.elapsed_ns();
    p.cursor = std::make_unique<fe::ReplayCursor>(*r.cursor);
    p.audio = std::make_unique<fe::AudioHash>(*r.audio);
    p.screen = std::make_unique<fe::Screen>(r.screen);
    p.play = g_play;
    p.last_tick = r.ticks;
    p.first_index = r.checkpoints.points().size();
    r.plan.add(r.step(), std::string(p.name) + ".t0");
    p.phase = SavePoint::Phase::kFirst;
}

// F9 at a later turn: the load through QuickStates; the harness's counters back. False if it
// was refused (the next turn tries again).
bool quick_load_point(Run &r, mt_cpu *cpu, SavePoint &p) {
    const u64 at = r.step();
    const SaveHashes before = game_save_hashes(r);
    tap_key(r, SDLK_F9);
    const fe::QuickRequest q = g_quick.take(r.actions, true);
    const auto t0 = std::chrono::steady_clock::now();
    const fe::QuickResult res = g_quick.run(q, run_quick_ops(r, cpu), r.data.string());
    if (q != fe::QuickRequest::kLoad || !res.done) {
        p.why = res.log;
        return false;
    }
    g_saves[2] = before;
    g_saves[3] = game_save_hashes(r);
    p.restore_ms = ms_since(t0);
    p.restored_at = at;
    std::printf("quick_state: F9 at step %llu: %s (notice \"%s\")\n",
                static_cast<unsigned long long>(at), res.log.c_str(), res.notice.c_str());
    check(r.clock.elapsed_ns() == p.clock_ns, "the clock is back at the quick-save's time");
    r.turns = p.turns;
    r.ticks = p.ticks;
    r.last_frame = p.last_frame;
    r.gl_frames = p.gl_frames;
    r.cursor = std::make_unique<fe::ReplayCursor>(*p.cursor);
    r.audio = std::make_unique<fe::AudioHash>(*p.audio);
    r.screen = *p.screen;
    g_play = p.play;
    p.second_index = r.checkpoints.points().size();
    return true;
}

int quick_state_setup(Run &r) {
    if (const int rc = save_state_setup(r); rc != 0) {
        return rc;
    }
    r.settings.save_states = true; // the Assist on (the turn hook's gate is QuickStates::take's)
    return 0;
}

void quick_state(Run &r, u64) {
    regress_common(r);
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        SavePoint &p = g_quick_point;
        const u64 step = r.step();
        switch (p.phase) {
        case SavePoint::Phase::kSeek: {
            if (step == k_quick_refused_step) {
                const fe::QuickOps ops = run_quick_ops(r, cpu);
                tap_key(r, SDLK_F5);
                g_refused_save =
                    g_quick.run(g_quick.take(r.actions, true), ops, r.data.string()).notice;
                tap_key(r, SDLK_F9);
                g_refused_load =
                    g_quick.run(g_quick.take(r.actions, true), ops, r.data.string()).notice;
                std::error_code ec;
                g_refused_wrote = fs::exists(r.data / "states", ec);
                std::printf("quick_state: at step %llu (%s): \"%s\", \"%s\"\n",
                            static_cast<unsigned long long>(step), regress_state(cpu).c_str(),
                            g_refused_save.c_str(), g_refused_load.c_str());
            }
            if (step < p.from) {
                return;
            }
            char why[256] = {};
            if (mt_snapshot_safe(cpu, why, sizeof why) == 1 && replay_pad_free(step)) {
                quick_save_point(r, cpu, p);
            } else {
                p.why = why;
            }
            return;
        }
        case SavePoint::Phase::kFirst:
            if (r.ticks != p.last_tick) {
                p.last_tick = r.ticks;
                r.plan.add(step, std::string(p.name) + ".t" + std::to_string(++p.ticks_seen));
                if (p.ticks_seen == k_save_ticks) {
                    p.end = step;
                    p.phase = SavePoint::Phase::kRestore;
                }
            }
            return;
        case SavePoint::Phase::kRestore:
            if (step <= p.end) {
                return;
            }
            if (quick_load_point(r, cpu, p)) {
                p.phase = SavePoint::Phase::kSecond;
            } else if (step > p.end + 20000) {
                p.phase = SavePoint::Phase::kDone;
                r.end_step = step;
            }
            return;
        case SavePoint::Phase::kSecond:
            if (step == p.end) {
                p.phase = SavePoint::Phase::kDone;
                r.end_step = step;
            }
            return;
        case SavePoint::Phase::kDone:
            return;
        }
    };
}

void quick_state_checks(Run &r, u64) {
    SavePoint &p = g_quick_point;
    check_save_point(r, p, 0);
    check(g_refused_save == "Not saved: not in a menu" &&
              g_refused_load == "Not loaded: not in a menu" && !g_refused_wrote,
          "in the NPC chat at step " + std::to_string(k_quick_refused_step) +
              " F5 and F9 are refused, with the reason, and write nothing (\"" + g_refused_save +
              "\", \"" + g_refused_load + "\")");
    check(g_quick.stats().saves == 1 && g_quick.stats().loads == 1 && g_quick.stats().refused == 2,
          "one quick_save and one quick_load ran, two were refused (" +
              std::to_string(g_quick.stats().saves) + ", " + std::to_string(g_quick.stats().loads) +
              "; " + std::to_string(g_quick.stats().refused) + " refused)");
    std::error_code ec;
    check(!g_state_path.empty() && fs::is_regular_file(g_state_path, ec) && !p.state.empty(),
          "the state is a file of its own: " + g_state_path + " (" +
              std::to_string(p.state.size()) + " bytes)");
    check(!fs::exists(g_state_path + MT_SAVES_TMP_SUFFIX, ec), "no temporary file is left");
    check(!g_saves[0].empty(),
          "the game has save files at the quick-save (" + std::to_string(g_saves[0].size()) + ")");
    check(g_saves[0] == g_saves[1], "the quick-save leaves the game's save files byte for byte");
    check(g_saves[2] == g_saves[3], "the quick-load leaves the game's save files byte for byte");
    for (const auto &[name, hash] : g_saves[0]) {
        std::printf("quick_state: %s %s\n", name.c_str(), hash.c_str());
    }
}

// --- the save-state census (#202, ADR 0105, docs/re/save-states.md) -----------------------------
//
// save_state_census: the replay MT_REPLAY from step MT_SNAPSHOT_STEP (default 46,000, near the
// fork prototype's point: map 2.8, the field) to the first turn where mt_snapshot_safe holds with
// the game's own words (game mode, popup, key pad panel) and no key held (the replay's hero walks
// there, and a held key is refused), at most k_census_window steps on. There no live host state
// type may lack a serializer (mt_snapshot_missing, #203): a type without one fails the test by
// name. A full save is made there in memory and read back; a partial one (MT_SNAPSHOT_PARTIAL)
// too, deflated and stored. Nothing is written to disk.
constexpr u64 k_census_window = 20000;

struct CensusRun {
    u64 step = 46000, taken_at = 0;
    bool taken = false, safe = false, full_refused = false, partial_saved = false,
         partial_read = false, full_read = false;
    std::size_t full_bytes = 0;
    std::string safe_why, full_why, read_why, state;
    std::vector<std::string> missing;
    std::size_t partial_bytes = 0;
};
CensusRun g_census;

int save_state_census_setup(Run &r) {
    if (const int rc = regress_setup(r); rc != 0) {
        return rc;
    }
    if (const char *n = std::getenv("MT_SNAPSHOT_STEP"); n != nullptr && *n != 0) {
        g_census.step = std::strtoull(n, nullptr, 10);
    }
    r.plan = fe::CheckpointPlan();
    r.end_step = g_census.step + k_census_window;
    return 0;
}

void snapshot_census_turn(mt_cpu *cpu) {
    CensusRun &c = g_census;
    c.taken = true;
    char why[512] = {};
    const char *names[64] = {};
    const u32 n = mt_snapshot_missing(mt_objc_current(), names, 64);
    for (u32 i = 0; i < n && i < 64; i++) {
        c.missing.emplace_back(names[i]);
    }
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    mt_process_image_sha256(mt_process_current(), meta.binary_sha256);
    std::uint8_t *bytes = nullptr;
    std::size_t size = 0;
    c.full_refused = mt_snapshot_save(cpu, &meta, &bytes, &size, why, sizeof why) == -1;
    c.full_why = why;
    if (!c.full_refused) {
        mt_snapshot_expect ex = mt_snapshot_expect_default();
        ex.binary_sha256 = meta.binary_sha256;
        c.full_bytes = size;
        c.full_read = mt_snapshot_read_header(bytes, size, &ex, nullptr, why, sizeof why) == 0;
        c.full_why = why;
        std::printf("save_state_census: a full state of %zu bytes\n", size);
    }
    std::free(bytes);
    meta.flags = MT_SNAPSHOT_PARTIAL;
    const auto t0 = std::chrono::steady_clock::now();
    c.partial_saved = mt_snapshot_save(cpu, &meta, &bytes, &size, why, sizeof why) == 0;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (c.partial_saved) {
        c.partial_bytes = size;
        std::printf("save_state_census: the partial save took %.0f ms\n", ms);
        mt_snapshot_expect ex = mt_snapshot_expect_default();
        ex.binary_sha256 = meta.binary_sha256;
        mt_snapshot_header h{};
        c.partial_read = mt_snapshot_read_header(bytes, size, &ex, &h, why, sizeof why) == 0;
        c.read_why = why;
        std::printf("save_state_census: a partial state of %zu bytes, %u sections, port %s, guest "
                    "time %llu us\n",
                    size, h.sections, h.port_version,
                    static_cast<unsigned long long>(h.guest_unix_us));
        std::uint8_t *stored = nullptr;
        std::size_t stored_size = 0;
        meta.flags = MT_SNAPSHOT_PARTIAL | MT_SNAPSHOT_STORE;
        if (mt_snapshot_save(cpu, &meta, &stored, &stored_size, why, sizeof why) == 0) {
            std::printf("save_state_census: %zu bytes stored (not deflated)\n", stored_size);
        }
        std::free(stored);
    } else {
        c.read_why = why;
    }
    std::free(bytes);
}

void save_state_census(Run &r, u64) {
    regress_common(r);
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        CensusRun &c = g_census;
        if (r.step() < c.step || c.taken) {
            return;
        }
        char why[256] = {};
        c.safe = mt_snapshot_safe(cpu, why, sizeof why) == 1;
        c.safe_why = why;
        if (c.safe) {
            c.taken_at = r.step();
            c.state = regress_state(cpu);
            snapshot_census_turn(cpu);
            r.end_step = r.step();
        }
    };
}

void save_state_census_checks(Run &, u64) {
    const CensusRun &c = g_census;
    check(c.taken && c.safe, "a safe point came within " + std::to_string(k_census_window) +
                                 " steps of step " + std::to_string(c.step) + " (at " +
                                 std::to_string(c.taken_at) + ", " + c.state +
                                 "; the last refusal: " + c.safe_why + ")");
    std::string listed;
    for (const std::string &name : c.missing) {
        listed += (listed.empty() ? "" : ", ") + name;
    }
    check(c.missing.empty(), "every live host state type has a serializer" +
                                 (listed.empty() ? std::string() : " (none: " + listed + ")"));
    std::printf("save_state_census: %zu live types without a serializer: %s\n", c.missing.size(),
                listed.c_str());
    check(!c.full_refused, "a full save is made (" + c.full_why + ")");
    check(c.full_read, "the full state reads back (" + c.full_why + ")");
    check(c.partial_saved, "a partial state is saved (" + c.read_why + ")");
    check(c.partial_read, "the partial state reads back (" + c.read_why + ")");
}

// The modes: the tasks a mode queues on the Run, and its checks after common_checks.
// `ticks`: the default TICKS, the game ticks of play common_checks asks for. `relaunch`: OUT_DIR
// and its data directory are kept from the run before (a second process on the first one's
// saves), not started afresh. `setup`, if set, runs before the boot and returns 0, or 77 to
// skip the mode.
struct Mode {
    const char *name;
    void (*queue)(Run &, u64);
    void (*checks)(Run &, u64);
    u64 ticks;
    bool relaunch = false;
    int (*setup)(Run &) = nullptr;
};
// --- the HUD extras (#191, ADR 0109, docs/re/hud-extras.md) --------------------------------------
//
// hud_extras: first_map's route to 2.3 (the tutorial's snail on 2.9 on the way) and a hunt there
// until a Red Potion has been picked up and the hero has gained a level on 2.3 (at most
// TICKS + 9,000 ticks), with exp_percent on. Every field turn:
//   - the EXP figure (exp_percent.h): read_exp's level and EXP are guest_state's hero's, its
//     need is the bundle's hero.dat row of the level, and the figure's whole percent is the
//     status tab's (EXP x 100 / need, PopupMenu_PaintStatusTab 0x56321); through level-ups;
//   - the game's own mob HP bar (the research's evidence that the port's would draw it twice):
//     a hit the mob lives on (its HP +0x88 down, above 0) starts the hit counter +0xD0 that
//     CMob::Paint 0x72685 draws the bar for, the counter runs 20 ticks unless the mob is hit or
//     killed meanwhile, and the bar's figures are the mob's HP and max HP +0x90, which is the
//     bundle's mob.dat HP of its id +0xA0;
//   - the game's own pick-up lines (likewise for the pick-up log): each Red Potion picked up
//     (item 3's count up) has a "Red Potion Obtained!" line in the CStateGame's notify list
//     +0x4C, the meso gained is the sum of the "Meso obtained.(+N)" lines, and no line stays more
//     than 5 s.
// Shots (hud_extras_NN.png: the screen at 1x with the figure laid over it as the window draws it;
// mob_hit_NN.png: the frame after a hit) stay in the test output directory, never committed.

// hero.dat's EXP column and mob.dat's HP column from the bundle (docs/formats/data-tables.md).
struct BundleTables {
    std::map<int, i32> need; // level -> EXP the level needs
    std::map<int, i32> mob_hp;
    bool ok = false;
};
BundleTables bundle_tables() {
    BundleTables t;
    const fs::path dir = fs::path(MT_BUNDLE_DIR) / "English" / "data";
    const auto rows = [](const std::vector<std::uint8_t> &b, int stride_want, auto each) {
        if (b.size() < 6) {
            return false;
        }
        const int rows = b[2] | (b[3] << 8), stride = b[4] | (b[5] << 8);
        if (stride != stride_want || b.size() != 6u + static_cast<std::size_t>(rows) * stride) {
            return false;
        }
        for (int r = 0; r < rows; r++) {
            each(&b[6 + static_cast<std::size_t>(r) * stride]);
        }
        return true;
    };
    const bool hero_ok =
        rows(world_map::read_bytes(dir / "hero.dat"), 18, [&t](const std::uint8_t *row) {
            i32 v = 0;
            std::memcpy(&v, row + 1, 4);
            t.need[row[0]] = v;
        });
    // mob.dat: id u8 +0, name char[16] +1, max HP i32 +17.
    const bool mob_ok =
        rows(world_map::read_bytes(dir / "mob.dat"), 50, [&t](const std::uint8_t *row) {
            i32 v = 0;
            std::memcpy(&v, row + 17, 4);
            t.mob_hp[row[0]] = v;
        });
    t.ok = hero_ok && mob_ok;
    return t;
}

// A line of the CStateGame's notify list +0x4C (CNotifyTextList: entries +0x04, a PtrArray;
// each CNotifyText: its wrapped lines +0x04 (char *[]), their count +0x08, its time +0x10).
struct NotifyLine {
    u32 addr = 0;
    std::uint64_t time = 0;
    std::string text;
};
std::vector<NotifyLine> notify_lines(const mt_cpu *cpu) {
    std::vector<NotifyLine> out;
    const u32 st = game_state(cpu);
    const u32 list = st != 0 && rd(cpu, st) == fe::kAutosaveGameVptr ? rd(cpu, st + 0x4C) : 0;
    for (const u32 e : list_items(cpu, list != 0 ? rd(cpu, list + 0x04) : 0, 20)) {
        const u32 lines = e != 0 ? rd(cpu, e + 0x04) : 0;
        const u32 n = e != 0 ? std::min(rd(cpu, e + 0x08), 4u) : 0;
        NotifyLine l;
        l.addr = e;
        l.time = rd(cpu, e + 0x10) | (std::uint64_t{rd(cpu, e + 0x14)} << 32);
        for (u32 i = 0; lines != 0 && i < n; i++) {
            const u32 s = rd(cpu, lines + 4 * i);
            for (u32 c = 0; s != 0 && c < 120 && s + c < cpu->mem_size && cpu->mem[s + c] != 0;
                 c++) {
                l.text += static_cast<char>(cpu->mem[s + c]);
            }
        }
        out.push_back(l);
    }
    return out;
}

struct HudExtrasRun {
    BundleTables tables;
    u64 field_ticks = 0, last_tick = 0;
    bool was_field = false;
    u64 hunt_start = 0, field_turns = 0, exp_turns = 0, exp_bad = 0, need_bad = 0, pct_bad = 0;
    std::string exp_first_bad;
    std::set<std::string> texts;
    std::set<int> levels;
    // Mobs: the last HP and hit counter per mob, the open hits (the tick each began).
    struct MobSeen {
        i32 id = 0, hp = 0, d0 = 0;
        u64 hit_tick = 0;
        bool open = false;
    };
    std::map<u32, MobSeen> mobs;
    u64 hits = 0, kills = 0, bars_one_second = 0, hits_counter_bad = 0, bars_closed = 0,
        bars_bad_length = 0, max_hp_bad = 0;
    u64 bar_ticks_min = UINT64_MAX, bar_ticks_max = 0;
    std::string hit_first_bad, hit_example;
    // Pick-ups.
    i32 last_potions = -1, last_meso = -1;
    u64 potion_pickups = 0, potion_lines = 0, meso_gains = 0, meso_lines = 0;
    std::int64_t meso_gained = 0, meso_in_lines = 0;
    std::set<std::pair<u32, std::uint64_t>> seen_lines;
    std::map<std::pair<u32, std::uint64_t>, std::pair<double, double>> line_life; // first, last
    std::vector<std::string> sample_lines;
    int shots = 0, hit_shots = 0;
    double next_shot = 0;
    u64 levels_on_23 = 0;
    i32 level_at_23 = -1;
};
std::shared_ptr<HudExtrasRun> g_hud_extras;

// The screen at 1x with the EXP figure laid over it at its window place, to OUT/name.png.
void hud_extras_shot(Run &r, mt_cpu *cpu, const std::string &figure, const std::string &name) {
    if (!r.screen.compose(cpu, mt_cf_absolute_time())) {
        return;
    }
    std::vector<std::uint8_t> px = r.screen.pixels();
    const int w = static_cast<int>(r.screen.width()), h = static_cast<int>(r.screen.height());
    if (!figure.empty()) {
        const fe::Rect image{0, 0, static_cast<float>(w), static_cast<float>(h)};
        const fe::Rect box = fe::exp_percent_rect(image, figure);
        const int unit = fe::hud_label_unit(fe::exp_percent_pixels(image));
        const std::vector<std::uint8_t> fig = fe::render_exp_percent(figure, unit);
        const int bw = fe::exp_percent_units_w(figure) * unit, bh = fe::kExpPercentUnitsH * unit;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                const int sx = static_cast<int>(box.x) + x, sy = static_cast<int>(box.y) + y;
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
                    continue;
                }
                const std::uint8_t *s = &fig[(static_cast<std::size_t>(y) * bw + x) * 4];
                std::uint8_t *d = &px[(static_cast<std::size_t>(sy) * w + sx) * 4];
                for (int c = 0; c < 3; c++) { // premultiplied over
                    d[c] = static_cast<std::uint8_t>(s[c] + (d[c] * (255 - s[3]) + 127) / 255);
                }
            }
        }
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4);
    if (surf != nullptr) {
        SDL_SavePNG(surf, (r.out / (name + ".png")).string().c_str());
        SDL_DestroySurface(surf);
    }
}

void hud_extras_tick(Run &r, mt_cpu *cpu) {
    HudExtrasRun &m = *g_hud_extras;
    // The field's ticks: the game updates its mobs (CMob::Flush counts the bar down) only there,
    // not under a popup (the death popup), a menu or a map change.
    const bool in_field = field(cpu);
    if (in_field && m.was_field) {
        m.field_ticks += r.ticks - m.last_tick;
    }
    m.was_field = in_field;
    m.last_tick = r.ticks;
    if (!in_field) {
        return;
    }
    m.field_turns++;
    const Hero h = hero(cpu);
    // The EXP figure.
    const fe::ExpView v = fe::read_exp(fe::guest_span(cpu));
    std::string figure;
    if (v.shown) {
        m.exp_turns++;
        figure = fe::exp_percent_text(v.exp, v.needed);
        m.texts.insert(figure);
        m.levels.insert(v.level);
        const auto need = m.tables.need.find(h.level);
        const bool need_ok = need != m.tables.need.end() && need->second == v.needed;
        const bool exp_ok = v.exp == h.exp && v.level == h.level;
        // The status tab's whole percent: the figure's integer part.
        const std::string whole = "EXP " +
                                  std::to_string(static_cast<long long>(std::int64_t{h.exp} * 100 /
                                                                        std::max(v.needed, 1))) +
                                  ".";
        const bool pct_ok = figure.rfind(whole, 0) == 0;
        m.exp_bad += exp_ok ? 0 : 1;
        m.need_bad += need_ok ? 0 : 1;
        m.pct_bad += pct_ok ? 0 : 1;
        if ((!exp_ok || !need_ok || !pct_ok) && m.exp_first_bad.empty()) {
            m.exp_first_bad = "level " + std::to_string(h.level) + " EXP " + std::to_string(h.exp) +
                              ": read " + std::to_string(v.level) + " / " + std::to_string(v.exp) +
                              " / " + std::to_string(v.needed) + " (" + figure + ")";
        }
    }
    if (h.on(2, 3)) {
        if (m.level_at_23 < 0) {
            m.level_at_23 = h.level;
        }
        m.levels_on_23 = static_cast<u64>(std::max(0, h.level - m.level_at_23));
    }
    // The game's own mob bars.
    for (const Mob &mob : mobs(cpu)) {
        const i32 d0 = rdi(cpu, mob.addr + 0xD0), max_hp = rdi(cpu, mob.addr + 0x90);
        HudExtrasRun::MobSeen &s = m.mobs[mob.addr];
        const bool same = s.id == mob.id && s.hp != 0;
        if (same && mob.hp < s.hp && mob.hp <= 0) {
            // A kill: CMob::Die leaves the counter as it was (TakeHit starts it only for a mob
            // that lives on), so the bar of an earlier hit runs out as before.
            m.kills++;
            s.open = false;
        } else if (same && mob.hp < s.hp) {
            m.hits++;
            // The hit set the counter (1, or 2 if the tick's update ran after it).
            const bool counter_ok = d0 >= 1 && d0 <= 2;
            const auto want = m.tables.mob_hp.find(mob.id);
            const bool max_ok = want != m.tables.mob_hp.end() && want->second == max_hp;
            m.hits_counter_bad += counter_ok ? 0 : 1;
            m.max_hp_bad += max_ok ? 0 : 1;
            if ((!counter_ok || !max_ok) && m.hit_first_bad.empty()) {
                m.hit_first_bad = "mob " + std::to_string(mob.id) + " HP " + std::to_string(s.hp) +
                                  " -> " + std::to_string(mob.hp) + " / " + std::to_string(max_hp) +
                                  ", counter " + std::to_string(d0);
            }
            if (m.hit_example.empty()) {
                m.hit_example = "mob " + std::to_string(mob.id) + " (" + std::to_string(mob.addr) +
                                "): HP " + std::to_string(s.hp) + " -> " + std::to_string(mob.hp) +
                                " of " + std::to_string(max_hp) + ", bar " +
                                std::to_string(max_hp > 0 ? 40 * mob.hp / max_hp : 0) +
                                " of 40 points";
            }
            s.open = true;
            s.hit_tick = m.field_ticks;
            if (m.hit_shots < 3) {
                hud_extras_shot(r, cpu, "", "mob_hit_" + std::to_string(m.hit_shots++));
            }
        }
        if (s.open && mob.id != s.id) {
            s.open = false; // the list's memory reused for another mob
        }
        if (s.open && d0 == 0) {
            // The bar goes when the counter has run to 20 (CMob::Flush: 1 a tick of the mob's
            // update); a mob the update skips for a while keeps it longer.
            s.open = false;
            m.bars_closed++;
            const u64 len = m.field_ticks - s.hit_tick;
            m.bar_ticks_min = std::min(m.bar_ticks_min, len);
            m.bar_ticks_max = std::max(m.bar_ticks_max, len);
            m.bars_bad_length += s.d0 >= 19 && len >= 18 ? 0 : 1;
            m.bars_one_second += len >= 18 && len <= 22 ? 1 : 0;
        }
        s.id = mob.id;
        s.hp = mob.hp;
        s.d0 = d0;
    }
    // The game's own pick-up lines, while on 2.3 (the item and the line come in one call).
    const i32 potions = item_count(cpu, h, 3);
    const bool on_23 = h.on(2, 3);
    if (m.last_potions >= 0 && potions > m.last_potions && on_23) {
        m.potion_pickups += static_cast<u64>(potions - m.last_potions);
    }
    if (m.last_meso >= 0 && h.meso > m.last_meso && on_23) {
        m.meso_gains++;
        m.meso_gained += h.meso - m.last_meso;
    }
    m.last_potions = potions;
    m.last_meso = h.meso;
    for (const NotifyLine &l : notify_lines(cpu)) {
        const auto key = std::make_pair(l.addr, l.time);
        auto life = m.line_life.find(key);
        if (life == m.line_life.end()) {
            m.line_life[key] = {r.t, r.t};
        } else {
            life->second.second = r.t;
        }
        if (!m.seen_lines.insert(key).second) {
            continue;
        }
        if (m.sample_lines.size() < 12) {
            m.sample_lines.push_back(l.text);
        }
        if (!on_23) {
            continue;
        }
        const std::string meso = "Meso obtained.(+";
        if (l.text == "Red Potion Obtained!") {
            m.potion_lines++;
        } else if (l.text.rfind(meso, 0) == 0) {
            m.meso_lines++;
            m.meso_in_lines += std::atoi(l.text.c_str() + meso.size());
        }
    }
    if (h.on(2, 3) && r.t >= m.next_shot && m.shots < 4) {
        hud_extras_shot(r, cpu, figure, "hud_extras_" + std::to_string(m.shots++));
        m.next_shot = r.t + 20;
    }
}

void hud_extras(Run &r, u64 goal) {
    attach_pad(r);
    g_hud_extras = std::make_shared<HudExtrasRun>();
    g_hud_extras->tables = bundle_tables();
    r.settings.exp_percent = true;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        hud_extras_tick(r, cpu);
    };
    then_new_game(r);
    then_tutorial(r);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_do(r, "the hunt begins", [&r](mt_cpu *cpu) {
        g_play.hunting = true;
        g_play.hunt_level = hero(cpu).level;
        g_play.hunt_events = mt_game_events_get_stats();
        g_hud_extras->hunt_start = r.ticks;
        std::printf("hud_extras: the hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(
        r, "map 2.3 until a Red Potion and a level (at least " + std::to_string(goal) + " ticks)",
        [&r, goal](mt_cpu *) {
            const HudExtrasRun &m = *g_hud_extras;
            const u64 t = r.ticks - m.hunt_start;
            return (t >= goal && m.potion_pickups > 0 && m.levels_on_23 > 0) || t >= goal + 9000;
        },
        static_cast<double>(goal + 9000) * 0.05 * 1.5 + 60, {2, 0, 42, 2, 3});
    then_finish(r);
}

void hud_extras_checks(Run &, u64) {
    const HudExtrasRun &m = *g_hud_extras;
    double longest = 0;
    for (const auto &[key, life] : m.line_life) {
        longest = std::max(longest, life.second - life.first);
    }
    std::string sample;
    for (const std::string &s : m.sample_lines) {
        sample += " \"" + s + "\"";
    }
    std::printf("hud_extras: %llu field turns, %llu with the figure; levels", //
                static_cast<unsigned long long>(m.field_turns),
                static_cast<unsigned long long>(m.exp_turns));
    for (const int l : m.levels) {
        std::printf(" %d", l);
    }
    std::printf("; %zu figures (from %s to %s)\n", m.texts.size(),
                m.texts.empty() ? "-" : m.texts.begin()->c_str(),
                m.texts.empty() ? "-" : m.texts.rbegin()->c_str());
    std::printf("hud_extras: %llu hits, %llu kills, %llu bars closed after %llu-%llu ticks; %s\n",
                static_cast<unsigned long long>(m.hits), static_cast<unsigned long long>(m.kills),
                static_cast<unsigned long long>(m.bars_closed),
                static_cast<unsigned long long>(m.bars_closed != 0 ? m.bar_ticks_min : 0),
                static_cast<unsigned long long>(m.bar_ticks_max), m.hit_example.c_str());
    std::printf("hud_extras: %llu Red Potions picked up (%llu lines), %llu meso gains (%llu "
                "lines); the longest line %.2f s; lines%s\n",
                static_cast<unsigned long long>(m.potion_pickups),
                static_cast<unsigned long long>(m.potion_lines),
                static_cast<unsigned long long>(m.meso_gains),
                static_cast<unsigned long long>(m.meso_lines), longest, sample.c_str());
    check(m.tables.ok, "the bundle's hero.dat and mob.dat read");
    check(m.field_turns > 0 && m.exp_turns == m.field_turns,
          "the figure shows in every field turn (" + std::to_string(m.exp_turns) + " of " +
              std::to_string(m.field_turns) + ")");
    check(m.exp_bad == 0 && m.need_bad == 0 && m.pct_bad == 0,
          "the figure is the hero's level and EXP over hero.dat's need, the status tab's whole "
          "percent (" +
              std::to_string(m.exp_bad) + " / " + std::to_string(m.need_bad) + " / " +
              std::to_string(m.pct_bad) + " turns off" +
              (m.exp_first_bad.empty() ? "" : "; first: " + m.exp_first_bad) + ")");
    check(m.levels.size() >= 3 && m.texts.size() >= 10,
          "the figure follows the EXP through levels (" + std::to_string(m.levels.size()) +
              " levels, " + std::to_string(m.texts.size()) + " figures)");
    check(m.hits >= 10 && m.hits_counter_bad == 0 && m.max_hp_bad == 0,
          "each hit a mob lives on starts the game's bar (CMob +0xD0) over its HP and its "
          "mob.dat max HP (" +
              std::to_string(m.hits) + " hits, " + std::to_string(m.hits_counter_bad) +
              " without the counter, " + std::to_string(m.max_hp_bad) + " with another max" +
              (m.hit_first_bad.empty() ? "" : "; first: " + m.hit_first_bad) + ")");
    check(m.bars_closed > 0 && m.bars_bad_length == 0 && m.bars_one_second * 2 > m.bars_closed,
          "the game's bar goes when its counter has run to 20, mostly 20 field ticks (1 s) after "
          "the hit (" +
              std::to_string(m.bars_closed) + " closed, " + std::to_string(m.bars_one_second) +
              " after 18-22 ticks, " + std::to_string(m.bars_bad_length) +
              " before the counter reached 20)");
    check(m.potion_pickups > 0 && m.potion_lines == m.potion_pickups,
          "each Red Potion picked up has the game's \"Red Potion Obtained!\" line (" +
              std::to_string(m.potion_lines) + " of " + std::to_string(m.potion_pickups) + ")");
    check(m.meso_gains > 0 && m.meso_in_lines == m.meso_gained,
          "the meso gained on 2.3 is the sum of the game's \"Meso obtained.(+N)\" lines (" +
              std::to_string(m.meso_gained) + " in " + std::to_string(m.meso_gains) + " gains, " +
              std::to_string(m.meso_in_lines) + " in " + std::to_string(m.meso_lines) + " lines)");
    check(longest > 4 && longest <= 5.1,
          "the game's lines stay at most 5 s (the longest " + std::to_string(longest) + " s)");
}

// --- the game's rules, traced (#214, docs/re/mechanics.md) ---------------------------------------
//
// mechanics: first_map's route to 2.3 and a hunt there until TICKS ticks and 60 kills (at most
// TICKS + 9,000 ticks). Every field turn it reads the hero and the mobs from guest memory and
// checks what changed against the rules docs/re/mechanics.md states, with the bundle's mob.dat
// and hero.dat as the inputs:
//   - damage: each HP loss of a mob that lives on is one hit of the basic attack with the hero's
//     dagger (the starting Wooden Club). From the hero's attack A (+0xA4), x = A * (A / 30 + 3)
//     / 5 (CHero's damage roll 0x64DA1); a normal hit lies in [(x - x / 3) * 110 / 100,
//     (x + x / 5) * 110 / 100] and a critical one (CMob::TakeHit 0x73715) is twice a normal
//     one. A killing hit takes at most that much;
//   - the hero's damage: each HP loss of the hero is one mob contact (CHero::CheckMobContact
//     0x65F05 -> 0x6F591), in the range of some mob on the map from its level +0x98 and the
//     hero's defense +0xA8;
//   - EXP: a kill adds the mob's mob.dat EXP (CMob::Die 0x73349 -> CHero::setExp 0x6654D),
//     carried over hero.dat's need through level-ups;
//   - drops: every drop that appears in a kill's turn is one the kill may give (0x5CAD9): the
//     mob's equipment id, one of its three etc ids, or meso (item_etc 0, 1 or 2 by amount) of
//     80 % to 120 % of its mob.dat meso; and the rates seen next to the rates the rules give.
// It prints a line per rule with what it saw, for the issue's evidence.
struct MechMob {
    i32 hp = 0, level = 0, exp = 0, meso = 0, equip = 0, equip_chance = 0;
    i32 etc[3] = {0, 0, 0}, etc_chance[3] = {0, 0, 0};
};
struct MechRun {
    std::map<int, MechMob> mob;
    std::map<int, i32> need; // hero.dat: level -> EXP the level needs
    bool ok = false;
    bool was_field = false;
    u64 hunt_start = 0;
    // The last turn.
    std::map<u32, std::pair<i32, i32>> mob_hp; // address -> (id, HP)
    std::set<u32> drop_addrs;
    Hero last{};
    i32 last_attack = -1, last_defense = -1, last_potions = -1;
    // Damage.
    u64 hits = 0, normal = 0, crits = 0, kill_hits = 0, hit_bad = 0;
    std::map<i32, std::pair<i32, i32>> seen_by_attack; // A -> (min, max) of the normal hits
    std::string hit_first_bad;
    // The hero's damage.
    u64 hero_hits = 0, hero_hit_bad = 0;
    std::string hero_hit_first_bad, hero_hit_example;
    // EXP.
    u64 kills = 0, exp_ok = 0, exp_bad = 0, level_ups = 0;
    std::string exp_first_bad, exp_example;
    // Drops.
    u64 drops = 0, drop_bad = 0, meso_drops = 0, etc_drops = 0, equip_drops = 0;
    i32 meso_min = INT32_MAX, meso_max = INT32_MIN;
    std::map<int, u64> kills_of;               // mob id -> kills
    std::map<std::pair<int, int>, u64> etc_of; // (mob id, slot) -> drops
    std::map<int, u64> meso_of, equip_of;      // mob id -> drops
    std::string drop_first_bad, drop_example;
};
std::shared_ptr<MechRun> g_mech;

void mech_tables(MechRun &m) {
    const fs::path dir = fs::path(MT_BUNDLE_DIR) / "English" / "data";
    const auto rows = [](const std::vector<std::uint8_t> &b, int stride_want, auto each) {
        if (b.size() < 6) {
            return false;
        }
        const int n = b[2] | (b[3] << 8), stride = b[4] | (b[5] << 8);
        if (stride != stride_want || b.size() != 6u + static_cast<std::size_t>(n) * stride) {
            return false;
        }
        for (int i = 0; i < n; i++) {
            each(&b[6 + static_cast<std::size_t>(i) * stride]);
        }
        return true;
    };
    const auto i16 = [](const std::uint8_t *p) {
        return static_cast<i32>(static_cast<std::int16_t>(p[0] | (p[1] << 8)));
    };
    const auto i32le = [](const std::uint8_t *p) {
        i32 v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const bool hero_ok = rows(world_map::read_bytes(dir / "hero.dat"), 18,
                              [&](const std::uint8_t *row) { m.need[row[0]] = i32le(row + 1); });
    // mob.dat (docs/formats/data-tables.md): hp i32 +0x11, level i16 +0x15, exp i16 +0x19,
    // meso i16 +0x1B, equip u8 +0x1D and its chance i32 +0x1E, etc slot i: u8 +0x22 + 5i, its
    // chance i32 +0x23 + 5i.
    const bool mob_ok =
        rows(world_map::read_bytes(dir / "mob.dat"), 50, [&](const std::uint8_t *row) {
            MechMob &o = m.mob[row[0]];
            o.hp = i32le(row + 0x11);
            o.level = i16(row + 0x15);
            o.exp = i16(row + 0x19);
            o.meso = i16(row + 0x1B);
            o.equip = row[0x1D];
            o.equip_chance = i32le(row + 0x1E);
            for (int i = 0; i < 3; i++) {
                o.etc[i] = row[0x22 + 5 * i];
                o.etc_chance[i] = i32le(row + 0x23 + 5 * i);
            }
        });
    m.ok = hero_ok && mob_ok;
}

// The basic attack's normal range from the hero's attack (CHero's damage roll 0x64DA1, skill 0,
// no star): C's truncating division, as the game's __divsi3.
std::pair<i32, i32> mech_basic_range(i32 a) {
    i32 x = a * (a / 30 + 3) / 5;
    if (x < 0) {
        x = 10;
    }
    return {(x + (-x) / 3) * 110 / 100, (x + x / 5) * 110 / 100};
}

// A mob's contact damage range (0x6F591) from its level and the hero's defense.
std::pair<i32, i32> mech_contact_range(i32 level, i32 defense) {
    const i32 d = level * level / 30 + level * 2 + 20 - defense * 7 / 3;
    if (d < 0) {
        return {1 * 90 / 100, (level >> 1) * 90 / 100};
    }
    return {(d - 2 - d / 4) * 90 / 100, (d + d / 4 + 2) * 90 / 100};
}

void mech_tick(Run &, mt_cpu *cpu) {
    MechRun &m = *g_mech;
    const bool in_field = field(cpu);
    const Hero h = hero(cpu);
    if (!in_field || h.addr == 0) {
        m.was_field = false;
        m.mob_hp.clear();
        m.drop_addrs.clear();
        return;
    }
    const i32 attack = rdi(cpu, h.addr + 0xA4), defense = rdi(cpu, h.addr + 0xA8);
    const i32 potions = item_count(cpu, h, 3) + item_count(cpu, h, 4) + item_count(cpu, h, 5);
    // The hero's weapon (+0xDC[1]) must be a dagger (item +0x8C = 2) for the basic attack's rule.
    const u32 weapon = rd(cpu, rd(cpu, h.addr + 0xDC) + 4);
    const bool dagger = weapon != 0 && rdi(cpu, weapon + 0x8C) == 2;
    const bool same = m.was_field && m.last.addr == h.addr && h.on(m.last.area, m.last.map);
    std::vector<int> killed;
    std::map<u32, std::pair<i32, i32>> now;
    for (const Mob &mob : mobs(cpu)) {
        now[mob.addr] = {mob.id, mob.hp};
        const auto it = m.mob_hp.find(mob.addr);
        if (!same || it == m.mob_hp.end() || it->second.first != mob.id || it->second.second <= 0 ||
            mob.hp >= it->second.second) {
            continue;
        }
        const i32 dealt = it->second.second - mob.hp;
        m.hits++;
        bool ok = false;
        // The attack before or after this turn (a level-up in the same turn raises it).
        for (const i32 a : {m.last_attack, attack}) {
            const auto [lo, hi] = mech_basic_range(a);
            if (mob.hp <= 0) {
                ok = ok || (dealt >= 1 && dealt <= 2 * hi);
            } else if (dealt >= lo && dealt <= hi) {
                ok = true;
                auto &seen = m.seen_by_attack[a];
                if (seen.first == 0 && seen.second == 0) {
                    seen = {dealt, dealt};
                }
                seen.first = std::min(seen.first, dealt);
                seen.second = std::max(seen.second, dealt);
            } else if (dealt >= 2 * lo && dealt <= 2 * hi && dealt % 2 == 0) {
                ok = true;
            }
        }
        if (mob.hp <= 0) {
            m.kill_hits++;
            killed.push_back(mob.id);
        } else {
            const auto [lo, hi] = mech_basic_range(attack);
            if (dealt >= lo && dealt <= hi) {
                m.normal++;
            } else if (dealt >= 2 * lo && dealt <= 2 * hi) {
                m.crits++;
            }
        }
        if (!ok || !dagger) {
            m.hit_bad++;
            if (m.hit_first_bad.empty()) {
                const auto [lo, hi] = mech_basic_range(attack);
                m.hit_first_bad =
                    "mob " + std::to_string(mob.id) + " lost " + std::to_string(dealt) + " HP (" +
                    std::to_string(it->second.second) + " -> " + std::to_string(mob.hp) +
                    ") at attack " + std::to_string(attack) + ": normal " + std::to_string(lo) +
                    "-" + std::to_string(hi) + (dagger ? "" : ", no dagger");
            }
        }
    }
    // The hero's damage: an HP loss in a turn without a potion drunk or a level-up.
    if (same && h.hp < m.last.hp && potions == m.last_potions && h.level == m.last.level) {
        const i32 lost = m.last.hp - h.hp;
        m.hero_hits++;
        bool ok = false;
        std::string ranges;
        for (const Mob &mob : mobs(cpu)) {
            const auto [lo, hi] = mech_contact_range(rdi(cpu, mob.addr + 0x98), m.last_defense);
            ok = ok || (h.hp > 0 ? lost >= lo && lost <= hi : lost <= hi);
            if (ranges.size() < 60) {
                ranges += " " + std::to_string(mob.id) + ":" + std::to_string(lo) + "-" +
                          std::to_string(hi);
            }
        }
        if (m.hero_hit_example.empty() && ok) {
            m.hero_hit_example = "HP " + std::to_string(m.last.hp) + " -> " + std::to_string(h.hp) +
                                 " at defense " + std::to_string(m.last_defense) + "; ranges" +
                                 ranges;
        }
        if (!ok) {
            m.hero_hit_bad++;
            if (m.hero_hit_first_bad.empty()) {
                m.hero_hit_first_bad = "HP " + std::to_string(m.last.hp) + " -> " +
                                       std::to_string(h.hp) + " at defense " +
                                       std::to_string(m.last_defense) + "; ranges" + ranges;
            }
        }
    }
    // EXP: the kills' mob.dat EXP through hero.dat's needs (CHero::setExp, up to level 99).
    if (same && !killed.empty()) {
        i32 level = m.last.level, exp = m.last.exp, gain = 0;
        for (const int id : killed) {
            gain += m.mob[id].exp;
            m.kills++;
            m.kills_of[id]++;
        }
        exp += gain;
        while (level < 99 && m.need.count(level) != 0 && exp >= m.need[level]) {
            exp -= m.need[level];
            level++;
        }
        if (level >= 99) {
            exp = 0;
        }
        const bool ok = level == h.level && exp == h.exp;
        m.level_ups += static_cast<u64>(std::max(0, h.level - m.last.level));
        (ok ? m.exp_ok : m.exp_bad)++;
        const std::string text = "level " + std::to_string(m.last.level) + " EXP " +
                                 std::to_string(m.last.exp) + " + " + std::to_string(gain) +
                                 " -> level " + std::to_string(h.level) + " EXP " +
                                 std::to_string(h.exp) + " (rule: " + std::to_string(level) +
                                 " / " + std::to_string(exp) + ")";
        if (!ok && m.exp_first_bad.empty()) {
            m.exp_first_bad = text;
        }
        if (h.level > m.last.level && m.exp_example.empty()) {
            m.exp_example = text;
        }
    }
    // Drops: the new ones of this turn, each one a kill of this turn may give.
    std::set<u32> drop_now;
    for (const Drop &d : drops(cpu)) {
        drop_now.insert(d.addr);
        if (!same || m.drop_addrs.count(d.addr) != 0) {
            continue;
        }
        m.drops++;
        const i32 table = rdi(cpu, d.addr + 0x84), count = rdi(cpu, d.addr + 0x100);
        bool ok = false;
        std::string what;
        for (const int id : killed) {
            const MechMob &o = m.mob[id];
            if (table == 0) {
                what = "equip " + std::to_string(d.id);
                if (o.equip_chance > 0 && d.id == o.equip && count == 1) {
                    ok = true;
                    m.equip_drops++;
                    m.equip_of[id]++;
                    break;
                }
            } else if (d.id <= 2) {
                what = "meso " + std::to_string(count) + " (item " + std::to_string(d.id) + ")";
                const i32 lo = o.meso * 8 / 10, hi = o.meso * 12 / 10;
                const int band = count < 100 ? 0 : count < 200 ? 1 : 2;
                if (count >= lo && count <= hi && count > 0 && d.id == band) {
                    ok = true;
                    m.meso_drops++;
                    m.meso_of[id]++;
                    m.meso_min = std::min(m.meso_min, count);
                    m.meso_max = std::max(m.meso_max, count);
                    break;
                }
            } else {
                what = "etc " + std::to_string(d.id) + " x" + std::to_string(count);
                for (int i = 0; i < 3 && !ok; i++) {
                    if (o.etc_chance[i] > 0 && o.etc[i] == d.id && count == 1) {
                        ok = true;
                        m.etc_drops++;
                        m.etc_of[{id, i}]++;
                    }
                }
                if (ok) {
                    break;
                }
            }
        }
        if (ok && m.drop_example.empty()) {
            m.drop_example = what + " from mob " + std::to_string(killed.front());
        }
        if (!ok) {
            m.drop_bad++;
            if (m.drop_first_bad.empty()) {
                m.drop_first_bad = what + " with " + std::to_string(killed.size()) + " kills";
            }
        }
    }
    m.drop_addrs = drop_now;
    m.mob_hp = now;
    m.last = h;
    m.last_attack = attack;
    m.last_defense = defense;
    m.last_potions = potions;
    m.was_field = true;
}

void mechanics(Run &r, u64 goal) {
    attach_pad(r);
    g_mech = std::make_shared<MechRun>();
    mech_tables(*g_mech);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        mech_tick(r, cpu);
    };
    then_new_game(r);
    then_tutorial(r);
    then_walk_to(r, 42);
    then_portal(r, 2, 3);
    then_do(r, "the hunt begins", [&r](mt_cpu *cpu) {
        g_play.hunting = true;
        g_play.hunt_level = hero(cpu).level;
        g_play.hunt_events = mt_game_events_get_stats();
        g_mech->hunt_start = r.ticks;
        std::printf("mechanics: the hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(r, "map 2.3 until 60 kills (at least " + std::to_string(goal) + " ticks)",
              [&r, goal](mt_cpu *) {
                  const MechRun &m = *g_mech;
                  const u64 t = r.ticks - m.hunt_start;
                  return (t >= goal && m.kills >= 60) || t >= goal + 9000;
              },
              static_cast<double>(goal + 9000) * 0.05 * 1.5 + 60, {2, 0, 42, 2, 3});
    then_finish(r);
}

void mechanics_checks(Run &, u64) {
    const MechRun &m = *g_mech;
    std::string ranges;
    for (const auto &[a, seen] : m.seen_by_attack) {
        const auto [lo, hi] = mech_basic_range(a);
        ranges += " A" + std::to_string(a) + ": rule " + std::to_string(lo) + "-" +
                  std::to_string(hi) + ", seen " + std::to_string(seen.first) + "-" +
                  std::to_string(seen.second) + ";";
    }
    std::printf("mechanics: damage: %llu hits (%llu normal, %llu critical, %llu killing), %llu "
                "off the rule;%s\n",
                static_cast<unsigned long long>(m.hits), static_cast<unsigned long long>(m.normal),
                static_cast<unsigned long long>(m.crits),
                static_cast<unsigned long long>(m.kill_hits),
                static_cast<unsigned long long>(m.hit_bad), ranges.c_str());
    std::printf("mechanics: the hero's damage: %llu hits, %llu off the rule; e.g. %s\n",
                static_cast<unsigned long long>(m.hero_hits),
                static_cast<unsigned long long>(m.hero_hit_bad), m.hero_hit_example.c_str());
    std::printf("mechanics: EXP: %llu kills in %llu turns as the rule, %llu off, %llu level-ups; "
                "e.g. %s\n",
                static_cast<unsigned long long>(m.kills), static_cast<unsigned long long>(m.exp_ok),
                static_cast<unsigned long long>(m.exp_bad),
                static_cast<unsigned long long>(m.level_ups), m.exp_example.c_str());
    std::printf("mechanics: drops: %llu new (%llu meso, %d-%d; %llu etc; %llu equipment), %llu "
                "off the rule; e.g. %s\n",
                static_cast<unsigned long long>(m.drops),
                static_cast<unsigned long long>(m.meso_drops), m.meso_drops ? m.meso_min : 0,
                m.meso_drops ? m.meso_max : 0, static_cast<unsigned long long>(m.etc_drops),
                static_cast<unsigned long long>(m.equip_drops),
                static_cast<unsigned long long>(m.drop_bad), m.drop_example.c_str());
    // The rates seen per mob, next to the rules' (meso 80 %; an etc slot (chance + level / 10)
    // in Rand_Next() % 1000; equipment (chance + level) in Rand_Next() % 100000, where
    // Rand_Next() is 15 bits, so it is (chance + level) / 32768).
    for (const auto &[id, n] : m.kills_of) {
        const MechMob &o = m.mob.at(id);
        const auto rate = [n](u64 k) {
            return n != 0 ? 100.0 * static_cast<double>(k) / static_cast<double>(n) : 0.0;
        };
        std::printf("mechanics: mob %d: %llu kills; meso %.0f %% (rule 80 %%)", id,
                    static_cast<unsigned long long>(n),
                    rate(m.meso_of.count(id) ? m.meso_of.at(id) : 0));
        for (int i = 0; i < 3; i++) {
            if (o.etc_chance[i] > 0) {
                const auto k = m.etc_of.find({id, i});
                std::printf("; etc %d %.0f %% (rule about %.1f %% at chance %d)", o.etc[i],
                            rate(k != m.etc_of.end() ? k->second : 0), o.etc_chance[i] / 10.0,
                            o.etc_chance[i]);
            }
        }
        if (o.equip_chance > 0) {
            std::printf("; equip %d %.1f %% (rule about %.1f %% at chance %d)", o.equip,
                        rate(m.equip_of.count(id) ? m.equip_of.at(id) : 0),
                        100.0 * o.equip_chance / 32768.0, o.equip_chance);
        }
        std::printf("\n");
    }
    check(m.ok, "the bundle's hero.dat and mob.dat read");
    check(m.hits >= 60 && m.hit_bad == 0,
          "each mob HP loss is a basic-attack hit in the rule's range, normal or critical (" +
              std::to_string(m.hits) + " hits, " + std::to_string(m.hit_bad) + " off" +
              (m.hit_first_bad.empty() ? "" : "; first: " + m.hit_first_bad) + ")");
    check(m.crits > 0 && m.crits * 10 < m.hits,
          "some hits are critical, a few percent (" + std::to_string(m.crits) + " of " +
              std::to_string(m.hits) + "; the rule: 2 % before the job advancement)");
    check(m.hero_hits > 0 && m.hero_hit_bad == 0,
          "each HP loss of the hero is a mob's contact damage in the rule's range (" +
              std::to_string(m.hero_hits) + " hits, " + std::to_string(m.hero_hit_bad) + " off" +
              (m.hero_hit_first_bad.empty() ? "" : "; first: " + m.hero_hit_first_bad) + ")");
    check(m.kills >= 60 && m.exp_bad == 0 && m.level_ups > 0,
          "each kill adds the mob's mob.dat EXP through hero.dat's needs (" +
              std::to_string(m.kills) + " kills, " + std::to_string(m.exp_bad) + " turns off, " +
              std::to_string(m.level_ups) + " level-ups" +
              (m.exp_first_bad.empty() ? "" : "; first: " + m.exp_first_bad) + ")");
    check(m.drops > 0 && m.drop_bad == 0 && m.meso_drops > 0 && m.etc_drops > 0,
          "each new drop is one the kill may give: its equipment, an etc slot, or 80-120 % of its "
          "meso (" +
              std::to_string(m.drops) + " drops, " + std::to_string(m.drop_bad) + " off" +
              (m.drop_first_bad.empty() ? "" : "; first: " + m.drop_first_bad) + ")");
}

constexpr Mode k_modes[] = {
    {"first_map", first_map, first_map_checks, k_default_ticks},
    {"regress_record", regress_record, regress_record_checks, 1, false, regress_record_setup},
    {"regress", regress, regress_checks, 1, false, regress_setup},
    {"save_state", save_state, save_state_checks, 1, false, save_state_setup},
    {"save_state_census", save_state_census, save_state_census_checks, 1, false,
     save_state_census_setup},
    {"quick_state", quick_state, quick_state_checks, 1, false, quick_state_setup},
    {"save_quit", save_quit, save_quit_checks, 1},
    {"save_relaunch", save_relaunch, save_relaunch_checks, 1, true},
    {"config_speed", config_speed, config_speed_checks, 1},
    {"game_speed", game_speed, game_speed_checks, 1},
    {"cash_shop_meso", cash_shop_meso, cash_shop_checks, 1},
    {"cash_shop_off", cash_shop_off, cash_shop_checks, 1},
    {"chapter_1_a", chapter_1_a, chapter_1_a_checks, 1},
    {"chapter_1_b", chapter_1_b, chapter_1_b_checks, 1},
    {"chapter_1_c", chapter_1_c, chapter_1_c_checks, 1},
    {"chapter_1_d", chapter_1_d, chapter_1_d_checks, 1},
    {"texture_dump", first_map, texture_dump_checks, k_default_ticks, false, texture_dump_setup},
    {"texture_pack", first_map, texture_pack_checks, k_default_ticks, false, texture_pack_setup},
    {"confirm_guard", confirm_guard, confirm_guard_checks, 1},
    {"game_options", game_options, game_options_checks, 1},
    {"autosave_scene", autosave_scene, autosave_scene_checks, 1},
    {"autosave_map", autosave_map, autosave_map_checks, 1, false, autosave_map_setup},
    {"autosave_quit", autosave_quit, autosave_quit_checks, 1},
    {"autosave_exit", autosave_exit, autosave_exit_checks, 1},
    {"autosave_reload", autosave_reload, autosave_reload_checks, 1, true},
    {"popup_menu_tabs", popup_menu_tabs, popup_menu_tabs_checks, 1},
    {"popup_menu_down", popup_menu_down, popup_menu_down_checks, 1},
    {"popup_menu_shoulders", popup_menu_shoulders, popup_menu_shoulders_checks, 1},
    {"widescreen_off", widescreen_flow, widescreen_off_checks, 2400, false, widescreen_off_setup},
    {"widescreen_on", widescreen_flow, widescreen_on_checks, 2400, false, widescreen_on_setup},
    {"popup_menu_scroll", popup_menu_scroll, popup_menu_scroll_checks, 1, false,
     popup_menu_scroll_setup},
    {"screen_shake", screen_shake, screen_shake_checks, 1, false, screen_shake_setup},
    {"flashes", flashes, flashes_checks, 1, false, flashes_setup},
    {"widescreen_pad_off", widescreen_pad, widescreen_pad_checks, 1, false,
     widescreen_pad_off_setup},
    {"widescreen_pad_on", widescreen_pad, widescreen_pad_checks, 1, false, widescreen_pad_on_setup},
    {"widescreen_filters", widescreen_filters, widescreen_filters_checks, 1, false,
     widescreen_filters_setup},
    {"auto_potion", auto_potion, auto_potion_checks, 1},
    {"turbo_attack", turbo_attack, turbo_attack_checks, 1},
    {"ui_animations_normal", ui_animations_normal, ui_animations_checks, 1},
    {"ui_animations_fast", ui_animations_fast, ui_animations_checks, 1},
    {"ui_animations_instant", ui_animations_instant, ui_animations_checks, 1},
    {"chat_text", chat_text, chat_text_checks, 1},
    {"text_pack", text_pack_play, text_pack_checks, 1, false, text_pack_setup},
    {"skip_intros_watched", skip_intros_watched, skip_intros_checks, 1},
    {"skip_intros_skipped", skip_intros_skipped, skip_intros_checks, 1},
    {"skip_intros_boss_watched", skip_intros_boss_watched, skip_intros_checks, 1, false,
     autosave_map_setup},
    {"skip_intros_boss_skipped", skip_intros_boss_skipped, skip_intros_checks, 1, false,
     autosave_map_setup},
    {"auto_loot", auto_loot, auto_loot_checks, 1},
    {"minimap", minimap, minimap_checks, 600},
    {"hud_labels", hud_labels, hud_labels_checks, 1, false, hud_labels_setup},
    {"minimap_wide", minimap_wide, minimap_wide_checks, 600, false, minimap_wide_setup},
    {"auto_run", auto_run, auto_run_checks, 1},
    {"hud_extras", hud_extras, hud_extras_checks, 600},
    {"mechanics", mechanics, mechanics_checks, 3000},
};

int usage() {
    std::printf("usage: play_real_test MODE OUT_DIR [TICKS]; MODE:");
    for (const Mode &m : k_modes) {
        std::printf(" %s", m.name);
    }
    std::printf("\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    const Mode *mode = nullptr;
    for (const Mode &m : k_modes) {
        if (argc >= 2 && std::string(argv[1]) == m.name) {
            mode = &m;
        }
    }
    if (argc < 3 || argc > 4 || mode == nullptr) {
        return usage();
    }
    const u64 goal = argc == 4 ? std::strtoull(argv[3], nullptr, 10) : mode->ticks;
    if (goal == 0) {
        return usage();
    }
    Run r;
    r.mode = argv[1];
    r.out = argv[2];
    r.data = r.out / "data";
    r.trace = "uikit,focus,events";
    if (const char *d = std::getenv("MT_GL_DUMP");
        d != nullptr && std::sscanf(d, "%u-%u", &g_dump.first, &g_dump.last) == 2) {
        r.gl_command = gl_dump;
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    if (mode->relaunch && !fs::is_directory(r.data / "saves")) {
        std::printf("skipped: no saves in %s (the run before it did not save)\n",
                    r.data.string().c_str());
        return 77;
    }
    if (!mode->relaunch) {
        fs::remove_all(r.out);
    }
    fs::create_directories(r.data);
    // A mode that goes on from an earlier run's save (MT_FROM_SAVE: that run's OUT_DIR).
    if (const char *from = std::getenv("MT_FROM_SAVE"); from != nullptr && *from != 0) {
        const fs::path data = fs::path(from) / "data";
        if (!fs::is_directory(data / "saves")) {
            std::printf("FAIL: MT_FROM_SAVE: no save in %s\n", data.string().c_str());
            return 1;
        }
        fs::copy(data, r.data, fs::copy_options::recursive);
        std::printf("the save of %s\n", from);
    }
    // The pads' events reach the test without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    g_run = &r;
    r.focus.attach(r.actions);
    if (mode->setup != nullptr) {
        if (const int skip = mode->setup(r); skip != 0) {
            SDL_Quit();
            return skip;
        }
    }
    mode->queue(r, goal);
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE));
    if (status == 0) {
        common_checks(r, goal);
        mode->checks(r, goal);
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
