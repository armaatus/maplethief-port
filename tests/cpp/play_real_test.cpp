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
//                                             autosave_exit, #179; autosave_death_exit,
//                                             #336);
//                                             widescreen_off, then widescreen_on with
//                                             MT_WIDESCREEN_BASE: CTests play.widescreen_off,
//                                             play.widescreen_on, #86 (and
//                                             widescreen_fill_on: play.widescreen_fill_on,
//                                             #425); widescreen_pad_off, then
//                                             widescreen_pad_on with
//                                             MT_WIDESCREEN_PAD_BASE: CTests
//                                             play.widescreen_pad_off, play.widescreen_pad_on,
//                                             #168 (and widescreen_pad_fill_on:
//                                             play.widescreen_pad_fill_on, #425);
//                                             widescreen_filters: CTest
//                                             play.widescreen_filters, #170;
//                                             widescreen_scene_off, then widescreen_scene_on
//                                             with MT_SCENE and MT_WIDESCREEN_SCENE_BASE:
//                                             CTests play.widescreen_scene_<n>_off_synthetic,
//                                             _on_synthetic, #396; minimap: CTest
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
//                                             docs/re/mechanics.md;
//                                             systems2_*: CTests play.systems2_*_synthetic,
//                                             #304, docs/testing/systems-sweep-2.md;
//                                             map_page: CTests play.map_page_*_synthetic,
//                                             #221)
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
#include <sstream>
#include <string>

#if defined(MT_HAVE_GENERATED_CODE)

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

#include "autosave.h"
#include "crash_report.h"
#include "data_copy.h"
#include "data_notice.h"
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
#include "maplethief/cg.h"
#include "maplethief/events.h"
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/hooks.h"
#include "maplethief/objc/host.h"
#include "maplethief/offline.h"
#include "maplethief/saves.h"
#include "maplethief/snapshot.h"
#include "maplethief/texpack.h"
#include "maplethief/text_events.h"
#include "maplethief/textpack.h"
#include "maplethief/widescreen.h"
#include "minimap.h"
#include "port_text.h"
#include "practice_state.h"
#include "present.h"
#include "price_line.h"
#include "quest_bot.h"
#include "quick_state.h"
#include "recovery.h"
#include "run_timer.h"
#include "save_state_input.h"
#include "save_tools.h"
#include "settings.h"
#include "skill_rules.h"
#include "speech.h"
#include "speech_text.h"
#include "uikit/uikit_internal.h"

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
    if (r.expect_trap) {
        check(r.outcome == MT_PROCESS_TRAPPED, "the run ended in the forced trap");
    } else {
        check(r.outcome == MT_PROCESS_STOPPED && r.done && r.result.trap_kind == 0,
              "the run ended from the script, no trap");
    }
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
        // The forced trap's own report (#335) is the run's point, not an error of it.
        if (r.expect_trap &&
            (line.find("guest crash") != std::string::npos || line.rfind("boot: trap: ", 0) == 0)) {
            continue;
        }
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

// --- the first-start copy (#262, ADR 0127's migration (b), data_copy.h) -------------------------
//
//   save_migrate   play.save_quit's save (MT_MIGRATE_FROM: its OUT_DIR; the slot as recorded)
//                  laid out as a build before ADR 0127 left the per-user folder (Documents/,
//                  gamecenter.json at the top) in OUT_DIR/old-user; a kit layout OUT_DIR/kit
//                  with an empty data/ (the probe's); the first-start copy as main.cpp runs it
//                  (first_start_copy, location game): the notice, the saves byte for byte in
//                  kit/data/saves; then the game on kit/data: save_relaunch's checks (Play, the
//                  first slot, Start: the hero as recorded; saved again, the same bytes). At the
//                  end the old folder is as it was.

std::map<std::string, std::vector<std::uint8_t>> g_migrate_old; // the old folder's files

std::map<std::string, std::vector<std::uint8_t>> files_below(const fs::path &dir) {
    std::map<std::string, std::vector<std::uint8_t>> t;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) {
            t[fs::relative(it->path(), dir).generic_string()] = read_file(it->path());
        }
    }
    return t;
}

int save_migrate_setup(Run &r) {
    const char *from_env = std::getenv("MT_MIGRATE_FROM");
    const fs::path from = from_env != nullptr ? fs::path(from_env) : fs::path();
    if (from.empty() || !fs::is_regular_file(from / "data" / "saves" / "theif0") ||
        !fs::is_regular_file(from / k_state_file)) {
        std::printf("skipped: no save of play.save_quit in MT_MIGRATE_FROM\n");
        return 77;
    }
    // The old per-user folder, in the layout before ADR 0127.
    const fs::path old = r.out / "old-user";
    std::error_code ec;
    fs::create_directories(old / "Documents");
    for (const auto &e : fs::directory_iterator(from / "data" / "saves")) {
        const std::string name = e.path().filename().string();
        fs::copy_file(e.path(), name == "gamecenter.json" ? old / name : old / "Documents" / name,
                      ec);
    }
    for (const auto &e : fs::directory_iterator(from / "data")) {
        if (e.path().filename() != "saves") {
            fs::copy(e.path(), old / e.path().filename(), fs::copy_options::recursive, ec);
        }
    }
    // What save_relaunch compares with.
    for (const fs::path &name :
         {k_state_file, fs::path("saved_theif0"), fs::path("saved_quest0")}) {
        fs::copy_file(from / name, r.out / name, fs::copy_options::overwrite_existing, ec);
    }
    // The slot as play.save_quit saved it (play.save_relaunch, if it ran, saved again since:
    // other never-written heap bytes).
    for (const char *name : {"theif0", "quest0"}) {
        fs::copy_file(from / (std::string("saved_") + name), old / "Documents" / name,
                      fs::copy_options::overwrite_existing, ec);
    }
    g_migrate_old = files_below(old);
    // The kit: the game folder's data/, as the probe left it.
    const fs::path kit = r.out / "kit";
    r.data = kit / "data";
    fs::create_directories(r.data);
    const fe::DataLocation loc = fe::data_location_at(r.data.string(), fe::DataPlace::kGame,
                                                      fe::DataReason::kDefault, kit.string());
    fe::take_data_notices();
    fe::DataCopyResult res;
    const bool ran = fe::first_start_copy(loc, old.string(), &res);
    const std::vector<std::string> notices = fe::take_data_notices();
    check(ran && res.ok && notices.size() == 1 &&
              notices[0] == fe::first_start_copied_text(old.string(), loc.dir),
          "the first start copies the old per-user folder into kit/data, with the notice (" +
              std::to_string(res.copied.size()) + " files" +
              (res.error.empty() ? "" : "; " + res.error) + ")");
    for (const char *name : {"theif0", "quest0"}) {
        check(read_file(r.data / "saves" / name) == read_file(old / "Documents" / name),
              std::string("kit/data/saves/") + name + " is the old Documents/" + name +
                  ", byte for byte");
    }
    check(!fe::first_start_copy(loc, old.string()), "a second start copies nothing");
    return 0;
}

void save_migrate_checks(Run &r, u64) {
    check(!g_migrate_old.empty() && files_below(r.out / "old-user") == g_migrate_old,
          "the old per-user folder is as it was (" + std::to_string(g_migrate_old.size()) +
              " files)");
}

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

void from_save(Run &r, Pred ready = nullptr); // below, with chapter 1's parts

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

// autosave_death_exit (#336): from a save in Henesys, a fall (the game's CHero::TakeDamage), then
// the death popup's B ("Go to main menu", popup 0x6D) and OK on its Yes: CStateGame::OnKeyPressed's
// hook checks the guards before the game leaves for the main menu, which never saves, and the
// dead hero refuses (the dying guard): slot 0 is unchanged. MT_AUTOSAVE_MODE=off: nothing is
// checked.
void fall(mt_cpu *cpu); // below, with the cash shop's runs

void autosave_death_exit(Run &r, u64) {
    const char *mode_env = std::getenv("MT_AUTOSAVE_MODE");
    static fe::Autosave mode = fe::Autosave::kOnQuit;
    mode = mode_env != nullptr && std::string(mode_env) == "off" ? fe::Autosave::kOff
                                                                 : fe::Autosave::kOnQuit;
    g_auto.saver.install_exit_to_menu(&mode, [&r] { return r.t; });
    from_save(r);
    auto before =
        std::make_shared<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>>();
    then_wait_for(r, "the hero stands", 10, settled, 0.5);
    then_do(r, "the files before the fall", [&r, before](mt_cpu *cpu) {
        *before = slot0_files(r);
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup (0x68)", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_press(r, B); // Go to main menu
    then_wait_for(r, "\"Do you really want to go MainMenu?\" (popup 0x6D)", 5,
                  [](mt_cpu *cpu) { return popup_id(cpu) == fe::kAutosaveDeathExitPopup; });
    then_do(r, "the guards on popup 0x6D", [](mt_cpu *cpu) {
        const fe::AutosaveCheck c = fe::check_autosave(cpu, fe::kAutosaveDeathExitPopup);
        check(c.guard == fe::AutosaveGuard::kDying && hero(cpu).hp <= 0,
              "with popup 0x6D let through, the dead hero refuses: " + c.describe());
    });
    then_wait_for(r, "Yes picked (the popup opens on it)", 3, exit_yes_picked);
    then_press(r, A);
    then_wait_for(r, "the main menu (the game left)", 20, [](mt_cpu *cpu) {
        return fe::check_autosave(cpu).guard == fe::AutosaveGuard::kNotInGame;
    });
    then_do(r, "the death exit's check", [&r, before](mt_cpu *) {
        const auto after = slot0_files(r);
        const fe::Autosaver::Stats &st = g_auto.saver.stats();
        if (mode == fe::Autosave::kOnQuit) {
            check(st.saves == 0 && st.skips == 1 && st.last == fe::AutosaveGuard::kDying &&
                      after == *before,
                  "Yes on popup 0x6D checked the guards once and the dying guard refused: slot 0 "
                  "unchanged, as the game");
        } else {
            check(st.saves + st.skips == 0 && after == *before,
                  "with autosave = off the death popup's exit checks nothing and saves nothing");
        }
        release_all(r);
        r.on_turn = nullptr;
        r.done = true;
    });
}
void autosave_death_exit_checks(Run &, u64) {}

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

// The key pad button with target-action `action`: 1 shown, 0 hidden, -1 none (#400, below).
int pad_button(mt_cpu *cpu, const char *action);
void then_touch(Run &r, std::string what, float x, float y); // below

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
    // No MT_OFFLINE_HIDE_CASH: Original, or hide_online_ui with the cash shop selling (the frontend
    // sets the flag only with cash_shop = off; game.cpp). The Cash button and the revives stay.
    mt_offline_ui_configure(0);
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait_field(r);
    then_check(r, "the Cash button is shown (#400)",
               [](mt_cpu *cpu) { return pad_button(cpu, "releaseCashButton:") == 1; });
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
    then_check(r, "the two revive buttons are shown (#400)", [](mt_cpu *cpu) {
        return pad_button(cpu, "releaseRevivalButton:") == 1 &&
               pad_button(cpu, "releaseGradeRevivalButton:") == 1;
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
void cash_shop_checks(Run &, u64) {
    mt_cash_shop_uninstall();
    mt_offline_ui_configure(0);
}

// --- the dead cash buttons hidden (#400, ADR 0084's addendum) ----------------------------------
//
// cash_buttons_hidden: hide_online_ui with cash_shop = off (MT_OFFLINE_HIDE_CASH, as in the
// Enhanced preset), a new game through the tutorial. In Henesys the Cash button is hidden
// (Map and Bag are not), a tap where it was and the pad's LT open no cash shop. A fall: the death
// popup's two revive buttons are hidden (Return to town and Go to main menu are not), the pad's X
// and Y do nothing, A returns to town; a second fall, B opens "go to the main menu?" (0x6D).
// cash_shop_off and cash_shop_meso check the buttons shown (Original; hide_online_ui with meso).

// The state of the key pad's button whose target-action is `action`: 1 shown, 0 hidden, -1 none.
int pad_button(mt_cpu *cpu, const char *action) {
    mt_objc *rt = mt_objc_current();
    std::vector<u32> todo{mt_input_pad_view()};
    while (!todo.empty()) {
        const u32 obj = todo.back();
        todo.pop_back();
        const mt_ui::View *v = obj != 0 ? mt_ui::view_of(cpu, obj) : nullptr;
        if (v == nullptr) {
            continue;
        }
        for (const mt_ui::TargetAction &t : v->actions) {
            const char *name = mt_objc_sel_name(rt, t.action);
            if (name != nullptr && std::strcmp(name, action) == 0) {
                return v->hidden ? 0 : 1;
            }
        }
        todo.insert(todo.end(), v->subviews.begin(), v->subviews.end());
    }
    return -1;
}

std::string pad_buttons_text(mt_cpu *cpu, std::initializer_list<const char *> actions) {
    std::string s;
    for (const char *a : actions) {
        const int st = pad_button(cpu, a);
        s += (s.empty() ? "" : ", ") + std::string(a) +
             (st == 1   ? " shown"
              : st == 0 ? " hidden"
                        : " absent");
    }
    return s;
}

void cash_buttons_hidden(Run &r, u64) {
    mt_cash_shop_install(MT_CASH_SHOP_OFF);
    // The cash flag alone: the new game's driver closes the promo banner, which the links' flag
    // hides (the title's overrides are menus.offline_ui's).
    mt_offline_ui_configure(MT_OFFLINE_HIDE_CASH);
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait_field(r);
    then_do(r, "the field's key pad", [&r](mt_cpu *cpu) {
        check(logged(r, 0, "offline: 2 key pad overrides hide the Cash button"),
              "the two key pad overrides are installed");
        check(pad_button(cpu, "releaseCashButton:") == 0 &&
                  pad_button(cpu, "releaseAsteriskButton:") == 1 &&
                  pad_button(cpu, "releaseClearButton:") == 1,
              "the Cash button is hidden, Map and Bag are not: " +
                  pad_buttons_text(cpu, {"releaseCashButton:", "releaseAsteriskButton:",
                                         "releaseClearButton:"}));
        save_shot(r, cpu, "field");
        g_cash.log0 = r.log.size();
        g_cash.stats0 = mt_cash_shop_get_stats();
    });
    then_touch(r, "a tap where the Cash button was (456, 26)", 456, 26);
    then_wait(r, 2.0);
    then_check(r, "the tap opens no cash shop: the field, panel 0", [](mt_cpu *cpu) {
        return field(cpu) && game_mode(cpu) == 0 && mt_input_panel() == 0;
    });
    then_trigger(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1.5);
    then_check(r, "the pad's LT (cash_shop) opens no cash shop", [](mt_cpu *cpu) {
        return field(cpu) && game_mode(cpu) == 0 && mt_input_panel() == 0;
    });
    then_do(r, "the hero falls (CHero::TakeDamage)", [](mt_cpu *cpu) {
        g_cash.meso0 = hero(cpu).meso;
        g_cash.elixirs0 = item_count(cpu, hero(cpu), 10);
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup (0x68)", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_do(r, "the death popup's buttons", [&r](mt_cpu *cpu) {
        check(pad_button(cpu, "releaseRevivalButton:") == 0 &&
                  pad_button(cpu, "releaseGradeRevivalButton:") == 0 &&
                  pad_button(cpu, "releaseReturnTownButton:") == 1 &&
                  pad_button(cpu, "releaseGoMainMenuButton:") == 1,
              "the two revives are hidden, Return to town and Go to main menu are not: " +
                  pad_buttons_text(cpu, {"releaseGradeRevivalButton:", "releaseRevivalButton:",
                                         "releaseReturnTownButton:", "releaseGoMainMenuButton:"}));
        save_shot(r, cpu, "death_popup");
    });
    then_press(r, X, 0.1, 0.4); // revive (-25): dropped
    then_press(r, Y, 0.1, 0.4); // grade revive (-35): dropped
    then_wait(r, 2.0);
    then_do(r, "X and Y do nothing", [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const mt_cash_stats st = mt_cash_shop_get_stats();
        check(popup_id(cpu) == 0x68 && h.hp <= 0 && h.meso == g_cash.meso0 &&
                  item_count(cpu, h, 10) == g_cash.elixirs0,
              "the death popup stays, the hero down, nothing bought (" + where(cpu) + ")");
        check(!logged(r, g_cash.log0, "the payment is cancelled") &&
                  st.refused == g_cash.stats0.refused,
              "no payment was asked for: the game never saw -25 or -35");
    });
    // Return to town (OK, then OK on its confirmation).
    then(r, "return to town", 40, [&r](mt_cpu *cpu, double el) {
        hold(r, A, popup_id(cpu) != -1 && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && hero(cpu).hp > 0;
    });
    then_wait_field(r);
    then_do(r, "the hero falls again", [](mt_cpu *cpu) { fall(cpu); });
    then_wait_for(
        r, "the death popup again", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_press(r, B); // Go to main menu
    then_wait_for(r, "\"Do you really want to go MainMenu?\" (popup 0x6D)", 5,
                  [](mt_cpu *cpu) { return popup_id(cpu) == 0x6D; });
    then_finish(r);
}

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
// Part a's play after the pad is attached (korean_chapter_1_a plays it too).
void chapter_1_a_steps(Run &r) {
    using namespace quest_bot;
    then_new_game(r);
    then_tutorial(r);
    then_stock(r, "Red Potions from Mr. Wang (Etc20, Henesys)", 2, 3, 20, 100);
    then_farm(r, "5 Red Snails on 2.3 (Elder's Advice)", 2, 3, {2}, defeated(0, 7, 5), 900);
    then_farm(r, "level 10 on 2.3", 2, 3, {}, level_at_least(10), 2400);
    then_talk_to(r, "Stan: Elder's Advice (main 7) done, Find the Dark Lord (8) given", 2, 0,
                 "Stan", 1, main_quests(7, 8));
    then_stock(r, "Red Potions", 2, 3, 40, 200);
    then_talk_to(r, "Darklord: Find the Dark Lord done, Job Advancement Evaluation (21) given", 0,
                 0, "Darklord", 1, main_quests(8, 21));
    then_farm(r, "10 Stumps on 0.4 (Job Advancement Evaluation)", 0, 4, {4}, defeated(0, 21, 10),
              1800);
    then_talk_to(r, "Darklord: the Job Advancement done (the Thief), Depart for Perion (9) given",
                 0, 0, "Darklord", 2, main_quests(21, 9));
    then_save(r);
    then_finish(r);
}
void chapter_1_a(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    chapter_1_a_steps(r);
}
void chapter_1_a_checks(Run &, u64) {}

// The run events (#403, ADR 0156) a part sees: a listener beside the frontend's, installed by
// from_save, keeps the speedrun kinds and prints each one as it comes.
std::vector<mt_game_event> g_run_events;
// The speedrun timer a part times (#406, run_timer.h): fed every event the listener hears.
maplethief::frontend::RunTimer *g_run_timer = nullptr;
void hear_run_event(void *, const mt_game_event *e) {
    if (g_run_timer != nullptr) {
        g_run_timer->on_event(*e);
    }
    if (e->kind < MT_GAME_EVENT_CHAPTER_END || e->kind > MT_GAME_EVENT_MAP_CHANGE) {
        return;
    }
    static const char *const names[] = {"chapter end", "job advance", "boss killed", "map change"};
    std::printf("run event: %s %d %d %d\n", names[e->kind - MT_GAME_EVENT_CHAPTER_END], e->value,
                e->more[0], e->more[1]);
    g_run_events.push_back(*e);
}

// A run event of `kind` with `value` (and `more`, when given) was heard in this part: once, or
// for a map change at least once.
void then_run_event(Run &r, const std::string &what, mt_game_event_kind kind, i32 value,
                    std::vector<i32> more = {}) {
    then_do(r, "run event: " + what, [what, kind, value, more](mt_cpu *) {
        int n = 0;
        for (const mt_game_event &e : g_run_events) {
            const bool same_more =
                more.empty() || (e.more[0] == more[0] && (more.size() < 2 || e.more[1] == more[1]));
            n += e.kind == static_cast<u32>(kind) && e.value == value && same_more ? 1 : 0;
        }
        // A map may be entered more than once on the way; a chapter, a job, a boss only once.
        const bool ok = kind == MT_GAME_EVENT_MAP_CHANGE ? n >= 1 : n == 1;
        check(ok, "run event " + what + ": heard " + std::to_string(n) + " time(s)");
    });
}

// The start of a part that goes on from an earlier part's save (MT_FROM_SAVE): the main menu,
// slot 0, the field.
void from_save(Run &r, Pred ready) {
    g_run_events.clear();
    mt_game_events_add_listener(hear_run_event, nullptr);
    // MT_MAP_DUMP="1.0 2.6": the route planner's platforms and moves of those maps, printed.
    if (const char *dump = std::getenv("MT_MAP_DUMP")) {
        int area = 0, map = 0, n = 0;
        for (const char *p = dump; std::sscanf(p, "%d.%d%n", &area, &map, &n) == 2; p += n) {
            std::printf("map %d.%d:\n%s", area, map,
                        world_map::describe(world_map::get(area, map)).c_str());
        }
    }
    g_play.anywhere = true;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_load_slot0(r, ready);
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
    then_stock(r, "Red Potions", 2, 3, 60, 200);
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

Pred meso_at_least(i32 meso) {
    return [meso](mt_cpu *cpu) { return hero(cpu).meso >= meso; };
}

// Part D: a hero built for Perion, then its main quests. The level-16 hero of part C cannot beat
// the Dark Stumps (level 29) with the plain attack and no armour (#77: 271 falls in 3,600
// game-s), so it first spends its SP on Double Step (then Nimble Body), puts Double Step on LB
// (quick slot 3), buys the level-10 armour and levels to 20 on 0.1, then wields a better dagger.
// Then Missing Master (10: 30 Leaves from the Dark Stumps on 1.2), the level-20 armour, Missing
// Master 2 (11: Bruce), Missing Father (side 23: Ayan's necklace to Bruce), Bruce's Research (12:
// 30 Blue Mushroom Caps on 2.2; 13: Stan), Elder Stan's Assumption (14: Roy), Roy's Advice (15:
// 20 Axe Stumps on 1.3, 20 Dark Stumps; 16 given: on to the Ant Tunnel), and Missing Father (side
// 22) done at Ayan.
void chapter_1_d(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}}; // Double Step, Nimble Body
    FarmStyle fight; // Double Step while MP lasts, Red and Blue Potions from the area's shop
    fight.skill = LB;
    fight.min_mp = 40;
    fight.mp_pct = 30;
    fight.restock = {Restock{3, 100, 300, 25, 400}, Restock{6, 60, 300, 5, 300}};
    then_spend_sp(r, "Double Step, then Nimble Body", build);
    then_quick_slot(r, "Double Step on LB (quick slot 3)", 3, 3);
    then_gear(r, "Red Woolen Hat", 1, 0, 0);
    then_gear(r, "Gidder Shoes", 1, 48, 4);
    then_gear(r, "Red Cloth Vest", 1, 12, 2);
    then_farm(r, "level 20 on 0.1 (Stumps, Orange Mushrooms)", 0, 1, {4, 6}, level_at_least(20),
              2400, fight);
    then_spend_sp(r, "level 20", build);
    then_gear(r, "the Triple-Tipped Zamadar (level 20)", 0, 26, 1);
    then_farm(r, "30 Leaves from the Dark Stumps on 1.2 (Missing Master)", 1, 2, {9}, ready(0, 10),
              2400, fight);
    // The meso this starts with follows the earlier parts' moves: 5,350 took 3,114 game-s, 804
    // more than 3,600 (#298's bot changes).
    then_farm(r, "7,800 meso on 1.2", 1, 2, {9}, meso_at_least(7800), 5400, fight);
    then_spend_sp(r, "Perion", build);
    then_gear(r, "the Red Pao (level 20)", 1, 14, 2, 300);
    then_gear(r, "the Red Loose Cap (level 20)", 1, 2, 0, 300);
    then_gear(r, "the Enamel Boots (level 20)", 1, 50, 4, 300);
    then_talk_to(r, "Manzie: Missing Master (10) done, Missing Master 2 (11) given", 1, 0,
                 "Manzie2", -1, main_quests(10, 11));
    then_talk_to(r, "Bruce: Missing Master 2 (11) done, Bruce's Research (12) given", 2, 0, "Bruce",
                 1, main_quests(11, 12));
    then_talk_to(r, "Bruce: the necklace, Missing Father (side 23) done", 2, 0, "Bruce", 2,
                 quest_is(1, 23, 2));
    then_farm(r, "30 Blue Mushroom Caps on 2.2 (Bruce's Research)", 2, 2, {14}, ready(0, 12), 3600,
              fight);
    then_talk_to(r, "Bruce: Bruce's Research (12) done, 13 given", 2, 0, "Bruce", 1,
                 main_quests(12, 13));
    then_talk_to(r, "Stan: Bruce's Research (13) done, Elder Stan's Assumption (14) given", 2, 0,
                 "Stan", 3, main_quests(13, 14));
    then_spend_sp(r, "Roy's Advice", build);
    then_talk_to(r, "Roy: Elder Stan's Assumption (14) done, Roy's Advice (15) given", 1, 0, "Loy",
                 1, main_quests(14, 15));
    then_farm(
        r, "20 Axe Stumps on 1.3 (Roy's Advice)", 1, 3, {12},
        [](mt_cpu *cpu) { return quest_bot::defeats(cpu, 0, 15, 12) >= 20; }, 3600, fight);
    then_farm(r, "20 Dark Stumps on 1.2 (Roy's Advice)", 1, 2, {9}, ready(0, 15), 3600, fight);
    then_talk_to(r, "Roy: Roy's Advice (15) done, Roy's Advice 2 (16) given", 1, 0, "Loy", 2,
                 main_quests(15, 16));
    then_talk_to(r, "Ayan: Missing Father (side 22) done", 1, 0, "Iyan", 1, quest_is(1, 22, 2));
    then_do(r, "part D's quests in guest memory", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::string main;
        for (u32 n = 10; n <= 16; n++) {
            main += " " + std::to_string(n) + ":" + std::to_string(quest_state(cpu, h, 0, n));
        }
        bool ok = quest_state(cpu, h, 0, 16) == 1;
        for (u32 n = 10; n <= 15; n++) {
            ok = ok && quest_state(cpu, h, 0, n) == 2;
        }
        check(ok, "main quests 10-15 done (2), 16 given (1):" + main);
        check(quest_state(cpu, h, 1, 22) == 2 && quest_state(cpu, h, 1, 23) == 2,
              "side quests 22 and 23 done: 22:" + std::to_string(quest_state(cpu, h, 1, 22)) +
                  " 23:" + std::to_string(quest_state(cpu, h, 1, 23)));
        std::printf("part D: %s, SP %d, Double Step %d, Nimble Body %d\n",
                    hero_text(cpu, h).c_str(), free_sp(cpu, h), skill_level(cpu, h, 3),
                    skill_level(cpu, h, 4));
    });
    then_save(r);
    then_finish(r);
}
void chapter_1_d_checks(Run &, u64) {}

// The quests' states in guest memory, each {sort (0 main, 1 side), number, state}, checked and
// printed ("side 24:1 26:2 ...").
void then_quests_are(Run &r, const std::string &what, std::vector<std::tuple<u32, u32, i32>> want) {
    then_do(r, what + " (guest memory)", [what, want](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::string s;
        bool ok = true;
        for (const auto &[sort, n, state] : want) {
            const i32 got = quest_state(cpu, h, sort, n);
            s += std::string(sort == 0 ? " main " : " side ") + std::to_string(n) + ":" +
                 std::to_string(got);
            ok = ok && got == state;
        }
        check(ok, what + ":" + s);
    });
}

// The fighting style of the parts after D: Double Step on LB while MP lasts, Red and Blue
// Potions restocked at the area's shop.
quest_bot::FarmStyle chapter_1_fight() {
    quest_bot::FarmStyle fight;
    fight.skill = quest_bot::LB;
    fight.min_mp = 40;
    fight.mp_pct = 30;
    fight.restock = {quest_bot::Restock{3, 100, 300, 25, 400},
                     quest_bot::Restock{6, 60, 300, 5, 300}};
    return fight;
}

// Part E: the side quests of Perion and Kerning City that the level-24 hero of part D can do
// with Double Step. Ayan gives Ayan's Mr. Right (side 24) and Afraid of Green Mushrooms (26: 30
// Green Mushrooms on 1.1), then the pig Repairing Blackbull's House (27). The Guard on 2.4
// gives The Security Guard's Mistrust (7, main 16 active), Stan Winter Preparations (34, main
// 12 done), the Dark Lord Dark Lord's Faith (8, side 7 active; before level 30, when he only
// offers the 2nd job), Alex finishes Alex's Worry (21, side 24 active) and gives Alex's Request
// (25) and A Present for Ayan (28), JM gives Father's Revenge 2 (31: 25 Octopus Legs on 0.2)
// and the smith Smith's Tool (33, side 7 started).
void chapter_1_e(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}}; // Double Step, Nimble Body
    then_spend_sp(r, "Double Step, then Nimble Body", build);
    then_talk_to(r, "Ayan: Ayan's Mr. Right (side 24) given", 1, 0, "Iyan", 2, quest_is(1, 24, 1));
    then_talk_to(r, "Ayan: Afraid of Green Mushrooms (side 26) given", 1, 0, "Iyan", 3,
                 quest_is(1, 26, 1));
    then_farm(r, "30 Green Mushrooms on 1.1 (Afraid of Green Mushrooms)", 1, 1, {10}, ready(1, 26),
              2400, fight);
    then_talk_to(r, "Ayan: Afraid of Green Mushrooms (side 26) done", 1, 0, "Iyan", 3,
                 quest_is(1, 26, 2));
    then_talk_to(r, "pig: Repairing Blackbull's House (side 27) given", 1, 0, "pig", 1,
                 quest_is(1, 27, 1));
    then_talk_to(r, "Guard: The Security Guard's Mistrust (side 7) given", 2, 4, "Guard", -1,
                 quest_is(1, 7, 1));
    then_talk_to(r, "Stan: Winter Preparations (side 34) given", 2, 0, "Stan", 6,
                 quest_is(1, 34, 1));
    then_talk_to(r, "Dark Lord: Dark Lord's Faith (side 8) given", 0, 0, "Darklord", 5,
                 quest_is(1, 8, 1));
    then_talk_to(r, "Alex: Alex's Worry (side 21) done", 0, 0, "Alex", 1, quest_is(1, 21, 2));
    then_talk_to(r, "Alex: Alex's Request (side 25) given", 0, 0, "Alex", 2, quest_is(1, 25, 1));
    then_talk_to(r, "Alex: A Present for Ayan (side 28) given", 0, 0, "Alex", 3,
                 quest_is(1, 28, 1));
    then_talk_to(r, "JM: Father's Revenge 2 (side 31) given", 0, 0, "JM", 4, quest_is(1, 31, 1));
    then_talk_to(r, "smith: Smith's Tool (side 33) given", 0, 0, "smith", 1, quest_is(1, 33, 1));
    then_farm(r, "25 Octopus Legs on 0.2 (Father's Revenge 2)", 0, 2, {8}, ready(1, 31), 2400,
              fight);
    then_talk_to(r, "JM: Father's Revenge 2 (side 31) done", 0, 0, "JM", 4, quest_is(1, 31, 2));
    then_spend_sp(r, "part E", build);
    then_quests_are(r, "part E's side quests",
                    {{1, 21, 2},
                     {1, 26, 2},
                     {1, 31, 2},
                     {1, 7, 1},
                     {1, 8, 1},
                     {1, 24, 1},
                     {1, 25, 1},
                     {1, 27, 1},
                     {1, 28, 1},
                     {1, 33, 1},
                     {1, 34, 1}});
    then_do(r, "part E's hero",
            [](mt_cpu *cpu) { std::printf("part E: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_save(r);
    then_finish(r);
}
void chapter_1_e_checks(Run &, u64) {}

// The hero's job in guest memory (hero +0x88 branch, +0x8C job tier; docs/formats/save.md) and in
// the save written (theif0 bytes 0 and 1).
void then_job_is(Run &r, i32 branch, i32 tier) {
    then_do(r, "the job in guest memory and in theif0", [&r, branch, tier](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 b = rdi(cpu, h.addr + 0x88), t = rdi(cpu, h.addr + 0x8C);
        const std::vector<std::uint8_t> save = read_save(r, "theif0");
        const i32 sb = save.size() > 1 ? save[0] : -1, st = save.size() > 1 ? save[1] : -1;
        check(b == branch && t == tier && sb == branch && st == tier,
              "the job: branch " + std::to_string(b) + " job_tier " + std::to_string(t) +
                  " in guest memory, branch " + std::to_string(sb) + " job_tier " +
                  std::to_string(st) + " in theif0 (want " + std::to_string(branch) + ", " +
                  std::to_string(tier) + "), level " + std::to_string(h.level));
    });
}

// Part F: area 1's stumps. On 1.3 the Dark Axe Stumps (mob 16: Dark Lord's Faith's 30, their
// Axes) and the Axe Stumps (12: Firewood) until 50 Axes (Repairing Blackbull's House 30, Smith's
// Tool 20) and 60 Firewood (Smith's Tool 30, Winter Preparations 30). Then the pig (27) in
// Perion, Dark Lord's Faith (8: the pass; before level 30, when the Dark Lord would offer only
// the 2nd job) and the smith (33) in Kerning City, the Guard (7 with the pass; Crafting Armor,
// 29, given) and Stan (34).
void chapter_1_f(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}};
    then_spend_sp(r, "Double Step, Nimble Body", build);
    then_farm(
        r, "30 Dark Axe Stumps, 50 Axes and 60 Firewood on 1.3", 1, 3, {12, 16},
        [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            return quest_bot::defeats(cpu, 1, 8, 16) >= 30 && item_count(cpu, h, 42) >= 50 &&
                   item_count(cpu, h, 41) >= 60;
        },
        5400, fight);
    then_talk_to(r, "pig: Repairing Blackbull's House (side 27) done", 1, 0, "pig", 1,
                 quest_is(1, 27, 2));
    then_talk_to(r, "Dark Lord: Dark Lord's Faith (side 8) done, the pass", 0, 0, "Darklord", 5,
                 quest_is(1, 8, 2));
    then_check(r, "the Ant Tunnel Pass (item 106) in the bag", has_item(106, 1));
    then_talk_to(r, "smith: Smith's Tool (side 33) done", 0, 0, "smith", 1, quest_is(1, 33, 2));
    then_talk_to(r, "Guard: The Security Guard's Mistrust (side 7) done", 2, 4, "Guard", -1,
                 quest_is(1, 7, 2));
    then_talk_to(r, "Guard: Crafting Armor (side 29) given", 2, 4, "Guard", -1, quest_is(1, 29, 1));
    then_talk_to(r, "Stan: Winter Preparations (side 34) done", 2, 0, "Stan", 6,
                 quest_is(1, 34, 2));
    then_spend_sp(r, "part F", build);
    then_quests_are(r, "part F's side quests",
                    {{1, 7, 2}, {1, 8, 2}, {1, 27, 2}, {1, 33, 2}, {1, 34, 2}, {1, 29, 1}});
    then_do(r, "part F's hero",
            [](mt_cpu *cpu) { std::printf("part F: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_save(r);
    then_finish(r);
}
void chapter_1_f_checks(Run &, u64) {}

// Part G: Henesys's side. A Present for Ayan (28: 30 Pig's Ribbons from the Ribbon Pigs on 2.2)
// to Alex; then, while Alex's Request (25) is active, portal212 on 2.1 opens to the Forest of
// Patience (2.6, a climb, then 2.7): Remem gives and ends Forest of Patience (15) with the rose
// (104) and sends the hero to Henesys. The rose to Alex (25 done), then Ayan's Mr. Right (24)
// done at Ayan. Then level 30 on 1.1, where the Dark Lord offers only the Job Advance (side 16):
// list 7, the Bandit (the dagger line; its reward, the Reef Claw, is a dagger), checked in guest
// memory and in the save.
void chapter_1_g(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Double Step, Nimble Body", build);
    then_farm(r, "30 Pig's Ribbons on 2.2 (A Present for Ayan)", 2, 2, {7}, ready(1, 28), 3600,
              fight);
    then_talk_to(r, "Remem: Forest of Patience (side 15) done, the rose", 2, 7, "Remem", -1,
                 quest_is(1, 15, 2), 1200);
    then_check(r, "the rose (item 104) in the bag", has_item(104, 1));
    then_talk_to(r, "Alex: A Present for Ayan (side 28) done", 0, 0, "Alex", 3, quest_is(1, 28, 2));
    then_talk_to(r, "Alex: Alex's Request (side 25) done", 0, 0, "Alex", 2, quest_is(1, 25, 2));
    then_talk_to(r, "Ayan: Ayan's Mr. Right (side 24) done", 1, 0, "Iyan", 2, quest_is(1, 24, 2));
    then_farm(r, "level 30 on 1.1 (Green Mushrooms)", 1, 1, {10}, level_at_least(30), 3600, fight);
    then_talk_to(r, "Dark Lord: Job Advance (side 16), the Bandit", 0, 0, "Darklord", 7,
                 quest_is(1, 16, 2));
    then_run_event(r, "the 2nd job (the Bandit: branch 2, tier 2)", MT_GAME_EVENT_JOB_ADVANCE, 2,
                   {2});
    then_equip(r, "the Reef Claw (the Job Advance's reward)", 28, 1);
    then_check(r, "the hero wields the Reef Claw", [](mt_cpu *cpu) { return worn(cpu, 1) == 28; });
    then_spend_sp(r, "the Bandit: Critical Stab, HP Mastery", build);
    then_quests_are(r, "part G's side quests",
                    {{1, 15, 2}, {1, 16, 2}, {1, 24, 2}, {1, 25, 2}, {1, 28, 2}});
    then_save(r);
    then_job_is(r, 2, 2);
    then_do(r, "part G's hero", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("part G: %s, SP %d, Critical Stab %d, HP Mastery %d\n",
                    hero_text(cpu, h).c_str(), free_sp(cpu, h), skill_level(cpu, h, 15),
                    skill_level(cpu, h, 17));
    });
    then_finish(r);
}
void chapter_1_g_checks(Run &, u64) {}

// Part H: area 3, through portal240 (open since side 7 is done). Crafting Armor (29: 30 Horned
// Mushroom Caps on 3.1) to the Guard (its reward, the Dark Silver Stealer, worn), the level-30
// Dark Guise and Line Boots from 3.0's equip shop (Equip30: showMarket 4), then Chrishrama (Cri,
// 3.0)'s Request 1 (side 9: 30 Evil Eye Tails on 3.4).
void chapter_1_h(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Critical Stab, HP Mastery", build);
    then_farm(r, "30 Horned Mushroom Caps on 3.1 (Crafting Armor)", 3, 1, {13}, ready(1, 29), 3600,
              fight);
    then_talk_to(r, "Guard: Crafting Armor (side 29) done", 2, 4, "Guard", -1, quest_is(1, 29, 2));
    then_equip(r, "the Dark Silver Stealer (Crafting Armor's reward)", 16, 2);
    then_gear(r, "the Dark Guise (level 30)", 3, 4, 0, 1500);
    then_gear(r, "the Line Boots (level 30)", 3, 53, 4, 1500);
    then_talk_to(r, "Chrishrama: Chrishrama's Request 1 (side 9) given", 3, 0, "Cri", -1,
                 quest_is(1, 9, 1));
    then_farm(r, "30 Evil Eye Tails on 3.4 (Chrishrama's Request 1)", 3, 4, {17}, ready(1, 9), 3600,
              fight);
    then_talk_to(r, "Chrishrama: Chrishrama's Request 1 (side 9) done", 3, 0, "Cri", -1,
                 quest_is(1, 9, 2));
    then_spend_sp(r, "part H", build);
    then_quests_are(r, "part H's side quests", {{1, 29, 2}, {1, 9, 2}});
    then_do(r, "part H's hero",
            [](mt_cpu *cpu) { std::printf("part H: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_save(r);
    then_finish(r);
}
void chapter_1_h_checks(Run &, u64) {}

// Part I: Chrishrama's Request 2 (side 10: 30 Curse Eye Tails on 3.6) and 3 (14: 50 Cold Eyes on
// 3.7).
void chapter_1_i(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Critical Stab, HP Mastery", build);
    then_talk_to(r, "Chrishrama: Chrishrama's Request 2 (side 10) given", 3, 0, "Cri", -1,
                 quest_is(1, 10, 1));
    then_farm(r, "30 Curse Eye Tails on 3.6 (Chrishrama's Request 2)", 3, 6, {23}, ready(1, 10),
              3600, fight);
    then_talk_to(r, "Chrishrama: Chrishrama's Request 2 (side 10) done", 3, 0, "Cri", -1,
                 quest_is(1, 10, 2));
    then_talk_to(r, "Chrishrama: Chrishrama's Request 3 (side 14) given", 3, 0, "Cri", -1,
                 quest_is(1, 14, 1));
    then_farm(r, "50 Cold Eyes on 3.7 (Chrishrama's Request 3)", 3, 7, {28}, ready(1, 14), 3600,
              fight);
    then_talk_to(r, "Chrishrama: Chrishrama's Request 3 (side 14) done", 3, 0, "Cri", -1,
                 quest_is(1, 14, 2));
    then_spend_sp(r, "part I", build);
    then_quests_are(r, "chapter 1's side quests of #161",
                    {{1, 7, 2},
                     {1, 8, 2},
                     {1, 9, 2},
                     {1, 10, 2},
                     {1, 14, 2},
                     {1, 15, 2},
                     {1, 16, 2},
                     {1, 21, 2},
                     {1, 24, 2},
                     {1, 25, 2},
                     {1, 26, 2},
                     {1, 27, 2},
                     {1, 28, 2},
                     {1, 29, 2},
                     {1, 31, 2},
                     {1, 33, 2},
                     {1, 34, 2}});
    then_do(r, "part I's hero",
            [](mt_cpu *cpu) { std::printf("part I: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_save(r);
    then_finish(r);
}
void chapter_1_i_checks(Run &, u64) {}

// The hero's other items (id:count) and equipment (ids) in guest memory, printed.
std::string bag_text(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    std::string s = "items";
    for (i32 n = 0; n < 48; n++) {
        const EtcItem it = etc_item(cpu, h, n);
        if (it.id >= 0) {
            s += " " + std::to_string(it.id) + ":" + std::to_string(it.count);
        }
    }
    s += "; equipment";
    const u32 inv = rd(cpu, h.addr + 0xD4);
    for (u32 i = 0; inv != 0 && i < 48; i++) {
        const u32 it = rd(cpu, inv + 4 * i);
        if (it != 0) {
            s += " " + std::to_string(rdi(cpu, it + 0x90));
        }
    }
    s += "; worn";
    for (i32 slot = 0; slot < 5; slot++) {
        s += " " + std::to_string(quest_bot::worn(cpu, slot));
    }
    return s + "; meso " + std::to_string(h.meso);
}
void then_bag(Run &r, const std::string &what) {
    then_do(r, what + ": the bag",
            [what](mt_cpu *cpu) { std::printf("%s: %s\n", what.c_str(), bag_text(cpu).c_str()); });
}

// What a chapter's end must leave alone (#307): CQuestList::ClearAll 0x7C61D resets the quests
// only, so the hero's level, EXP, max HP and MP, job, items, equipment and meso read the
// same before and after the trip.
std::string kept_text(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    return "level " + std::to_string(h.level) + " EXP " + std::to_string(h.exp) + " max HP " +
           std::to_string(h.max_hp) + " max MP " + std::to_string(h.max_mp) + " job " +
           std::to_string(rdi(cpu, h.addr + 0x88)) + "/" + std::to_string(rdi(cpu, h.addr + 0x8C)) +
           "; " + bag_text(cpu);
}
std::shared_ptr<std::string> then_take_kept(Run &r) {
    auto before = std::make_shared<std::string>();
    then_do(r, "the hero before the trip", [before](mt_cpu *cpu) { *before = kept_text(cpu); });
    return before;
}
void then_kept(Run &r, const std::shared_ptr<std::string> &before) {
    then_do(r, "the trip's clearAll kept the hero", [before](mt_cpu *cpu) {
        const std::string now = kept_text(cpu);
        check(now == *before, "the trip kept the hero: " + now +
                                  (now == *before ? "" : " (before the trip: " + *before + ")"));
    });
}

// Part J (#162): Sabitrama (Sabi, 3.0, x 632) ends Roy's Advice 2 (16) and gives Sabitrama's
// Help (17: 30 Charms of the Undead, 45, from the Zombie Mushrooms on 3.3), then ends 17 and
// gives Defeat Malady (18).
void chapter_1_j(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Critical Stab, HP Mastery", build);
    then_bag(r, "part J's start");
    then_talk_to(r, "Sabitrama: Roy's Advice 2 (16) done, Sabitrama's Help (17) given", 3, 0,
                 "Sabi", -1, main_quests(16, 17));
    then_farm(r, "30 Charms of the Undead on 3.3 (Sabitrama's Help)", 3, 3, {15}, ready(0, 17),
              3600, fight);
    then_talk_to(r, "Sabitrama: Sabitrama's Help (17) done, Defeat Malady (18) given", 3, 0, "Sabi",
                 -1, main_quests(17, 18));
    then_spend_sp(r, "part J", build);
    then_quests_are(r, "part J's main quests", {{0, 16, 2}, {0, 17, 2}, {0, 18, 1}});
    then_do(r, "part J's hero",
            [](mt_cpu *cpu) { std::printf("part J: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part J's end");
    then_save(r);
    then_finish(r);
}
void chapter_1_j_checks(Run &, u64) {}

// Part K (#162): the first boss. portal371 (3.7) opens while Defeat Malady (18) is active and
// leads to 3.8, whose map script (MelodyMap) shows the boss's lines and starts the boss scene.
// Malady (mob 64) killed, Sabitrama there (Sabi2) ends 18, gives Master in Danger (19) and
// sends the hero to Henesys; Stan (list 4) ends 19 and gives 20.
void then_boss_end(Run &r, const std::string &what, Pred left, double timeout = 300);

void chapter_1_k(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Critical Stab, HP Mastery", build);
    // The boss map has no way out but death or the boss's end: the potions are bought first,
    // and the fight restocks none. Malady flies: the bot waits under it on the floor and
    // attacks when its body is in front of the hero.
    // A first try with 100 Red Potions ran out with Malady at 2,500 of 15,000 HP, about 1,300
    // game-s in; Blue Potions went at about one a minute.
    then_stock(r, "Red Potions (Etc30)", 3, 3, 200, 500, 480);
    then_stock(r, "Blue Potions (Etc30)", 3, 6, 60, 500);
    FarmStyle boss = fight;
    boss.restock = {};
    boss.reach_up = 100;
    boss.hp_pct = 50;
    then_farm(r, "Malady on 3.8 (Defeat Malady)", 3, 8, {64, 65}, ready(0, 18), 3600, boss);
    then_run_event(r, "portal371 to the boss's map 3.8", MT_GAME_EVENT_MAP_CHANGE, 3, {8});
    then_run_event(r, "Malady (mob 64) killed", MT_GAME_EVENT_BOSS_KILLED, 64);
    // Malady's closing scene (4) moves Sabitrama (Sabi2) from where the map file puts him:
    // then_talk_to follows the live object. Original bug 19 locked this scene in the Release
    // chain (#306: Malady killed with the hero knocked back), so it goes through then_boss_end.
    then_boss_end(
        r, "Malady's closing scene (A through it)", [](mt_cpu *cpu) { return hero(cpu).on(3, 8); },
        600);
    then(r, "Sabi2 after the scene", 5, [](mt_cpu *cpu, double) {
        int x = 0, y = 0;
        const bool seen = live_object(cpu, "Sabi2", x, y);
        std::printf("after the scene: Sabi2 %s at %d,%d; %s\n", seen ? "visible" : "hidden", x, y,
                    hero_text(cpu, hero(cpu)).c_str());
        return true;
    });
    then_talk_to(r, "Sabitrama (3.8): Defeat Malady (18) done, Master in Danger (19) given", 3, 8,
                 "Sabi2", -1, main_quests(18, 19));
    then_check(r, "Sabitrama sent the hero to Henesys (2.0)",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_talk_to(r, "Stan: Master in Danger (19) done, 20 given", 2, 0, "Stan", 4,
                 main_quests(19, 20));
    then_spend_sp(r, "part K", build);
    then_quests_are(r, "part K's main quests", {{0, 18, 2}, {0, 19, 2}, {0, 20, 1}});
    then_do(r, "part K's hero",
            [](mt_cpu *cpu) { std::printf("part K: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_save(r);
    then_finish(r);
}
void chapter_1_k_checks(Run &, u64) {}

// Part L (#162): the end of chapter 1. The Pirate (2.0, x 744) gives the Pirate's Request (side
// 11: 30 Horned Mushroom Caps, 44, on 3.1; 30 Green Mushroom Caps, 40, on 1.1) only once main
// 20 is active, and offers the trip to Orbis only once 11 is done (Pirate.out quest). Every
// chapter-1 quest is read before the trip, whose clearAll resets them; then the hero stands on
// Orbis (4.1) in chapter 2, in guest memory and in the save.
void chapter_1_l(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    // The speedrun timer (#406) on Chapter 1's last split, from this save before the trip to
    // Orbis: a run of the shipped category's last split only (the ones before it are earlier
    // parts'), started now on the host's monotonic clock and the game's ticks.
    namespace fe = maplethief::frontend;
    static fe::RunTimer timer(fe::RunClock{[] {
                                               return fe::RunInstant{
                                                   static_cast<double>(SDL_GetTicksNS()) / 1e9,
                                                   mt_input_get_stats().ticks};
                                           },
                                           [] { return std::string("2026-11-26 00:00:00"); }});
    then_do(r, "the speedrun timer: Chapter 1's last split, from here", [](mt_cpu *) {
        std::vector<std::string> problems;
        std::optional<fe::RunCategory> last;
        for (const auto &[name, text] : fe::shipped_run_category_files()) {
            if (name == "chapter_1") {
                last = fe::parse_run_category(name, text, &problems);
            }
        }
        check(last.has_value() && problems.empty(), "the shipped chapter_1 category parses");
        last->splits.erase(last->splits.begin(), last->splits.end() - 1);
        timer.set_category(*last, std::nullopt);
        g_run_timer = &timer;
        timer.start();
        check(timer.state() == fe::RunTimer::State::kRunning, "the timer runs");
    });
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_spend_sp(r, "Critical Stab, HP Mastery", build);
    then_talk_to(r, "Pirate: the Pirate's Request (side 11) given", 2, 0, "Pirate", -1,
                 quest_is(1, 11, 1));
    then_farm(r, "30 Green Mushroom Caps on 1.1 (the Pirate's Request)", 1, 1, {10},
              has_item(40, 30), 3600, fight);
    then_farm(r, "30 Horned Mushroom Caps on 3.1 (the Pirate's Request)", 3, 1, {13},
              has_item(44, 30), 3600, fight);
    then_talk_to(r, "Pirate: the Pirate's Request (side 11) done", 2, 0, "Pirate", -1,
                 quest_is(1, 11, 2));
    then_quests_are(r, "chapter 1 before the trip to Orbis",
                    {{0, 16, 2},
                     {0, 17, 2},
                     {0, 18, 2},
                     {0, 19, 2},
                     {0, 20, 1},
                     {1, 11, 2},
                     {1, 16, 2},
                     {1, 34, 2}});
    then_bag(r, "before the trip");
    const std::shared_ptr<std::string> kept = then_take_kept(r);
    then_do(r, "main 20's record active before the trip", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(quest_active(cpu, h, 0, 20) && !quest_active(cpu, h, 0, 40),
              "main 20 in the active list, main 40 not (original bug 16: kept)");
    });
    then_talk_to(r, "Pirate: the trip to Orbis (the end of chapter 1)", 2, 0, "Pirate", -1,
                 [](mt_cpu *cpu) {
                     const Hero h = hero(cpu);
                     return h.chapter == 2 && h.on(4, 1);
                 });
    then_run_event(r, "chapter 1 ends (the trip to Orbis)", MT_GAME_EVENT_CHAPTER_END, 1);
    then_run_event(r, "the trip to Orbis (4.1)", MT_GAME_EVENT_MAP_CHANGE, 4, {1});
    then_do(r, "the speedrun timer: Chapter 1's last split fired, the log has both times",
            [&r](mt_cpu *) {
                namespace fe = maplethief::frontend;
                g_run_timer = nullptr;
                std::vector<fe::RunRecord> runs = timer.take_records();
                check(timer.state() == fe::RunTimer::State::kFinished && runs.size() == 1,
                      "the run finished at the split (" + std::to_string(runs.size()) +
                          " records)");
                std::string path, why;
                bool best = false;
                check(fe::write_run(r.out.string(), runs[0], nullptr, &path, &best, &why),
                      "the run log written: " + why);
                std::ifstream in(path, std::ios::binary);
                const std::string text{std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()};
                std::printf("run log %s:\n%s", path.c_str(), text.c_str());
                std::vector<std::string> problems;
                const std::optional<fe::RunRecord> back = fe::parse_run_log(text, &problems);
                check(back.has_value() && back->finished && back->times.size() == 1 &&
                          back->times[0].ms > 0 && back->times[0].ticks > 0 &&
                          back->times[0] == back->end,
                      "the log holds the split's real time and game ticks");
            });
    then_do(r, "chapter 2 on Orbis (4.1) in guest memory", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::string q;
        for (u32 n = 16; n <= 20; n++) {
            q += " main " + std::to_string(n) + ":" + std::to_string(quest_state(cpu, h, 0, n));
        }
        q += " side 11:" + std::to_string(quest_state(cpu, h, 1, 11));
        // CQuestList::ClearAll 0x7C61D keeps the record of the main quest still at 1 (main 20)
        // and renumbers it 40: chapter 2's first main quest (docs/re/original-bugs.md, bug 16).
        check(quest_active(cpu, h, 0, 40) && !quest_active(cpu, h, 0, 20) &&
                  active_quests(cpu, h).size() == 1,
              "main 20's record carried into chapter 2 as main 40, the only active quest");
        check(h.chapter == 2 && h.on(4, 1),
              "chapter " + std::to_string(h.chapter) + " in the quest manager, map " +
                  std::to_string(h.area) + "." + std::to_string(h.map) +
                  " (want 2, 4.1); after "
                  "clearAll:" +
                  q);
    });
    then_bag(r, "after the trip");
    then_kept(r, kept);
    then_save(r);
    then_do(r, "chapter 2 on Orbis (4.1) in the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> theif = read_save(r, "theif0");
        const std::vector<std::uint8_t> quest = read_save(r, "quest0");
        const int area = theif.size() > 0x41D ? theif[0x41C] : -1;
        const int map = theif.size() > 0x41D ? theif[0x41D] : -1;
        const int chapter = quest.empty() ? -1 : quest[0];
        check(chapter == 2 && area == 4 && map == 1,
              "the save: chapter " + std::to_string(chapter) + " in quest0, map " +
                  std::to_string(area) + "." + std::to_string(map) + " in theif0 (want 2, 4.1)");
    });
    then_finish(r);
}
void chapter_1_l_checks(Run &, u64) {}

// The counts a Craft or a Trade changes: other items, equipment carried or worn, meso.
struct Counts {
    std::map<i32, i32> items, equip;
    i32 meso = 0;
};
Counts counts(mt_cpu *cpu, const std::vector<i32> &items, const std::vector<i32> &equip) {
    const Hero h = hero(cpu);
    Counts c;
    for (const i32 id : items) {
        c.items[id] = item_count(cpu, h, id);
    }
    for (const i32 id : equip) {
        c.equip[id] = quest_bot::equip_owned(cpu, h, id);
    }
    c.meso = h.meso;
    return c;
}
std::string counts_text(const Counts &c) {
    std::string s;
    for (const auto &[id, n] : c.items) {
        s += " item " + std::to_string(id) + ":" + std::to_string(n);
    }
    for (const auto &[id, n] : c.equip) {
        s += " equipment " + std::to_string(id) + ":" + std::to_string(n);
    }
    return s + " meso " + std::to_string(c.meso);
}

// Part M (#162, carried from #161): chapter 2's Craft (77) and Trade (78). The chapter-1 smith
// and pig list them only from main 40 (chapter 2's); in chapter 2 the smith (0.0) runs
// chapter2/smith.out and the pig (1.0) common/pig.out, which open them at once. The Pirate on
// Orbis (Pirate_Orbis, 4.1, list 1) takes the hero back to Henesys. One Trade: 50 Cold Eye
// Tails (112) for 3 Bronze (99), the pig's list 3. One Craft: the smith's shoes (list 5), its
// first pair (51: 2 Topaz, 93, 7 Bronze and 700 meso). Each checked in guest memory before and
// after.
void chapter_1_m(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2", [](mt_cpu *cpu) { return hero(cpu).chapter == 2; });
    then_bag(r, "part M's start");
    then_talk_to(r, "Pirate (Orbis): back to Victoria Island (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    const std::vector<i32> items = {112, 99, 93}, equip = {51};
    auto trade = std::make_shared<Counts>();
    then_do(r, "before the Trade", [trade, items, equip](mt_cpu *cpu) {
        *trade = counts(cpu, items, equip);
        std::printf("before the Trade:%s\n", counts_text(*trade).c_str());
    });
    then_talk_to(
        r, "pig: Trade (78), 50 Cold Eye Tails for 3 Bronze (list 3)", 1, 0, "pig", 3,
        [trade](mt_cpu *cpu) { return item_count(cpu, hero(cpu), 99) >= trade->items[99] + 3; });
    then_do(r, "after the Trade", [trade, items, equip](mt_cpu *cpu) {
        Counts now = counts(cpu, items, equip);
        check(now.items[112] == trade->items[112] - 50 && now.items[99] == trade->items[99] + 3 &&
                  quest_state(cpu, hero(cpu), 1, 78) == 2,
              "the Trade (side 78:" + std::to_string(quest_state(cpu, hero(cpu), 1, 78)) +
                  "): before" + counts_text(*trade) + "; after" + counts_text(now));
    });
    auto craft = std::make_shared<Counts>();
    then_do(r, "before the Craft", [craft, items, equip](mt_cpu *cpu) {
        *craft = counts(cpu, items, equip);
        std::printf("before the Craft:%s\n", counts_text(*craft).c_str());
    });
    then_talk_to(
        r, "smith: Craft (77), the first shoes (lists 5, 1)", 0, 0, "smith", {5, 1},
        [craft](mt_cpu *cpu) { return equip_owned(cpu, hero(cpu), 51) > craft->equip[51]; });
    then_do(r, "after the Craft", [craft, items, equip](mt_cpu *cpu) {
        Counts now = counts(cpu, items, equip);
        check(now.equip[51] == craft->equip[51] + 1 && now.items[93] == craft->items[93] - 2 &&
                  now.items[99] == craft->items[99] - 7 && now.meso == craft->meso - 700 &&
                  quest_state(cpu, hero(cpu), 1, 77) == 2,
              "the Craft (side 77:" + std::to_string(quest_state(cpu, hero(cpu), 1, 77)) +
                  "): before" + counts_text(*craft) + "; after" + counts_text(now));
    });
    then_bag(r, "part M's end");
    then_save(r);
    then_finish(r);
}
void chapter_1_m_checks(Run &, u64) {}

// --- chapter 2 (#298, docs/testing/chapter-2-quests.md) --------------------------------------

Pred job_at(i32 branch, i32 tier); // below, with the job advancements

// Part A (#298): from the start of chapter 2 (tests/starts/chapter_2.json, part L's save) on
// 4.1. Heracle (4.0) rewards main 40 (chapter 1's last, carried over) and gives main 0 (20 of mob
// 40 on 4.2), ends it and gives main 1; the Fairy (4.0) ends 1 and gives main 2 (40 of item 58,
// from mob 38). The level-35 hero of chapter 1 kills about four of the level-56 mob 40 a minute
// and falls every few minutes (its HP potion, item 3, heals 50 HP, and area 4 sells none); part B
// goes on at level 60 (chapter-2-quests.md).
void chapter_2_a(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_1_fight();
    const std::vector<std::pair<i32, i32>> build = {{3, 15}, {4, 20}, {15, 15}, {17, 15}};
    then_check(r, "chapter 2 on 4.1", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 2 && h.on(4, 1);
    });
    then_spend_sp(r, "skills 3 and 4", build);
    then_bag(r, "part A's start");
    then_talk_to(r, "Heracle: main 40 rewarded, main 0 given", 4, 0, "Heracle", -1,
                 main_quests(40, 0));
    then_farm(r, "20 of mob 40 on 4.2 (main 0)", 4, 2, {40}, ready(0, 0), 3600, fight);
    then_talk_to(r, "Heracle: main 0 done, main 1 given", 4, 0, "Heracle", -1, main_quests(0, 1));
    then_talk_to(r, "Fairy: main 1 done, main 2 given", 4, 0, "Fairy", -1, main_quests(1, 2));
    then_quests_are(r, "part A's main quests", {{0, 40, 2}, {0, 0, 2}, {0, 1, 2}, {0, 2, 1}});
    then_do(r, "part A's hero",
            [](mt_cpu *cpu) { std::printf("part A: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part A's end");
    then_save(r);
    then_finish(r);
}
void chapter_2_a_checks(Run &, u64) {}

// The fighting style of chapter 2 from part B: skill 16 on LB while MP lasts, HP potion item 4
// on Y and MP potion item 7 on B, restocked at the area's shop (Etc40 on 4.0, Etc90 on 9.0).
quest_bot::FarmStyle chapter_2_fight() {
    quest_bot::FarmStyle fight;
    fight.skill = quest_bot::LB;
    fight.min_mp = 40;
    fight.mp_pct = 30;
    fight.hp_pct = 45;
    fight.restock = {quest_bot::Restock{4, 150, 500, 30, 2000},
                     quest_bot::Restock{7, 60, 500, 8, 2000}};
    return fight;
}
// Branch 2's skills in chapter 2 by id: chapter 1's (3, 4, 15), then 16 and, from the 3rd job,
// 21 (attack and accuracy up, for mob 68) and 20, then 17, 18 and 22.
const std::vector<std::pair<i32, i32>> k_chapter_2_build = {
    {3, 15}, {4, 20}, {15, 15}, {16, 15}, {21, 10}, {20, 15}, {17, 15}, {18, 15}, {22, 10}};
// Before the 3rd job only up to 16: the rest of the SP waits for 21.
const std::vector<std::pair<i32, i32>> k_chapter_2_before_job(k_chapter_2_build.begin(),
                                                              k_chapter_2_build.begin() + 4);

// Part B (#298, synthetic: tests/starts/raise/chapter_2_b.json over part A's save): the hero at
// level 60 (the game's level-up rules: 2 SP a level, HP and MP full) with items 4 and 7 on Y and
// B. The SP on skill 16 (LB) and the masteries; Pirate_Orbis (4.1, list 1) to 2.0, the Darklord
// (0.0) gives the 3rd job, checked in guest memory and in theif0; the Pirate (2.0) back to 4.1;
// then main 2 (40 of item 58 from mob 38 on 4.3) at the Fairy, who gives 3.
void chapter_2_b(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_2_fight();
    then_check(r, "level 60, items 4 and 7 on Y and B", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.level == 60 && quick_slot_item(cpu, h, 1) == 4 && quick_slot_item(cpu, h, 2) == 7;
    });
    then_spend_sp(r, "level 60", k_chapter_2_before_job);
    then_quick_slot(r, "skill 16 on LB (quick slot 3)", 16, 3);
    then_stock(r, "item 4 (Etc40)", 4, 4, 150, 500, 600);
    then_stock(r, "item 7 (Etc40)", 4, 7, 60, 500, 600);
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_talk_to(r, "Darklord (0.0): the 3rd job advancement (level 60)", 0, 0, "Darklord", -1,
                 job_at(2, 3), 600);
    then_run_event(r, "the 3rd job (branch 2, tier 3)", MT_GAME_EVENT_JOB_ADVANCE, 2, {3});
    then_save(r);
    then_job_is(r, 2, 3);
    then_spend_sp(r, "the 3rd job", k_chapter_2_build);
    then_talk_to(r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1,
                 [](mt_cpu *cpu) { return hero(cpu).on(4, 1); });
    then_farm(r, "40 of item 58 from mob 38 on 4.3 (main 2)", 4, 3, {38}, ready(0, 2), 3600, fight);
    then_talk_to(r, "Fairy: main 2 done, main 3 given", 4, 0, "Fairy", -1, main_quests(2, 3));
    then_quests_are(r, "part B's main quests", {{0, 2, 2}, {0, 3, 1}});
    then_do(r, "part B's hero",
            [](mt_cpu *cpu) { std::printf("part B: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part B's end");
    then_save(r);
    then_finish(r);
}
void chapter_2_b_checks(Run &, u64) {}

// Part C (#298): the way to main 3's item (105, the reward of side 37). Ericsson (4.0) gives
// side 0 (with main 3 active); Wilson (4.2) ends it (list 0) and gives side 1 (list 1: 50 of mob 51
// on 4.11), ends it and gives side 37 (list 7). portal4111 (4.11) opens while 37 is active and its
// kill not yet made, and leads to 4.17, whose map script (WolfMap) starts the first boss of
// chapter 2: mob 66, 23,000 HP. Wilson ends 37 with item 105; the Fairy ends main 3 and gives 4,
// Heracle ends 4 and gives 5.
void chapter_2_c(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_2_fight();
    then_spend_sp(r, "part C", k_chapter_2_build);
    then_stock(r, "item 4 (Etc40)", 4, 4, 200, 500, 600);
    then_stock(r, "item 7 (Etc40)", 4, 7, 80, 500, 600);
    then_talk_to(r, "Ericsson: side 0 given", 4, 0, "Ericsson", 1, quest_is(1, 0, 1));
    then_talk_to(r, "Wilson: side 0 done", 4, 2, "Wilson", 0, quest_is(1, 0, 2));
    then_talk_to(r, "Wilson: side 1 given", 4, 2, "Wilson", 1, quest_is(1, 1, 1));
    then_farm(r, "50 of mob 51 on 4.11 (side 1)", 4, 11, {51}, ready(1, 1), 3600, fight);
    then_talk_to(r, "Wilson: side 1 done", 4, 2, "Wilson", 1, quest_is(1, 1, 2));
    then_talk_to(r, "Wilson: side 37 given", 4, 2, "Wilson", 7, quest_is(1, 37, 1));
    then_stock(r, "item 4 for the boss (Etc40)", 4, 4, 300, 500, 900);
    then_stock(r, "item 7 for the boss (Etc40)", 4, 7, 100, 500, 600);
    FarmStyle boss = fight;
    boss.restock = {};
    boss.hp_pct = 55;
    boss.land_first = true;
    then_farm(r, "mob 66 on 4.17 (side 37)", 4, 17, {66}, ready(1, 37), 3600, boss);
    then_run_event(r, "portal4111 to the boss's map 4.17", MT_GAME_EVENT_MAP_CHANGE, 4, {17});
    then_run_event(r, "mob 66 killed", MT_GAME_EVENT_BOSS_KILLED, 66);
    // Chapter 1's drops and 4.11's fill the bag's other-item slots: no room for item 105.
    then_sell(r, "materials (Etc40)", 4, 4, {105, 120});
    then_talk_to(r, "Wilson: side 37 done, item 105", 4, 2, "Wilson", 7, quest_is(1, 37, 2));
    then_check(r, "item 105 (side 37's reward) in the bag", has_item(105, 1));
    then_talk_to(r, "Fairy: main 3 done (item 105), main 4 given", 4, 0, "Fairy", -1,
                 main_quests(3, 4));
    then_talk_to(r, "Heracle: main 4 done, main 5 given", 4, 0, "Heracle", -1, main_quests(4, 5));
    then_quests_are(r, "part C's quests",
                    {{0, 3, 2}, {0, 4, 2}, {0, 5, 1}, {1, 0, 2}, {1, 1, 2}, {1, 37, 2}});
    then_do(r, "part C's hero",
            [](mt_cpu *cpu) { std::printf("part C: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part C's end");
    then_save(r);
    then_finish(r);
}
void chapter_2_c_checks(Run &, u64) {}

// Part D (#298): main 5 (item 120): Pirate_Orbis (list 1) to 2.0, then 1.0, whose chapter-2
// map script (PerionMap) plays scene 20 on the first visit with 5 active, ends 5 and gives main 6.
// The Pirate (2.0) back to 4.1. In area 5 Barun (5.1) gives side 2 (list 1: 20 each of mobs 18,
// 19 and 20 on 5.0-5.3) once 6 is active; its end opens chapter 2's portal531 (5.3) to 9.1.
// Alchester (9.0) ends 6 and gives main 7 (list 1).
void chapter_2_d(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_2_fight();
    then_spend_sp(r, "part D", k_chapter_2_build);
    then_check(r, "item 120 in the bag", has_item(120, 1));
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_go(r, "1.0", {1, 0, 376, 399, 30}, 600);
    then_wait_for(r, "chapter 2's PerionMap (1.0, scene 20): main 5 done, main 6 given", 120,
                  main_quests(5, 6), 0.5);
    then_talk_to(r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1,
                 [](mt_cpu *cpu) { return hero(cpu).on(4, 1); });
    then_talk_to(r, "Barun: side 2 given", 5, 1, "Barun", 1, quest_is(1, 2, 1));
    then_farm(
        r, "20 of mob 18 on 5.0 (side 2)", 5, 0, {18, 19},
        [](mt_cpu *cpu) { return quest_bot::defeats(cpu, 1, 2, 18) >= 20; }, 3600, fight);
    then_farm(
        r, "20 of mob 20 on 5.2 (side 2)", 5, 2, {19, 20},
        [](mt_cpu *cpu) { return quest_bot::defeats(cpu, 1, 2, 20) >= 20; }, 3600, fight);
    then_farm(r, "20 of mob 19 on 5.3 (side 2)", 5, 3, {19, 20}, ready(1, 2), 3600, fight);
    then_talk_to(r, "Barun: side 2 done", 5, 1, "Barun", 1, quest_is(1, 2, 2));
    then_talk_to(r, "Alchester (9.0): main 6 done, main 7 given", 9, 0, "Alchester", 1,
                 main_quests(6, 7), 900);
    then_run_event(r, "chapter 2's portal531 (5.3) to 9.1", MT_GAME_EVENT_MAP_CHANGE, 9, {1});
    then_quests_are(r, "part D's quests", {{0, 5, 2}, {0, 6, 2}, {0, 7, 1}, {1, 2, 2}});
    then_do(r, "part D's hero",
            [](mt_cpu *cpu) { std::printf("part D: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part D's end");
    then_save(r);
    then_finish(r);
}
void chapter_2_d_checks(Run &, u64) {}

// Part E (#298): area 9 (the ground is ice, the hero slides). Main 7 (25 of mob 41 on 9.2) at
// Alchester (list 2), who gives main 71 and ends it (list 3) once Skadru's side 35 (list 4: 3 of
// item 69 from mob 34 on 9.1) and the SnowQueen's side 36 (list 3: 30 of item 63 from mob 25 on
// 9.1) are done; then main 72 (30 of item 66 from mob 45 on 9.3) and, at its end (list 4), main
// 8.
void chapter_2_e(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_2_fight();
    then_spend_sp(r, "part E", k_chapter_2_build);
    // The bag's other-item slots are full of chapter 1's and 2's drops: no room for item 69.
    then_sell(r, "materials (Etc90)", 9, 4, {55, 63, 66, 69});
    then_stock(r, "item 4 (Etc90)", 9, 4, 200, 500, 600);
    then_stock(r, "item 7 (Etc90)", 9, 7, 80, 500, 600);
    then_farm(r, "25 of mob 41 on 9.2 (main 7)", 9, 2, {41}, ready(0, 7), 3600, fight);
    then_talk_to(r, "Alchester: main 7 done, main 71 given", 9, 0, "Alchester", 2,
                 main_quests(7, 71));
    then_talk_to(r, "Skadru: side 35 given", 9, 0, "Skadru", 4, quest_is(1, 35, 1));
    then_farm(r, "3 of item 69 from mob 34 on 9.1 (side 35)", 9, 1, {34}, ready(1, 35), 3600,
              fight);
    then_talk_to(r, "Skadru: side 35 done", 9, 0, "Skadru", 4, quest_is(1, 35, 2));
    then_talk_to(r, "SnowQueen: side 36 given", 9, 0, "SnowQueen", 3, quest_is(1, 36, 1));
    then_farm(r, "30 of item 63 from mob 25 on 9.1 (side 36)", 9, 1, {25}, ready(1, 36), 3600,
              fight);
    then_talk_to(r, "SnowQueen: side 36 done", 9, 0, "SnowQueen", 3, quest_is(1, 36, 2));
    then_talk_to(r, "Alchester: main 71 done, main 72 given", 9, 0, "Alchester", 3,
                 main_quests(71, 72));
    then_farm(r, "30 of item 66 from mob 45 on 9.3 (main 72)", 9, 3, {45, 41}, ready(0, 72), 3600,
              fight);
    then_talk_to(r, "Alchester: main 72 done, main 8 given", 9, 0, "Alchester", 4,
                 main_quests(72, 8));
    then_quests_are(r, "part E's quests",
                    {{0, 7, 2}, {0, 71, 2}, {0, 72, 2}, {0, 8, 1}, {1, 35, 2}, {1, 36, 2}});
    then_do(r, "part E's hero",
            [](mt_cpu *cpu) { std::printf("part E: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part E's end");
    then_save(r);
    then_finish(r);
}
void chapter_2_e_checks(Run &, u64) {}

// Part F (#298, synthetic: tests/starts/raise/chapter_2_f.json over part E's save): the second
// boss and the end of chapter 2. Chapter 2's portal942 (9.4) leads to 9.5 while main 8 is active
// and its kill not yet made. 9.5 has no map script: no map object names YetiMap.out (the
// knowledge base lists it unused), so no boss scene starts the fight; the boss (mob 68, 32,000
// HP) is one of the map's own spawns, there from the start (docs/re/boss-fights.md). Its death
// plays end scene 8 (CMob::Die 0x73349). Alchester ends 8 (list 5) and gives main 9, which has no
// reward call: Pirate_Orbis (4.1) ends the chapter (list 2, with 9 active): clearAll, then
// chapter 3 on 6.0.
void chapter_2_f(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    // The speedrun timer (#434) on Any%'s chapter 2 splits, mob 68's death and chapter 2's end:
    // the shipped category's splits from "Chapter 2 boss" to "Chapter 2" only (the ones before
    // are earlier parts', the ones after chapter 3's), started now as part L's is.
    namespace fe = maplethief::frontend;
    static fe::RunTimer timer(fe::RunClock{[] {
                                               return fe::RunInstant{
                                                   static_cast<double>(SDL_GetTicksNS()) / 1e9,
                                                   mt_input_get_stats().ticks};
                                           },
                                           [] { return std::string("2026-11-27 00:00:00"); }});
    then_do(r, "the speedrun timer: Any%'s chapter 2 boss and chapter 2's end, from here",
            [](mt_cpu *) {
                std::vector<std::string> problems;
                std::optional<fe::RunCategory> any;
                for (const auto &[name, text] : fe::shipped_run_category_files()) {
                    if (name == "any") {
                        any = fe::parse_run_category(name, text, &problems);
                    }
                }
                check(any.has_value() && problems.empty(), "the shipped any category parses");
                auto &s = any->splits;
                const auto first = std::find_if(s.begin(), s.end(), [](const fe::RunSplit &x) {
                    return x.kind == MT_GAME_EVENT_BOSS_KILLED && x.a == 68;
                });
                check(first != s.end() && first + 1 != s.end() &&
                          (first + 1)->kind == MT_GAME_EVENT_CHAPTER_END && (first + 1)->a == 2,
                      "Any% splits on mob 68, then on chapter 2's end");
                s.erase(first + 2, s.end());
                s.erase(s.begin(), first);
                timer.set_category(*any, std::nullopt);
                g_run_timer = &timer;
                timer.start();
                check(timer.state() == fe::RunTimer::State::kRunning, "the timer runs");
            });
    const FarmStyle fight = chapter_2_fight();
    then_spend_sp(r, "part F", k_chapter_2_build);
    then_stock(r, "item 4 for the boss (Etc90)", 9, 4, 300, 500, 900);
    then_stock(r, "item 7 for the boss (Etc90)", 9, 7, 100, 500, 600);
    // Mob 68 is level 121 and hits for about 470 a second: with the hero's dagger 28 (attack
    // 25-30) the first try took 1,200 of its 32,000 HP before the hero fell. A player would buy
    // Equip90's level-60 dagger 34 (77,700 meso there) and cast skill 21 (attack and accuracy
    // up) for the fight. Even so the level-62 hero fell 46 times in an hour (at best 6,400 HP a
    // try; the boss heals when the map reloads), so part F is synthetic: it starts at level 70
    // (tests/starts/raise/chapter_2_f.json over part E's save).
    then_gear(r, "dagger 34 (Equip90)", 9, 34, 1, 2000);
    then_quick_slot(r, "skill 21 on RB (quick slot 4)", 21, 4);
    FarmStyle boss = fight;
    boss.restock = {};
    boss.hp_pct = 55;
    boss.land_first = true;
    boss.buff = RB;
    boss.buff_mp = 400;
    // How mob 68's fight starts (docs/re/boss-fights.md): 9.5 has no map script (YetiMap.out
    // is never named), so no boss message and no boss scene; mob 68 is the map's own spawn and is
    // there from the first tick. 4.17's WolfMap, by contrast, plays the boss scene (panel 3).
    auto nav = std::make_shared<Nav>();
    auto since = std::make_shared<double>(-1);
    auto scene = std::make_shared<bool>(false);
    then_go(r, "portal942 (9.4)", at_object(9, 4, "portal942", 10), 600);
    then_heal(r, "full HP before 9.5", 95);
    then(r, "enter 9.5: mob 68 there, no boss scene", 600,
         [&r, nav, since, scene](mt_cpu *cpu, double el) {
             const Hero h = hero(cpu);
             if (!h.on(9, 5)) {
                 if (!interrupted(r, cpu, el)) {
                     drive(r, cpu, el, *nav, Goal{9, 5, 0, 0, 10});
                 }
                 return false;
             }
             if (*since < 0) {
                 *since = el;
                 release_all(r);
             }
             *scene = *scene || game_mode(cpu) == 9 || mt_input_panel() == 3;
             if (el - *since < 3) {
                 return false;
             }
             bool yeti = false;
             for (const Mob &m : mobs(cpu)) {
                 yeti = yeti || m.id == 68;
             }
             check(yeti && !*scene, std::string("9.5 after 3 s: mob 68 ") +
                                        (yeti ? "there" : "missing") + ", boss scene " +
                                        (*scene ? "played" : "none"));
             return true;
         });
    then_farm(r, "mob 68 on 9.5 (main 8)", 9, 5, {68}, ready(0, 8), 3600, boss);
    // Original bug 19 (docs/re/original-bugs.md): land_first only stops attacks from starting,
    // and mob 68 hits about every 1.5 s, so a blow already under way can still kill it while the
    // hero is knocked back; the end scene (game mode 9) then takes no key. When A has not moved
    // it on for 5 s with CHero +0x119 still set, the run clears that byte, as #420's fix (on in
    // Enhanced) will, and says so; the scene and the rest are the game's own.
    auto locked = std::make_shared<double>(-1);
    then(r, "mob 68's end scene", 120, [&r, locked](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (game_mode(cpu) != 9 || !h.knocked) {
            *locked = -1;
            return !interrupted(r, cpu, el);
        }
        interrupted(r, cpu, el);
        if (*locked < 0) {
            *locked = el;
        } else if (el - *locked >= 5) {
            std::printf("original bug 19: the end scene took no key for 5 s with the hero knocked "
                        "back; the run clears CHero +0x119 (#420)\n");
            cpu->mem[h.addr + 0x119] = 0;
            *locked = -1;
        }
        return false;
    });
    then_run_event(r, "portal942 to 9.5", MT_GAME_EVENT_MAP_CHANGE, 9, {5});
    then_run_event(r, "mob 68 killed", MT_GAME_EVENT_BOSS_KILLED, 68);
    then_do(r, "the speedrun timer: Any%'s Chapter 2 boss split fired", [](mt_cpu *) {
        check(timer.state() == maplethief::frontend::RunTimer::State::kRunning &&
                  timer.times().size() == 1 && timer.times()[0].ticks > 0,
              "one split (Chapter 2 boss) at mob 68's death, " +
                  std::to_string(timer.times().size()) + " split(s) so far");
        std::printf("Any%% split Chapter 2 boss: %lld ms, %llu ticks\n",
                    static_cast<long long>(timer.times()[0].ms),
                    static_cast<unsigned long long>(timer.times()[0].ticks));
    });
    then_talk_to(r, "Alchester: main 8 done, main 9 given", 9, 0, "Alchester", 5, main_quests(8, 9),
                 900);
    then_quests_are(r, "chapter 2 before the trip", {{0, 8, 2}, {0, 9, 1}});
    then_bag(r, "before the trip");
    const std::shared_ptr<std::string> kept = then_take_kept(r);
    then_talk_to(
        r, "Pirate_Orbis (4.1): the end of chapter 2 (list 2)", 4, 1, "Pirate_Orbis", 2,
        [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            return h.chapter == 3 && h.on(6, 0);
        },
        900);
    then_run_event(r, "chapter 2 ends", MT_GAME_EVENT_CHAPTER_END, 2);
    then_run_event(r, "the trip to 6.0", MT_GAME_EVENT_MAP_CHANGE, 6, {0});
    then_do(r, "the speedrun timer: Any%'s Chapter 2 split fired, the run finished", [](mt_cpu *) {
        g_run_timer = nullptr;
        const std::vector<maplethief::frontend::RunRecord> runs = timer.take_records();
        check(timer.state() == maplethief::frontend::RunTimer::State::kFinished &&
                  runs.size() == 1 && runs[0].finished && runs[0].times.size() == 2 &&
                  runs[0].times[0].ticks < runs[0].times[1].ticks &&
                  runs[0].times[1] == runs[0].end,
              "the run finished at chapter 2's end, after the Chapter 2 boss split (" +
                  std::to_string(runs.size()) + " records)");
    });
    then_do(r, "chapter 3 on 6.0 in guest memory", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::string q;
        for (const u32 n : {0u, 5u, 8u, 9u, 71u, 72u}) {
            q += " main " + std::to_string(n) + ":" + std::to_string(quest_state(cpu, h, 0, n));
        }
        check(h.chapter == 3 && h.on(6, 0),
              "chapter " + std::to_string(h.chapter) + " in the quest manager, map " +
                  std::to_string(h.area) + "." + std::to_string(h.map) +
                  " (want 3, 6.0); after clearAll:" + q);
    });
    then_kept(r, kept);
    then_save(r);
    then_do(r, "chapter 3 on 6.0 in the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> theif = read_save(r, "theif0");
        const std::vector<std::uint8_t> quest = read_save(r, "quest0");
        const int area = theif.size() > 0x41D ? theif[0x41C] : -1;
        const int map = theif.size() > 0x41D ? theif[0x41D] : -1;
        const int chapter = quest.empty() ? -1 : quest[0];
        check(chapter == 3 && area == 6 && map == 0,
              "the save: chapter " + std::to_string(chapter) + " in quest0, map " +
                  std::to_string(area) + "." + std::to_string(map) + " in theif0 (want 3, 6.0)");
    });
    then_do(r, "part F's hero",
            [](mt_cpu *cpu) { std::printf("part F: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_finish(r);
}
void chapter_2_f_checks(Run &, u64) {}

// --- chapter 3 (#300, docs/testing/chapter-3-quests.md) --------------------------------------

// The fighting style of chapter 3: chapter 2's, restocked at Etc60 (6.0) and Etc90 (9.0).
quest_bot::FarmStyle chapter_3_fight() { return chapter_2_fight(); }

// A boss's end scene (game mode 9, CMob::Die 0x73349) through to the field, A tapped by
// interrupted. Original bug 19 (docs/re/original-bugs.md): a boss killed while the hero is knocked
// back leaves the scene's box deaf to every key; when A has not moved it on for 5 s with CHero
// +0x119 still set, the run clears that byte (as #420's fix will) and says so. `left` says when
// the scene is over (the map it ends on).
void then_boss_end(Run &r, const std::string &what, Pred left, double timeout) {
    auto locked = std::make_shared<double>(-1);
    then(r, what, timeout, [&r, locked, left](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (game_mode(cpu) != 9 || !h.knocked) {
            *locked = -1;
            const bool busy = quest_bot::interrupted(r, cpu, el);
            return !busy && field(cpu) && left(cpu);
        }
        quest_bot::interrupted(r, cpu, el);
        if (*locked < 0) {
            *locked = el;
        } else if (el - *locked >= 5) {
            std::printf("original bug 19: the end scene took no key for 5 s with the hero knocked "
                        "back; the run clears CHero +0x119 (#420)\n");
            cpu->mem[h.addr + 0x119] = 0;
            *locked = -1;
        }
        return false;
    });
}

// Chapter 3's main quests 0-8, read.
std::string chapter_3_mains(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    std::string q;
    for (u32 n = 0; n <= 8; n++) {
        q += " main " + std::to_string(n) + ":" + std::to_string(quest_state(cpu, h, 0, n));
    }
    return q + " main 40:" + std::to_string(quest_state(cpu, h, 0, 40));
}

// Part A (#300): from chapter 2's end (play.chapter_2_f_synthetic's save, or
// tests/starts/chapter_3.json) on 6.0. ZenumistChief (7.0, list 0) rewards main 40 (chapter 2's
// last, carried over by the trip) and gives main 0, which needs item 117: BrokerHan (6.0, list 1)
// gives side 0 and ends it for 10 of item 8 (Etc60 sells it) and 10,000 meso, with item 117.
// ZenumistChief ends 0 (list 1) and gives main 1 (40 of mob 63 on 7.1), ends 1 (list 2) and gives
// main 2.
void chapter_3_a(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_3_fight();
    then_check(r, "chapter 3 on 6.0", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.on(6, 0);
    });
    then_spend_sp(r, "part A", k_chapter_2_build);
    then_bag(r, "part A's start");
    then_sell(r, "materials (Etc60)", 6, 4, {});
    then_stock(r, "item 4 (Etc60)", 6, 4, 200, 12000, 600);
    then_stock(r, "item 7 (Etc60)", 6, 7, 80, 12000, 600);
    then_talk_to(r, "ZenumistChief (7.0): main 40 rewarded, main 0 given", 7, 0, "ZenumistChief", 0,
                 main_quests(40, 0));
    then_talk_to(r, "BrokerHan (6.0): side 0 given", 6, 0, "BrokerHan", 1, quest_is(1, 0, 1));
    then_stock(r, "10 of item 8 (Etc60, side 0)", 6, 8, 10, 10000, 600);
    then_talk_to(r, "BrokerHan: side 0 done, item 117", 6, 0, "BrokerHan", 1, quest_is(1, 0, 2));
    then_check(r, "item 117 (side 0's reward) in the bag", has_item(117, 1));
    then_talk_to(r, "ZenumistChief: main 0 done, main 1 given", 7, 0, "ZenumistChief", 1,
                 main_quests(0, 1));
    then_farm(r, "40 of mob 63 on 7.1 (main 1)", 7, 1, {63}, ready(0, 1), 3600, fight);
    then_talk_to(r, "ZenumistChief: main 1 done, main 2 given", 7, 0, "ZenumistChief", 2,
                 main_quests(1, 2));
    then_quests_are(r, "part A's quests", {{0, 40, 2}, {0, 0, 2}, {0, 1, 2}, {0, 2, 1}, {1, 0, 2}});
    then_do(r, "part A's hero",
            [](mt_cpu *cpu) { std::printf("part A: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part A's end");
    then_save(r);
    then_finish(r);
}
void chapter_3_a_checks(Run &, u64) {}

// Part B (#300): the way to main 3, which ZenumistChief gives (list 3) only once side 4 is done.
// AlchadnoChief (8.0) gives side 58 (item 118, the Alcadno card) on a talk; BrokerHan (list 4)
// gives side 15 while 58 is active (40 of mob 60 on 8.2) and ends it with item 118;
// AlchadnoChief ends 58, then gives side 2 (list 2). Humanoid3 (8.4) plays the Magatia scenes
// on the first talk while side 2 is active and side 4 not started (scene 16, then 17 on 8.6, 18
// on 7.6, 19 on 8.4: each but the last ends with a map change, command 52); the next talk gives
// side 4 and ends it.
void chapter_3_b(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_3_fight();
    then_spend_sp(r, "part B", k_chapter_2_build);
    then_talk_to(r, "AlchadnoChief (8.0): side 58 given", 8, 0, "AlchadnoChief", -1,
                 quest_is(1, 58, 1));
    then_talk_to(r, "BrokerHan: side 15 given", 6, 0, "BrokerHan", 4, quest_is(1, 15, 1));
    then_farm(r, "40 of mob 60 on 8.2 (side 15)", 8, 2, {60}, ready(1, 15), 3600, fight);
    then_talk_to(r, "BrokerHan: side 15 done, item 118", 6, 0, "BrokerHan", 4, quest_is(1, 15, 2));
    then_check(r, "item 118 (side 15's reward) in the bag", has_item(118, 1));
    then_talk_to(r, "AlchadnoChief: side 58 done", 8, 0, "AlchadnoChief", -1, quest_is(1, 58, 2));
    then_talk_to(r, "AlchadnoChief: side 2 given", 8, 0, "AlchadnoChief", 2, quest_is(1, 2, 1));
    // The first talk plays the scenes (runAnimation(16)); the next starts side 4 and rewards it.
    then_talk_to(r, "Humanoid3 (8.4): the Magatia scenes 16-19, then side 4 done", 8, 4,
                 "Humanoid3", -1, quest_is(1, 4, 2), 1200);
    then_talk_to(r, "ZenumistChief: main 2 done, main 3 given", 7, 0, "ZenumistChief", 3,
                 main_quests(2, 3));
    then_quests_are(r, "part B's quests",
                    {{0, 2, 2}, {0, 3, 1}, {1, 58, 2}, {1, 15, 2}, {1, 2, 1}, {1, 4, 2}});
    then_do(r, "part B's hero",
            [](mt_cpu *cpu) { std::printf("part B: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part B's end");
    then_save(r);
    then_finish(r);
}
void chapter_3_b_checks(Run &, u64) {}

// Part C (#300): the first boss of chapter 3. portal on 7.1 to 7.4 opens while main 3 is active
// and its kill not yet made; 7.4's map script (HomunMap) plays the entrance scene (9) and the
// boss (mob 67, 48,000 HP) is the map's own spawn. Its death plays end scene 10. ZenumistChief
// ends 3 (list 4) with item 126 and gives main 4; AlchadnoChief ends 4 and gives main 5 (list 0):
// 30 of item 84 (mob 63, 7.1) and 25 of item 82 (mob 56, 7.2), ended at list 1, which gives main 6.
void chapter_3_c(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    const FarmStyle fight = chapter_3_fight();
    then_spend_sp(r, "part C", k_chapter_2_build);
    then_stock(r, "item 4 for the boss (Etc60)", 6, 4, 300, 2000, 900);
    then_stock(r, "item 7 for the boss (Etc60)", 6, 7, 100, 2000, 600);
    then_quick_slot(r, "skill 21 on RB (quick slot 4)", 21, 4);
    FarmStyle boss = fight;
    boss.restock = {};
    boss.hp_pct = 55;
    boss.land_first = true;
    boss.buff = RB;
    boss.buff_mp = 400;
    then_heal(r, "full HP before 7.4", 95);
    then_farm(r, "mob 67 on 7.4 (main 3)", 7, 4, {67}, ready(0, 3), 3600, boss);
    then_boss_end(r, "mob 67's end scene", [](mt_cpu *) { return true; });
    then_run_event(r, "the way to the boss's map 7.4", MT_GAME_EVENT_MAP_CHANGE, 7, {4});
    then_run_event(r, "mob 67 killed", MT_GAME_EVENT_BOSS_KILLED, 67);
    then_talk_to(r, "ZenumistChief: main 3 done (item 126), main 4 given", 7, 0, "ZenumistChief", 4,
                 main_quests(3, 4), 900);
    then_check(r, "item 126 (main 3's reward) in the bag", has_item(126, 1));
    then_talk_to(r, "AlchadnoChief: main 4 done, main 5 given", 8, 0, "AlchadnoChief", 0,
                 main_quests(4, 5));
    then_farm(
        r, "30 of item 84 from mob 63 on 7.1 (main 5)", 7, 1, {63},
        [](mt_cpu *cpu) { return item_count(cpu, hero(cpu), 84) >= 30; }, 3600, fight);
    then_farm(r, "25 of item 82 from mob 56 on 7.2 (main 5)", 7, 2, {56}, ready(0, 5), 3600, fight);
    then_talk_to(r, "AlchadnoChief: main 5 done, main 6 given", 8, 0, "AlchadnoChief", 1,
                 main_quests(5, 6));
    then_quests_are(r, "part C's quests", {{0, 3, 2}, {0, 4, 2}, {0, 5, 2}, {0, 6, 1}});
    then_do(r, "part C's hero",
            [](mt_cpu *cpu) { std::printf("part C: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part C's end");
    then_save(r);
    then_finish(r);
}
void chapter_3_c_checks(Run &, u64) {}

// Part D (#300): the 4th job and the way to the last boss. ZenumistChief (list 12) gives side 25
// and item 119 (getItem) once the 3rd job is taken and main 0 and 2 are done; Pirate_Magatia
// (6.0, list 1) to 4.1, Pirate_Orbis (4.1, list 1) to 2.0, and the Darklord (0.0) hands side 25 in
// and gives the 4th advancement (getClassUpCount 3 -> 4), checked in guest memory and in theif0.
// Then by the Pirate (2.0) to 4.1 and over area 5 to 9.0 (5.3's portal to 9.1 is open in chapter
// 3): Alchester ends main 6 (list 0) and gives main 7 (mob 69 on 10.5). The part saves on 9.0.
void chapter_3_d(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_spend_sp(r, "part D", k_chapter_2_build);
    then_talk_to(r, "ZenumistChief: side 25 given, item 119", 7, 0, "ZenumistChief", 12,
                 quest_is(1, 25, 1));
    then_check(r, "item 119 (side 25's book) in the bag", has_item(119, 1));
    then_talk_to(r, "Pirate_Magatia (6.0): to 4.1 (list 1)", 6, 0, "Pirate_Magatia", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(4, 1); });
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_talk_to(r, "Darklord (0.0): side 25 handed in, the 4th job advancement", 0, 0, "Darklord",
                 -1, job_at(2, 4), 600);
    then_run_event(r, "the 4th job (branch 2, tier 4)", MT_GAME_EVENT_JOB_ADVANCE, 2, {4});
    then_save(r);
    then_job_is(r, 2, 4);
    then_quests_are(r, "side 25 handed in", {{1, 25, 2}});
    then_spend_sp(r, "the 4th job", k_chapter_2_build);
    then_talk_to(r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1,
                 [](mt_cpu *cpu) { return hero(cpu).on(4, 1); });
    then_talk_to(r, "Alchester (9.0): main 6 done, main 7 given", 9, 0, "Alchester", 0,
                 main_quests(6, 7), 1200);
    then_stock(r, "item 4 for the boss (Etc90)", 9, 4, 300, 2000, 900);
    then_stock(r, "item 7 for the boss (Etc90)", 9, 7, 100, 2000, 600);
    then_quests_are(r, "part D's quests", {{0, 6, 2}, {0, 7, 1}, {1, 25, 2}});
    then_do(r, "part D's hero",
            [](mt_cpu *cpu) { std::printf("part D: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_bag(r, "part D's end");
    then_save(r);
    then_finish(r);
}
void chapter_3_d_checks(Run &, u64) {}

// --- the last boss and the ending (part E, watched and skipped) ---
//
// From part D's save (or its raise) on 9.0 with main 7 active: chapter 3's portal1031 (10.3) leads
// to 10.5 while 7 is active; 10.5's map script (BalogMap) plays the entrance scene (14) while
// 7's kill is missing. Mob 69 (80,000 HP) killed, CMob::Die starts end scene 1
// (CAniManager::makeAniEndingZacum, 202 steps), whose command 52 loads 2.11 and scene 2
// (makeAniEndingHenesis, 430 steps), which ends with command 28: a warp to 2.0, free play. Then
// Alchester (9.0, list 1) ends main 7 and gives main 8, which no script ends.
//   chapter_3_e          the scenes watched: A through the boxes, as a player taps them
//   chapter_3_e_skipped  the same with skip_intros on (ADR 0096): no test input in the scenes
// Each run writes the state the ending left (ending_state.txt); the skipped run compares its own
// with the watched run's (MT_SKIP_WATCHED). The boss fight differs between the two (the skip
// shortens the entrance scene, and the boss's attack cycle runs with the frames, qol.md), so only
// what the ending sets is compared: the map, the chapter, the quests, and both scenes run to
// their last step.
struct Ending {
    bool skip = false;
    i32 last_mode = -1;
    // Per scene map: the highest step seen in game mode 9 after the kill (10.5: scene 1, 2.11:
    // scene 2) and the A presses the flow made there.
    i32 step_1 = -1, step_2 = -1;
    u64 presses = 0;
    bool a_down = false;
    std::vector<std::pair<std::string, std::string>> state;
} g_ending;

bool boss_69_dead() {
    for (const mt_game_event &e : g_run_events) {
        if (e.kind == MT_GAME_EVENT_BOSS_KILLED && e.value == 69) {
            return true;
        }
    }
    return false;
}

void chapter_3_ending(Run &r, bool skip) {
    using namespace quest_bot;
    from_save(r);
    r.settings_from_env = false; // watched and skipped compared here
    r.settings.skip_intros = skip;
    g_ending = Ending{};
    g_ending.skip = skip;
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        if (!boss_69_dead()) {
            return;
        }
        const fe::SceneView v = fe::read_scene_view(fe::guest_reader(cpu));
        if (v.scene) {
            const Hero h = hero(cpu);
            i32 &step = h.on(2, 11) ? g_ending.step_2 : g_ending.step_1;
            step = std::max(step, v.step);
            const bool a = std::find(r.held.begin(), r.held.end(), A) != r.held.end();
            g_ending.presses += a && !g_ending.a_down ? 1 : 0;
            g_ending.a_down = a;
        }
    };
    const FarmStyle fight = chapter_3_fight();
    then_spend_sp(r, "part E", k_chapter_2_build);
    then_quick_slot(r, "skill 21 on RB (quick slot 4)", 21, 4);
    FarmStyle boss = fight;
    boss.restock = {};
    boss.hp_pct = 55;
    boss.land_first = true;
    boss.buff = RB;
    boss.buff_mp = 400;
    then_go(r, "portal1031 (10.3)", at_object(10, 3, "portal1031", 10), 1200);
    then_heal(r, "full HP before 10.5", 95);
    then_farm(r, "mob 69 on 10.5 (main 7)", 10, 5, {69}, ready(0, 7), 3600, boss);
    // A watched scene is tapped through by interrupted (A every 0.8 s); a skipped one gets no
    // input from the test: the skip taps the boxes.
    auto locked = std::make_shared<double>(-1);
    then(r, skip ? "the ending skipped: no input" : "the ending watched: A through its boxes", 900,
         [&r, skip, locked](mt_cpu *cpu, double el) {
             const Hero h = hero(cpu);
             const i32 mode = game_mode(cpu);
             if (mode == 9 && h.knocked) {
                 if (*locked < 0) {
                     *locked = el;
                 } else if (el - *locked >= 5) {
                     std::printf("original bug 19: the end scene took no key for 5 s with the hero "
                                 "knocked back; the run clears CHero +0x119 (#420)\n");
                     cpu->mem[h.addr + 0x119] = 0;
                     *locked = -1;
                 }
             } else {
                 *locked = -1;
             }
             if (skip && !field(cpu)) {
                 release_all(r);
                 return false;
             }
             if (quest_bot::interrupted(r, cpu, el)) {
                 return false;
             }
             return h.on(2, 0) && field(cpu) && g_ending.step_2 >= 0;
         });
    then_do(r, "the state the ending left", [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        auto &s = g_ending.state;
        s.emplace_back("map", std::to_string(h.area) + "." + std::to_string(h.map));
        s.emplace_back("chapter", std::to_string(h.chapter));
        s.emplace_back("game_mode", std::to_string(game_mode(cpu)));
        for (u32 sort = 0; sort < 2; sort++) {
            std::string q;
            for (u32 n = 0; n < 100; n++) {
                if (const i32 v = quest_state(cpu, h, sort, n); v != 0) {
                    q += std::to_string(n) + "=" + std::to_string(v) + " ";
                }
            }
            s.emplace_back(sort == 0 ? "quests.main" : "quests.side", q);
        }
        s.emplace_back("scene_1_last_step", std::to_string(g_ending.step_1));
        s.emplace_back("scene_2_last_step", std::to_string(g_ending.step_2));
        std::printf("the ending %s: scene 1 to step %d, scene 2 to step %d, %llu test presses; "
                    "the skip: %llu scenes, %llu taps; %s\n",
                    g_ending.skip ? "skipped" : "watched", g_ending.step_1, g_ending.step_2,
                    static_cast<unsigned long long>(g_ending.presses),
                    static_cast<unsigned long long>(r.scene_skip.stats().scenes),
                    static_cast<unsigned long long>(r.scene_skip.stats().taps),
                    hero_text(cpu, h).c_str());
    });
    then_do(r, "the ending's state written", [&r](mt_cpu *) {
        std::ofstream f(r.out / "ending_state.txt", std::ios::trunc);
        for (const auto &[k, v] : g_ending.state) {
            f << k << " = " << v << "\n";
        }
    });
    then_run_event(r, "portal1031 to the boss's map 10.5", MT_GAME_EVENT_MAP_CHANGE, 10, {5});
    then_run_event(r, "mob 69 killed", MT_GAME_EVENT_BOSS_KILLED, 69);
    // The ending's two map loads (scene 1's command 52 to 2.11, scene 2's command 28 to 2.0) go
    // through world +0x98/+0x9C and game mode 8, not CMapleWorld::changeMap: the run event is
    // CStateGame::LoadMap's (#424), with no portal.
    then_run_event(r, "scene 1's command 52 to 2.11", MT_GAME_EVENT_MAP_CHANGE, 2, {11, -1});
    then_run_event(r, "scene 2's command 28 to 2.0", MT_GAME_EVENT_MAP_CHANGE, 2, {0, -1});
    then_do(r, "the ending's maps: scene 2 on 2.11, then 2.0", [](mt_cpu *cpu) {
        check(g_ending.step_2 >= 0 && hero(cpu).on(2, 0), "scene 2 ran on 2.11 (to step " +
                                                              std::to_string(g_ending.step_2) +
                                                              ") and the field after it is 2.0");
    });
    then_check(r, "free play after the ending: the field of 2.0", [](mt_cpu *cpu) {
        return field(cpu) && hero(cpu).on(2, 0) && hero(cpu).chapter == 3;
    });
    then_talk_to(r, "Pirate (2.0): to 4.1", 2, 0, "Pirate", -1,
                 [](mt_cpu *cpu) { return hero(cpu).on(4, 1); });
    then_talk_to(r, "Alchester (9.0): main 7 done, main 8 given", 9, 0, "Alchester", 1,
                 main_quests(7, 8), 1200);
    then_do(r, "chapter 3's main quests after the ending", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        bool ok =
            h.chapter == 3 && quest_state(cpu, h, 0, 8) == 1 && quest_state(cpu, h, 0, 40) == 2;
        for (u32 n = 0; n <= 7; n++) {
            ok = ok && quest_state(cpu, h, 0, n) == 2;
        }
        check(ok, "main 0-7 done, main 8 open, chapter 3:" + chapter_3_mains(cpu));
    });
    then_bag(r, "after the ending");
    then_save(r);
    then_do(r, "the post-game save: chapter 3 on 9.0, main 8 open", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> theif = read_save(r, "theif0");
        const std::vector<std::uint8_t> quest = read_save(r, "quest0");
        const int area = theif.size() > 0x41D ? theif[0x41C] : -1;
        const int map = theif.size() > 0x41D ? theif[0x41D] : -1;
        const int chapter = quest.empty() ? -1 : quest[0];
        const int main8 = quest.size() > 9 ? quest[1 + 8] : -1;
        check(chapter == 3 && area == 9 && map == 0 && main8 == 1,
              "the save: chapter " + std::to_string(chapter) + ", main 8 " + std::to_string(main8) +
                  " in quest0, map " + std::to_string(area) + "." + std::to_string(map) +
                  " in theif0 (want 3, 1, 9.0)");
    });
    then_do(r, "part E's hero",
            [](mt_cpu *cpu) { std::printf("part E: %s\n", hero_text(cpu, hero(cpu)).c_str()); });
    then_finish(r);
}
void chapter_3_e(Run &r, u64) { chapter_3_ending(r, false); }
void chapter_3_e_skipped(Run &r, u64) { chapter_3_ending(r, true); }

void chapter_3_e_checks(Run &r, u64) {
    const Ending &e = g_ending;
    // Scene 1 ends at step 202 (command 52), scene 2 at 430 (command 28): qol.md's table.
    check(e.step_1 >= 202 && e.step_2 >= 430,
          "both ending scenes ran to their last step: scene 1 to " + std::to_string(e.step_1) +
              " (of 202), scene 2 to " + std::to_string(e.step_2) + " (of 430)");
    if (!e.skip) {
        check(e.presses > 0 && r.scene_skip.stats().scenes == 0, "watched: A tapped the boxes (" +
                                                                     std::to_string(e.presses) +
                                                                     " presses), no skip ran");
        return;
    }
    // The skip ran on the entrance scene and on both ending scenes.
    check(e.presses == 0 && r.scene_skip.stats().scenes >= 3,
          "skipped: no test input in the ending, " + std::to_string(r.scene_skip.stats().scenes) +
              " scenes skipped (entrance, two "
              "ending scenes), " +
              std::to_string(r.scene_skip.stats().taps) + " taps");
    const char *watched = std::getenv("MT_SKIP_WATCHED");
    if (watched == nullptr || *watched == 0) {
        check(false, "MT_SKIP_WATCHED names the watched run's OUT_DIR");
        return;
    }
    std::ifstream f(fs::path(watched) / "ending_state.txt");
    std::map<std::string, std::string> want;
    for (std::string line; std::getline(f, line);) {
        const std::size_t eq = line.find(" = ");
        if (eq != std::string::npos) {
            want[line.substr(0, eq)] = line.substr(eq + 3);
        }
    }
    check(!want.empty(), std::string("the watched run's ending state is in ") + watched);
    std::size_t same = 0;
    for (const auto &[key, v] : e.state) {
        const auto it = want.find(key);
        const std::string w = it != want.end() ? it->second : "(missing)";
        if (w == v) {
            same++;
        } else {
            check(false, key + ": watched '" + w + "', skipped '" + v + "'");
        }
    }
    check(same == e.state.size() && same > 0,
          "skipped = watched in " + std::to_string(same) + " of " + std::to_string(e.state.size()) +
              " fields (map, chapter, quests, both scenes' last steps)");
}

// --- after the ending (part F, three processes) ---
//
//   chapter_3_postgame_quit      from part E's save (chapter 3 on 9.0, main 8 open): loaded and
//                                saved again at once and the pair compared; then free play goes
//                                on (a kill on 9.1), Save, the hero's state and the files
//                                recorded, and the process ends
//   chapter_3_postgame_relaunch  a fresh process on that data: the files are as saved, the load
//                                gives the hero as recorded, Save again (compared), play goes on
//   chapter_3_postgame_reload    a third process: it loads the relaunch's save and saves it again,
//                                and the bytes are the same, every one the game writes
// The game's load re-adds each saved stack of other items through the inventory, which tops up
// the first stack of that id that is not full (docs/formats/save.md#etc-inventory-after-a-load):
// a save with two partial stacks of one item comes back with the counts moved between them, the
// ids and totals the same. A save made after a load holds no such pair, so it reloads byte for
// byte (the reload process). The first two compares allow only that move and print it; the
// never-written heap bytes (unwritten(), docs/formats/save.md) are left out and counted.
constexpr std::size_t k_etc_at = 0x325, k_etc_slots = 48;

std::string save_diff(const char *name, const std::vector<std::uint8_t> &a,
                      const std::vector<std::uint8_t> &b, bool topped_up, int *heap,
                      std::string *moved) {
    std::string diff;
    const bool theif = std::string(name) == "theif0";
    const auto in_etc = [theif](std::size_t i) {
        return theif && i >= k_etc_at && i < k_etc_at + 3 * k_etc_slots;
    };
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        const int x = i < a.size() ? a[i] : -1, y = i < b.size() ? b[i] : -1;
        if (x == y || (topped_up && in_etc(i) && (i - k_etc_at) % 3 != 0)) {
            continue;
        }
        if (unwritten(name, i)) {
            ++*heap;
            continue;
        }
        char buf[48];
        std::snprintf(buf, sizeof buf, " +0x%03zX %d->%d", i, x, y);
        diff += buf;
    }
    if (!topped_up || !theif || a.size() < k_etc_at + 3 * k_etc_slots || a.size() != b.size()) {
        return diff;
    }
    // The other items' counts: each id's total the same, the stacks that moved printed.
    std::map<int, int> total_a, total_b;
    for (std::size_t s = 0; s < k_etc_slots; s++) {
        const std::size_t o = k_etc_at + 3 * s;
        const int id = a[o];
        const int ca = static_cast<std::int16_t>(a[o + 1] | (a[o + 2] << 8));
        const int cb = static_cast<std::int16_t>(b[o + 1] | (b[o + 2] << 8));
        if (id == 0xFF) {
            continue;
        }
        total_a[id] += ca;
        total_b[id] += cb;
        if (ca != cb) {
            *moved += " slot " + std::to_string(s) + " item " + std::to_string(id) + " " +
                      std::to_string(ca) + "->" + std::to_string(cb);
        }
    }
    if (total_a != total_b) {
        diff += " (the other items' totals differ)";
    }
    return diff;
}

void record_save(Run &r, mt_cpu *cpu, const std::string &prefix);

// Save, then theif0 and quest0 compared with `dir`/`prefix`<name>: every byte the game writes,
// or with `topped_up` the other items' counts as each id's total (the load's top-up, above). With
// `next`, the hero's state and the pair are recorded as `next`<name> while "saved" shows.
void then_save_same(Run &r, const std::string &what, const fs::path &dir, const std::string &prefix,
                    bool topped_up, const std::string &next = "") {
    then_save(r, [&r, what, dir, prefix, topped_up, next](mt_cpu *cpu) {
        if (!next.empty()) {
            record_save(r, cpu, next);
        }
        for (const char *name : {"theif0", "quest0"}) {
            const std::vector<std::uint8_t> first = read_file(dir / (prefix + name));
            const std::vector<std::uint8_t> again = read_save(r, name);
            int heap = 0;
            std::string moved;
            const std::string diff = save_diff(name, first, again, topped_up, &heap, &moved);
            check(!first.empty() && first.size() == again.size() && diff.empty(),
                  what + ": " + name + " the same" +
                      (topped_up ? " but for the load's top-up of item stacks" : " byte for byte") +
                      " (" + std::to_string(again.size()) + " bytes; " + std::to_string(heap) +
                      " never-written heap bytes differ" +
                      (moved.empty() ? "" : "; moved:" + moved) + ")" + diff);
        }
    });
}

// Records the hero's state and the save pair as `prefix`theif0 / quest0 in OUT_DIR.
void record_save(Run &r, mt_cpu *cpu, const std::string &prefix) {
    write_state(r.out / k_state_file, saved_state(cpu));
    for (const char *name : {"theif0", "quest0"}) {
        std::error_code ec;
        fs::copy_file(r.data / "saves" / name, r.out / (prefix + name),
                      fs::copy_options::overwrite_existing, ec);
        check(!ec, std::string("the save's ") + name + " is recorded as " + prefix + name);
    }
}

void chapter_3_postgame_quit(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    // The pair as part E saved it, before this process writes anything.
    then_do(r, "part E's save kept", [&r](mt_cpu *) {
        for (const char *name : {"theif0", "quest0"}) {
            std::error_code ec;
            fs::copy_file(r.data / "saves" / name, r.out / (std::string("loaded_") + name),
                          fs::copy_options::overwrite_existing, ec);
            check(!ec, std::string("part E's ") + name + " is kept");
        }
    });
    then_check(r, "the post-game save loaded: chapter 3 on 9.0, main 8 open", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.on(9, 0) && quest_state(cpu, h, 0, 8) == 1 &&
               quest_state(cpu, h, 0, 7) == 2;
    });
    then_save_same(r, "part E's save loaded and saved again", r.out, "loaded_", true);
    // Free play after the ending: a map, a kill, the quests as they were.
    auto first = std::make_shared<std::int64_t>(-1);
    then_farm(
        r, "a kill on 9.1 after the ending", 9, 1, {34, 25},
        [first](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            const std::int64_t exp = h.exp + std::int64_t{1000000000} * h.level;
            *first = *first < 0 ? exp : *first;
            return exp > *first;
        },
        1200, chapter_3_fight());
    then_go(r, "back to 9.0", {9, 0, -1, -1, 30}, 600);
    then_save(r, [&r](mt_cpu *cpu) {
        record_save(r, cpu, "saved_");
        const Hero h = hero(cpu);
        check(h.chapter == 3 && h.on(9, 0) && quest_state(cpu, h, 0, 8) == 1,
              "saved after the ending, chapter 3 on 9.0, main 8 open; " + hero_text(cpu, h));
    });
    then_finish(r);
}
void chapter_3_postgame_quit_checks(Run &, u64) {}

// The relaunch and the reload: before the boot the files are what the process before saved
// (`prefix`), the load gives the hero as recorded, and Save writes the same bytes (`topped_up`
// as then_save_same), recorded as `next`.
void postgame_load(Run &r, const std::string &prefix, const std::string &next, bool topped_up) {
    using namespace quest_bot;
    for (const char *name : {"theif0", "quest0"}) {
        const std::vector<std::uint8_t> saved = read_file(r.out / (prefix + name));
        check(!saved.empty() && read_save(r, name) == saved,
              std::string("before the boot saves/") + name + " holds the bytes saved (" +
                  std::to_string(saved.size()) + ")");
    }
    from_save(r);
    then_do(r, "the hero after the load", [&r](mt_cpu *cpu) {
        const auto want = read_state(r.out / k_state_file);
        auto got = saved_state(cpu);
        std::string diff;
        for (const auto &[k, v] : want) {
            const auto it = got.find(k);
            const std::string g = it != got.end() ? it->second : "<none>";
            const bool near = (k == "x" || k == "y") && it != got.end() &&
                              std::abs(std::stoi(g) - std::stoi(v)) <= 2;
            // The other items' stacks may be topped up by the load (above).
            const bool stack = k.rfind("etc_", 0) == 0 && it != got.end() &&
                               g.substr(0, g.find(' ')) == v.substr(0, v.find(' '));
            if (g != v && !near && !stack) {
                diff += "\n  " + k + ": saved " + v + ", loaded " + g;
            }
        }
        check(!want.empty() && got.size() == want.size() && diff.empty(),
              "a fresh process loads the post-game hero as saved: " + std::to_string(want.size()) +
                  " facts (level " + got["level"] + ", chapter " + got["chapter"] + ", map " +
                  got["map"] + ", main quests " + got["main_quests"].substr(0, 9) + ")" + diff);
    });
    then_save_same(r, "the load saved again", r.out, prefix, topped_up, next);
}

void chapter_3_postgame_relaunch(Run &r, u64) {
    using namespace quest_bot;
    postgame_load(r, "saved_", "again_", true);
    then_go(r, "on to 9.1 after the relaunch", {9, 1, -1, -1, 30}, 600);
    then_check(r, "free play goes on: chapter 3 on 9.1, main 8 still open", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.on(9, 1) && quest_state(cpu, h, 0, 8) == 1;
    });
    then_finish(r);
}
void chapter_3_postgame_relaunch_checks(Run &, u64) {}

// The relaunch's save, made after a load, reloads byte for byte.
void chapter_3_postgame_reload(Run &r, u64) {
    postgame_load(r, "again_", "reloaded_", false);
    then_finish(r);
}
void chapter_3_postgame_reload_checks(Run &, u64) {}

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
//
// widescreen_fill_on (#425, ADR 0214) is widescreen_on with fill's 768-point GL layer (no window
// to cap it): Henesys 2.0 and 2.3 drawn 768 wide, every tick equal to the off run's. On both
// maps its wide shot is checked for the tiled backdrop's seam at GL x = 480: where the picture
// shows only the backdrop left of it (each of the 10 columns before 480 equal to the column 160
// to its left: the backdrop's period), the 10 columns after 480 continue it the same way.

struct Seam {
    u64 rows = 0;     // rows that show only the backdrop just left of x = 480
    u64 seamless = 0; // of them, those that continue it past x = 480
};

struct WideRun {
    std::ofstream state;
    u64 last_tick = ~0ull;
    u64 lines = 0;
    std::map<std::string, std::set<u32>> views; // "area.map" -> the view widths seen
    std::map<std::string, u64> ticks_on;        // "area.map" -> ticks seen there
    std::map<std::string, Seam> seams;          // "area.map" -> the seam check (fill)
};
WideRun g_wide;

// The wide picture's width: the run's GL layer (568, or 768 for widescreen = fill, #425).
u32 wide_width(const Run &r) { return r.widescreen > 480u ? r.widescreen : MT_WIDESCREEN_WIDE; }

// The window's picture: the GL layer's first `view` points centred in a wide_width-point
// picture, the 480-point composited screen over its middle (Presenter::draw's wings), to
// OUT_DIR/name.png. `pad` (#168): the key pad's groups as the frontend places them, and the
// strips of its keys moved into the wings (keypad_wings_composite) over the wings.
// The window's wide picture now (wide_width x 320 at 1x: the GL frame, the screen image over its
// middle and, with `pad`, the key pad's strips in the wings) into `out`; its view's width in
// `view_out`. False when it cannot be composed (or the render scale is not 1).
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
    const u32 w = wide_width(r);
    constexpr u32 h = MT_WIDESCREEN_HEIGHT;
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
    const u32 wing = (w - 480) / 2;
    if (pad && fe::keypad_wings_composite(cpu, now, wing, 1, &strips)) {
        // Premultiplied over: the left strip on columns 0 ... wing - 1, the right one on
        // 480 + wing ... w - 1.
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

// wide_pixels to OUT_DIR/name.png. `over`, if set, draws on the wide_width x 320 picture (RGBA8)
// before it is written (the minimap's corner).
void wide_shot(Run &r, mt_cpu *cpu, const std::string &name, bool pad = false,
               const std::function<void(std::vector<std::uint8_t> &)> &over = {}) {
    const u32 w = wide_width(r);
    constexpr u32 h = MT_WIDESCREEN_HEIGHT;
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

// The tiled backdrop's seam at GL x = 480 in the window's picture now (fill, #425): see above.
void wide_seam(Run &r, mt_cpu *cpu, const std::string &map) {
    std::vector<std::uint8_t> px;
    u32 view = 0;
    if (!wide_pixels(r, cpu, false, &px, &view) || view < 480 + 10) {
        return;
    }
    const u32 w = wide_width(r), left = (w - view) / 2; // the GL frame's column 0
    const auto same = [&px, w, left](u32 y, u32 x) {    // GL column x equals x - 160
        const std::uint8_t *a = &px[(static_cast<std::size_t>(y) * w + left + x) * 4];
        return std::memcmp(a, a - 160 * 4, 3) == 0;
    };
    Seam &s = g_wide.seams[map];
    for (u32 y = 0; y < MT_WIDESCREEN_HEIGHT; y++) {
        bool before = true, after = true;
        for (u32 d = 1; d <= 10; d++) {
            before = before && same(y, 480 - d);
            after = after && same(y, 480 + d - 1);
        }
        s.rows += before ? 1 : 0;
        s.seamless += before && after ? 1 : 0;
    }
    std::printf("seam %s: %llu rows show only the backdrop left of x 480, %llu continue it\n",
                map.c_str(), static_cast<unsigned long long>(s.rows),
                static_cast<unsigned long long>(s.seamless));
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
        if (r.widescreen == MT_WIDESCREEN_FILL) {
            wide_seam(r, cpu, map);
        }
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

int widescreen_fill_on_setup(Run &r) {
    const int code = widescreen_on_setup(r);
    r.widescreen = MT_WIDESCREEN_FILL;
    return code;
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
    const u32 wide = r.widescreen; // 568, or fill's 768 (#425)
    const std::string points = std::to_string(wide) + " points";
    for (const std::string &l : g_menus.lines) {
        check(l.find(" view " + std::to_string(wide)) != std::string::npos,
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
        "%s: %llu state lines; %llu turns, %llu wide, %llu changes, %llu camera hook calls, %llu "
        "backdrops stretched, %llu tiled; views%s\n",
        r.mode.c_str(), static_cast<unsigned long long>(g_wide.lines),
        static_cast<unsigned long long>(st.turns), static_cast<unsigned long long>(st.wide_turns),
        static_cast<unsigned long long>(st.changes),
        static_cast<unsigned long long>(st.camera_calls),
        static_cast<unsigned long long>(st.backdrops), static_cast<unsigned long long>(st.tiles),
        views_text().c_str());
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
    check(has("2.0", wide) && has("2.3", wide),
          "Henesys (2.0) and the field 2.3 are drawn " + points + " wide");
    check(st.backdrops + st.tiles > 0 && st.camera_calls > 0,
          "the backdrop is widened (tiled or stretched), the camera hooked");
    if (wide == MT_WIDESCREEN_FILL) {
        // The tiled backdrop's seam (#425): Henesys's backdrop repeats every 160 points.
        check(st.tiles > 0, "Henesys's backdrop is tiled, not stretched");
        for (const char *map : {"2.0", "2.3"}) {
            const Seam s = g_wide.seams[map];
            check(s.rows >= 20 && s.seamless * 100 >= s.rows * 95,
                  std::string("no seam at x = 480 on ") + map + ": " + std::to_string(s.seamless) +
                      " of " + std::to_string(s.rows) +
                      " rows showing the backdrop left of it continue it past it");
        }
    }
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

// --- widescreen scenes (#396, ADR 0090's addendum, docs/re/widescreen.md) ----------------------
//
// widescreen_scene_off and widescreen_scene_on start from a built start on a map (MT_FROM_SAVE,
// tests/starts/widescreen_scene_*.json: synthetic) and, a second into the field, queue scene
// MT_SCENE as the map scripts' runAnimation does (CStateGame +0x1E4 = the scene, then
// QueueScene 0x80935: game mode 10). A goes down once a second while a speech box waits, until
// the field shows for 2 s (a scene that ends in another scene, 16 to 19, runs on through the
// chain). Each run writes the state line of widescreen_off at every game tick (state.txt) and,
// at every tick, the game mode, the scene (+0x1E4), its step, the view width E, the camera's x
// (CMMap +0x70) and the hero's map and x (scene.txt). widescreen_scene_on runs at 568 points and
// compares state.txt with the off run's (MT_WIDESCREEN_SCENE_BASE) line by line. It also checks
// E at the scene's edges: for a scene on mt_widescreen_scene_wide's list that starts or ends in
// a wide field, E on the last field tick equals E on the first scene tick (and the same at the
// scene's end); an unlisted scene is drawn at 480. Both runs write window shots every 25 steps
// and at each step a picture is up (OUT_DIR/scene_<n>_step_<s>.png, scene_<n>_picture_<s>.png,
// the wide picture in the on run; never committed).

void wr(mt_cpu *cpu, u32 addr, u32 v);                // below, with systems sweep 2
u32 call(mt_cpu *cpu, u32 fn, std::vector<u32> args); // the same

struct SceneEdge {
    u64 tick = 0;
    i32 from_mode = 0, to_mode = 0;
    u32 from_view = 0, to_view = 0;
    i32 scene = -1;
    std::string map;
};
struct WideSceneRun {
    i32 scene = -1;
    std::ofstream log;
    u64 last_tick = ~0ull;
    i32 last_mode = -1;
    u32 last_view = 0;
    i32 last_scene = -1;
    std::string last_map;
    std::vector<SceneEdge> edges;              // field (0) <-> scene (9, 10)
    std::map<i32, std::set<u32>> views;        // scene -> the view widths in game mode 9
    std::map<i32, std::set<std::string>> maps; // scene -> the maps it ran on
    std::set<std::string> shots;
    bool queued = false; // by the test (else the map's script played it)
};
WideSceneRun g_wscene;

bool scene_mode(i32 mode) { return mode == 9 || mode == 10; }

// Once a game tick, after wide_tick: scene.txt's line, the edges, a shot every 25 steps.
void wide_scene_tick(Run &r, mt_cpu *cpu) {
    if (r.ticks == g_wscene.last_tick) {
        return;
    }
    g_wscene.last_tick = r.ticks;
    const u32 st = game_state(cpu);
    if (st == 0 || rd(cpu, st) != 0x119B78u) {
        return;
    }
    const Hero h = hero(cpu);
    const i32 mode = game_mode(cpu);
    const i32 scene = rdi(cpu, st + 0x1E4);
    const fe::SceneView v = fe::read_scene_view(fe::guest_reader(cpu));
    const u32 map = rd(cpu, st + 0x2C);
    const i32 camera = map != 0 ? rdi(cpu, map + 0x70) : -1;
    const u32 view = mt_widescreen_view();
    const std::string where = std::to_string(h.area) + "." + std::to_string(h.map);
    g_wscene.log << r.ticks << " mode " << mode << " scene " << scene << " step " << v.step
                 << " view " << view << " camera " << camera << " hero " << where << " " << h.x
                 << '\n';
    const bool was = scene_mode(g_wscene.last_mode), is = scene_mode(mode);
    if (g_wscene.last_mode >= 0 && was != is && (g_wscene.last_mode == 0 || mode == 0)) {
        g_wscene.edges.push_back({r.ticks, g_wscene.last_mode, mode, g_wscene.last_view, view,
                                  is ? scene : g_wscene.last_scene,
                                  is ? where : g_wscene.last_map});
    }
    if (mode == 9) {
        g_wscene.views[scene].insert(view);
        g_wscene.maps[scene].insert(where);
        // A picture up (CStateGame +0x498: the five slots' images, docs/re/qol.md's command 30).
        const u32 slots = rd(cpu, st + 0x498);
        bool picture = false;
        for (u32 i = 0; slots != 0 && i < 5; i++) {
            picture = picture || rd(cpu, slots + 4 * i) != 0;
        }
        const std::string name = "scene_" + std::to_string(scene) +
                                 (picture ? "_picture_" : "_step_") + std::to_string(v.step);
        if (v.step >= 0 && (picture || v.step % 25 == 0) && g_wscene.shots.insert(name).second) {
            if (r.widescreen != 0) {
                wide_shot(r, cpu, name);
            } else {
                save_shot(r, cpu, name);
            }
        }
    }
    g_wscene.last_mode = mode;
    g_wscene.last_view = view;
    g_wscene.last_scene = scene;
    g_wscene.last_map = where;
}

void widescreen_scene(Run &r, u64) {
    from_save(r, [](mt_cpu *cpu) { return field(cpu) || game_mode(cpu) == 9; });
    std::function<void(mt_cpu *)> inner = r.on_turn;
    r.on_turn = [&r, inner](mt_cpu *cpu) {
        inner(cpu);
        wide_tick(r, cpu);
        wide_scene_tick(r, cpu);
    };
    // A map whose script plays the scene on entry (PerionMap: 15, 20) plays it on its own.
    then(r, "the field, or the scene on the way in", 600, [&r](mt_cpu *cpu, double el) {
        hold(r, A, mt_input_panel() == 3 && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && el >= 1.0;
    });
    then_do(r, "scene " + std::to_string(g_wscene.scene) + " queued (runAnimation)",
            [](mt_cpu *cpu) {
                if (g_wscene.views.count(g_wscene.scene) != 0) {
                    std::printf("scene %d played on the way in\n", g_wscene.scene);
                    return;
                }
                const u32 st = game_state(cpu);
                wr(cpu, st + 0x1E4, static_cast<u32>(g_wscene.scene));
                call(cpu, 0x80935u, {st});
                g_wscene.queued = true;
            });
    auto since = std::make_shared<double>(-1);
    then(r, "A through the scene, then 2 s of field", 600, [&r, since](mt_cpu *cpu, double el) {
        hold(r, A, mt_input_panel() == 3 && std::fmod(el, 1.0) < 0.1);
        if (!field(cpu) || g_wscene.views.empty()) {
            *since = -1;
            return false;
        }
        *since = *since < 0 ? el : *since;
        return el - *since >= 2.0;
    });
    then_finish(r);
}

int widescreen_scene_off_setup(Run &r) {
    const char *scene = std::getenv("MT_SCENE");
    if (scene == nullptr || std::sscanf(scene, "%d", &g_wscene.scene) != 1 || g_wscene.scene < 0 ||
        g_wscene.scene > 20) {
        std::printf("FAIL: MT_SCENE must name a scene, 0 ... 20\n");
        return 1;
    }
    g_wscene.log.open(r.out / "scene.txt", std::ios::binary | std::ios::trunc);
    return widescreen_off_setup(r);
}

int widescreen_scene_on_setup(Run &r) {
    const char *base = std::getenv("MT_WIDESCREEN_SCENE_BASE");
    if (base == nullptr || !fs::is_regular_file(fs::path(base) / "state.txt")) {
        std::printf("skipped: MT_WIDESCREEN_SCENE_BASE names no widescreen_scene_off run\n");
        return 77;
    }
    r.widescreen = MT_WIDESCREEN_WIDE;
    return widescreen_scene_off_setup(r);
}

std::string scene_views_text() {
    std::string text;
    for (const auto &[scene, set] : g_wscene.views) {
        text += " scene " + std::to_string(scene) + " on";
        for (const std::string &m : g_wscene.maps[scene]) {
            text += " " + m;
        }
        text += ":";
        for (const u32 v : set) {
            text += " " + std::to_string(v);
        }
    }
    return text;
}

void widescreen_scene_common_checks() {
    g_wide.state.close();
    g_wscene.log.close();
    std::printf("widescreen_scene: %llu state lines; views%s\n",
                static_cast<unsigned long long>(g_wide.lines), scene_views_text().c_str());
    for (const SceneEdge &e : g_wscene.edges) {
        std::printf("edge at tick %llu: mode %d -> %d, view %u -> %u (scene %d on %s)\n",
                    static_cast<unsigned long long>(e.tick), e.from_mode, e.to_mode, e.from_view,
                    e.to_view, e.scene, e.map.c_str());
    }
    check(g_wscene.views.count(g_wscene.scene) != 0,
          "scene " + std::to_string(g_wscene.scene) + " ran (game mode 9)");
    check(g_wide.lines > 0, "the state was written at every tick");
}

void widescreen_scene_off_checks(Run &, u64) {
    widescreen_scene_common_checks();
    const mt_widescreen_stats st = mt_widescreen_get_stats();
    check(st.changes == 0 && st.wide_turns == 0, "widescreen off: the view never left 480 points");
}

void widescreen_scene_on_checks(Run &r, u64) {
    widescreen_scene_common_checks();
    // At each edge between the field and a scene: a listed scene keeps the field's E, an
    // unlisted one drops to 480.
    for (const SceneEdge &e : g_wscene.edges) {
        const bool wide = mt_widescreen_scene_wide(static_cast<u32>(e.scene)) != 0;
        const std::string what = "scene " + std::to_string(e.scene) + " on " + e.map + ", mode " +
                                 std::to_string(e.from_mode) + " -> " + std::to_string(e.to_mode) +
                                 ": view " + std::to_string(e.from_view) + " -> " +
                                 std::to_string(e.to_view);
        if (wide) {
            check(e.from_view == e.to_view, "a listed scene keeps the field's width: " + what);
        } else {
            check(e.from_view == MT_WIDESCREEN_NARROW || e.to_view == MT_WIDESCREEN_NARROW,
                  "an unlisted scene is 480 points wide: " + what);
        }
    }
    for (const auto &[scene, set] : g_wscene.views) {
        if (mt_widescreen_scene_wide(static_cast<u32>(scene)) == 0) {
            check(set == std::set<u32>{MT_WIDESCREEN_NARROW},
                  "unlisted scene " + std::to_string(scene) + " drawn at 480 points");
        }
    }
    std::ifstream a(r.out / "state.txt", std::ios::binary);
    std::ifstream b(fs::path(std::getenv("MT_WIDESCREEN_SCENE_BASE")) / "state.txt",
                    std::ios::binary);
    std::string la, lb, first;
    u64 n = 0;
    bool same = true;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(a, la));
        const bool gb = static_cast<bool>(std::getline(b, lb));
        if (!ga && !gb) {
            break;
        }
        n++;
        if (ga != gb || la != lb) {
            same = false;
            first = "line " + std::to_string(n) + ": on '" + la + "', off '" + lb + "'";
            break;
        }
    }
    check(same && n > 0, "the game state at every tick equals the 3:2 run's (" + std::to_string(n) +
                             " ticks)" + (same ? "" : ": " + first));
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
            check(mt_widescreen_view() == r.widescreen, "Henesys (2.0) is drawn " +
                                                            std::to_string(r.widescreen) +
                                                            " points wide with the key pad shown");
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
            const float off = static_cast<float>(r.widescreen - MT_WIDESCREEN_NARROW) / 2;
            check(mt_widescreen_view() == r.widescreen && fe::installed_keypad_wide().off == off &&
                      r.input.side_margin() == off,
                  "2.3 is drawn " + std::to_string(r.widescreen) +
                      " points wide with the key pad shown, its groups (E - 480) / 2 = " +
                      std::to_string(static_cast<int>(off)) + " points out");
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

int widescreen_pad_fill_on_setup(Run &r) {
    const int code = widescreen_pad_on_setup(r);
    r.widescreen = MT_WIDESCREEN_FILL;
    return code;
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
    check(g_wpad.hold_view == r.widescreen && st.hits > 0 && st.remapped > 0,
          "widescreen on: the hold at " + std::to_string(r.widescreen) +
              " points, the mask read through the hook");
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

// chat_text's flow up to the field after MacArthur's dialog (speech_dialogue's too).
void queue_chat_text(Run &r) {
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
}

void chat_text(Run &r, u64) {
    queue_chat_text(r);
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
// With its extras on (#397, minimap_mobs and minimap_name): on 2.3 in every turn a mob dot for
// every live mob of the list (guest_state's, the dead flag +0x0C & 0x10 clear) where its feet map
// to, and the name the knowledge base gives 2.3 in the run's language (MAPLETHIEF_LANGUAGE),
// written by `mtport.compendium.world map-name` to MT_MINIMAP_NAME (minimap.kb_name_2_3), on its
// strip.
struct MinimapRun {
    u64 field_turns = 0, shown_turns = 0, hero_mismatch = 0, menu_turns = 0, menu_shown = 0;
    u64 on_23 = 0, dot_bad = 0, border_turns = 0, moves = 0, moves_left = 0, moves_right = 0;
    u64 moves_wrong = 0, marker_bad = 0, hunt_start = 0;
    u64 mob_turns = 0, mobs_live = 0, mobs_bad = 0, name_bad = 0, name_turns = 0;
    std::string want_name, lang, first_mob_bad, got_name;
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
    const fe::MinimapView v = fe::read_minimap(
        fe::guest_span(cpu), fe::MinimapExtras{r.settings.minimap_mobs, r.settings.minimap_name});
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
            if (mk.kind == fe::MinimapMarker::kMob) {
                continue; // the mob list's, checked below
            }
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
    // The extras: a mob dot for every live mob; the name the knowledge base's.
    std::vector<fe::MinimapDot> dots = fe::minimap_dots(v, l);
    m.mob_turns++;
    for (const Mob &mob : mobs(cpu)) {
        if ((rd(cpu, mob.addr + 0x0C) & fe::kMinimapMobDead) != 0) {
            continue;
        }
        m.mobs_live++;
        const bool marked =
            std::find(v.markers.begin(), v.markers.end(),
                      fe::MinimapMarker{fe::MinimapMarker::kMob, mob.x, mob.y}) != v.markers.end();
        bool dotted = false;
        for (const fe::MinimapDot &d : dots) {
            dotted = dotted || (d.color.r == fe::kMinimapMob.r && d.color.g == fe::kMinimapMob.g &&
                                d.color.b == fe::kMinimapMob.b &&
                                dot_holds(l, d.rect, static_cast<float>(mob.x),
                                          static_cast<float>(mob.y - v.cell_h)));
        }
        if (!marked || !dotted) {
            if (m.mobs_bad++ == 0) {
                m.first_mob_bad = "mob " + std::to_string(mob.id) + " at (" +
                                  std::to_string(mob.x) + ", " + std::to_string(mob.y) + ")" +
                                  (marked ? "" : " has no marker") + (dotted ? "" : " has no dot");
            }
        }
    }
    m.name_turns++;
    if (v.name != m.want_name || !fe::minimap_name_strip(v, l).shown) {
        m.name_bad++;
        m.got_name = v.name;
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

void then_korean_terms(Run &r); // the Korean runs' (below)

void minimap(Run &r, u64 goal) {
    attach_pad(r);
    g_minimap = std::make_shared<MinimapRun>();
    r.settings.minimap = fe::Minimap::kOverlay;
    r.settings.minimap_mobs = r.settings.minimap_name = true;
    // The knowledge base's name of 2.3 in the run's language (minimap.kb_name_2_3's file).
    const char *lang = std::getenv("MAPLETHIEF_LANGUAGE");
    g_minimap->lang = lang != nullptr && std::string(lang) == "ko" ? "ko" : "en";
    if (const char *names = std::getenv("MT_MINIMAP_NAME")) {
        std::ifstream in(names);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind(g_minimap->lang + "\t", 0) == 0) {
                g_minimap->want_name = line.substr(g_minimap->lang.size() + 1);
            }
        }
    }
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        minimap_tick(r, cpu);
    };
    if (g_minimap->lang == "ko") {
        then_korean_terms(r); // the Korean bundle's terms screen first
    }
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
    check(m.mob_turns == m.on_23 && m.mobs_live > 0 && m.mobs_bad == 0,
          "a mob dot for every live mob on 2.3 in every turn (" + std::to_string(m.mobs_live) +
              " mob-turns over " + std::to_string(m.mob_turns) + " turns, " +
              std::to_string(m.mobs_bad) + " without" +
              (m.first_mob_bad.empty() ? "" : "; first: " + m.first_mob_bad) + ")");
    check(!m.want_name.empty() && m.name_turns > 0 && m.name_turns == m.on_23 && m.name_bad == 0,
          "the name on 2.3 is the knowledge base's (" + m.lang + ": '" + m.want_name +
              "') on its strip in every turn (" + std::to_string(m.name_bad) + " of " +
              std::to_string(m.name_turns) + " not" +
              (m.got_name.empty() && m.name_bad == 0 ? "" : "; got '" + m.got_name + "'") + ")");
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

// --- the playthrough's checkpoints (#306, docs/testing/regression-replay.md) -------------------
//
// MT_CHECKPOINTS=N (scripts/playthrough sets it): a mode the bot plays (a chapter's part, the
// post-game) takes ADR 0104's checkpoints as the regression run does: the composited screen, the
// audio so far, the GL frame and regress_state's line, every N steps (0: none) and one "end" at
// the end of the turn the part is done in. It writes them to OUT_DIR/checkpoints_MODE.txt (the
// post-game's processes share OUT_DIR) and prints their digest. The replay's own modes keep
// their plan. MT_NUDGE_TICK=STEP moves the virtual clock one game tick more at that step: a
// deliberate one-tick change, for checking that the comparison catches one.

bool playthrough_setup(Run &r) {
    if (const char *n = std::getenv("MT_NUDGE_TICK"); n != nullptr && *n != 0) {
        r.nudge_step = std::strtoull(n, nullptr, 10);
        std::printf("playthrough: the clock moves one tick more at step %llu (MT_NUDGE_TICK)\n",
                    static_cast<unsigned long long>(r.nudge_step));
    }
    const char *every = std::getenv("MT_CHECKPOINTS");
    if (every == nullptr || *every == 0 || r.replay != nullptr || r.record != nullptr) {
        return false;
    }
    r.audio = std::make_unique<fe::AudioHash>();
    if (!r.state) {
        r.state = regress_state;
    }
    r.plan.set_every(std::strtoull(every, nullptr, 10));
    r.end_checkpoint = true;
    return true;
}

void playthrough_checks(Run &r) {
    check(r.checkpoints.find("end") != nullptr, "the part's end checkpoint was taken");
    const fs::path path = r.out / ("checkpoints_" + r.mode + ".txt");
    std::ofstream log(path, std::ios::binary);
    log << r.checkpoints.text();
    log.close();
    check(static_cast<bool>(log), path.filename().string() + " is written");
    std::printf("playthrough: %s: %zu checkpoints, digest %s\n", r.mode.c_str(),
                r.checkpoints.points().size(), r.checkpoints.digest().c_str());
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
    // MT_SAVE_STATE_MEASURE=1 (#334, ADR 0151): the same point saved stored (not deflated) for
    // its raw size and time, and the deflated state written as a file the way quick_state.cpp
    // writes one (mt_saves_replace_file), for the write's time.
    if (std::getenv("MT_SAVE_STATE_MEASURE") != nullptr) {
        mt_snapshot_meta raw = meta;
        raw.flags |= MT_SNAPSHOT_STORE;
        const auto t1 = std::chrono::steady_clock::now();
        if (mt_snapshot_save(cpu, &raw, &bytes, &size, why, sizeof why) == 0) {
            const double raw_ms = ms_since(t1);
            std::free(bytes);
            const std::string path = (r.out / (std::string(p.name) + ".state")).string();
            const auto t2 = std::chrono::steady_clock::now();
            const int wrote = mt_saves_replace_file(path.c_str(), p.state.data(), p.state.size());
            std::printf("save_state: measure %s scale %u: raw %zu bytes in %.0f ms, deflated %zu "
                        "bytes in %.0f ms, file %s in %.0f ms\n",
                        p.name, r.settings.render_scale, size, raw_ms, p.state.size(), p.save_ms,
                        wrote == 0 ? "written" : "NOT written", ms_since(t2));
        }
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

// --- practice states per split (#409, practice_state.h, ADR 0189) ------------------------------
//
// practice_state: from part c's save (MT_FROM_SAVE), the hero in Perion (1.0), the speedrun timer
// times a synthetic category of three splits: the map change to 1.2, the one back to 1.0, and
// one never reached. The run starts at once (a run from a save, as chapter_1_l's). Each split
// arms a practice state, taken at the first safe turn after it (PracticeStates on the frontend's
// game_quick_ops, as game.cpp's turn hook runs it) to OUT_DIR/data/states/practice_test/: two
// splits, two states. Then F7 picks the first split's state and F8 loads it: the guest digest
// (the FNV-1a of mt_snapshot_save of the game right after the load) equals the state file's as
// taken, the hero's state line is the one at the take, and the timer is practice with its segment
// timer back at zero. The way back to 1.0 then ends the practised segment (and, a split of the
// practice, takes split 2's state again).
namespace practice_test {
fe::PracticeStates g_states;
fe::RunTimer *g_timer = nullptr;
struct Taken {
    std::string path, digest, state;
};
std::map<std::size_t, Taken> g_taken;
std::string g_loaded_digest, g_loaded_state, g_load_log;
fe::RunTime g_before_load, g_after_load;
std::size_t g_picked = 0;
std::uint64_t g_saves_at_load = 0; // the practice states taken before the load
bool g_practice = false;
std::size_t g_next = 0;
std::optional<fe::RunTime> g_segment;
std::vector<fe::RunRecord> g_records;
constexpr const char *k_category = "practice_test";

// The hero's state, without the harness's own counters (a load does not rewind them).
std::string hero_state(mt_cpu *cpu) {
    const Hero h = hero(cpu);
    char buf[200];
    std::snprintf(buf, sizeof buf, "map=%d.%d x=%d y=%d hp=%d mp=%d level=%d exp=%d meso=%d",
                  h.area, h.map, h.x, h.y, h.hp, h.mp, h.level, h.exp, h.meso);
    return buf;
}

std::string bytes_digest(const std::vector<std::uint8_t> &b) {
    return fe::hex64(fe::fnv1a64(b.data(), b.size()));
}

// The guest digest now: the game's state file at this moment, as a practice state takes it.
std::string guest_digest(mt_cpu *cpu, std::uint32_t slot) {
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    mt_process_image_sha256(mt_process_current(), meta.binary_sha256);
    meta.slot = slot;
    std::uint8_t *bytes = nullptr;
    std::size_t size = 0;
    char why[256] = {};
    if (mt_snapshot_save(cpu, &meta, &bytes, &size, why, sizeof why) != 0) {
        return std::string("no save: ") + why;
    }
    const std::string d = fe::hex64(fe::fnv1a64(bytes, size));
    std::free(bytes);
    return d;
}

fe::RunCategory category() {
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> c = fe::parse_run_category(k_category,
                                                              "title = Practice test\n"
                                                              "split map_change 1.2 = Dark Stumps\n"
                                                              "split map_change 1.0 = Perion\n"
                                                              "split chapter_end 9 = Never\n",
                                                              &problems);
    check(c.has_value() && problems.empty(), "the test category parses");
    return c.value_or(fe::RunCategory{});
}
} // namespace practice_test

void practice_state(Run &r, u64) {
    using namespace quest_bot;
    namespace pt = practice_test;
    from_save(r);
    static fe::RunTimer timer(fe::RunClock{[] {
                                               return fe::RunInstant{
                                                   static_cast<double>(SDL_GetTicksNS()) / 1e9,
                                                   mt_input_get_stats().ticks};
                                           },
                                           [] { return std::string("2026-11-27 00:00:00"); }});
    pt::g_timer = &timer;
    // Every turn: a split the timer reached arms its state; an armed state is taken at the first
    // safe turn.
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        if (const std::optional<std::size_t> s = pt::g_timer->take_reached()) {
            pt::g_states.reached(*s, pt::g_timer->category().splits.size());
            std::printf("practice: split %zu reached at step %llu\n", *s,
                        static_cast<unsigned long long>(r.step()));
        }
        if (!pt::g_states.armed()) {
            return;
        }
        const std::size_t split = *pt::g_states.armed();
        const fe::QuickResult res =
            pt::g_states.take_armed(run_quick_ops(r, cpu), r.data.string(), pt::k_category);
        if (res.done) {
            const fe::AutosaveCheck c = fe::check_autosave(cpu);
            pt::Taken t;
            t.path = fe::practice_state_path(r.data.string(), pt::k_category, c.slot, split);
            t.digest = pt::bytes_digest(
                fe::read_quick_state(t.path).value_or(std::vector<std::uint8_t>{}));
            t.state = pt::hero_state(cpu);
            std::printf("practice: %s; digest %s, %s\n", res.log.c_str(), t.digest.c_str(),
                        t.state.c_str());
            pt::g_taken[split] = t;
        }
    };
    then_do(r, "the speedrun timer: the test category, a run from here", [](mt_cpu *) {
        g_run_timer = pt::g_timer;
        pt::g_timer->set_category(pt::category(), std::nullopt);
        pt::g_timer->start();
        check(pt::g_timer->state() == fe::RunTimer::State::kRunning, "the timer runs");
    });
    const wm::Object *back = wm::get(1, 2).portal(1); // 1.2's portal to Perion
    const Goal on_1_2 = back != nullptr ? Goal{1, 2, back->x, back->y, 40} : Goal{1, 2, 0, 0, 40};
    then_go(r, "the Dark Stumps' map (1.2), by its portal back", on_1_2, 300);
    then_wait_for(r, "the practice state after split 1 (1.2)", 60,
                  [](mt_cpu *) { return pt::g_taken.count(1) == 1; });
    then_go(r, "back to Perion (1.0)", {1, 0, 376, 399, 30}, 300);
    then_wait_for(r, "the practice state after split 2 (1.0)", 60,
                  [](mt_cpu *) { return pt::g_taken.count(2) == 1; });
    then_wait(r, 1);
    // F7 picks the first split with a state, F8 loads it, as game.cpp's run_practice_state.
    then_do(r, "F7: pick; F8: load the first split's state", [&r](mt_cpu *cpu) {
        const fe::QuickOps ops = run_quick_ops(r, cpu);
        const fe::RunCategory &c = pt::g_timer->category();
        tap_key(r, SDLK_F7);
        check(pt::g_states.take(r.actions, true) == fe::PracticeRequest::kPick, "F7 picks");
        const std::optional<std::size_t> picked =
            pt::g_states.pick(pt::g_timer->picked(), c.splits.size(), ops, r.data.string(),
                              pt::k_category, fe::practice_state_exists);
        pt::g_picked = picked.value_or(0);
        pt::g_timer->set_picked(picked);
        tap_key(r, SDLK_F8);
        check(pt::g_states.take(r.actions, true) == fe::PracticeRequest::kLoad, "F8 loads");
        pt::g_before_load = pt::g_timer->elapsed();
        pt::g_saves_at_load = pt::g_states.stats().saves;
        const fe::QuickResult res =
            pt::g_states.load(ops, r.data.string(), pt::k_category, pt::g_picked);
        pt::g_load_log = res.log;
        if (!res.done) {
            return;
        }
        const fe::AutosaveCheck slot = fe::check_autosave(cpu);
        pt::g_loaded_digest = pt::guest_digest(cpu, static_cast<std::uint32_t>(slot.slot));
        pt::g_loaded_state = pt::hero_state(cpu);
        pt::g_timer->start_practice(pt::g_picked);
        pt::g_practice = pt::g_timer->practice();
        pt::g_next = pt::g_timer->next_split();
        pt::g_after_load = pt::g_timer->elapsed();
        pt::g_records = pt::g_timer->take_records();
        std::printf("practice: %s; digest %s, %s; overlay:\n", res.log.c_str(),
                    pt::g_loaded_digest.c_str(), pt::g_loaded_state.c_str());
        for (const std::string &line : pt::g_timer->overlay_lines()) {
            std::printf("  %s\n", line.c_str());
        }
    });
    then_go(r, "back to Perion (1.0) again: the practised segment", {1, 0, 376, 399, 30}, 300);
    then_do(r, "the practised segment's time", [](mt_cpu *) {
        pt::g_segment = pt::g_timer->last_segment();
        for (const std::string &line : pt::g_timer->overlay_lines()) {
            std::printf("  %s\n", line.c_str());
        }
        g_run_timer = nullptr;
    });
    then_finish(r);
}

void practice_state_checks(Run &, u64) {
    namespace pt = practice_test;
    check(pt::g_taken.size() == 2 && pt::g_saves_at_load == 2,
          "two splits took two practice states before the load (" +
              std::to_string(pt::g_saves_at_load) + ")");
    std::error_code ec;
    for (const auto &[split, t] : pt::g_taken) {
        check(fs::is_regular_file(t.path, ec),
              "split " + std::to_string(split) + "'s state is a file: " + t.path);
    }
    check(pt::g_picked == 1, "F7 picked the first split (" + std::to_string(pt::g_picked) + ")");
    check(pt::g_states.stats().loads == 1, "F8 loaded it: " + pt::g_load_log);
    const pt::Taken &first = pt::g_taken[1];
    check(!first.digest.empty() && pt::g_loaded_digest == first.digest,
          "the guest digest after the load is the one taken (" + pt::g_loaded_digest + ", taken " +
              first.digest + ")");
    check(pt::g_loaded_state == first.state,
          "the hero is where the state was taken (" + pt::g_loaded_state + ")");
    check(pt::g_practice && pt::g_next == 1,
          "the load marked the run as practice, timing split 2 (next " +
              std::to_string(pt::g_next) + ")");
    check(pt::g_records.size() == 1 && !pt::g_records[0].finished,
          "the run before the load ended as reset");
    check(pt::g_after_load.ms < pt::g_before_load.ms && pt::g_after_load.ms < 1000,
          "the segment timer restarted (" + fe::format_run_time(pt::g_before_load.ms) + " -> " +
              fe::format_run_time(pt::g_after_load.ms) + ")");
    check(pt::g_segment.has_value() && pt::g_segment->ms > 0 && pt::g_segment->ticks > 0,
          "the way back to 1.0 ended the practised segment (" +
              (pt::g_segment ? fe::format_run_time(pt::g_segment->ms) : std::string("none")) + ")");
}

// --- the save-state census (#202, ADR 0105, docs/re/save-states.md) -----------------------------
//
// --- crash recovery points across two processes (#335, ADR 0151, recovery.h) -------------------
//
// recovery_crash: the replay MT_REPLAY with crash_recovery on through the frontend's
// RecoveryPoints (the game's ops, the game-save hook, the timer at kRecoveryTestInterval), from a
// fresh data directory with #198's marker set. Points are taken at the replay's map loads and
// every 20 guest seconds; the first one at or after step 46,000 (the tutorial's map 2.8) whose
// turn has the replay's pad free is the point the test resumes. Taking it is the last point:
// 200 game ticks follow with a checkpoint after each (the uninterrupted run, OUT_DIR/
// uninterrupted.txt), the harness's own counters at the point go to OUT_DIR/harness.txt (and its
// last GL frame to harness_gl.rgba), then the write finishes and the run traps on purpose (as
// --force-crash trap): the marker stays, the points stay.
// recovery_resume: a new process on the same OUT_DIR. The marker says the last run crashed;
// find_recovery_point offers the newest point; #198's box (the test's prompt) answers Resume; the
// point is armed. The replay plays from its start until the character is in the field at a safe
// turn: the point is restored there (the frontend's input let go, the virtual clock re-based),
// the harness's counters from harness.txt, and 200 ticks run with a checkpoint after each: their
// log (OUT_DIR/resumed.txt) and digest must equal recovery_crash's.
constexpr double kRecoveryTestInterval = 20;
constexpr u64 kRecoveryFrom = 46000;
struct RecoveryRun {
    fe::RecoveryPoints points;
    std::optional<fe::RecoveryOps> ops;
    bool chosen = false, resumed = false, crashed = false;
    u64 point_step = 0, ticks_seen = 0, last_tick = 0, end = 0;
    std::size_t first_index = 0;
    std::uint64_t taken = 0;
    fe::RecoveryOffer offer;
    std::string drop;
    fe::CheckpointLog window;
    // The harness at the newest point.
    std::map<std::string, u64> harness;
    std::vector<std::uint8_t> gl;
    std::map<u32, i32> mob_hp;
};
RecoveryRun g_recovery;

fs::path recovery_crashes(const Run &r) { return r.data / "crashes"; }

// RecoveryOps on the game, made once (it hashes the binary): the replay's pad must be free at a
// point too (as save_state's), and a restore lets go of it and of the frontend's layers.
const fe::RecoveryOps &recovery_ops(Run &r, mt_cpu *cpu) {
    RecoveryRun &g = g_recovery;
    if (!g.ops) {
        fe::RecoveryOps ops = fe::game_recovery_ops(
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
        ops.safe = [&r, safe = ops.safe](std::string *why) {
            if (!replay_pad_free(r.step())) {
                *why = "a key is held (the replay's pad)";
                return false;
            }
            return safe(why);
        };
        g.ops = std::move(ops);
    }
    return *g.ops;
}

// The harness at a point: what the next checkpoints depend on besides the game.
void recovery_keep_harness(Run &r) {
    RecoveryRun &g = g_recovery;
    g.harness = {{"step", r.step()},
                 {"turns", r.turns},
                 {"ticks", r.ticks},
                 {"last_frame", static_cast<u64>(r.last_frame)},
                 {"gl_frames", r.gl_frames},
                 {"clock_ns", r.clock.elapsed_ns()},
                 {"audio_hash", r.audio->hash()},
                 {"audio_frames", r.audio->frames()},
                 {"kills", g_play.kills},
                 {"gl_index", r.screen.gl_frame()},
                 {"gl_width", r.screen.gl_width()},
                 {"gl_height", r.screen.gl_height()},
                 {"gl_view", r.screen.gl_view()}};
    g.gl = r.screen.gl_pixels();
    g.mob_hp = g_play.mob_hp;
}

void recovery_write_harness(Run &r) {
    const RecoveryRun &g = g_recovery;
    std::ofstream out(r.out / "harness.txt", std::ios::binary);
    for (const auto &[k, v] : g.harness) {
        out << k << " " << v << "\n";
    }
    for (const auto &[addr, hp] : g.mob_hp) {
        out << "mob " << addr << " " << hp << "\n";
    }
    std::ofstream(r.out / "harness_gl.rgba", std::ios::binary)
        .write(reinterpret_cast<const char *>(g.gl.data()),
               static_cast<std::streamsize>(g.gl.size()));
}

bool recovery_read_harness(Run &r) {
    RecoveryRun &g = g_recovery;
    std::ifstream in(r.out / "harness.txt", std::ios::binary);
    std::string key;
    while (in >> key) {
        if (key == "mob") {
            u32 addr = 0;
            i32 hp = 0;
            in >> addr >> hp;
            g.mob_hp[addr] = hp;
        } else {
            u64 v = 0;
            in >> v;
            g.harness[key] = v;
        }
    }
    g.gl = read_file(r.out / "harness_gl.rgba");
    return g.harness.count("turns") != 0 &&
           g.gl.size() == g.harness["gl_width"] * g.harness["gl_height"] * 4;
}

int recovery_setup(Run &r) {
    if (const int rc = regress_setup(r); rc != 0) {
        return rc;
    }
    r.plan = fe::CheckpointPlan(); // only the window's checkpoints, one after each tick
    r.end_step = UINT64_MAX;       // recovery_crash's trap or recovery_resume's window ends it
    fe::install_game_save_counter();
    g_recovery.points.set_data_dir(r.data.string());
    g_recovery.points.set_interval(kRecoveryTestInterval);
    return 0;
}

int recovery_crash_setup(Run &r) {
    if (const int rc = recovery_setup(r); rc != 0) {
        return rc;
    }
    r.settings.crash_recovery = true;
    r.expect_trap = true;
    // #198's marker, as main.cpp sets it at a game's start; nothing ends it.
    const fe::LastRun last = fe::crash_marker_begin(recovery_crashes(r).string());
    check(!last.crashed, "a fresh data directory: no crash before");
    return 0;
}

void recovery_crash(Run &r, u64) {
    regress_common(r);
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        RecoveryRun &g = g_recovery;
        if (g.crashed) {
            return;
        }
        const u64 step = r.step();
        if (g.end != 0 && step > g.end) {
            // The forced crash: the write done (as the run's end after a trap), then a guest trap.
            g.points.wait();
            recovery_write_harness(r);
            std::ofstream(r.out / "uninterrupted.txt", std::ios::binary) << [&r] {
                fe::CheckpointLog log;
                const std::vector<fe::Checkpoint> &all = r.checkpoints.points();
                for (std::size_t i = g.first_index; i < all.size(); i++) {
                    log.add(all[i]);
                }
                g_recovery.window = log;
                return log.text();
            }();
            g.crashed = true;
            std::printf("recovery: the forced trap at step %llu\n",
                        static_cast<unsigned long long>(step));
            mt_trap(cpu, MT_TRAP_UDF, cpu->r[MT_LR], 0x7E57u);
            return;
        }
        if (g.chosen) {
            if (r.ticks != g.last_tick) {
                g.last_tick = r.ticks;
                r.plan.add(step, "recovery_t" + std::to_string(++g.ticks_seen));
                if (g.ticks_seen == k_save_ticks) {
                    g.end = step;
                }
            }
            return;
        }
        const fe::RecoveryTurn t =
            g.points.turn(recovery_ops(r, cpu), r.settings.crash_recovery,
                          static_cast<double>(r.clock.elapsed_ns()) / 1e9, fe::game_saves_seen());
        if (!t.taken) {
            return;
        }
        g.taken++;
        recovery_keep_harness(r);
        std::printf("recovery: point %llu at step %llu (%s, %s)\n",
                    static_cast<unsigned long long>(g.taken), static_cast<unsigned long long>(step),
                    fe::recovery_trigger_name(t.trigger), regress_state(cpu).c_str());
        if (step >= kRecoveryFrom) {
            g.chosen = true;
            g.point_step = step;
            g.last_tick = r.ticks;
            g.first_index = r.checkpoints.points().size();
            r.plan.add(step, "recovery_t0");
        }
    };
}

void recovery_crash_checks(Run &r, u64) {
    const RecoveryRun &g = g_recovery;
    check(g.taken >= 2, "points were taken before the chosen one (" + std::to_string(g.taken) +
                            ", so recovery-2 holds one)");
    check(g.chosen, "a point at or after step " + std::to_string(kRecoveryFrom) + " (step " +
                        std::to_string(g.point_step) + ")");
    check(g.crashed && g.window.points().size() == k_save_ticks + 1,
          "the window's " + std::to_string(k_save_ticks + 1) + " checkpoints, then the trap (" +
              std::to_string(g.window.points().size()) + ")");
    const fe::RecoveryPoints::Stats s = g_recovery.points.stats();
    check(s.written == s.taken && s.failed == 0, "every point written (" +
                                                     std::to_string(s.written) + " of " +
                                                     std::to_string(s.taken) + ")");
    check(fs::is_regular_file(r.data / "states" / "recovery-1.state") &&
              fs::is_regular_file(r.data / "states" / "recovery-2.state"),
          "recovery-1.state and recovery-2.state are kept after the crash");
    check(fs::exists(recovery_crashes(r) / fe::kCrashMarkerName), "#198's marker is kept");
    if (!g.window.points().empty()) {
        std::printf("recovery: uninterrupted window from step %llu, digest %s, %zu bytes raw, %zu "
                    "written\n",
                    static_cast<unsigned long long>(g.point_step), g.window.digest().c_str(),
                    s.last_raw, s.last_written);
    }
}

int recovery_resume_setup(Run &r) {
    if (const int rc = recovery_setup(r); rc != 0) {
        return rc;
    }
    RecoveryRun &g = g_recovery;
    if (!fs::is_regular_file(r.out / "uninterrupted.txt") || !recovery_read_harness(r)) {
        std::printf("skipped: no recovery_crash run in %s\n", r.out.string().c_str());
        return 77;
    }
    r.settings.crash_recovery = false; // the resume only: no new point in the window
    // The start, as main.cpp: the marker, the offer, #198's box answered Resume.
    const fe::LastRun last = fe::crash_marker_begin(recovery_crashes(r).string());
    check(last.crashed, "the marker says the last run crashed");
    const fe::DataLocation loc = fe::data_location_at(r.data.string());
    g.offer = fe::find_recovery_point(r.data.string(), loc.saves, last.crashed, last.crash_file);
    check(g.offer.offered && g.offer.slot == 0 &&
              g.offer.path == fe::recovery_path(r.data.string(), 1),
          "the newest point is offered (" + (g.offer.offered ? g.offer.path : g.offer.why) + ")");
    int asked = 0;
    fe::set_recovery_prompt_for_test([&asked](const std::string &, const fe::RecoveryOffer &) {
        asked++;
        return fe::RecoveryChoice::kResume;
    });
    const fe::StartDecision d = fe::decide_start(false, last, false, g.offer);
    fe::set_recovery_prompt_for_test(nullptr);
    check(asked == 1 && d.resume && !d.safe_start, "#198's box asked once and Resume was chosen");
    if (d.resume) {
        g.points.arm(g.offer);
    }
    return 0;
}

void recovery_resume(Run &r, u64) {
    regress_common(r);
    r.on_turn = [&r, monitor_turn = r.on_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        RecoveryRun &g = g_recovery;
        const u64 step = r.step();
        if (g.resumed) {
            if (r.ticks != g.last_tick) {
                g.last_tick = r.ticks;
                r.plan.add(step, "recovery_t" + std::to_string(++g.ticks_seen));
                if (g.ticks_seen == k_save_ticks) {
                    r.end_step = step;
                }
            }
            return;
        }
        if (!g.points.armed()) {
            return;
        }
        const fe::RecoveryTurn t =
            g.points.turn(recovery_ops(r, cpu), false,
                          static_cast<double>(r.clock.elapsed_ns()) / 1e9, fe::game_saves_seen());
        if (t.dropped) {
            g.drop = t.notice;
            r.end_step = step;
            return;
        }
        if (!t.resumed) {
            return;
        }
        // The harness as it was at the point, in recovery_crash's process.
        std::map<std::string, u64> &h = g.harness;
        std::printf("recovery: resumed at step %llu (%s) to step %llu\n",
                    static_cast<unsigned long long>(step), regress_state(cpu).c_str(),
                    static_cast<unsigned long long>(h["step"]));
        check(r.clock.elapsed_ns() == h["clock_ns"],
              "the clock is back at the point's time (" + std::to_string(r.clock.elapsed_ns()) +
                  " ns, saved " + std::to_string(h["clock_ns"]) + ")");
        r.turns = h["turns"];
        r.ticks = h["ticks"];
        r.last_frame = static_cast<std::int64_t>(h["last_frame"]);
        r.gl_frames = h["gl_frames"];
        r.cursor = std::make_unique<fe::ReplayCursor>(r.replay);
        while (r.cursor->next(h["step"]) != nullptr) {
        }
        r.audio->resume(h["audio_hash"], h["audio_frames"]);
        r.screen.set_gl_frame(static_cast<std::uint32_t>(h["gl_index"]), g.gl.data(),
                              static_cast<std::uint32_t>(h["gl_width"]),
                              static_cast<std::uint32_t>(h["gl_height"]),
                              static_cast<std::uint32_t>(h["gl_view"]));
        g_play.kills = h["kills"];
        g_play.mob_hp = g.mob_hp;
        g.resumed = true;
        g.last_tick = r.ticks;
        g.first_index = r.checkpoints.points().size();
        r.plan.add(r.step(), "recovery_t0");
    };
}

void recovery_resume_checks(Run &r, u64) {
    RecoveryRun &g = g_recovery;
    check(g.resumed, "the armed point is restored once the character is in the field" +
                         (g.drop.empty() ? std::string() : " (dropped: " + g.drop + ")"));
    check(!fs::exists(r.data / "states" / "recovery-2.state") &&
              fs::is_regular_file(r.data / "states" / "recovery-1.state"),
          "the restored point's file is gone and the older point is the newest");
    std::ifstream in(r.out / "uninterrupted.txt", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    fe::CheckpointLog before, after;
    std::string error;
    check(fe::CheckpointLog::parse(text, &before, &error), "uninterrupted.txt reads " + error);
    const std::vector<fe::Checkpoint> &all = r.checkpoints.points();
    for (std::size_t i = g.first_index; i < all.size(); i++) {
        after.add(all[i]);
    }
    std::ofstream(r.out / "resumed.txt", std::ios::binary) << after.text();
    check(before.points().size() == k_save_ticks + 1 && after.points().size() == k_save_ticks + 1,
          "both windows took " + std::to_string(k_save_ticks + 1) + " checkpoints (" +
              std::to_string(before.points().size()) + ", " +
              std::to_string(after.points().size()) + ")");
    const fe::LogDifference d = fe::compare_logs(before, after);
    check(d.same, "the resumed run's " + std::to_string(after.points().size()) +
                      " checkpoints equal the uninterrupted run's" +
                      (d.same ? std::string()
                              : " (first difference at " + std::to_string(d.index) + ":\n  " + d.a +
                                    "\n  " + d.b + ")"));
    std::printf("recovery: uninterrupted digest %s, resumed digest %s\n", before.digest().c_str(),
                after.digest().c_str());
}

// recovery_off: the whole replay as `regress` plays it, with the frontend's recovery points
// running every turn as game.cpp runs them and crash_recovery as the settings have it (off by
// default and in both Original presets): no point is taken, no file is written in states/, and
// the checkpoint log equals play.regress's (MT_RECOVERY_BASE: that run's checkpoints.txt).
int recovery_off_setup(Run &r) {
    if (const int rc = regress_setup(r); rc != 0) {
        return rc;
    }
    fe::install_game_save_counter();
    g_recovery.points.set_data_dir(r.data.string());
    return 0;
}

void recovery_off(Run &r, u64) {
    regress_common(r);
    r.on_turn = [&r, regress_turn = r.on_turn](mt_cpu *cpu) {
        regress_turn(cpu);
        g_recovery.points.turn(recovery_ops(r, cpu), r.settings.crash_recovery,
                               static_cast<double>(r.clock.elapsed_ns()) / 1e9,
                               fe::game_saves_seen());
    };
}

void recovery_off_checks(Run &r, u64 goal) {
    regress_checks(r, goal);
    g_recovery.points.wait();
    check(!r.settings.crash_recovery, "crash_recovery is off");
    check(g_recovery.points.stats().taken == 0, "no recovery point is taken");
    std::error_code ec;
    check(!fs::exists(r.data / "states", ec) || fs::is_empty(r.data / "states", ec),
          "no file is written in states/");
    const char *base = std::getenv("MT_RECOVERY_BASE");
    if (base == nullptr || !fs::is_regular_file(base)) {
        std::printf("recovery_off: no MT_RECOVERY_BASE to compare with\n");
        return;
    }
    std::ifstream in(base, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    fe::CheckpointLog before;
    std::string error;
    check(fe::CheckpointLog::parse(text, &before, &error), std::string(base) + " reads " + error);
    const fe::LogDifference d = fe::compare_logs(before, r.checkpoints);
    check(d.same, "the checkpoints equal play.regress's (digest " + before.digest() +
                      ", this run " + r.checkpoints.digest() + ")" +
                      (d.same ? std::string()
                              : " (first difference at " + std::to_string(d.index) + ":\n  " + d.a +
                                    "\n  " + d.b + ")"));
}

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
        u64 hit_tick = 0, seen_at = 0; // seen_at: r.ticks of the last look at the counter
        bool open = false;
    };
    std::map<u32, MobSeen> mobs;
    u64 hits = 0, kills = 0, bars_one_second = 0, hits_counter_bad = 0, bars_closed = 0,
        bars_bad_length = 0, max_hp_bad = 0;
    u64 bar_ticks_min = UINT64_MAX, bar_ticks_max = 0;
    std::string hit_first_bad, hit_example, bar_first_bad;
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
            // The test looks at the counter only on some ticks, and the field's tick count
            // leaves out a tick at each popup or menu, so the counter can run its last steps
            // between two looks: it must have been able to reach 19 by now.
            const bool ran_out = static_cast<u64>(std::max(s.d0, 0)) + (r.ticks - s.seen_at) >= 19;
            m.bars_bad_length += ran_out ? 0 : 1;
            if (!ran_out && m.bar_first_bad.empty()) {
                m.bar_first_bad = "mob " + std::to_string(mob.id) + " after " +
                                  std::to_string(len) + " field ticks, counter " +
                                  std::to_string(s.d0) + " " +
                                  std::to_string(r.ticks - s.seen_at) + " ticks before";
            }
            m.bars_one_second += len >= 18 && len <= 22 ? 1 : 0;
        }
        s.id = mob.id;
        s.hp = mob.hp;
        s.d0 = d0;
        s.seen_at = r.ticks;
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
              " before the counter reached 20" +
              (m.bar_first_bad.empty() ? "" : "; first: " + m.bar_first_bad) + ")");
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

// --- job advancements from built saves (#302) ---------------------------------------------------
//
// The Dark Lord (NPC 3 on 0.0) gives every job advancement, by the script of the hero's chapter
// (the knowledge base's 11 paths, docs/testing/skills-and-jobs.md). Each mode starts from a
// built save at one advancement point (tests/starts/jobs_*.json, MT_FROM_SAVE), talks to him by
// play and picks the branch in his list (classUp2: 6 the Assassin, 7 the Bandit; chapter 1's
// main quest 21 is row 2 of his quest list), then checks the job (hero +0x88 branch, +0x8C
// job_tier) in guest memory and in the theif0 it saves, and the reward weapon in the bag. The
// starts equal no chained state, so every mode's test is named synthetic (start-states.md).
//
// Each step also writes one line to OUT_DIR/jobs.txt, what the guest held before the talk and
// the job after it (the compendium's advancement rows are checked against these by
// tests/test_compendium_skills_port.py, #222):
//   chapter <c> level <l> branch <b> tier <t> quests <sort>.<n>=<state>... -> branch <b> tier <t>
// The quests are every one not at 0 (getQuestState's view: 1 active, 2 done).
struct JobStep {
    std::vector<int> choices;
    i32 branch = 0, tier = 0;
    i32 reward = -1; // the equip id the advancement gives, -1 none
    std::vector<std::tuple<u32, u32, i32>> quests = {}; // {sort, number, state} after it
};

Pred job_at(i32 branch, i32 tier) {
    return [branch, tier](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.addr != 0 && rdi(cpu, h.addr + 0x88) == branch && rdi(cpu, h.addr + 0x8C) == tier;
    };
}

// The guest's side of a jobs.txt line before the talk: chapter, level, job and the quests.
std::string job_before(const mt_cpu *cpu) {
    const Hero h = hero(cpu);
    std::string out = "chapter " + std::to_string(h.chapter) + " level " + std::to_string(h.level) +
                      " branch " + std::to_string(rdi(cpu, h.addr + 0x88)) + " tier " +
                      std::to_string(rdi(cpu, h.addr + 0x8C)) + " quests";
    for (u32 sort = 0; sort < 2; sort++) {
        for (u32 n = 0; n < 100; n++) {
            const i32 st = quest_state(cpu, h, sort, n);
            if (st > 0) {
                out +=
                    " " + std::to_string(sort) + "." + std::to_string(n) + "=" + std::to_string(st);
            }
        }
    }
    return out;
}

void job_path(Run &r, const std::vector<JobStep> &steps) {
    using namespace quest_bot;
    from_save(r);
    std::ofstream(r.out / "jobs.txt", std::ios::trunc);
    auto before = std::make_shared<std::string>();
    for (const JobStep &s : steps) {
        const std::string what =
            "branch " + std::to_string(s.branch) + " job_tier " + std::to_string(s.tier);
        then_do(r, "the hero before the talk",
                [before](mt_cpu *cpu) { *before = job_before(cpu); });
        then_talk_to(r, "Dark Lord: the advancement to " + what, 0, 0, "Darklord", s.choices,
                     job_at(s.branch, s.tier), 600);
        then_do(r, "the advancement in jobs.txt", [&r, before](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            const std::string line = *before + " -> branch " +
                                     std::to_string(rdi(cpu, h.addr + 0x88)) + " tier " +
                                     std::to_string(rdi(cpu, h.addr + 0x8C)) + "\n";
            std::printf("jobs: %s", line.c_str());
            std::ofstream(r.out / "jobs.txt", std::ios::app) << line;
        });
        if (s.reward >= 0) {
            then_check(
                r, "the advancement's weapon (equip " + std::to_string(s.reward) + ") in the bag",
                [id = s.reward](mt_cpu *cpu) {
                    return quest_bot::equip_owned(cpu, hero(cpu), id) > 0;
                });
        }
        if (!s.quests.empty()) {
            then_quests_are(r, "the quests the advancement hands in", s.quests);
        }
        then_save(r);
        then_job_is(r, s.branch, s.tier);
    }
    then_do(r, "the hero after the advancements", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("jobs: %s, branch %d job_tier %d\n", hero_text(cpu, h).c_str(),
                    rdi(cpu, h.addr + 0x88), rdi(cpu, h.addr + 0x8C));
    });
    then_finish(r);
}
void jobs_checks(Run &, u64) {}

// 1-1-0: chapter 1's first advancement, main quest 21 handed in (row 2), from a level-10 hero.
void jobs_1_1(Run &r, u64) { job_path(r, {{{2}, 0, 1, 24, {{0, 21, 2}, {0, 9, 1}}}}); }
// 1-2-1 and 1-2-2: chapter 1's second, from a level-30 Rogue with main quest 21 done.
void jobs_1_2_assassin(Run &r, u64) { job_path(r, {{{6}, 1, 2, 40}}); }
void jobs_1_2_bandit(Run &r, u64) { job_path(r, {{{7}, 2, 2, 28}}); }
// 2-2-1, 2-2-2, 2-3-same: chapter 2's script.
void jobs_2_2_assassin(Run &r, u64) { job_path(r, {{{6}, 1, 2, 40}}); }
void jobs_2_2_bandit(Run &r, u64) { job_path(r, {{{7}, 2, 2, 28}}); }
void jobs_2_3_assassin(Run &r, u64) { job_path(r, {{{-1}, 1, 3, -1}}); }
void jobs_2_3_bandit(Run &r, u64) { job_path(r, {{{-1}, 2, 3, -1}}); }
// 3-1-0, 3-2-1, 3-2-2, then 3-3-same and 3-4-same in one run (side quest 25 active from the
// start: the script offers the 4th job only once the 3rd is taken, and hands side 25 in). In
// chapter 3 the advancements are side quests 1, 16 and 42 on the Dark Lord's path (#301: the
// chain's hero has its jobs from earlier chapters, so these runs are where they reach state 2).
void jobs_3_1(Run &r, u64) { job_path(r, {{{-1}, 0, 1, 36, {{1, 1, 2}}}}); }
void jobs_3_2_assassin(Run &r, u64) { job_path(r, {{{6}, 1, 2, 40, {{1, 16, 2}}}}); }
void jobs_3_2_bandit(Run &r, u64) { job_path(r, {{{7}, 2, 2, 28, {{1, 16, 2}}}}); }
void jobs_3_4_assassin(Run &r, u64) {
    job_path(r, {{{-1}, 1, 3, -1, {{1, 42, 2}}}, {{-1}, 1, 4, -1, {{1, 25, 2}}}});
}
void jobs_3_4_bandit(Run &r, u64) {
    job_path(r, {{{-1}, 2, 3, -1, {{1, 42, 2}}}, {{-1}, 2, 4, -1, {{1, 25, 2}}}});
}

// --- every skill from built saves (#302) ---------------------------------------------------------
//
// A tier-4 Assassin (claw, stars) and a tier-4 Bandit (dagger), level 50 with free SP and every
// skill at 0, on 1.3 (mobs 12 and 16; tests/starts/skills_*.json, MT_FROM_SAVE). Each skill is
// learned to level 2 on the skill tab, a passive's stat checked on learning, every other skill
// put on quick slot 3 (LB) and used on the mobs. A watcher (skill_tick) checks every turn against
// docs/re/mechanics.md and skill_rules.h: the MP paid, each mob's HP loss as k hits of the skill
// in use (normal or critical, the star's ATT for star hits; a claw's punch does 1), the stars
// taken, Drain's heal, Shadow Web's bind and no damage, each buff's lifetime against its duration,
// Dark Sight's harmless contact, Meso Guard's meso paid, Pickpocket's meso drops. The other
// branch's weapon skill (Double Step for the claw, Lucky Seven for the dagger) is refused. The
// starts equal no chained state: the tests are named synthetic.
struct SkillStats {
    u64 casts = 0, mp_bad = 0, hits = 0, exact = 0, punches = 0, hit_bad = 0;
    u64 star_ok = 0, star_bad = 0, stars = 0, heals = 0, heal_bad = 0, binds = 0, max_bind = 0;
    u64 near = 0, harm = 0, guard_ok = 0, guard_bad = 0, picks = 0, pick_bad = 0;
    i32 lifetime = -1; // a buff's ticks on (its counter's last value)
    std::string first_bad, example;
    void bad(const std::string &s) {
        if (first_bad.empty()) {
            first_bad = s;
        }
    }
};
struct SkillRun {
    std::map<i32, skill_rules::SkillRow> rows;
    std::map<i32, i32> hp_sum, mp_sum; // hero.dat: level -> the HP and MP sums to it
    std::map<i32, MechMob> mob;
    i32 star = 12, star_attack = 0;
    bool thrower = false; // the Assassin's run: the bot keeps throwing distance
    bool claw = false;    // the weapon is a claw (item +0x8C = 1): an attack on a mob right next
                          // to the hero is a punch, also from a skill's key
    // The phase: the skill pressed (MP, stats) and the skill whose hits land (the basic attack
    // while a buff is on), the stats' key (+100 with Shadow Partner on).
    i32 use = -1, hit = -1;
    bool was = false;
    Hero last{};
    i32 last_attack = 0, last_stars = 0, last_potions = 0;
    std::map<u32, std::pair<i32, i32>> mob_hp;
    std::set<u32> drop_addrs;
    std::map<i32, i32> buff_on; // a buff's id -> its counter's last value
    std::map<i32, SkillStats> stats;
    // Snapshots of the hero's stats (attack, defense, accuracy, avoid, critical, max HP, max MP).
    std::array<i32, 7> before{};
};
std::shared_ptr<SkillRun> g_skills;

std::array<i32, 7> hero_stats(mt_cpu *cpu, const Hero &h) {
    return {rdi(cpu, h.addr + 0xA4),
            rdi(cpu, h.addr + 0xA8),
            rdi(cpu, h.addr + 0xB0),
            rdi(cpu, h.addr + 0xAC),
            rdi(cpu, h.addr + 0xB4),
            h.max_hp,
            h.max_mp};
}

// The hero's buffs: the list at skills +0xD64 (CSkill::TickBuffs 0x7F669), each {skill id +0x00,
// ticks since the cast +0x04}.
std::map<i32, i32> buffs(const mt_cpu *cpu, const Hero &h) {
    std::map<i32, i32> out;
    const u32 skills = h.addr != 0 ? rd(cpu, h.addr + 0x114) : 0;
    for (const u32 b : list_items(cpu, skills != 0 ? rd(cpu, skills + 0xD64) : 0, 16)) {
        if (b != 0) {
            out[rdi(cpu, b)] = rdi(cpu, b + 4);
        }
    }
    return out;
}

void skill_tables(SkillRun &k) {
    const fs::path dir = fs::path(MT_BUNDLE_DIR) / "English" / "data";
    k.rows = skill_rules::parse_skills(world_map::read_bytes(dir / "skill.dat"));
    MechRun m;
    mech_tables(m);
    k.mob = m.mob;
    const std::vector<std::uint8_t> hero = world_map::read_bytes(dir / "hero.dat");
    i32 hp = 0, mp = 0;
    for (std::size_t o = 6; o + 18 <= hero.size(); o += 18) {
        hp += static_cast<std::int16_t>(hero[o + 5] | (hero[o + 6] << 8));
        mp += static_cast<std::int16_t>(hero[o + 7] | (hero[o + 8] << 8));
        k.hp_sum[hero[o]] = hp;
        k.mp_sum[hero[o]] = mp;
    }
    // item_etc (the stride from its header): a star's ATT, i16 +0x46 (docs/formats/data-tables.md).
    const std::vector<std::uint8_t> etc = world_map::read_bytes(dir / "item_etc.dat");
    const std::size_t stride =
        etc.size() >= 6 ? static_cast<std::size_t>(etc[4] | (etc[5] << 8)) : 0;
    const std::size_t at = 6 + stride * static_cast<std::size_t>(k.star);
    if (stride != 0 && at + 0x48 <= etc.size() && etc[at] == k.star) {
        k.star_attack = static_cast<std::int16_t>(etc[at + 0x46] | (etc[at + 0x47] << 8));
    }
}

void skill_tick(Run &r, mt_cpu *cpu) {
    using skill_rules::SkillRow;
    SkillRun &k = *g_skills;
    const Hero h = hero(cpu);
    if (!field(cpu) || h.addr == 0) {
        k.was = false;
        return;
    }
    const i32 attack = rdi(cpu, h.addr + 0xA4);
    const u32 weapon = rd(cpu, rd(cpu, h.addr + 0xDC) + 4);
    k.claw = weapon != 0 && rdi(cpu, weapon + 0x8C) == 1;
    const i32 stars = item_count(cpu, h, k.star);
    const i32 potions = item_count(cpu, h, 5) + item_count(cpu, h, 8);
    const std::map<i32, i32> on = buffs(cpu, h);
    // Each buff's lifetime: its counter counts the ticks it is on (not while a menu is open) and
    // the buff ends when the counter reaches the duration.
    for (auto it = k.buff_on.begin(); it != k.buff_on.end();) {
        if (on.count(it->first) == 0) {
            k.stats[it->first].lifetime = it->second;
            it = k.buff_on.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &[id, elapsed] : on) {
        k.buff_on[id] = elapsed;
    }
    const bool same = k.was && k.last.addr == h.addr && h.on(k.last.area, k.last.map);
    const bool partner = on.count(14) != 0;
    std::map<u32, std::pair<i32, i32>> now;
    std::vector<const Mob *> hit_mobs;
    const std::vector<Mob> all = mobs(cpu);
    for (const Mob &m : all) {
        now[m.addr] = {m.id, m.hp};
    }
    if (same && k.use >= 0 && k.rows.count(k.use) != 0) {
        const SkillRow &use = k.rows.at(k.use);
        const SkillRow &hit = k.rows.count(k.hit) != 0 ? k.rows.at(k.hit) : use;
        const i32 use_level = quest_bot::skill_level(cpu, h, k.use),
                  hit_level = quest_bot::skill_level(cpu, h, k.hit);
        SkillStats &st = k.stats[k.use + (partner && k.use != 14 ? 100 : 0)];
        const std::string at = " (" + std::to_string(r.ticks) + ")";
        // MP: each drop is the cost of the skill pressed.
        if (h.mp < k.last.mp && potions == k.last_potions && use.type >= 2) {
            const i32 drop = k.last.mp - h.mp, cost = skill_rules::mp_cost(use, use_level);
            if (drop == cost) {
                st.casts++;
            } else {
                st.mp_bad++;
                st.bad("MP " + std::to_string(k.last.mp) + " -> " + std::to_string(h.mp) +
                       ", the rule's cost " + std::to_string(cost) + at);
            }
        }
        // Each mob's HP loss.
        i32 heal_max = 0, heal_cap = 0; // the heal of the HP lost; a kill's heal comes from the
        u64 kills = 0;                  // rolled damage, which can pass the mob's HP
        for (const Mob &m : all) {
            const auto it = k.mob_hp.find(m.addr);
            if (it == k.mob_hp.end() || it->second.first != m.id || it->second.second <= 0 ||
                m.hp >= it->second.second) {
                continue;
            }
            const i32 dealt = it->second.second - m.hp;
            const bool killed = m.hp <= 0;
            kills += killed ? 1 : 0;
            hit_mobs.push_back(&m);
            st.hits++;
            if (k.claw && (dealt == 1 || dealt == 2)) { // a punch, or a critical punch
                st.punches++;
                continue;
            }
            if (k.hit == 13) { // Shadow Web binds and does no damage
                st.hit_bad++;
                st.bad("mob " + std::to_string(m.id) + " lost " + std::to_string(dealt) +
                       " HP under Shadow Web" + at);
                continue;
            }
            const i32 hits = skill_rules::hits_on_one(hit, hit_level, partner);
            bool ok = false;
            for (const i32 a : {k.last_attack, attack}) {
                const i32 x = skill_rules::hit_x(hit, hit_level, a, k.star_attack);
                ok = ok || skill_rules::loss_fits(dealt, x, hits, killed);
                const skill_rules::Range one = skill_rules::hit_range(x);
                if (!killed && dealt >= one.lo && dealt <= one.hi) {
                    st.exact++;
                    if (st.example.empty()) {
                        st.example = "mob " + std::to_string(m.id) + " lost " +
                                     std::to_string(dealt) + " at A " + std::to_string(a) + ": x " +
                                     std::to_string(x) + ", one hit " + std::to_string(one.lo) +
                                     "-" + std::to_string(one.hi);
                    }
                    break;
                }
            }
            if (!ok) {
                st.hit_bad++;
                const i32 x = skill_rules::hit_x(hit, hit_level, attack, k.star_attack);
                const skill_rules::Range one = skill_rules::hit_range(x);
                st.bad("mob " + std::to_string(m.id) + " lost " + std::to_string(dealt) + " (" +
                       std::to_string(it->second.second) + " -> " + std::to_string(m.hp) +
                       ") at A " + std::to_string(attack) + ", skill " + std::to_string(k.hit) +
                       " level " + std::to_string(hit_level) + ": up to " + std::to_string(hits) +
                       " hits of " + std::to_string(one.lo) + "-" + std::to_string(one.hi) + at);
            }
            const i32 most =
                killed ? skill_rules::hits_on_one(hit, hit_level, partner) * 2 *
                             skill_rules::hit_range(
                                 skill_rules::hit_x(hit, hit_level, attack, k.star_attack))
                                 .hi
                       : dealt;
            heal_max += dealt * skill_rules::etc_value(hit, hit_level) / 100;
            heal_cap += most * skill_rules::etc_value(hit, hit_level) / 100;
        }
        // Stars: a use's stars leave over a few ticks, never more than a use's worth in one; the
        // phase's total is checked against its casts (skill_report).
        if (stars < k.last_stars) {
            const i32 used = k.last_stars - stars;
            const i32 per = skill_rules::stars_used(k.hit, partner);
            st.stars += static_cast<u64>(used);
            if (per > 0 && used <= per) {
                st.star_ok++;
            } else {
                st.star_bad++;
                st.bad(std::to_string(used) + " stars taken in a turn, " + std::to_string(per) +
                       " a use" + at);
            }
        }
        // Drain heals a share of the damage, up to max HP.
        if (k.hit == 7 && h.hp > k.last.hp && potions == k.last_potions) {
            const i32 healed = h.hp - k.last.hp;
            const i32 room = h.max_hp - k.last.hp;
            const i32 want = std::min(heal_max, room), cap = std::min(heal_cap, room);
            if (healed <= cap && healed >= want - static_cast<i32>(hit_mobs.size())) {
                st.heals++;
            } else {
                st.heal_bad++;
                st.bad("Drain healed " + std::to_string(healed) + ", the rule " +
                       std::to_string(want) + "-" + std::to_string(cap) + at);
            }
        }
        // Shadow Web (CSkill::ShadowWeb 0x7EB8D) sets a caught mob's bind state (+0xD4) to 1, which
        // the mob's tick moves on to 2, and its bind ticks (+0xD8) to @etc.
        if (k.use == 13) {
            for (const Mob &m : all) {
                const i32 flag = rdi(cpu, m.addr + 0xD4);
                const u64 ticks = static_cast<u64>(std::max(0, rdi(cpu, m.addr + 0xD8)));
                if (flag != 0) {
                    st.max_bind = std::max(st.max_bind, ticks);
                    st.binds++;
                }
            }
        }
        // Contact under Dark Sight (5) and Meso Guard (22).
        const i32 lost = k.last.hp - h.hp;
        if (on.count(5) != 0) {
            SkillStats &ds = k.stats[5];
            for (const Mob &m : all) {
                if (m.hp > 0 && std::abs(m.x - h.x) <= 20 && std::abs(m.y - h.y) <= 6) {
                    ds.near++;
                    break;
                }
            }
            if (lost > 0 && potions == k.last_potions) {
                ds.harm++;
                ds.bad("the hero lost " + std::to_string(lost) + " HP under Dark Sight" + at);
            }
        }
        if (on.count(22) != 0 && lost > 0 && potions == k.last_potions && h.hp > 0) {
            SkillStats &mg = k.stats[22];
            const i32 pct =
                skill_rules::etc_value(k.rows.at(22), quest_bot::skill_level(cpu, h, 22));
            const i32 paid = k.last.meso - h.meso;
            // The blow D was halved (lost = D / 2, or D - D / 2) and (D / 2) x @etc / 100 paid.
            if (paid == lost * pct / 100 || paid == (lost - 1) * pct / 100) {
                mg.guard_ok++;
                if (mg.example.empty()) {
                    mg.example = "lost " + std::to_string(lost) + " HP, paid " +
                                 std::to_string(paid) + " meso at " + std::to_string(pct) + " %";
                }
            } else {
                mg.guard_bad++;
                mg.bad("lost " + std::to_string(lost) + " HP, paid " + std::to_string(paid) +
                       " meso at " + std::to_string(pct) + " %" + at);
            }
        }
        // Pickpocket (19): meso a hit drops, on turns without a kill (a kill's drops are its own).
        if (on.count(19) != 0 && kills == 0) {
            SkillStats &pp = k.stats[19];
            for (const Drop &d : drops(cpu)) {
                if (k.drop_addrs.count(d.addr) != 0 || rdi(cpu, d.addr + 0x84) == 0 || d.id > 2) {
                    continue;
                }
                const i32 count = rdi(cpu, d.addr + 0x100);
                bool ok = false;
                for (const Mob *m : hit_mobs) {
                    const i32 meso = k.mob.count(m->id) != 0 ? k.mob.at(m->id).meso : 0;
                    ok = ok || (count >= std::max(1, meso / 18 - 5) && count <= meso / 18);
                }
                (ok ? pp.picks : pp.pick_bad)++;
                if (!ok) {
                    pp.bad("a meso drop of " + std::to_string(count) + " under Pickpocket" + at);
                } else if (pp.example.empty()) {
                    pp.example = "a hit dropped " + std::to_string(count) + " meso";
                }
            }
        }
    }
    std::set<u32> drop_now;
    for (const Drop &d : drops(cpu)) {
        drop_now.insert(d.addr);
    }
    k.drop_addrs = drop_now;
    k.mob_hp = now;
    k.last = h;
    k.last_attack = attack;
    k.last_stars = stars;
    k.last_potions = potions;
    k.was = true;
}

// The skill phase: `use` pressed, `hit` the skill whose hits land.
void then_phase(Run &r, i32 use, i32 hit) {
    then_do(r, "skills: phase " + std::to_string(use) + " (hits of " + std::to_string(hit) + ")",
            [use, hit](mt_cpu *) {
                g_skills->use = use;
                g_skills->hit = hit;
            });
}

quest_bot::FarmStyle skill_style(bool skill) {
    quest_bot::FarmStyle s;
    s.skill = skill ? quest_bot::LB : SDL_GAMEPAD_BUTTON_INVALID;
    s.hp_pct = 30;
    s.mp_pct = 30;
    s.restock = {};
    s.keep = g_skills->thrower ? 50 : 0;
    return s;
}

// Learns `id` to level 2 and checks the stat it adds (passives, CSkill::LearnLevels 0x7F99D):
// stat index i of hero_stats grows by `gain` (from the snapshot taken before).
void then_learn(Run &r, i32 id, std::function<std::array<i32, 7>(mt_cpu *, const Hero &)> want) {
    then_do(r, "skills: the stats before skill " + std::to_string(id),
            [](mt_cpu *cpu) { g_skills->before = hero_stats(cpu, hero(cpu)); });
    quest_bot::then_spend_sp(r, "skill " + std::to_string(id) + " to level 2", {{id, 2}});
    then_do(r, "skills: skill " + std::to_string(id) + " learned", [id, want](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(quest_bot::skill_level(cpu, h, id) == 2,
              "skill " + std::to_string(id) + " is at level 2 (" +
                  std::to_string(quest_bot::skill_level(cpu, h, id)) + ")");
        if (want) {
            const std::array<i32, 7> got = hero_stats(cpu, h), w = want(cpu, h);
            std::string text;
            for (std::size_t i = 0; i < got.size(); i++) {
                text += " " + std::to_string(g_skills->before[i]) + "->" + std::to_string(got[i]) +
                        (got[i] == w[i] ? "" : " (rule " + std::to_string(w[i]) + ")");
            }
            check(got == w, "skill " + std::to_string(id) +
                                ": ATT DEF ACC AVOID crit maxHP maxMP as the rule:" + text);
        }
    });
}

// The rule for a passive at level 2: the snapshot with its stat raised.
std::function<std::array<i32, 7>(mt_cpu *, const Hero &)> passive(i32 id) {
    return [id](mt_cpu *, const Hero &h) {
        const SkillRun &k = *g_skills;
        std::array<i32, 7> w = k.before;
        const i32 etc = skill_rules::etc_value(k.rows.at(id), 2);
        switch (id) {
        case 4: // Nimble Body: accuracy and avoid
            w[2] += etc;
            w[3] += etc;
            break;
        case 6:
        case 15:
            w[4] += etc;
            break; // Critical Throw / Stab
        case 8:
        case 17:
            w[5] += k.hp_sum.at(h.level) * etc / 100;
            break; // HP Mastery
        case 9:
        case 18:
            w[6] += k.mp_sum.at(h.level) * etc / 100;
            break; // MP Mastery
        case 11:
        case 20:
            w[0] += etc;
            break; // Weapon Mastery
        default:
            break;
        }
        return w;
    };
}

// Uses the attack skill on LB until `casts` MP drops and a hit in one hit's range.
void then_attack_with(Run &r, i32 id, u64 casts, i32 key = -1) {
    then_phase(r, id, id);
    quest_bot::then_farm(
        r, "skill " + std::to_string(id) + " on the mobs of 1.3", 1, 3, {12, 16},
        [id, casts, key](mt_cpu *) {
            const SkillStats &s = g_skills->stats[key >= 0 ? key : id];
            return s.casts >= casts && s.exact > 0;
        },
        600, skill_style(true));
}

// Casts the buff on LB once (MP paid, the buff on), the snapshot taken first.
void then_cast(Run &r, i32 id) {
    then_phase(r, id, id);
    then_do(r, "skills: the stats before casting " + std::to_string(id),
            [](mt_cpu *cpu) { g_skills->before = hero_stats(cpu, hero(cpu)); });
    then(r, "skills: cast " + std::to_string(id), 30, [&r, id](mt_cpu *cpu, double el) {
        release_all(r);
        hold(r, quest_bot::LB,
             std::fmod(el, 2.0) >= 0.5 && std::fmod(el, 2.0) < 0.6 &&
                 buffs(cpu, hero(cpu)).count(id) == 0);
        return buffs(cpu, hero(cpu)).count(id) != 0 && el >= 0.7;
    });
}

// Walks among the mobs of 1.3 without attacking: until one is next to the hero (`cast` false),
// or until Dark Sight ends (`cast` true; they touch the hero and do no harm).
void then_among_mobs(Run &r, bool cast) {
    quest_bot::FarmStyle s = skill_style(false);
    s.no_attack = true;
    quest_bot::then_farm(
        r, cast ? "Dark Sight among the mobs of 1.3" : "walk to a mob of 1.3", 1, 3, {12, 16},
        [cast](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            if (cast) {
                return buffs(cpu, h).count(5) == 0;
            }
            for (const Mob &m : mobs(cpu)) {
                if (m.hp > 0 && std::abs(m.x - h.x) <= 20 && std::abs(m.y - h.y) <= 6) {
                    return true;
                }
            }
            return false;
        },
        120, s);
}

// The other branch's weapon skill on LB: no MP paid, no attack (CSkill::setActiveSkill 0x7EFF1).
void then_refused(Run &r, i32 id) {
    then_phase(r, id, id);
    auto mp = std::make_shared<i32>(0);
    then_do(r, "skills: MP before " + std::to_string(id),
            [mp](mt_cpu *cpu) { *mp = hero(cpu).mp; });
    then(r, "skills: press " + std::to_string(id) + " three times", 6, [&r](mt_cpu *, double el) {
        release_all(r);
        hold(r, quest_bot::LB, std::fmod(el, 1.5) < 0.1);
        return el >= 4.6;
    });
    then_wait_field(r);
    then_do(r, "skills: " + std::to_string(id) + " refused", [mp, id](mt_cpu *cpu) {
        check(hero(cpu).mp == *mp && g_skills->stats[id].casts == 0,
              "skill " + std::to_string(id) + " is refused with this weapon: MP " +
                  std::to_string(*mp) + " -> " + std::to_string(hero(cpu).mp));
    });
}

void skills_common(Run &r, bool thrower) {
    attach_pad(r);
    g_skills = std::make_shared<SkillRun>();
    g_skills->thrower = thrower;
    skill_tables(*g_skills);
    from_save(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        skill_tick(r, cpu);
    };
}

void skills_assassin(Run &r, u64) {
    using quest_bot::then_quick_slot;
    skills_common(r, true);
    then_phase(r, 1, 1);
    quest_bot::then_farm(
        r, "the basic throw on the mobs of 1.3", 1, 3, {12, 16},
        [](mt_cpu *) { return g_skills->stats[1].exact >= 3 && g_skills->stats[1].star_ok > 0; },
        600, skill_style(false));
    for (const i32 id : {4, 6, 8, 9, 11}) {
        then_learn(r, id, passive(id));
    }
    then_learn(r, 2, nullptr);
    then_quick_slot(r, "Lucky Seven on LB", 2, 3);
    then_attack_with(r, 2, 6);
    then_learn(r, 3, nullptr);
    then_quick_slot(r, "Double Step on LB", 3, 3);
    then_refused(r, 3);
    then_learn(r, 7, nullptr);
    then_quick_slot(r, "Drain on LB", 7, 3);
    then_attack_with(r, 7, 6);
    then_learn(r, 10, nullptr);
    then_quick_slot(r, "Avenger on LB", 10, 3);
    then_attack_with(r, 10, 6);
    then_learn(r, 13, nullptr);
    then_quick_slot(r, "Shadow Web on LB", 13, 3);
    then_phase(r, 13, 13);
    quest_bot::then_farm(
        r, "Shadow Web on the mobs of 1.3", 1, 3, {12, 16},
        [](mt_cpu *) { return g_skills->stats[13].casts >= 3 && g_skills->stats[13].binds > 0; },
        600, skill_style(true));
    then_learn(r, 12, nullptr);
    then_quick_slot(r, "Concentration on LB", 12, 3);
    then_cast(r, 12);
    then_do(r, "skills: Concentration's ATT and ACC", [](mt_cpu *cpu) {
        const SkillRun &k = *g_skills;
        const std::array<i32, 7> got = hero_stats(cpu, hero(cpu));
        const skill_rules::SkillRow &c = k.rows.at(12);
        check(got[0] == k.before[0] + skill_rules::window_damage(c, 2) &&
                  got[2] == k.before[2] + skill_rules::etc_value(c, 2),
              "Concentration: ATT " + std::to_string(k.before[0]) + " -> " +
                  std::to_string(got[0]) + ", ACC " + std::to_string(k.before[2]) + " -> " +
                  std::to_string(got[2]) + " (rule +" +
                  std::to_string(skill_rules::window_damage(c, 2)) + ", +" +
                  std::to_string(skill_rules::etc_value(c, 2)) + ")");
    });
    then_learn(r, 5, nullptr);
    then_quick_slot(r, "Dark Sight on LB", 5, 3);
    then_among_mobs(r, false);
    then_cast(r, 5);
    then_among_mobs(r, true);
    then_learn(r, 14, nullptr);
    then_quick_slot(r, "Shadow Partner on LB", 14, 3);
    then_cast(r, 14);
    then_quick_slot(r, "Lucky Seven on LB, with Shadow Partner", 2, 3);
    then_attack_with(r, 2, 3, 102);
    then_wait_for(r, "every buff ends", 120,
                  [](mt_cpu *cpu) { return buffs(cpu, hero(cpu)).empty(); });
    then_finish(r);
}

void skills_bandit(Run &r, u64) {
    using quest_bot::then_quick_slot;
    skills_common(r, false);
    then_phase(r, 0, 0);
    quest_bot::then_farm(
        r, "the dagger swing on the mobs of 1.3", 1, 3, {12, 16},
        [](mt_cpu *) { return g_skills->stats[0].exact >= 3; }, 600, skill_style(false));
    for (const i32 id : {4, 15, 17, 18, 20}) {
        then_learn(r, id, passive(id));
    }
    then_learn(r, 3, nullptr);
    then_quick_slot(r, "Double Step on LB", 3, 3);
    then_attack_with(r, 3, 6);
    then_learn(r, 2, nullptr);
    then_quick_slot(r, "Lucky Seven on LB", 2, 3);
    then_refused(r, 2);
    then_learn(r, 16, nullptr);
    then_quick_slot(r, "Savage Blow on LB", 16, 3);
    then_attack_with(r, 16, 6);
    then_learn(r, 23, nullptr);
    then_quick_slot(r, "Boomerang Stab on LB", 23, 3);
    then_attack_with(r, 23, 6);
    then_learn(r, 21, nullptr);
    then_quick_slot(r, "Concentration on LB", 21, 3);
    then_cast(r, 21);
    then_do(r, "skills: Concentration's ATT and ACC", [](mt_cpu *cpu) {
        const SkillRun &k = *g_skills;
        const std::array<i32, 7> got = hero_stats(cpu, hero(cpu));
        const skill_rules::SkillRow &c = k.rows.at(21);
        check(got[0] == k.before[0] + skill_rules::window_damage(c, 2) &&
                  got[2] == k.before[2] + skill_rules::etc_value(c, 2),
              "Concentration: ATT " + std::to_string(k.before[0]) + " -> " +
                  std::to_string(got[0]) + ", ACC " + std::to_string(k.before[2]) + " -> " +
                  std::to_string(got[2]));
    });
    then_learn(r, 5, nullptr);
    then_quick_slot(r, "Dark Sight on LB", 5, 3);
    then_among_mobs(r, false);
    then_cast(r, 5);
    then_among_mobs(r, true);
    then_learn(r, 19, nullptr);
    then_quick_slot(r, "Pickpocket on LB", 19, 3);
    then_cast(r, 19);
    then_phase(r, 19, 0);
    quest_bot::then_farm(
        r, "the dagger swing under Pickpocket", 1, 3, {12, 16},
        [](mt_cpu *cpu) {
            return g_skills->stats[19].picks > 0 || buffs(cpu, hero(cpu)).count(19) == 0;
        },
        600, skill_style(false));
    then_learn(r, 22, nullptr);
    then_quick_slot(r, "Meso Guard on LB", 22, 3);
    then_cast(r, 22);
    then_phase(r, 22, 0);
    quest_bot::then_farm(
        r, "the dagger swing under Meso Guard", 1, 3, {12, 16},
        [](mt_cpu *cpu) {
            return g_skills->stats[22].guard_ok >= 2 || buffs(cpu, hero(cpu)).count(22) == 0;
        },
        600, skill_style(false));
    then_wait_for(r, "every buff ends", 120,
                  [](mt_cpu *cpu) { return buffs(cpu, hero(cpu)).empty(); });
    then_finish(r);
}

// The checks of one skill's stats: attacks (casts, hits, stars, Drain), buffs (cast, lifetime).
void skill_report(i32 key, const std::string &name) {
    const SkillRun &k = *g_skills;
    const auto it = k.stats.find(key);
    const SkillStats s = it != k.stats.end() ? it->second : SkillStats{};
    const i32 id = key % 100;
    const skill_rules::SkillRow &row = k.rows.at(id);
    std::printf(
        "skills: %s (%d): %llu casts (%llu off the MP rule), %llu hits (%llu in one hit's "
        "range, %llu punches, %llu off), stars %llu ok %llu off, heals %llu ok %llu off, "
        "binds up to %llu, lifetime %d, near %llu harm %llu, guard %llu ok %llu off, "
        "pickpocket %llu ok %llu off, %llu stars; e.g. %s%s%s\n",
        name.c_str(), key, static_cast<unsigned long long>(s.casts),
        static_cast<unsigned long long>(s.mp_bad), static_cast<unsigned long long>(s.hits),
        static_cast<unsigned long long>(s.exact), static_cast<unsigned long long>(s.punches),
        static_cast<unsigned long long>(s.hit_bad), static_cast<unsigned long long>(s.star_ok),
        static_cast<unsigned long long>(s.star_bad), static_cast<unsigned long long>(s.heals),
        static_cast<unsigned long long>(s.heal_bad), static_cast<unsigned long long>(s.max_bind),
        s.lifetime, static_cast<unsigned long long>(s.near),
        static_cast<unsigned long long>(s.harm), static_cast<unsigned long long>(s.guard_ok),
        static_cast<unsigned long long>(s.guard_bad), static_cast<unsigned long long>(s.picks),
        static_cast<unsigned long long>(s.pick_bad), static_cast<unsigned long long>(s.stars),
        s.example.c_str(), s.first_bad.empty() ? "" : "; first off: ", s.first_bad.c_str());
    const bool no_bad = s.mp_bad == 0 && s.hit_bad == 0 && s.star_bad == 0 && s.heal_bad == 0 &&
                        s.harm == 0 && s.guard_bad == 0 && s.pick_bad == 0;
    check(no_bad, name + ": nothing off the rules" +
                      (s.first_bad.empty() ? "" : " (first: " + s.first_bad + ")"));
    if (row.type == 0) {
        check(s.exact > 0, name + ": hits in one hit's range");
    } else if (row.type == 3 && id != 13) {
        check(s.casts > 0 && s.exact > 0, name + ": casts at the MP cost and hits in range");
    } else if (id == 13) {
        check(s.casts > 0 && s.binds > 0 &&
                  s.max_bind >= static_cast<u64>(skill_rules::etc_value(row, 2) - 1) &&
                  s.max_bind <= static_cast<u64>(skill_rules::etc_value(row, 2)),
              name + ": casts bind mobs for @etc ticks (" + std::to_string(s.max_bind) + ", rule " +
                  std::to_string(skill_rules::etc_value(row, 2)) + ") and hurt none");
    } else if (row.type == 2) {
        const i32 d = skill_rules::duration(row, 2);
        check(s.casts == 1 && std::abs(s.lifetime - d) <= 1,
              name + ": one cast at the MP cost, on for its duration (" +
                  std::to_string(s.lifetime) + " ticks, rule " + std::to_string(d) + ")");
    }
    if (const i32 per = skill_rules::stars_used(id, key >= 100); per > 0) {
        const bool total = row.type == 0
                               ? s.stars > 0
                               : std::abs(static_cast<std::int64_t>(s.stars) -
                                          static_cast<std::int64_t>(s.casts) * per) <= per;
        check(s.star_ok > 0 && total, name + ": " + std::to_string(s.stars) + " stars for " +
                                          std::to_string(s.casts) + " casts, " +
                                          std::to_string(per) + " a use, never more in one tick");
    }
    if (id == 7) {
        check(s.heals > 0, name + ": heals @etc % of its damage");
    }
}

void skills_assassin_checks(Run &, u64) {
    for (const auto &[key, name] :
         std::vector<std::pair<i32, std::string>>{{1, "the throw"},
                                                  {2, "Lucky Seven"},
                                                  {7, "Drain"},
                                                  {10, "Avenger"},
                                                  {13, "Shadow Web"},
                                                  {12, "Concentration"},
                                                  {5, "Dark Sight"},
                                                  {14, "Shadow Partner"},
                                                  {102, "Lucky Seven with Shadow Partner"}}) {
        skill_report(key, name);
    }
    check(g_skills->stats[5].near > 0, "Dark Sight: mobs touched the hero while it was on");
}

void skills_bandit_checks(Run &, u64) {
    for (const auto &[key, name] : std::vector<std::pair<i32, std::string>>{{0, "the dagger swing"},
                                                                            {3, "Double Step"},
                                                                            {16, "Savage Blow"},
                                                                            {23, "Boomerang Stab"},
                                                                            {21, "Concentration"},
                                                                            {5, "Dark Sight"},
                                                                            {19, "Pickpocket"},
                                                                            {22, "Meso Guard"}}) {
        skill_report(key, name);
    }
    check(g_skills->stats[5].near > 0, "Dark Sight: mobs touched the hero while it was on");
    check(g_skills->stats[19].picks > 0, "Pickpocket: hits dropped meso");
    check(g_skills->stats[22].guard_ok > 0, "Meso Guard: blows paid in meso");
}

// --- The weapon rules of passives and buffs (#307, docs/testing/skills-and-jobs.md) ----------
//
// skills_weapon_assassin: the Assassin of skills_assassin with no weapon; skills_weapon_bandit:
// the Bandit of skills_bandit with a claw and stars. Each learns its branch's passives (the rule
// checked as in skills_*) and presses each buff on LB: cast (on the buff list, MP paid) or
// refused (no buff, MP unchanged). CSkill::setActiveSkill 0x7EFF1's weapon check holds for the
// attack skills only: every buff is cast.

std::map<i32, bool> g_weapon_buffs; // buff id -> cast with this weapon

// Presses buff `id` up to three times; records whether it went on.
void then_buff_try(Run &r, i32 id) {
    then_phase(r, id, id);
    auto mp = std::make_shared<i32>(0);
    then_do(r, "skills: MP before " + std::to_string(id),
            [mp](mt_cpu *cpu) { *mp = hero(cpu).mp; });
    then(r, "skills: press " + std::to_string(id) + " up to three times", 6,
         [&r, id](mt_cpu *cpu, double el) {
             const bool on = buffs(cpu, hero(cpu)).count(id) != 0;
             release_all(r);
             hold(r, quest_bot::LB, std::fmod(el, 1.5) < 0.1 && !on);
             return (on && el >= 0.7) || el >= 4.6;
         });
    then_wait_field(r);
    then_do(r, "skills: " + std::to_string(id) + " with this weapon", [mp, id](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const bool on = buffs(cpu, h).count(id) != 0;
        g_weapon_buffs[id] = on;
        std::printf("weapon rule: buff %d %s, MP %d -> %d\n", id, on ? "cast" : "refused", *mp,
                    h.mp);
        check(on == (h.mp < *mp), "buff " + std::to_string(id) + ": MP paid only when it is on");
    });
}

// With nothing worn and no skill learned, the hero's ATT, DEF, ACC and avoid are hero.dat's
// columns 14-17 of the level (docs/formats/data-tables.md#herodat; CHero +0xA4/+0xA8/+0xB0/+0xAC).
void then_bare_stats(Run &r) {
    then_do(r, "skills: the bare hero's stats are hero.dat's", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const std::vector<std::uint8_t> dat =
            world_map::read_bytes(fs::path(MT_BUNDLE_DIR) / "English" / "data" / "hero.dat");
        std::array<i32, 4> want{-1, -1, -1, -1};
        for (std::size_t o = 6; o + 18 <= dat.size(); o += 18) {
            if (dat[o] == h.level) {
                want = {dat[o + 14], dat[o + 15], dat[o + 16], dat[o + 17]};
            }
        }
        const std::array<i32, 7> got = hero_stats(cpu, h);
        check(got[0] == want[0] && got[1] == want[1] && got[2] == want[2] && got[3] == want[3],
              "level " + std::to_string(h.level) + ": ATT DEF ACC AVOID " + std::to_string(got[0]) +
                  " " + std::to_string(got[1]) + " " + std::to_string(got[2]) + " " +
                  std::to_string(got[3]) + ", hero.dat " + std::to_string(want[0]) + " " +
                  std::to_string(want[1]) + " " + std::to_string(want[2]) + " " +
                  std::to_string(want[3]));
    });
}

void skills_weapon(Run &r, bool thrower, std::initializer_list<i32> passives,
                   std::initializer_list<i32> buff_ids) {
    using quest_bot::then_quick_slot;
    g_weapon_buffs.clear();
    skills_common(r, thrower);
    if (!thrower) {
        then_bare_stats(r);
    }
    for (const i32 id : passives) {
        then_learn(r, id, passive(id));
    }
    for (const i32 id : buff_ids) {
        then_learn(r, id, nullptr);
        then_quick_slot(r, "skill " + std::to_string(id) + " on LB", id, 3);
        then_buff_try(r, id);
    }
    then_finish(r);
}
void skills_weapon_assassin(Run &r, u64) { skills_weapon(r, false, {4, 6, 8, 9, 11}, {5, 12, 14}); }
void skills_weapon_bandit(Run &r, u64) {
    skills_weapon(r, true, {4, 15, 17, 18, 20}, {5, 19, 21, 22});
}

void skills_weapon_checks(Run &, u64) {
    std::string text;
    for (const auto &[id, on] : g_weapon_buffs) {
        text += " " + std::to_string(id) + (on ? " cast" : " refused");
    }
    // Played (#307): every buff goes on with no weapon and with the other branch's weapon.
    check(!g_weapon_buffs.empty() && std::all_of(g_weapon_buffs.begin(), g_weapon_buffs.end(),
                                                 [](const auto &b) { return b.second; }),
          "weapon rule: every buff cast without the branch's weapon:" + text);
}

// --- Speech: dialogue and the scenes' speech boxes (#391, ADR 0187, speech_text.h) -------------
//
// speech_dialogue: chat_text's flow (a new game through the intro's speech boxes and MacArthur's
// first dialog on 2.8, A as soon as each box shows) with the speech on the log backend
// (MT_SPEECH=on) or off (unset: the base run). The flow's own monitor reads every box from guest
// memory each tick; the run records each text event (maplethief/text_events.h) and each
// utterance the log backend is asked to say. In the field after the dialog the state is written
// to OUT_DIR/speech_state.txt: the game's rand() state (0x11A510), the tick, and the hero, the
// quests and the mobs as scene_state reads them. Checks, with the speech on:
//   - every box the monitor saw is one event, in the same order, of the same kind, with the same
//     text (white space aside); each event has a speaker and valid UTF-8 text;
//   - each page and each scene box is one utterance, in the same order, "speaker: text";
//   - MT_SPEECH_BASE=<the off run's OUT_DIR>: every state line equals the off run's.
// With it off: no event and no utterance (the wrappers are off). Only counts and hashes are
// printed and compared, never the game's text.
struct SpeechRun {
    bool on = false;
    std::unique_ptr<fe::Speech> speech;
    std::unique_ptr<fe::SpeechText> text;
    struct Event {
        std::uint32_t kind = 0, speaker_kind = 0;
        std::string speaker, text;
    };
    std::vector<Event> events;
    fe::LogSpeechBackend *log = nullptr;                    // the speech's backend
    std::vector<std::string> said;                          // what it was asked to say
    std::vector<std::pair<std::string, std::string>> boxes; // the monitor's: kind, lines
    ChatBoxView last;
    u64 last_tick = 0;
    std::vector<std::pair<std::string, std::string>> state;
} g_sd;

void record_text_event(void *, const mt_text_event *e) {
    g_sd.events.push_back(SpeechRun::Event{e->kind, e->speaker_kind, e->speaker, e->text});
}

// The text without its white space (the scene boxes' lines end in "\r") and the monitor's
// line ends ('|').
std::string bare(const std::string &s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '|') {
            out.push_back(c);
        }
    }
    return out;
}

bool valid_utf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80                 ? 1
                              : c >= 0xC2 && c <= 0xDF ? 2
                              : c >= 0xE0 && c <= 0xEF ? 3
                              : c >= 0xF0 && c <= 0xF4 ? 4
                                                       : 0;
        if (n == 0 || i + n > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < n; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += n;
    }
    return true;
}

void speech_dialogue(Run &r, u64) {
    r.settings_from_env = false; // the on and off runs alike, whatever MAPLETHIEF_SETTINGS says
    g_sd.on = std::getenv("MT_SPEECH") != nullptr && std::string(std::getenv("MT_SPEECH")) == "on";
    auto backend = std::make_unique<fe::LogSpeechBackend>([](const std::string &) {});
    g_sd.log = backend.get();
    g_sd.speech = std::make_unique<fe::Speech>(std::move(backend));
    g_sd.speech->set_enabled(g_sd.on);
    g_sd.text = std::make_unique<fe::SpeechText>();
    mt_text_events_add_listener(record_text_event, nullptr);
    queue_chat_text(r);
    const std::function<void(mt_cpu *)> monitor_turn = r.on_turn;
    r.on_turn = [&r, monitor_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        // As game.cpp's turn: the events since the last turn said, then the speech's own turn.
        g_sd.text->turn(*g_sd.speech, r.settings, r.t);
        g_sd.speech->turn(r.t);
        if (r.ticks == g_sd.last_tick) {
            return;
        }
        g_sd.last_tick = r.ticks;
        const ChatBoxView v = chat_box_view(cpu);
        if (!v.kind.empty() && (!v.same_box(g_sd.last) || v.text != g_sd.last.text)) {
            g_sd.boxes.emplace_back(v.kind, v.text);
        }
        g_sd.last = v;
    };
    then_do(r, "the state in the field after the dialog", [&r](mt_cpu *cpu) {
        g_sd.state = scene_state(cpu);
        g_sd.state.emplace_back("ticks", std::to_string(r.ticks));
        std::ofstream f(r.out / "speech_state.txt", std::ios::trunc);
        for (const auto &[k, v] : g_sd.state) {
            f << k << " = " << v << "\n";
        }
    });
    then_finish(r);
}

void speech_dialogue_checks(Run &, u64) {
    SpeechRun &s = g_sd;
    mt_text_events_remove_listener(record_text_event, nullptr);
    for (const fe::Utterance &u : s.log->said()) {
        s.said.push_back(u.text);
    }
    u64 pages = 0, answers = 0, scene_boxes = 0;
    for (const SpeechRun::Event &e : s.events) {
        pages += e.kind == MT_TEXT_EVENT_PAGE ? 1 : 0;
        answers += e.kind == MT_TEXT_EVENT_ANSWER ? 1 : 0;
        scene_boxes += e.kind == MT_TEXT_EVENT_SCENE_BOX ? 1 : 0;
    }
    std::string all;
    for (const std::string &u : s.said) {
        all += u + "\n";
    }
    const fe::SpeechStats st = s.speech->stats();
    // Released here: left to the process's exit, the destructors aborted it.
    s.text.reset();
    s.speech.reset();
    std::printf("speech_dialogue (speech %s): %zu boxes seen; events: %llu pages, %llu answers, "
                "%llu scene boxes; %zu utterances (hash %s); said %llu, interrupted %llu\n",
                s.on ? "on" : "off", s.boxes.size(), static_cast<unsigned long long>(pages),
                static_cast<unsigned long long>(answers),
                static_cast<unsigned long long>(scene_boxes), s.said.size(),
                fe::hex64(fe::fnv1a64(all.data(), all.size())).c_str(),
                static_cast<unsigned long long>(st.spoken),
                static_cast<unsigned long long>(st.interrupted));
    check(!s.state.empty(), "the run reached the field after MacArthur's dialog");
    if (!s.on) {
        check(s.events.empty() && s.said.empty(),
              "speech off: no text event and nothing said (" + std::to_string(s.events.size()) +
                  " events, " + std::to_string(s.said.size()) + " utterances)");
    } else {
        // The events but the answers, against the boxes the monitor saw.
        std::vector<const SpeechRun::Event *> shown;
        for (const SpeechRun::Event &e : s.events) {
            if (e.kind != MT_TEXT_EVENT_ANSWER) {
                shown.push_back(&e);
            }
        }
        u64 npc = 0, speech = 0;
        bool same = shown.size() == s.boxes.size();
        for (std::size_t i = 0; i < s.boxes.size(); i++) {
            const bool is_npc = s.boxes[i].first == "npc";
            (is_npc ? npc : speech)++;
            same = same && i < shown.size() &&
                   shown[i]->kind == (is_npc ? MT_TEXT_EVENT_PAGE : MT_TEXT_EVENT_SCENE_BOX) &&
                   bare(shown[i]->text) == bare(s.boxes[i].second);
        }
        check(npc >= 2 && speech >= 1, "the run shows NPC pages and scene speech boxes (" +
                                           std::to_string(npc) + " pages, " +
                                           std::to_string(speech) + " speech boxes)");
        check(same, "every box shown is one text event, in order, of its kind, with its text (" +
                        std::to_string(shown.size()) + " events, " +
                        std::to_string(s.boxes.size()) + " boxes)");
        bool whole = true, named = true;
        for (const SpeechRun::Event &e : s.events) {
            whole = whole && valid_utf8(e.text) && valid_utf8(e.speaker) &&
                    (e.kind == MT_TEXT_EVENT_ANSWER || !e.text.empty());
            named = named && !e.speaker.empty() && e.speaker_kind != MT_TEXT_SPEAKER_NONE;
        }
        check(whole, "every event's text and speaker are whole UTF-8");
        check(named, "every event has a speaker");
        bool lines = s.said.size() == shown.size();
        for (std::size_t i = 0; lines && i < s.said.size(); i++) {
            lines = valid_utf8(s.said[i]) &&
                    s.said[i].rfind(shown[i]->speaker + ": " + shown[i]->text, 0) == 0;
        }
        check(lines, "each page and scene box is said once, in order, as \"speaker: text\" (" +
                         std::to_string(s.said.size()) + " utterances)");
    }
    const char *base_dir = std::getenv("MT_SPEECH_BASE");
    if (base_dir == nullptr || *base_dir == 0) {
        return;
    }
    std::ifstream in(fs::path(base_dir) / "speech_state.txt", std::ios::binary);
    std::vector<std::pair<std::string, std::string>> base;
    for (std::string line; std::getline(in, line);) {
        const std::size_t eq = line.find(" = ");
        if (eq != std::string::npos) {
            base.emplace_back(line.substr(0, eq), line.substr(eq + 3));
        }
    }
    std::size_t differ = 0;
    for (std::size_t i = 0; i < s.state.size() && i < base.size(); i++) {
        if (s.state[i] != base[i]) {
            differ++;
            std::printf("speech_dialogue: %s differs from the off run's\n",
                        s.state[i].first.c_str());
        }
    }
    check(!base.empty() && base.size() == s.state.size() && differ == 0,
          "the rand() state, the tick, the hero, the quests and the mobs equal the off run's (" +
              std::to_string(s.state.size()) + " lines, " + std::to_string(differ) + " differ)");
}

// --- systems sweep 2 (#304): the cash shop's tiles and restores, scrolls, Pigmy, portal620 ------
//
// Each mode starts from a built save (tests/starts/systems2_*.json, MT_FROM_SAVE) that equals no
// chained state, so every test is named synthetic (docs/testing/start-states.md). What each one
// asserts is in docs/testing/systems-sweep-2.md.

// A guest word written by the test, where the game's own code would have written it.
void wr(mt_cpu *cpu, u32 addr, u32 v) {
    if (addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(cpu->mem + addr, &v, 4);
    }
}

// A guest function called from the turn hook, the registers kept.
u32 call(mt_cpu *cpu, u32 fn, std::vector<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.data(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

// CHero::ExpToLevelUp 0x65919: the EXP the hero's level needs.
i32 exp_need(mt_cpu *cpu) { return static_cast<i32>(call(cpu, 0x65919u, {hero(cpu).addr})); }

// The hero's level and EXP after `gain` EXP (CHero::setExp carries the rest into a level-up).
std::pair<i32, i32> after_exp(i32 level, i32 exp, i32 need, i32 gain) {
    return exp + gain >= need ? std::pair{level + 1, exp + gain - need}
                              : std::pair{level, exp + gain};
}

// Presses A on every popup until `done` holds in the field, each popup id seen kept in `seen`.
// With `in_field` false, `done` alone ends it (a screen other than the field may follow).
void then_answer(Run &r, std::string what, Pred done, std::shared_ptr<std::set<i32>> seen,
                 SDL_GamepadButton first = A, bool in_field = true, double timeout = 60) {
    struct S {
        double opened = -1;
        bool firsted = false;
    };
    auto s = std::make_shared<S>();
    then(r, what, timeout, [&r, done, seen, first, in_field, s](mt_cpu *cpu, double el) {
        const i32 p = popup_id(cpu);
        if (p != -1) {
            seen->insert(p);
            s->opened = s->opened < 0 ? el : s->opened;
            const double o = el - s->opened;
            const SDL_GamepadButton b = s->firsted ? A : first;
            hold(r, b, o >= 0.6 && std::fmod(o - 0.6, 1.0) < 0.1);
            if (o >= 0.6 && std::fmod(o - 0.6, 1.0) < 0.1) {
                s->firsted = true;
            }
            return false;
        }
        s->opened = -1;
        release_all(r);
        return (field(cpu) || !in_field) && done(cpu);
    });
}

std::string seen_text(const std::set<i32> &seen) {
    std::string s;
    for (const i32 p : seen) {
        char b[16];
        std::snprintf(b, sizeof b, "%s0x%X", s.empty() ? "" : " ", static_cast<unsigned>(p));
        s += b;
    }
    return s;
}

// --- Speech: notify lines and popups (#392, ADR 0191, speech_text.h) ---------------------------
//
// speech_notices: from a built start (tests/starts/speech_notices.json, MT_FROM_SAVE: a level-10
// Beginner with a knife on the hunting field 2.3), the hunt until the hero has gained EXP and
// the notify list has shown a line besides the kill's (a pick-up or meso); then a fall, the
// death popup (0x68) and its Return to town, answered with OK and Yes (0x69), back in town.
// With the speech on the log backend (MT_SPEECH=on) or off (unset: the base run). Each turn the
// run reads the notify list (+0x4C) from guest memory and keeps every new line; the text events
// and the utterances are recorded. In town the state goes to OUT_DIR/speech_state.txt as
// speech_dialogue's. Checks, with the speech on:
//   - each notice event is a line the list showed, in the same order; at least one EXP line
//     (the kill) and one other on the list +0x4C (the pick-up or meso);
//   - the popups 0x68 (no message, kind 0) then 0x69 (a message, kind 1: Yes / No);
//   - the notices said as kNotice utterances (a turn's lines in one), the popups as kDialogue:
//     the death popup in the port's words, 0x69 its message then "Yes or No.";
//   - MT_SPEECH_BASE=<the off run's OUT_DIR>: every state line equals the off run's.
// With it off: no event and no utterance. Only counts and hashes are printed, never the text.
struct NoticesRun {
    bool on = false;
    std::unique_ptr<fe::Speech> speech;
    std::unique_ptr<fe::SpeechText> text;
    struct Event {
        std::uint32_t kind = 0;
        i32 index = 0, value = 0;
        std::string speaker, text;
    };
    std::vector<Event> events;
    fe::LogSpeechBackend *log = nullptr;
    std::set<std::pair<u32, std::uint64_t>> seen; // the list's entries seen: address, time
    std::vector<std::string> lines;               // their text, in order
    std::size_t lines_at_hunt = 0;
    i32 exp_at_hunt = -1;
    std::shared_ptr<std::set<i32>> popups = std::make_shared<std::set<i32>>();
    std::vector<std::pair<std::string, std::string>> state;
} g_sn;

void record_notice_event(void *, const mt_text_event *e) {
    g_sn.events.push_back(NoticesRun::Event{e->kind, e->index, e->value, e->speaker, e->text});
}

void speech_notices(Run &r, u64) {
    r.settings_from_env = false;
    g_sn.on = std::getenv("MT_SPEECH") != nullptr && std::string(std::getenv("MT_SPEECH")) == "on";
    auto backend = std::make_unique<fe::LogSpeechBackend>([](const std::string &) {});
    g_sn.log = backend.get();
    g_sn.speech = std::make_unique<fe::Speech>(std::move(backend));
    g_sn.speech->set_enabled(g_sn.on);
    g_sn.text = std::make_unique<fe::SpeechText>();
    mt_text_events_add_listener(record_notice_event, nullptr);
    from_save(r);
    const std::function<void(mt_cpu *)> monitor_turn = r.on_turn;
    r.on_turn = [&r, monitor_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        g_sn.text->turn(*g_sn.speech, r.settings, r.t);
        g_sn.speech->turn(r.t);
        for (const NotifyLine &l : notify_lines(cpu)) {
            if (g_sn.seen.insert({l.addr, l.time}).second) {
                g_sn.lines.push_back(l.text);
            }
        }
    };
    then_wait_field(r);
    then_do(r, "the hunt begins", [](mt_cpu *cpu) {
        g_play.hunting = true;
        g_sn.lines_at_hunt = g_sn.lines.size();
        g_sn.exp_at_hunt = hero(cpu).exp;
        std::printf("speech_notices: the hunt from %s\n", hero_text(cpu, hero(cpu)).c_str());
    });
    then_hunt(r, "map 2.3 until a kill's EXP and a pick-up's line",
              [](mt_cpu *cpu) {
                  return hero(cpu).exp > g_sn.exp_at_hunt &&
                         g_sn.lines.size() >= g_sn.lines_at_hunt + 2;
              },
              600, {2, 0, 42, 2, 3});
    then_do(r, "the hero falls", [&r](mt_cpu *cpu) {
        g_play.hunting = false;
        release_all(r);
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup (0x68)", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_answer(
        r, "Return to town (0x68 OK, 0x69 Yes)",
        [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            return h.hp > 0 && h.on(2, 0);
        },
        g_sn.popups);
    then_do(r, "the state in town", [&r](mt_cpu *cpu) {
        g_sn.state = scene_state(cpu);
        g_sn.state.emplace_back("ticks", std::to_string(r.ticks));
        std::ofstream f(r.out / "speech_state.txt", std::ios::trunc);
        for (const auto &[k, v] : g_sn.state) {
            f << k << " = " << v << "\n";
        }
    });
    then_finish(r);
}

void speech_notices_checks(Run &, u64) {
    NoticesRun &s = g_sn;
    mt_text_events_remove_listener(record_notice_event, nullptr);
    // The notices still queued behind the last popup, said as the voice frees up.
    for (int i = 1; i <= 60; i++) {
        s.speech->turn(1e6 + 30.0 * i);
    }
    std::vector<std::pair<fe::SpeechKind, std::string>> said;
    for (const fe::Utterance &u : s.log->said()) {
        said.emplace_back(u.kind, u.text);
    }
    std::vector<const NoticesRun::Event *> notices, popups;
    for (const NoticesRun::Event &e : s.events) {
        (e.kind == MT_TEXT_EVENT_NOTICE ? notices : popups).push_back(&e);
    }
    std::string all;
    for (const auto &[k, t] : said) {
        all += (k == fe::SpeechKind::kNotice ? "n " : "d ") + t + "\n";
    }
    const fe::SpeechText::Stats ts = s.text->stats();
    s.text.reset();
    s.speech.reset();
    std::printf("speech_notices (speech %s): %zu list lines seen, popups %s; events: %zu "
                "notices, %zu popups; %zu utterances (hash %s), %llu notice utterances\n",
                s.on ? "on" : "off", s.lines.size(), seen_text(*s.popups).c_str(), notices.size(),
                popups.size(), said.size(), fe::hex64(fe::fnv1a64(all.data(), all.size())).c_str(),
                static_cast<unsigned long long>(ts.notice_utterances));
    check(!s.state.empty(), "the run reached the town after the death popup");
    check(s.popups->count(0x68) == 1 && s.popups->count(0x69) == 1,
          "the game showed the death popup and its Return to town (" + seen_text(*s.popups) + ")");
    if (!s.on) {
        check(s.events.empty() && said.empty(), "speech off: no text event and nothing said (" +
                                                    std::to_string(s.events.size()) + " events, " +
                                                    std::to_string(said.size()) + " utterances)");
    } else {
        // The list +0x4C's lines, against the notice events of that list.
        std::vector<std::string> heard;
        u64 exp = 0, other = 0;
        for (const NoticesRun::Event *e : notices) {
            if (e->value == 0) {
                heard.push_back(e->text);
                (e->text.rfind("EXP ", 0) == 0 ? exp : other)++;
            }
        }
        // The run reads the list once a turn, so a line the list drops within the turn it came
        // (its most entries reached) is an event the run never saw listed: the lines are the
        // events in order, some events between them.
        std::size_t at = 0;
        for (std::size_t i = 0; at < s.lines.size() && i < heard.size(); i++) {
            at += bare(heard[i]) == bare(s.lines[at]) ? 1 : 0;
        }
        check(at == s.lines.size() && !s.lines.empty(),
              "each line the notify list showed is a notice event, in order, with its text (" +
                  std::to_string(heard.size()) + " events, " + std::to_string(s.lines.size()) +
                  " lines)");
        check(exp >= 1 && other >= 1, "a kill's EXP line and a pick-up or meso line (" +
                                          std::to_string(exp) + " EXP, " + std::to_string(other) +
                                          " other)");
        bool whole = true;
        for (const NoticesRun::Event &e : s.events) {
            whole = whole && valid_utf8(e.text) && valid_utf8(e.speaker);
        }
        check(whole, "every event's text is whole UTF-8");
        // The popups: 0x68 then 0x69 (the death popup is reopened after a sub-popup it cancels;
        // here none is cancelled).
        std::vector<i32> ids;
        for (const NoticesRun::Event *e : popups) {
            ids.push_back(e->index);
        }
        const auto death = std::find(ids.begin(), ids.end(), 0x68);
        const auto town = std::find(ids.begin(), ids.end(), 0x69);
        check(death != ids.end() && town != ids.end() && death < town,
              "the popup events: the death popup, then its Return to town");
        const NoticesRun::Event *ask = town != ids.end() ? popups[town - ids.begin()] : nullptr;
        check(ask != nullptr && ask->value == 1 && !ask->text.empty(),
              "Return to town: kind 1 (Yes / No) with a message");
        // The utterances: the notices as kNotice, the popups as kDialogue in their words.
        std::string notice_text;
        u64 dialogue = 0;
        bool death_said = false, town_said = false;
        for (const auto &[k, t] : said) {
            if (k == fe::SpeechKind::kNotice) {
                notice_text += t + "\n";
            } else {
                dialogue++;
                death_said = death_said || t == fe::tr(fe::Txt::speech_death_popup);
                const std::string yes_no = fe::tr(fe::Txt::speech_popup_yes_no);
                town_said = town_said || (ask != nullptr && t == ask->text + " " + yes_no);
            }
        }
        bool every = true;
        for (const std::string &h : heard) {
            every = every && notice_text.find(h) != std::string::npos;
        }
        check(every && ts.notice_utterances >= 1 && ts.notice_utterances <= notices.size(),
              "each notify line is said in a kNotice utterance (" +
                  std::to_string(ts.notice_utterances) + " utterances for " +
                  std::to_string(notices.size()) + " lines)");
        check(death_said && town_said && dialogue == popups.size(),
              "the death popup said in the port's words, Return to town as its message and "
              "\"Yes or No.\", each popup one dialogue utterance");
    }
    const char *base_dir = std::getenv("MT_SPEECH_BASE");
    if (base_dir == nullptr || *base_dir == 0) {
        return;
    }
    std::ifstream in(fs::path(base_dir) / "speech_state.txt", std::ios::binary);
    std::vector<std::pair<std::string, std::string>> base;
    for (std::string line; std::getline(in, line);) {
        const std::size_t eq = line.find(" = ");
        if (eq != std::string::npos) {
            base.emplace_back(line.substr(0, eq), line.substr(eq + 3));
        }
    }
    std::size_t differ = 0;
    for (std::size_t i = 0; i < s.state.size() && i < base.size(); i++) {
        if (s.state[i] != base[i]) {
            differ++;
            std::printf("speech_notices: %s differs from the off run's\n",
                        s.state[i].first.c_str());
        }
    }
    check(!base.empty() && base.size() == s.state.size() && differ == 0,
          "the rand() state, the tick, the hero, the quests and the mobs equal the off run's (" +
              std::to_string(s.state.size()) + " lines, " + std::to_string(differ) + " differ)");
}

// --- the cash shop's tiles, the Comeback Pack and the Reverse item ------------------------------
//
// systems2_cash_tiles: cash_shop = meso, a level-30 Rogue on 0.0 with 3,000,000 meso, 50 Red
// Potions and one Reverse (item_etc 130). Every tile tapped once by the focus: the six products
// the policy sells are bought at the quote shown (InitializeSP and SP Marble: item 129 / 132 in
// the bag; EXP Explosion 40 % / 80 %: that share of the level's need, carried into a level-up),
// the fake server confirms each receipt and crestore is empty after it; the four meso packs are
// not sold (ADR 0072): the payment is cancelled, nothing changes and nothing connects. Then a
// fall and the Comeback Pack (Y, -35): revived, 30 Power Elixirs, 40 % EXP, riprestore2 empty.
// Then a fall with Reverse in the bag and Return to town: the death popup's OK always asks 0x69
// and returns to town; the Reverse's revive is the handler of popup 0x6B, which no code opens
// (docs/re/mechanics.md), so the Reverse stays in the bag.
struct CashTile {
    const char *action;
    u32 product;
    i32 item; // the item granted, -1 none
};
constexpr CashTile k_cash_tiles[] = {
    {"releaseMeso3000Button:", MT_CASH_INITIALIZE_SP, 129},
    {"releaseMeso10000Button:", MT_CASH_SP_MARBLE, 132},
    {"releaseMeso22000Button:", MT_CASH_MESOPACK3, -1},
    {"releaseMeso50000Button:", MT_CASH_MESOPACK4, -1},
    {"releaseMeso120000Button:", MT_CASH_MESOPACK5, -1},
    {"releaseMeso300000Button:", MT_CASH_MESOPACK6, -1},
    {"releaseExp40Button:", MT_CASH_EXP40, -1},
    {"releaseExp80Button:", MT_CASH_EXP80, -1},
};

struct Before {
    Hero h{};
    i32 need = 0, item = 0, elixirs = 0, reverse = 0;
    std::int64_t quote = 0;
    std::size_t log = 0;
    mt_cash_stats stats{};
    mt_stubs_server_stats server{};
};
Before g_before;

void remember(Run &r, mt_cpu *cpu, u32 product, i32 item) {
    g_before = Before{};
    g_before.h = hero(cpu);
    g_before.need = exp_need(cpu);
    g_before.item = item >= 0 ? item_count(cpu, g_before.h, item) : 0;
    g_before.elixirs = item_count(cpu, g_before.h, 10);
    g_before.reverse = item_count(cpu, g_before.h, 130);
    g_before.quote = product < MT_CASH_PRODUCT_COUNT ? mt_cash_shop_quote(product) : -1;
    g_before.log = r.log.size();
    g_before.stats = mt_cash_shop_get_stats();
    g_before.server = mt_stubs_get_server_stats();
}

bool shop_open(Run &r, mt_cpu *cpu) {
    return mt_input_panel() == 9 && game_mode(cpu) == 0xB && r.focus.active();
}

// The EXP of `pct` % of the need remembered, against the hero now.
void check_exp_gain(mt_cpu *cpu, i32 pct, const std::string &what) {
    const Hero h = hero(cpu);
    const i32 gain = g_before.need * pct / 100;
    const auto [level, exp] = after_exp(g_before.h.level, g_before.h.exp, g_before.need, gain);
    check(h.level == level && h.exp == exp,
          what + ": +" + std::to_string(pct) + " % of " + std::to_string(g_before.need) +
              " EXP: level " + std::to_string(g_before.h.level) + " EXP " +
              std::to_string(g_before.h.exp) + " -> level " + std::to_string(h.level) + " EXP " +
              std::to_string(h.exp) + " (expected " + std::to_string(level) + " / " +
              std::to_string(exp) + ")");
}

void systems2_cash_tiles(Run &r, u64) {
    mt_cash_shop_install(MT_CASH_SHOP_MESO);
    from_save(r);
    for (const CashTile &t : k_cash_tiles) {
        const std::string name = mt_cash_product_suffix(t.product);
        then_cash_shop_open(r);
        then_do(r, name + ": before the tap", [&r, t](mt_cpu *cpu) {
            remember(r, cpu, t.product, t.item);
            const mt_cash_view v = mt_cash_shop_view();
            check(view_quote(v, t.product) == g_before.quote,
                  std::string(mt_cash_product_suffix(t.product)) + ": the price line quotes " +
                      std::to_string(g_before.quote) + " (" + lines_text(v) + ")");
        });
        then_focus(r, t.action);
        then_press(r, A, 0.1, 0.2);
        if (mt_cash_price(mt_cash_default_knobs(), t.product, 1, 0, 0) >= 0) {
            then_wait_for(r, name + ": Purchase complete! (popup 0x26C)", 30,
                          [](mt_cpu *cpu) { return popup_id(cpu) == 0x26C; });
            then_do(r, name + ": bought for meso", [&r, t, name](mt_cpu *cpu) {
                const Hero h = hero(cpu);
                const mt_cash_stats st = mt_cash_shop_get_stats();
                const mt_stubs_server_stats sv = mt_stubs_get_server_stats();
                check(g_before.quote > 0 && h.meso == g_before.h.meso - g_before.quote &&
                          st.sold == g_before.stats.sold + 1 && st.last_price == g_before.quote,
                      name +
                          ": the meso went down by the quote: " + std::to_string(g_before.h.meso) +
                          " - " + std::to_string(g_before.quote) + " = " + std::to_string(h.meso));
                if (t.item >= 0) {
                    check(item_count(cpu, h, t.item) == g_before.item + 1,
                          name + ": item_etc " + std::to_string(t.item) + " in the bag (" +
                              std::to_string(g_before.item) + " -> " +
                              std::to_string(item_count(cpu, h, t.item)) + ")");
                } else {
                    check_exp_gain(cpu, t.product == MT_CASH_EXP40 ? 40 : 80, name);
                }
                check(sv.accepted == g_before.server.accepted + 1 &&
                          sv.confirmed == g_before.server.confirmed + 1 &&
                          logged(r, g_before.log,
                                 "fake server: receipt (op 2) for " MT_CASH_PRODUCT_PREFIX + name),
                      name + ": the fake server confirmed its receipt (op 2 -> op 3)");
                check(receipts(r, "crestore") == 0, name + ": crestore holds no receipt");
                check(saved_meso(r) == h.meso, name + ": theif0 saved with the meso after it");
            });
            then_press(r, A); // OK: back to the shop
            then_wait_for(
                r, name + ": the shop again", 10, [&r](mt_cpu *cpu) { return shop_open(r, cpu); },
                0.5);
        } else {
            then_wait_for(
                r, name + ": the payment cancelled, the shop back", 15,
                [&r](mt_cpu *cpu) {
                    return mt_cash_shop_get_stats().refused > g_before.stats.refused &&
                           shop_open(r, cpu);
                },
                1.0);
            then_do(r, name + ": not sold", [name](mt_cpu *cpu) {
                const Hero h = hero(cpu);
                check(g_before.quote == -1 && h.meso == g_before.h.meso &&
                          h.exp == g_before.h.exp &&
                          mt_cash_shop_get_stats().sold == g_before.stats.sold &&
                          mt_stubs_get_server_stats().accepted == g_before.server.accepted,
                      name + ": a meso pack is not sold for meso (ADR 0072): cancelled, " +
                          hero_text(cpu, h));
            });
        }
        then_press(r, B);
        then_wait_field(r);
    }
    // The Comeback Pack on the death popup.
    then_do(r, "the hero falls", [&r](mt_cpu *cpu) {
        remember(r, cpu, MT_CASH_COMEBACK, -1);
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup (0x68)", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    then_do(r, "the Comeback Pack's price", [](mt_cpu *) {
        g_before.quote = view_quote(mt_cash_shop_view(), MT_CASH_COMEBACK);
        check(g_before.quote > 0,
              "the price line quotes the Comeback Pack: " + lines_text(mt_cash_shop_view()));
    });
    then_press(r, Y, 0.1, 0.2); // -35: Grade revival
    then_wait_for(
        r, "revived in place", 30,
        [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            return field(cpu) && popup_id(cpu) == -1 && h.hp == h.max_hp && h.hp > 0;
        },
        1.0);
    then_do(r, "the Comeback Pack bought for meso", [&r](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.meso == g_before.h.meso - g_before.quote &&
                  item_count(cpu, h, 10) == g_before.elixirs + 30 &&
                  h.on(g_before.h.area, g_before.h.map),
              "Comeback Pack: revived on " + where(cpu) + " for " + std::to_string(g_before.quote) +
                  " meso with 30 Power Elixirs (" + std::to_string(g_before.elixirs) + " -> " +
                  std::to_string(item_count(cpu, h, 10)) + ")");
        check_exp_gain(cpu, 40, "Comeback Pack");
        check(receipts(r, "riprestore2") == 0 &&
                  mt_stubs_get_server_stats().confirmed == g_before.server.confirmed + 1,
              "Comeback Pack: the fake server confirmed it, riprestore2 holds no receipt");
    });
    // The Reverse: Return to town on the death popup with item 130 in the bag.
    then_do(r, "the hero falls with a Reverse", [&r](mt_cpu *cpu) {
        remember(r, cpu, MT_CASH_PRODUCT_COUNT, -1);
        fall(cpu);
    });
    then_wait_for(
        r, "the death popup again", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; }, 1.0);
    auto seen = std::make_shared<std::set<i32>>();
    then_answer(
        r, "Return to town with a Reverse",
        [](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            return h.hp > 0;
        },
        seen);
    then_do(r, "Return to town with a Reverse", [seen](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("reverse: popups %s; %s\n", seen_text(*seen).c_str(),
                    hero_text(cpu, h).c_str());
        check(seen->count(0x69) == 1 && seen->count(0x6B) == 0,
              "Return to town asks 0x69, never the Reverse's popup 0x6B (" + seen_text(*seen) +
                  ")");
        check(
            item_count(cpu, h, 130) == g_before.reverse && g_before.reverse == 1 &&
                h.meso == g_before.h.meso - g_before.h.meso / 10 && h.hp == h.max_hp,
            "the Reverse stays in the bag and the hero returns to town for a tenth of its meso (" +
                std::to_string(g_before.h.meso) + " -> " + std::to_string(h.meso) + ")");
    });
    then_finish(r);
}

// --- the four restores --------------------------------------------------------------------------
//
// systems2_cash_restores: cash_shop = off, the same start, with a synthetic receipt (the port's
// "maplethief-offline 1 <product id>") written into each of the game's four receipt lists before
// the boot, as a session killed between a purchase and its grant leaves them. The fake server
// answers restores of synthetic receipts in every mode (ADR 0072), so each list is restored by the
// game's own flow (docs/re/cash-shop.md, Restores), free: totalrestore (exppack1) in the field
// (popup 0x267, op 4 -> op 5, 0x269), crestore (InitializeSP) on the Cash button (0x25D, op 2 ->
// op 3, 0x25E), riprestore1 (powpackage) on Revival (0x262) and riprestore2 (ComebackPack) on
// Grade revival (0x263). Each list ends empty, no meso is taken. The two revivals grant as a
// purchase does; the other two are original bugs 17 and 18 (docs/re/original-bugs.md): crestore
// sends its one receipt twice and grants it twice, and totalrestore's op 5 grants no EXP pack
// (the list emptied, "Restoration Complete!", then the shop opens).
int systems2_cash_restores_setup(Run &r) {
    const auto write_list = [&r](const char *name, const char *product) {
        char text[200];
        const std::string id = std::string(MT_CASH_PRODUCT_PREFIX) + product;
        const u32 n = mt_stubs_receipt_text(id.c_str(), text, sizeof text);
        std::string bytes;
        bytes += '\1'; // i16 count
        bytes += '\0';
        bytes += '\0'; // flag
        bytes += static_cast<char>(n & 0xFF);
        bytes += static_cast<char>(n >> 8);
        bytes.append(text, n);
        std::ofstream(r.data / "saves" / name, std::ios::binary) << bytes;
    };
    write_list("totalrestore", "exppack1");
    write_list("crestore", "InitializeSP");
    write_list("riprestore1", "powpackage");
    write_list("riprestore2", "ComebackPack");
    return 0;
}

void systems2_cash_restores(Run &r, u64) {
    mt_cash_shop_install(MT_CASH_SHOP_OFF);
    // The game asks for totalrestore's restore (popup 0x267) as soon as the field shows.
    from_save(r, [](mt_cpu *cpu) { return field(cpu) || popup_id(cpu) == 0x267; });
    auto seen = std::make_shared<std::set<i32>>();
    // totalrestore: CStateGame::Paint finds it in the field.
    then_do(r, "before the restores", [&r](mt_cpu *cpu) {
        remember(r, cpu, MT_CASH_PRODUCT_COUNT, 129);
        g_before.server = mt_stubs_server_stats{};
        check(receipts(r, "totalrestore") == 1 && receipts(r, "crestore") == 1 &&
                  receipts(r, "riprestore1") == 1 && receipts(r, "riprestore2") == 1,
              "one synthetic receipt in each of the four lists");
    });
    then_answer(
        r, "totalrestore restored",
        [seen](mt_cpu *cpu) {
            return seen->count(0x269) == 1 && popup_id(cpu) == -1 && game_mode(cpu) != 0;
        },
        seen, A, false);
    then_do(r, "after totalrestore", [&r](mt_cpu *cpu) {
        std::printf("totalrestore: game mode %d panel %d, list %d; %s\n", game_mode(cpu),
                    static_cast<int>(mt_input_panel()), receipts(r, "totalrestore"),
                    hero_text(cpu, hero(cpu)).c_str());
        check(game_mode(cpu) == 11, "totalrestore: the cash shop opens after 0x269");
    });
    then(r, "the shop closed", 20, [&r](mt_cpu *cpu, double el) {
        hold(r, B, !field(cpu) && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && popup_id(cpu) == -1 && el > 1.0;
    });
    then_do(r, "totalrestore: the list emptied, no EXP", [&r, seen](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("totalrestore: list %d; %s\n", receipts(r, "totalrestore"),
                    hero_text(cpu, h).c_str());
        check(seen->count(0x267) == 1 && seen->count(0x269) == 1,
              "totalrestore: popups 0x267 and 0x269 (" + seen_text(*seen) + ")");
        check(receipts(r, "totalrestore") == 0, "totalrestore: the list is empty");
        check(h.level == g_before.h.level && h.exp == g_before.h.exp && h.meso == g_before.h.meso,
              "original bug 18: totalrestore's op 5 grants no EXP Explosion 40 % (" +
                  hero_text(cpu, h) + ")");
        check(logged(r, g_before.log,
                     "fake server: receipt (op 4) for " MT_CASH_PRODUCT_PREFIX "exppack1"),
              "totalrestore: the receipt went as op 4");
        seen->clear();
        remember(r, cpu, MT_CASH_PRODUCT_COUNT, 129);
    });
    // crestore: the Cash button.
    then_trigger(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    then_answer(
        r, "crestore restored", [&r](mt_cpu *) { return receipts(r, "crestore") == 0; }, seen, A,
        false);
    then_do(r, "crestore: Initialize SP granted", [&r, seen](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(seen->count(0x25D) == 1 && seen->count(0x25E) == 1,
              "crestore: popups 0x25D and 0x25E (" + seen_text(*seen) + ")");
        check(receipts(r, "crestore") == 0, "crestore: the list is empty");
        check(count_logged(r, g_before.log,
                           "fake server: receipt (op 2) for " MT_CASH_PRODUCT_PREFIX
                           "InitializeSP") == 2,
              "original bug 17: crestore sends its one receipt twice (op 2)");
        check(item_count(cpu, h, 129) == g_before.item + 2 && h.meso == g_before.h.meso,
              "original bug 17: two Initialize SP for one receipt, free (" +
                  std::to_string(g_before.item) + " -> " + std::to_string(item_count(cpu, h, 129)) +
                  "; " + hero_text(cpu, h) + ")");
        seen->clear();
    });
    then(r, "the shop closed", 20, [&r](mt_cpu *cpu, double el) {
        hold(r, B, !field(cpu) && std::fmod(el, 1.0) < 0.1);
        return field(cpu) && popup_id(cpu) == -1 && el > 1.0;
    });
    // riprestore1 and riprestore2: the two revive buttons of the death popup.
    for (const auto &[name, b, a, e] :
         {std::tuple{"riprestore1", X, 0x262, 0}, std::tuple{"riprestore2", Y, 0x263, 40}}) {
        const std::string list = name;
        const SDL_GamepadButton button = b;
        const i32 ask = a, exp = e;
        then_do(r, list + ": the hero falls", [&r](mt_cpu *cpu) {
            remember(r, cpu, MT_CASH_PRODUCT_COUNT, -1);
            fall(cpu);
        });
        then_wait_for(
            r, list + ": the death popup", 20, [](mt_cpu *cpu) { return popup_id(cpu) == 0x68; },
            1.0);
        then_answer(
            r, list + " restored",
            [&r, list](mt_cpu *cpu) { return receipts(r, list.c_str()) == 0 && hero(cpu).hp > 0; },
            seen, button);
        then_do(r, list + ": revived", [list, ask, exp, seen](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            check(seen->count(ask) == 1, list + ": the restore's popup (" + seen_text(*seen) + ")");
            check(h.hp == h.max_hp && h.on(g_before.h.area, g_before.h.map) &&
                      item_count(cpu, h, 10) == g_before.elixirs + 30 && h.meso == g_before.h.meso,
                  list + ": revived in place with 30 Power Elixirs, free (" + hero_text(cpu, h) +
                      ")");
            if (exp > 0) {
                check_exp_gain(cpu, exp, list + " (ComebackPack)");
            }
            seen->clear();
        });
    }
    then_do(r, "the server's count", [](mt_cpu *) {
        const mt_stubs_server_stats sv = mt_stubs_get_server_stats();
        check(sv.accepted >= 4 && sv.confirmed == 5 && sv.rejected == 0,
              "the fake server confirmed 5 receipts, crestore's twice (" +
                  std::to_string(sv.accepted) + " connects accepted, " +
                  std::to_string(sv.confirmed) + " confirmed)");
    });
    then_finish(r);
}

void systems2_cash_checks(Run &, u64) { mt_cash_shop_uninstall(); }

// --- the upgrade scrolls ------------------------------------------------------------------------
//
// A level-30 Bandit on 0.0 wearing five fixed-stat items (hat 4, weapon 28, overall 16,
// earrings 64, shoes 53), each with 2 upgrade slots, and the 10 scrolls in the bag. Each scroll
// is used by the pad from the item tab (A on its cell, A on "use", Yes on the question 0x65), and
// the result (popup 0x66, CDataMgr +0x109) and the worn item are checked against
// docs/re/mechanics.md: the target slot (ScrollTargetSlot 0x691E5), one upgrade slot used, the
// chance from the successes so far (fixed stats: 100, 75, 50, 25, 10, 0, 0 %; type 8 always),
// and on success the scroll's six item_etc stats added to the item (+0xD8 ATT ... +0xEC MP) and
// to the hero.
//   systems2_scrolls_success: no success yet: every scroll succeeds (100 %).
//   systems2_scrolls_failure: 5 successes each: the five type-3 scrolls fail (0 %), the five
//     type-8 ones still succeed; then the item tab refuses a scroll on an item with no slot left
//     (popup 0x67), and its sub-list offers no quick slot (CHero::KeyAttack would use a scroll
//     there with none of the checks).
struct ScrollRow {
    i32 type = 0;
    std::array<i32, 6> stats{}; // ATT, DEF, ACC, AVOID, HP, MP
};
std::map<i32, ScrollRow> scroll_rows() {
    std::map<i32, ScrollRow> out;
    const std::vector<std::uint8_t> etc =
        world_map::read_bytes(fs::path(MT_BUNDLE_DIR) / "English" / "data" / "item_etc.dat");
    const std::size_t stride =
        etc.size() >= 6 ? static_cast<std::size_t>(etc[4] | (etc[5] << 8)) : 0;
    const auto i16 = [&etc](std::size_t at) {
        return static_cast<i32>(static_cast<std::int16_t>(etc[at] | (etc[at + 1] << 8)));
    };
    for (const i32 id : {19, 20, 21, 22, 23, 133, 134, 135, 136, 137}) {
        const std::size_t at = 6 + stride * static_cast<std::size_t>(id);
        if (stride == 0 || at + 0x4E > etc.size() || etc[at] != id) {
            continue;
        }
        ScrollRow s;
        s.type = etc[at + 0x41];
        s.stats = {i16(at + 0x46), i16(at + 0x48), i16(at + 0x4A),
                   i16(at + 0x4C), i16(at + 0x42), i16(at + 0x44)};
        out[id] = s;
    }
    return out;
}

// ScrollTargetSlot 0x691E5: the worn slot (0 hat, 1 weapon, 2 overall, 3 earrings, 4 shoes).
i32 scroll_slot(i32 id) {
    switch (id) {
    case 19:
    case 134:
        return 1;
    case 20:
    case 133:
        return 2;
    case 21:
    case 137:
        return 0;
    case 22:
    case 136:
        return 4;
    default:
        return 3;
    }
}

struct Worn {
    i32 id = -1, rolled = 0, slots = 0, successes = 0;
    std::array<i32, 6> stats{};
};
Worn worn_item(mt_cpu *cpu, i32 slot) {
    Worn w;
    const Hero h = hero(cpu);
    const u32 eq = h.addr != 0 ? rd(cpu, h.addr + 0xDC) : 0;
    const u32 it = eq != 0 ? rd(cpu, eq + 4 * static_cast<u32>(slot)) : 0;
    if (it != 0) {
        w.id = rdi(cpu, it + 0x90);
        w.rolled = rdi(cpu, it + 0xD4);
        w.slots = rdi(cpu, it + 0xCC);
        w.successes = rdi(cpu, it + 0xD0);
        for (u32 i = 0; i < 6; i++) {
            w.stats[i] = rdi(cpu, it + 0xD8 + 4 * i);
        }
    }
    return w;
}

// CItem::ApplyScroll 0x69511's chance for the item as it is.
i32 scroll_chance(const Worn &w, i32 type) {
    constexpr i32 fixed[7] = {100, 75, 50, 25, 10, 0, 0};
    constexpr i32 rolled[7] = {100, 80, 60, 40, 30, 20, 10};
    if (type == 8) {
        return 100;
    }
    const i32 n = std::clamp(w.successes, 0, 6);
    return w.rolled != 0 ? rolled[n] : fixed[n];
}

std::string worn_text(const Worn &w) {
    std::string s = "item " + std::to_string(w.id) + ", " + std::to_string(w.slots) + " slots, " +
                    std::to_string(w.successes) + " successes, stats";
    for (const i32 v : w.stats) {
        s += " " + std::to_string(v);
    }
    return s;
}

struct ScrollRun {
    std::map<i32, ScrollRow> rows;
    Worn before{};
    std::array<i32, 7> hero_before{};
    i32 count = 0;
    std::set<i32> popups;
    i32 result = -1; // CDataMgr +0x109 when popup 0x66 opened
    u64 successes = 0, failures = 0, refusals = 0;
    i32 sub_rows = 0; // the rows of the scroll's sub-list, when a run asked for row 2
};
std::shared_ptr<ScrollRun> g_scrolls;

// The hero's etc item `id`: its cell in the item tab's list (the carried items in order).
i32 etc_cell(mt_cpu *cpu, i32 id) {
    const Hero h = hero(cpu);
    const u32 inv = h.addr != 0 ? rd(cpu, h.addr + 0xD8) : 0;
    i32 cell = 0;
    for (u32 i = 0; inv != 0 && i < 48; i++) {
        const u32 it = rd(cpu, inv + 4 * i);
        if (it != 0 && rdi(cpu, it + 0x90) == id) {
            return cell;
        }
        cell += it != 0 ? 1 : 0;
    }
    return -1;
}

// Scroll `id` from the item tab by the pad: Start, right to the item tab (1), A opens its list,
// left / right to the scroll's cell, A opens its sub-list (CDataMgr +0xBC; the row +0xC8), and
// A on row `entry`: 0 uses it (the game asks 0x65, Yes), 2 puts it on a quick slot (the slot
// +0xD0, left / right, A); a sub-list that wraps back to row 0 before row `entry` ends the
// task with its row count in g_scrolls->sub_rows. Every popup is answered with A and
// remembered; B to the field.
void then_item_tab(Run &r, std::string what, i32 id, i32 entry, u32 quick = 1) {
    struct S {
        double pressed = -10;
        bool acted = false, used = false;
        i32 last_row = -1; // the sub-list's row before the last down
    };
    auto s = std::make_shared<S>();
    then(r, "item tab: " + what, 60, [&r, s, id, entry, quick](mt_cpu *cpu, double el) {
        const auto press = [&](SDL_GamepadButton b) {
            s->pressed = el;
            hold(r, b, true);
            if (quest_bot::trace_on()) {
                const u32 dm = data_mgr(cpu);
                std::printf("item tab: %s: mode %d tab %d sub %d list %d cursor %d row %d slot "
                            "choice %d popup %d\n",
                            SDL_GetGamepadStringForButton(b), game_mode(cpu), menu_tab(cpu),
                            menu_sub(cpu), cpu->mem[dm + 0xBC], rdi(cpu, dm + 0xD8),
                            rdi(cpu, dm + 0xC8), cpu->mem[dm + 0xCC], popup_id(cpu));
            }
        };
        if (el - s->pressed >= 0.1) {
            release_all(r);
        }
        if (el - s->pressed < 0.5) {
            return false;
        }
        const u32 m = data_mgr(cpu);
        const i32 p = popup_id(cpu);
        if (p != -1) {
            g_scrolls->popups.insert(p);
            if (p == 0x66 && g_scrolls->result < 0) {
                g_scrolls->result = cpu->mem[m + 0x109];
            }
            s->acted = s->acted || p != 0x65;
            s->used = s->used || p == 0x65;
            press(A);
            return false;
        }
        const Hero h = hero(cpu);
        const bool slotted =
            rdi(cpu, h.addr + 0xE0 + 8 * quick) == 1 && rdi(cpu, h.addr + 0xE4 + 8 * quick) == id;
        s->acted = s->acted || (entry == 2 && slotted);
        if (s->used && !s->acted) {
            if (quest_bot::trace_on() && std::fmod(el, 1.0) < 0.02) {
                const Worn w = worn_item(cpu, 1);
                std::printf("item tab: waiting: mode %d popup %d applying %d result %d hero "
                            "effect %d; %s\n",
                            game_mode(cpu), popup_id(cpu), cpu->mem[m + 0x108], cpu->mem[m + 0x109],
                            rdi(cpu, h.addr + 0x138), worn_text(w).c_str());
            }
            return false; // the scroll's effect plays; CHero::Flush ends it with popup 0x66
        }
        if (field(cpu)) {
            if (s->acted) {
                return true;
            }
            press(START);
            return false;
        }
        if (game_mode(cpu) != 1) {
            return false;
        }
        if (s->acted) {
            press(B);
            return false;
        }
        if (menu_tab(cpu) != 1) {
            press(menu_sub(cpu) != 0 ? B : menu_tab(cpu) < 1 ? RIGHT : LEFT);
            return false;
        }
        if (menu_sub(cpu) == 0) {
            press(A);
            return false;
        }
        if (cpu->mem[m + 0xCC] != 0) { // choosing the quick slot
            const i32 slot = rdi(cpu, m + 0xD0);
            press(slot < static_cast<i32>(quick)   ? RIGHT
                  : slot > static_cast<i32>(quick) ? LEFT
                                                   : A);
            return false;
        }
        if (cpu->mem[m + 0xBC] == 0) {
            const i32 cursor = rdi(cpu, m + 0xD8);
            const i32 cell = etc_cell(cpu, id);
            if (cell < 0) {
                s->acted = true;
                return false;
            }
            press(cursor < cell ? RIGHT : cursor > cell ? LEFT : A);
            return false;
        }
        const i32 row = rdi(cpu, m + 0xC8);
        if (s->last_row > 0 && row == 0 && row < entry) { // wrapped: no row `entry`
            g_scrolls->sub_rows = s->last_row + 1;
            s->acted = true;
            return false;
        }
        s->last_row = row < entry ? row : -1;
        press(row < entry ? DOWN : row > entry ? UP : A);
        return false;
    });
}

enum class ScrollWant { kSuccess, kFailure, kRefused };

void then_scroll(Run &r, i32 id, ScrollWant want) {
    const std::string name = "scroll " + std::to_string(id);
    then_do(r, name + ": before", [id](mt_cpu *cpu) {
        ScrollRun &k = *g_scrolls;
        const Hero h = hero(cpu);
        k.before = worn_item(cpu, scroll_slot(id));
        k.hero_before = hero_stats(cpu, h);
        k.count = item_count(cpu, h, id);
        k.popups.clear();
        k.result = -1;
    });
    then_item_tab(r, name, id, 0);
    then_do(r, name + ": after", [id, want, name](mt_cpu *cpu) {
        ScrollRun &k = *g_scrolls;
        const Hero h = hero(cpu);
        const ScrollRow &row = k.rows[id];
        const Worn w = worn_item(cpu, scroll_slot(id));
        const std::array<i32, 7> hs = hero_stats(cpu, h);
        const i32 chance = scroll_chance(k.before, row.type);
        std::printf("%s (type %d, chance %d %%): popups %s, result %d; %s -> %s\n", name.c_str(),
                    row.type, chance, seen_text(k.popups).c_str(), k.result,
                    worn_text(k.before).c_str(), worn_text(w).c_str());
        if (want == ScrollWant::kRefused) {
            k.refusals++;
            check(k.popups.count(0x67) == 1 && k.popups.count(0x65) == 0 &&
                      w.slots == k.before.slots && w.successes == k.before.successes &&
                      w.stats == k.before.stats && item_count(cpu, h, id) == k.count,
                  name + ": no upgrade slot left: refused (popup 0x67), the scroll kept");
            return;
        }
        const bool success = want == ScrollWant::kSuccess;
        check(chance == (success ? 100 : 0),
              name + ": the chance from the item's history is " + std::to_string(chance) + " %");
        check(k.popups.count(0x65) == 1 && k.popups.count(0x66) == 1 &&
                  k.result == (success ? 1 : 0),
              name + (success ? ": succeeded" : ": failed") + " (popup 0x66, result " +
                  std::to_string(k.result) + ")");
        std::array<i32, 6> stats = k.before.stats;
        for (std::size_t i = 0; success && i < 6; i++) {
            stats[i] += row.stats[i];
        }
        check(w.id == k.before.id && w.slots == k.before.slots - 1 &&
                  w.successes == k.before.successes + (success ? 1 : 0) && w.stats == stats,
              name + ": the worn item in slot " + std::to_string(scroll_slot(id)) +
                  ": one slot used" + (success ? ", the scroll's stats added" : ", unchanged"));
        // The hero's ATT, DEF, ACC, AVOID, max HP and max MP follow the item (hero_stats:
        // attack, defense, accuracy, avoid, critical, max HP, max MP).
        const std::array<i32, 6> delta = {hs[0] - k.hero_before[0], hs[1] - k.hero_before[1],
                                          hs[2] - k.hero_before[2], hs[3] - k.hero_before[3],
                                          hs[5] - k.hero_before[5], hs[6] - k.hero_before[6]};
        std::array<i32, 6> expect{};
        for (std::size_t i = 0; success && i < 6; i++) {
            expect[i] = row.stats[i];
        }
        check(delta == expect, name + ": the hero's stats changed by the scroll's");
        check(item_count(cpu, h, id) == k.count - 1, name + ": one scroll used");
        (success ? k.successes : k.failures)++;
    });
}

void scrolls_run(Run &r, bool fresh) {
    g_scrolls = std::make_shared<ScrollRun>();
    g_scrolls->rows = scroll_rows();
    from_save(r);
    then_wait_field(r);
    then_do(r, "the worn items", [fresh](mt_cpu *cpu) {
        check(g_scrolls->rows.size() == 10, "item_etc: the 10 scrolls' rows");
        for (i32 slot = 0; slot < 5; slot++) {
            const Worn w = worn_item(cpu, slot);
            check(w.slots == 2 && w.successes == (fresh ? 0 : 5) && w.rolled == 0,
                  "slot " + std::to_string(slot) + ": " + worn_text(w));
        }
    });
    for (const i32 id : {19, 20, 21, 22, 23}) {
        then_scroll(r, id, fresh ? ScrollWant::kSuccess : ScrollWant::kFailure);
    }
    for (const i32 id : {133, 134, 135, 136, 137}) {
        then_scroll(r, id, ScrollWant::kSuccess);
    }
    if (fresh) {
        then_finish(r);
        return;
    }
    // No slot left: the item tab refuses both kinds.
    for (const i32 id : {19, 133}) {
        then_scroll(r, id, ScrollWant::kRefused);
    }
    // A scroll on quick slot 1 would reach CHero::KeyAttack, which calls UseItem with none of the
    // item tab's checks; but the scroll's sub-list has no row 2 (assign a quick slot).
    then_item_tab(r, "Weapon Enchantment Scroll to a quick slot", 19, 2, 1);
    then_do(r, "no quick slot for a scroll", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("scroll sub-list: %d rows; quick slot 1 kind %d item %d\n", g_scrolls->sub_rows,
                    rdi(cpu, h.addr + 0xE8), rdi(cpu, h.addr + 0xEC));
        check(g_scrolls->sub_rows == 2,
              "a scroll's sub-list has two rows (use, throw away): no quick slot");
        check(rdi(cpu, h.addr + 0xE8) != 1 || rdi(cpu, h.addr + 0xEC) != 19,
              "quick slot 1 does not hold the scroll");
    });
    then_finish(r);
}

void systems2_scrolls_success(Run &r, u64) { scrolls_run(r, true); }
void systems2_scrolls_failure(Run &r, u64) { scrolls_run(r, false); }
void systems2_scrolls_checks(Run &, u64) {
    const ScrollRun &k = *g_scrolls;
    std::printf("scrolls: %llu succeeded, %llu failed, %llu refused\n",
                static_cast<unsigned long long>(k.successes),
                static_cast<unsigned long long>(k.failures),
                static_cast<unsigned long long>(k.refusals));
}

// --- the Pigmy dialog ---------------------------------------------------------------------------
//
// systems2_pigmy: no shipped map places the Pigmy NPCs (obj kinds 40 and 41), so the test does
// what the object handler 0x63191 does for kind 40 (CStateGame +0xAC = 36, +0xCC = 0) and calls
// the game's CStateGame::SetPigmeState(state, 1) 0x823A1. A level-30 Rogue with a Pink Pigmy Egg
// (item_etc 91) walks the dialog by the pad (state CStateGame +0xC8):
//   1. Row 1 of state 1 (hatch: the egg is there, the bag has room) leads to state 6, Accept /
//      Refuse. Its OK goes on only with the chat box's row +0x30 at 0, but the box copies
//      questSelector there, still 1 from the list's row, and nothing in Accept / Refuse sets it:
//      Accept closes the dialog as Refuse (the Pigmy is unreachable content,
//      docs/re/original-bugs.md). No connect.
//   2. The test then calls SetPigmeState(state, 7), the state Accept would reach: Accept (row 0
//      after the close) goes to 0xF ("Connecting to network."), where the game connects
//      CMSocket. The stub refuses at once (no synthetic receipt waits; network.md), so the state
//      goes through 0x10 (the failure) to 0x11 within a tick, OK closes it: one connect, none
//      accepted, no frame.
// The egg stays in the bag.
struct PigmyRun {
    std::array<std::vector<i32>, 2> states;
    std::array<mt_stubs_server_stats, 3> stats{}; // before each phase and at the end
    i32 eggs = 0;
};
std::shared_ptr<PigmyRun> g_pigmy;

i32 pigme_state(const mt_cpu *cpu) {
    const Hero h = hero(cpu);
    return h.world != 0 ? rdi(cpu, h.world + 0xC8) : -1;
}

std::string pigmy_states(const std::vector<i32> &states) {
    std::string out;
    for (const i32 st : states) {
        char b[16];
        std::snprintf(b, sizeof b, " 0x%X", static_cast<unsigned>(st));
        out += b;
    }
    return out;
}

// SetPigmeState(state, `first`), then the pad through the dialog until the field shows: row 1 (the
// hatch) in a list, OK on everything else.
void then_pigmy_dialog(Run &r, std::size_t phase, i32 first) {
    then_do(r, "the Pigmy dialog (SetPigmeState " + std::to_string(first) + ")",
            [phase, first](mt_cpu *cpu) {
                const Hero h = hero(cpu);
                g_pigmy->stats[phase] = mt_stubs_get_server_stats();
                wr(cpu, h.world + 0xAC, 36);
                wr(cpu, h.world + 0xCC, 0);
                call(cpu, 0x823A1u, {h.world, static_cast<u32>(first)});
            });
    auto pressed = std::make_shared<double>(-10);
    then(r, "the Pigmy dialog to the field", 120, [&r, pressed, phase](mt_cpu *cpu, double el) {
        const i32 st = pigme_state(cpu);
        std::vector<i32> &states = g_pigmy->states[phase];
        if (states.empty() || states.back() != st) {
            states.push_back(st);
        }
        if (el - *pressed >= 0.1) {
            release_all(r);
        }
        if (el - *pressed < 0.8) {
            return false;
        }
        if (field(cpu) && !quest_bot::chatting(cpu)) {
            return el > 2.0;
        }
        const quest_bot::Chat c = quest_bot::chat(cpu);
        std::printf("pigmy: state 0x%X, chat mode %d page %d of %d, row %d, popup %d\n",
                    static_cast<unsigned>(st), c.mode, c.page, c.pages, c.sel, popup_id(cpu));
        *pressed = el;
        if (popup_id(cpu) == -1 && c.mode == 2 && c.sel != 1) {
            hold(r, c.sel < 1 ? DOWN : UP, true);
            return false;
        }
        hold(r, A, true);
        return false;
    });
}

void systems2_pigmy(Run &r, u64) {
    g_pigmy = std::make_shared<PigmyRun>();
    from_save(r);
    then_wait_field(r);
    then_do(r, "the egg", [](mt_cpu *cpu) { g_pigmy->eggs = item_count(cpu, hero(cpu), 91); });
    then_pigmy_dialog(r, 0, 1);
    then_pigmy_dialog(r, 1, 7);
    then_do(r, "the Pigmy dialog left", [](mt_cpu *cpu) {
        PigmyRun &k = *g_pigmy;
        k.stats[2] = mt_stubs_get_server_stats();
        for (std::size_t i = 0; i < 2; i++) {
            std::printf("pigmy %zu: states%s; connects %u accepted %u frames %u\n", i + 1,
                        pigmy_states(k.states[i]).c_str(),
                        k.stats[i + 1].connects - k.stats[i].connects,
                        k.stats[i + 1].accepted - k.stats[i].accepted,
                        k.stats[i + 1].frames - k.stats[i].frames);
        }
        const auto has = [&k](std::size_t i, i32 st) {
            return std::find(k.states[i].begin(), k.states[i].end(), st) != k.states[i].end();
        };
        check(has(0, 1) && has(0, 6) && !has(0, 7) && !has(0, 0xF),
              "the hatch's Accept (state 6) closes as Refuse, no state 7:" +
                  pigmy_states(k.states[0]));
        check(k.stats[1].connects == k.stats[0].connects && k.stats[1].frames == k.stats[0].frames,
              "the first dialog connected nowhere");
        check(has(1, 7) && has(1, 0xF) && has(1, 0x11),
              "from state 7: 0xF (connecting) -> 0x11 (through 0x10, the failure):" +
                  pigmy_states(k.states[1]));
        check(k.stats[2].connects == k.stats[1].connects + 1 &&
                  k.stats[2].accepted == k.stats[1].accepted &&
                  k.stats[2].frames == k.stats[1].frames,
              "the network stub: one connect, refused, no frame sent");
        check(item_count(cpu, hero(cpu), 91) == k.eggs && k.eggs == 1,
              "the Pink Pigmy Egg is still in the bag");
        check(field(cpu) && !quest_bot::chatting(cpu),
              "the dialog is closed, the hero in the field");
    });
    then_finish(r);
}
void systems2_pigmy_checks(Run &, u64) {}

// --- portal620 ----------------------------------------------------------------------------------
//
// systems2_portal620: a level-60 Bandit of chapter 2 on 6.2 walks to its only portal, portal620,
// and takes it. common/portal620 warps to (6, 1, 2), and 6.1 has no portal of kind 2, so the map
// change 0x868D5 writes no position: the hero lands on 6.1 at the coordinates it had on 6.2, not
// at portal611 (docs/re/original-bugs.md, bug 14). The run reads the first position on 6.1.
// systems2_portal620_fixed: the same with fix_original_bugs on (#388): the warp is (6, 1, 1) and
// the hero lands at portal611.
struct PortalRun {
    i32 x62 = 0, y62 = 0, x61 = 0, y61 = 0;
    bool landed = false;
};
std::shared_ptr<PortalRun> g_portal;

void portal620_run(Run &r, bool fixed) {
    r.settings.fix_original_bugs = fixed;
    g_portal = std::make_shared<PortalRun>();
    from_save(r);
    then_wait_field(r);
    quest_bot::then_go(r, "portal620 on 6.2", quest_bot::at_object(6, 2, "portal620", 4), 120);
    then(r, "portal620", 30, [&r](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (h.on(6, 2)) {
            g_portal->x62 = h.x;
            g_portal->y62 = h.y;
            hold(r, UP, std::fmod(el, 1.5) < 0.4);
            return false;
        }
        hold(r, UP, false);
        if (h.on(6, 1) && !g_portal->landed) {
            g_portal->landed = true;
            g_portal->x61 = h.x;
            g_portal->y61 = h.y;
        }
        return g_portal->landed;
    });
    then_wait_field(r);
    then_do(r, "where the hero landed on 6.1", [fixed](mt_cpu *cpu) {
        const PortalRun &k = *g_portal;
        const world_map::Object *p611 = world_map::get(6, 1).object("portal611");
        const world_map::Object *p610 = world_map::get(6, 1).object("portal610");
        std::printf("portal620 (fix_original_bugs %s): left 6.2 at (%d, %d), landed on 6.1 at "
                    "(%d, %d); portal610 (%d, %d), portal611 (%d, %d); now %s\n",
                    fixed ? "on" : "off", k.x62, k.y62, k.x61, k.y61,
                    p610 != nullptr ? p610->x : -1, p610 != nullptr ? p610->y : -1,
                    p611 != nullptr ? p611->x : -1, p611 != nullptr ? p611->y : -1,
                    where(cpu).c_str());
        check(k.landed, "portal620 leads to 6.1");
        check(p611 != nullptr, "6.1 has portal611");
        const i32 dx = p611 != nullptr ? std::abs(k.x61 - p611->x) : 0;
        if (fixed) {
            check(dx <= 40 && !(k.x61 == k.x62 && k.y61 == k.y62),
                  "bug 14 fixed: the hero lands at portal611, the portal back to 6.2 (" +
                      std::to_string(dx) + " points away)");
        } else {
            check(k.x61 == k.x62 && k.y61 == k.y62,
                  "original bug 14: the hero keeps its 6.2 coordinates on 6.1");
            check(dx > 100, "the hero is not at portal611, the portal back to 6.2");
        }
        check(hero(cpu).hp > 0 && field(cpu), "the hero is alive in the field on 6.1");
    });
    then_finish(r);
}
void systems2_portal620(Run &r, u64) { portal620_run(r, false); }
void systems2_portal620_fixed(Run &r, u64) { portal620_run(r, true); }
void systems2_portal620_checks(Run &, u64) {}

// --- a boss killed while the hero is knocked back (original bug 19, #420) ---------------------
//
// systems2_boss_knockback: a level-70 Bandit of chapter 2 on 9.5, mob 68's map (no map script,
// the boss there from the first tick). The run keeps mob 68 at 1 HP and the bot kills it; when
// CMob::Die 0x73349 has queued the end scene (game mode 10), before CStateGame::Paint starts it
// (StartScene 0x853E5), the run sets the hero's knock-back bytes CHero +0x119 and +0x108, as a
// blow that landed mid knock-back leaves them. Then A every 0.6 s. With fix_original_bugs off the
// scene's box takes no key: still game mode 9, +0x119 still set, after 60 s of keys
// (docs/re/original-bugs.md, bug 19). systems2_boss_knockback_fixed: the same with it on: the
// hook on StartScene clears both bytes and the scene plays through to the field.
struct BossKnockRun {
    bool forced = false;
    double scene_at = -1;
};
std::shared_ptr<BossKnockRun> g_boss_knock;

void boss_knockback_run(Run &r, bool fixed) {
    using namespace quest_bot;
    r.settings.fix_original_bugs = fixed;
    g_boss_knock = std::make_shared<BossKnockRun>();
    from_save(r);
    then_wait_field(r);
    // One hit kills it: its HP held at 1 while the field runs (the scene queued, nothing more).
    // Each turn, after the run's own monitor.
    auto monitor_turn = r.on_turn;
    r.on_turn = [monitor_turn](mt_cpu *cpu) {
        monitor_turn(cpu);
        if (game_mode(cpu) == 0) {
            for (const Mob &m : mobs(cpu)) {
                if (m.id == 68 && m.hp > 1) {
                    std::uint32_t one = 1;
                    std::memcpy(cpu->mem + m.addr + 0x88, &one, 4);
                }
            }
        }
        if (game_mode(cpu) == 10 && !g_boss_knock->forced) {
            const Hero h = hero(cpu);
            cpu->mem[h.addr + 0x119] = 1;
            cpu->mem[h.addr + 0x108] = 1;
            g_boss_knock->forced = true;
            std::printf("boss knock-back: the end scene queued; CHero +0x119 and +0x108 set\n");
        }
    };
    FarmStyle boss;
    boss.restock = {};
    then_farm(
        r, "mob 68 on 9.5 (held at 1 HP)", 9, 5, {68},
        [](mt_cpu *cpu) { return game_mode(cpu) == 9 || game_mode(cpu) == 10; }, 300, boss);
    then(r, "the end scene starts", 10,
         [](mt_cpu *cpu, double) { return game_mode(cpu) == 9 && g_boss_knock->forced; });
    if (fixed) {
        then(r, "A through the end scene to the field", 600, [&r](mt_cpu *cpu, double el) {
            if (g_boss_knock->scene_at < 0) {
                g_boss_knock->scene_at = el;
                check(!hero(cpu).knocked, "bug 19 fixed: the scene starts with CHero +0x119 clear");
            }
            hold(r, A, std::fmod(el, 0.6) < 0.1);
            if (field(cpu)) {
                hold(r, A, false);
                std::printf("boss knock-back (fix_original_bugs on): the end scene played through "
                            "in %.1f s; now %s\n",
                            el, where(cpu).c_str());
                return true;
            }
            return false;
        });
    } else {
        then(r, "A for 60 s in the end scene", 70, [&r](mt_cpu *cpu, double el) {
            hold(r, A, std::fmod(el, 0.6) < 0.1);
            if (game_mode(cpu) != 9 || !hero(cpu).knocked) {
                hold(r, A, false);
                check(false, "original bug 19: the end scene left game mode 9 with the hero "
                             "knocked back (" +
                                 where(cpu) + ")");
                return true;
            }
            if (el < 60) {
                return false;
            }
            hold(r, A, false);
            std::printf("boss knock-back (fix_original_bugs off): after 60 s of A still %s\n",
                        where(cpu).c_str());
            check(true, "original bug 19: game mode 9 and CHero +0x119 set after 60 s of keys");
            return true;
        });
    }
    then_finish(r);
}
void systems2_boss_knockback(Run &r, u64) { boss_knockback_run(r, false); }
void systems2_boss_knockback_fixed(Run &r, u64) { boss_knockback_run(r, true); }
void systems2_boss_knockback_checks(Run &, u64) {}

// --- shops 0, 1 and 16 from an existing merchant (restored_content, #426) ------------------------
//
// docs/re/original-bugs.md "Unreachable content": with restored_content on, Etc00 (0.0, shop 5),
// Equip90 and Etc90 (9.0, shops 8 and 13) answer the talk with a list box of two rows, their usual
// shop and shop 16, 0 or 1 (mt_restored_shop_for). The bot picks each row in turn: the shop that
// opens has exactly market.dat's listings for that shop, in the file's order, at its prices.
// With it off the talk opens the usual shop at once: no chat box at any turn, no list box built.
// systems2_restored_shops_0 / _9 run on 0.0 / 9.0 (the starts restored_shops_0_0 / _9_0);
// systems2_restored_shops_0_off / _9_off the same with the setting off.

// market.dat's listings of `shop` in the file's order: (table, item, price); the all-zero rows
// (unused) are not listings. The file: a 6-byte header, then 7-byte rows
// (docs/formats/data-tables.md).
struct MarketListing {
    i32 table, item, price;
};
std::vector<MarketListing> market_listings(i32 shop) {
    const std::vector<std::uint8_t> raw =
        read_file((fs::path(MT_BUNDLE_DIR) / "English/data/market.dat").string());
    std::vector<MarketListing> out;
    for (std::size_t at = 6; at + 7 <= raw.size(); at += 7) {
        i32 price = 0;
        std::memcpy(&price, raw.data() + at + 3, 4);
        const bool unused = raw[at] == 0 && raw[at + 1] == 0 && raw[at + 2] == 0 && price == 0;
        if (!unused && raw[at] == shop) {
            out.push_back({raw[at + 1], raw[at + 2], price});
        }
    }
    return out;
}

struct ShopVisit {
    quest_bot::Nav nav;
    double pressed = -10;
    bool chatted = false;      // a chat box was seen during the talk
    std::vector<i32> values;   // the list box's row values, as seen on its last page
    Shop seen;                 // the shop as it opened
    std::vector<ShopRow> rows; // its Buy rows
};

// Talks to merchant `npc` on (area, map); on a list box picks the row of value `choose`; done in
// the shop, which it records, then B back to the field.
void then_shop_from(Run &r, const std::string &what, int area, int map, const std::string &npc,
                    i32 choose, std::shared_ptr<ShopVisit> v) {
    using namespace quest_bot;
    const Goal g = at_object(area, map, npc, 6);
    then(r, "shop: " + what, 240, [&r, g, choose, v](mt_cpu *cpu, double el) {
        const auto press = [&](SDL_GamepadButton b) {
            if (el - v->pressed >= 0.6) {
                v->pressed = el;
                hold(r, b, true);
            }
        };
        if (el - v->pressed >= 0.1) {
            release_all(r);
        }
        if (in_shop(cpu)) {
            v->seen = shop(cpu);
            v->rows.clear();
            for (i32 i = 0; i < v->seen.rows; i++) {
                v->rows.push_back(shop_row(cpu, v->seen, i));
            }
            release_all(r);
            return true;
        }
        if (chatting(cpu)) {
            v->chatted = true;
            const Chat c = chat(cpu);
            if (c.mode == 2 && c.last_page() && !c.values.empty()) {
                v->values = c.values;
                i32 want = 0;
                for (std::size_t i = 0; i < c.values.size(); i++) {
                    want = c.values[i] == choose ? static_cast<i32>(i) : want;
                }
                press(c.sel < want ? DOWN : (c.sel > want ? UP : A));
            } else {
                press(A);
            }
            return false;
        }
        if (game_mode(cpu) == 2 || !field(cpu)) {
            press(A); // a popup over the shop or the field
            return false;
        }
        if (drive(r, cpu, el, v->nav, g)) {
            press(UP);
        }
        return false;
    });
    then(r, "B closes the shop: " + what, 20, [&r](mt_cpu *cpu, double el) {
        hold(r, B, std::fmod(el, 0.6) < 0.1);
        if (field(cpu)) {
            release_all(r);
            return true;
        }
        return false;
    });
}

// The visit's shop against market.dat's listings of `shop`, and with restored_content (`on`) the
// Monster Carnival ticket the town's etc merchant sells after them (#427: an unused row, the
// file's last ones).
void check_visit(const ShopVisit &v, i32 shop, const std::string &what, bool on = false) {
    std::vector<MarketListing> want = market_listings(shop);
    for (int area : {0, 4, 6}) {
        const mt_restored_carnival_npc *npc = mt_restored_carnival_npc_on(area, 0);
        if (on && npc != nullptr && npc->shop == shop) {
            want.push_back({1, npc->ticket, npc->price});
        }
    }
    bool same = v.rows.size() == want.size();
    for (std::size_t i = 0; same && i < want.size(); i++) {
        same = v.rows[i].id == want[i].item && v.rows[i].price == want[i].price;
    }
    std::printf("%s: shop of %d Buy rows (market.dat lists %zu for shop %d), kind %d, rows %s\n",
                what.c_str(), v.seen.rows, want.size(), shop, v.seen.kind,
                same ? "as listed" : "differ");
    check(v.seen.rows == static_cast<i32>(want.size()) && !want.empty(),
          what + ": the shop has market.dat's " + std::to_string(want.size()) +
              " listings of shop " + std::to_string(shop));
    check(same, what + ": its rows are the listings, in order, at their prices");
    check(v.seen.kind == (want.empty() || want[0].table == 0 ? 0 : 1),
          what + ": the Buy tab sells from the listings' table");
}

void restored_shops_run(Run &r, int area, bool on) {
    using namespace quest_bot;
    r.settings.restored_content = on;
    from_save(r);
    then_wait_field(r);
    struct Merchant {
        const char *npc;
        i32 usual, restored;
    };
    const std::vector<Merchant> merchants =
        area == 0 ? std::vector<Merchant>{{"Etc00", 5, 16}}
                  : std::vector<Merchant>{{"Equip90", 8, 0}, {"Etc90", 13, 1}};
    for (const Merchant &m : merchants) {
        const std::string npc = m.npc;
        then_check(
            r,
            npc + " offers shop " + std::to_string(m.restored) + " beside shop " +
                std::to_string(m.usual) + " on " + std::to_string(area) + ".0",
            [area, m](mt_cpu *) { return mt_restored_shop_for(area, 0, m.usual) == m.restored; });
        auto before = std::make_shared<mt_restored_shop_stats>();
        then_do(r, npc + ": the list boxes so far",
                [before](mt_cpu *) { *before = mt_hooks_restored_shop_stats(); });
        if (on) {
            auto restored = std::make_shared<ShopVisit>();
            then_shop_from(r, npc + ": the restored row", area, 0, npc, m.restored, restored);
            then_do(r, npc + ": shop " + std::to_string(m.restored), [=](mt_cpu *) {
                const std::vector<i32> rows = {m.usual, m.restored};
                check(restored->chatted && restored->values == rows,
                      npc + ": a list box of two rows, shop " + std::to_string(m.usual) +
                          " and shop " + std::to_string(m.restored));
                check_visit(*restored, m.restored, npc + ", the restored row", true);
            });
            auto usual = std::make_shared<ShopVisit>();
            then_shop_from(r, npc + ": the usual row", area, 0, npc, m.usual, usual);
            then_do(r, npc + ": shop " + std::to_string(m.usual), [=](mt_cpu *) {
                check(usual->chatted, npc + ": the list box again");
                check_visit(*usual, m.usual, npc + ", the usual row", true);
                const mt_restored_shop_stats after = mt_hooks_restored_shop_stats();
                check(after.boxes - before->boxes == 2 && after.opened - before->opened == 2,
                      npc + ": two list boxes, a shop opened from each");
            });
        } else {
            auto visit = std::make_shared<ShopVisit>();
            then_shop_from(r, npc, area, 0, npc, m.usual, visit);
            then_do(r, npc + ": shop " + std::to_string(m.usual) + " at once", [=](mt_cpu *) {
                check(!visit->chatted,
                      npc + " (restored_content off): the shop opens with no chat box");
                check_visit(*visit, m.usual, npc + " (off)");
                const mt_restored_shop_stats after = mt_hooks_restored_shop_stats();
                check(after.boxes == before->boxes && after.boxes == 0,
                      npc + " (off): no list box built");
            });
        }
    }
    then_check(r, "the hero is alive in the field",
               [](mt_cpu *cpu) { return hero(cpu).hp > 0 && field(cpu); });
    then_finish(r);
}
void systems2_restored_shops_0(Run &r, u64) { restored_shops_run(r, 0, true); }
void systems2_restored_shops_0_off(Run &r, u64) { restored_shops_run(r, 0, false); }
void systems2_restored_shops_9(Run &r, u64) { restored_shops_run(r, 9, true); }
void systems2_restored_shops_9_off(Run &r, u64) { restored_shops_run(r, 9, false); }
void systems2_restored_shops_checks(Run &, u64) {}

// --- Monster Carnival from a Spiegelman NPC (restored_content, #427) ---------------------------
//
// docs/re/original-bugs.md "Unreachable content": with restored_content on, Kerning City (0.0)
// has one object more than 00.obj, NPC 38 (kind 42) named Spiegelman1 at the table's cell
// (mt_restored_carnival_npc_on), and its etc merchant sells ticket A (item_etc 47). The hero,
// level 25 with one ticket (the start restored_carnival_0_0), talks to the NPC: its list's first
// row, then Accept; the script takes the ticket and warps to the room 0.5 with the carnival on
// (CStateGame +0x404, mode +0x40C 0). The round runs out (the bot answers the popups, nothing
// else), the result box closes on A, CStateGame::resultMonCa 0x819E5 pays the EXP of the final
// score's band, and the hero is back on 0.0. The NPC's pages are in English: the scripts' Korean
// literals shown as their ChapterN_MonsterCarnival_Spiegelman keys (no Hangul in any page). The
// score starts at 0 (the restore sets it as the room loads; the original leaves it unset).
// systems2_restored_carnival_off: the same start with the setting off: 0.0's object list is
// 00.obj's, record for record, and the merchant sells no ticket.

// CStateGame's carnival (docs/re/mechanics.md): on +0x404 (a byte), the phase +0x400, the mode
// +0x40C, the score +0x434.
struct Carnival {
    bool on = false;
    i32 phase = -1, mode = -1, score = 0;
};
Carnival carnival(const mt_cpu *cpu) {
    Carnival c;
    const u32 game = game_state(cpu);
    if (game != 0) {
        c.on = cpu->mem[game + 0x404] != 0;
        c.phase = rdi(cpu, game + 0x400);
        c.mode = rdi(cpu, game + 0x40C);
        c.score = rdi(cpu, game + 0x434);
    }
    return c;
}

// The current map's live object list (quest_bot.h's live_object: CMMap *(*0xF13C0)).
struct LiveObject {
    i32 cell = 0, kind = 0;
    std::string name;
};
std::vector<LiveObject> live_objects(const mt_cpu *cpu) {
    std::vector<LiveObject> out;
    const u32 map = rd(cpu, rd(cpu, 0xF13C0));
    const u32 objs = map != 0 ? rd(cpu, map + 0x48) : 0;
    const u32 names = map != 0 ? rd(cpu, map + 0x54) : 0;
    const i32 n = map != 0 ? std::min(rdi(cpu, map + 0x4C), 300) : 0;
    for (i32 k = 0; objs != 0 && names != 0 && k < n; k++) {
        const u32 o = rd(cpu, objs + 4 * static_cast<u32>(k));
        const u32 at = rd(cpu, names + 4 * static_cast<u32>(k));
        LiveObject l;
        l.cell = o != 0 ? rd16(cpu, o) : -1;
        l.kind = o != 0 ? rd16(cpu, o + 2) : -1;
        for (u32 i = at; at != 0 && i < cpu->mem_size && cpu->mem[i] != 0 && i < at + 255; i++) {
            l.name.push_back(static_cast<char>(cpu->mem[i]));
        }
        out.push_back(l);
    }
    return out;
}

// The objects of /map/<area><map>.obj, the map script left out (docs/formats/obj.md).
std::vector<LiveObject> file_objects(int area, int map) {
    const std::vector<std::uint8_t> raw = read_file(
        (fs::path(MT_BUNDLE_DIR) / ("map/" + std::to_string(area) + std::to_string(map) + ".obj"))
            .string());
    std::vector<LiveObject> out;
    std::size_t at = 1;
    for (std::size_t i = 0; !raw.empty() && i < raw[0] && at + 4 <= raw.size(); i++) {
        std::int16_t cell = 0;
        std::memcpy(&cell, raw.data() + at, 2);
        const std::size_t len = raw[at + 3];
        if (cell != -1) {
            out.push_back({cell, raw[at + 2] == 0xFF ? -1 : raw[at + 2],
                           std::string(raw.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                       raw.begin() + static_cast<std::ptrdiff_t>(at + 4 + len))});
        }
        at += 4 + len;
    }
    return out;
}

struct CarnivalRun {
    std::vector<std::string> pages; // the NPC's pages (NPC 38), as the text events read them
    Carnival last;                  // the carnival as last seen on
    Hero before;                    // the hero when the round ended
    i32 need = 0;                   // the EXP its level needed then
    i32 first_score = -1;           // the score when the round was first seen
    std::size_t mobs = 0;           // the most monsters on the room's map during the round
    bool started = false, ended = false;
};
CarnivalRun g_carnival;

void record_carnival_page(void *, const mt_text_event *e) {
    if ((e->kind == MT_TEXT_EVENT_PAGE || e->kind == MT_TEXT_EVENT_ANSWER) &&
        e->speaker_kind == MT_TEXT_SPEAKER_NPC && e->speaker_id == 38) {
        g_carnival.pages.emplace_back(e->text);
    }
}

// Hangul in UTF-8: U+AC00..U+D7A3 start with 0xEA..0xED.
bool has_hangul(const std::string &s) {
    for (const char c : s) {
        const auto b = static_cast<std::uint8_t>(c);
        if (b >= 0xEA && b <= 0xED) {
            return true;
        }
    }
    return false;
}

// resultMonCa's EXP for `mode` (the table at 0xEAD40; docs/re/mechanics.md): the score's band.
i32 carnival_exp(i32 mode, i32 score) {
    static const i32 bands[3][3] = {{1500, 2000, 2500}, {800, 1400, 2000}, {2000, 2500, 3000}};
    static const i32 exp[3][4] = {
        {250, 500, 1000, 1500}, {2700, 3500, 4000, 4700}, {4500, 6200, 7000, 7700}};
    i32 band = 0;
    while (band < 3 && score > bands[mode][band]) {
        band++;
    }
    return exp[mode][band];
}

// The NPC on (area, 0) and its round: mode = the script's number - 1, its room.
struct CarnivalTown {
    i32 area, mode, room_area, room_map, width;
};
constexpr CarnivalTown k_carnival_towns[] = {{0, 0, 0, 5, 40}, {4, 1, 4, 18, 45}, {6, 2, 6, 3, 57}};

void restored_carnival_run(Run &r, int area, bool on) {
    using namespace quest_bot;
    r.settings.restored_content = on;
    g_carnival = CarnivalRun{};
    from_save(r);
    then_wait_field(r);
    CarnivalTown town = k_carnival_towns[0];
    for (const CarnivalTown &t : k_carnival_towns) {
        town = t.area == area ? t : town;
    }
    const std::string where = std::to_string(area) + ".0";
    const std::string room = std::to_string(town.room_area) + "." + std::to_string(town.room_map);
    const mt_restored_carnival_npc *npc = mt_restored_carnival_npc_on(area, 0);
    const std::string script = "Spiegelman" + std::to_string(town.mode + 1);
    const i32 ticket = 47 + town.mode;
    check(npc != nullptr && npc->script == script && npc->ticket == ticket,
          where + "'s NPC is " + script + ", its ticket item_etc " + std::to_string(ticket));
    if (npc == nullptr) {
        then_finish(r);
        return;
    }
    then_do(r, where + "'s object list", [on, npc, area, where](mt_cpu *cpu) {
        const std::vector<LiveObject> live = live_objects(cpu);
        const std::vector<LiveObject> file = file_objects(area, 0);
        bool same = live.size() >= file.size();
        for (std::size_t i = 0; same && i < file.size(); i++) {
            same = live[i].cell == file[i].cell && live[i].kind == file[i].kind &&
                   live[i].name == file[i].name;
        }
        std::printf("%s: %zu live objects, its .obj has %zu\n", where.c_str(), live.size(),
                    file.size());
        check(same, where + "'s objects begin with its .obj's, record for record");
        if (on) {
            check(live.size() == file.size() + 1 && live.back().kind == 42 &&
                      live.back().cell == npc->cell && live.back().name == npc->script,
                  "restored_content: one object more, kind 42 " + std::string(npc->script) +
                      " at its cell");
        } else {
            check(live.size() == file.size(),
                  "restored_content off: " + where + "'s list is its .obj's");
        }
    });
    // The ticket in the town's etc merchant's shop (Etc00's list box's usual row with the
    // setting on).
    const std::string merchant = "Etc" + std::to_string(area) + "0";
    auto visit = std::make_shared<ShopVisit>();
    then_shop_from(r, merchant, area, 0, merchant, npc->shop, visit);
    then_do(r, merchant + ": its shop", [on, visit, npc, merchant](mt_cpu *) {
        check_visit(*visit, npc->shop, merchant + (on ? " with the ticket" : ", no ticket"), on);
    });
    if (!on) {
        then_check(r, "restored_content off: no object added, no ticket listed", [](mt_cpu *) {
            const mt_restored_carnival_stats s = mt_hooks_restored_carnival_stats();
            return s.npcs == 0 && s.tickets == 0 && s.rounds == 0;
        });
        then_finish(r);
        return;
    }
    then_do(r, "listen to the NPC's pages", [](mt_cpu *) {
        mt_text_events_install(1);
        mt_text_events_add_listener(record_carnival_page, nullptr);
    });
    const Goal g{area, 0, (npc->cell % town.width) * 16 + 8, (npc->cell / town.width + 1) * 16 - 1,
                 6};
    then_talk_to(
        r, script + ": to the carnival", g, npc->script, {1},
        [](mt_cpu *cpu) { return carnival(cpu).on; }, 120);
    then_check(r, "the carnival is on in room " + room + ", the ticket taken",
               [town, ticket](mt_cpu *cpu) {
                   const Hero h = hero(cpu);
                   const Carnival c = carnival(cpu);
                   std::printf("carnival: map %d.%d x %d y %d mode %d phase %d, tickets %d\n",
                               h.area, h.map, h.x, h.y, c.mode, c.phase,
                               item_count(cpu, h, ticket));
                   return c.on && c.mode == town.mode && h.on(town.room_area, town.room_map) &&
                          item_count(cpu, h, ticket) == 0;
               });
    then(r, "the round runs out, the result box closes", 400, [&r, area](mt_cpu *cpu, double el) {
        const Carnival c = carnival(cpu);
        const Hero h = hero(cpu);
        hold(r, A,
             (popup_id(cpu) != -1 || chatting(cpu) || game_mode(cpu) != 0) &&
                 std::fmod(el, 0.6) < 0.1);
        if (c.on) {
            if (!g_carnival.started) {
                g_carnival.first_score = c.score;
            }
            g_carnival.started = true;
            g_carnival.last = c;
            if (c.phase == 1) {
                g_carnival.mobs = std::max(g_carnival.mobs, mobs(cpu).size());
            }
            if (c.phase < 2) {
                g_carnival.before = h;
                g_carnival.need = exp_need(cpu);
            }
            return false;
        }
        if (g_carnival.started && field(cpu) && h.on(area, 0)) {
            release_all(r);
            g_carnival.ended = true;
            return true;
        }
        return false;
    });
    then_do(r, "the round's EXP", [town, where, script](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 gain = carnival_exp(town.mode, g_carnival.last.score);
        const auto [level, exp] =
            after_exp(g_carnival.before.level, g_carnival.before.exp, g_carnival.need, gain);
        std::printf("carnival: score %d, EXP %d -> level %d EXP %d (was level %d EXP %d)\n",
                    g_carnival.last.score, gain, h.level, h.exp, g_carnival.before.level,
                    g_carnival.before.exp);
        check(g_carnival.ended, "the round ended and the hero is back on " + where);
        std::printf("carnival: at most %zu monsters in the room during the round\n",
                    g_carnival.mobs);
        check(g_carnival.mobs > 0, "the round's waves put monsters in the room");
        check(g_carnival.first_score == 0,
              "the round's score started at 0 (the original leaves it uninitialised before the "
              "first round)");
        check(h.level == level && h.exp == exp,
              "resultMonCa paid the score band's EXP (" + std::to_string(gain) + ")");
        mt_text_events_remove_listener(record_carnival_page, nullptr);
        bool hangul = false;
        for (const std::string &p : g_carnival.pages) {
            hangul = hangul || has_hangul(p);
        }
        std::printf("carnival: %zu pages and answers of NPC 38\n", g_carnival.pages.size());
        check(!g_carnival.pages.empty() && !hangul,
              script + "'s pages are in the game's language (English), not its Korean literals");
        const mt_restored_carnival_stats s = mt_hooks_restored_carnival_stats();
        check(s.npcs >= 2 && s.tickets == 3 && s.rounds == 1,
              "the NPC added on each load of " + where +
                  ", the three tickets listed once, one round's score started from 0");
    });
    then_check(r, "the hero is alive in the field",
               [](mt_cpu *cpu) { return hero(cpu).hp > 0 && field(cpu); });
    then_finish(r);
}
void systems2_restored_carnival(Run &r, u64) { restored_carnival_run(r, 0, true); }
void systems2_restored_carnival_off(Run &r, u64) { restored_carnival_run(r, 0, false); }
void systems2_restored_carnival_4(Run &r, u64) { restored_carnival_run(r, 4, true); }
void systems2_restored_carnival_6(Run &r, u64) { restored_carnival_run(r, 6, true); }
void systems2_restored_carnival_checks(Run &, u64) {}

// --- The unreached maps 4.5 and 4.8 (restored_content, #428) ----------------------------------
//
// docs/re/original-bugs.md "Unreachable content": 4.5 and 4.8 are a third road through Orbis's
// garden; their own portals lead to each other and out to 4.3 and 4.10, but no reached map leads
// in. With restored_content on, 4.3's list starts with one portal more, kind 2 portal480
// (mt_restored_portal_on), then 43.obj's records. The hero (level 60, chapter 3, the start
// restored_maps_4_3) walks every way in and out by the d-pad: 4.3's added portal to 4.5, up
// 4.5 to its portal 1 and into 4.8, back through 4.8's portal 0 to 4.5 and its portal 0 to 4.3,
// where he lands on 4.3's own portal 2 (the arrival takes the last kind-2 object, not the added
// one); then 4.3 to 4.5 to 4.8 again and out through 4.8's portal 1 to 4.10. Each landing is the
// target portal's arrival point (x = anchor + 16). systems2_restored_maps_off: the same start
// with the setting off: 4.3's list is 43.obj's, record for record, and its portal 2 (portal432)
// still leads to 4.6, as shipped.

// Where a warp to portal (col, row) puts the hero: the anchor's x + 16, its y (obj.md).
struct Landing {
    int area, map, col, row;
};
struct RestoredMapsRun {
    std::vector<std::string> landed; // "4.5 at 568,399" per warp, in order
    bool all_ok = true;
};
RestoredMapsRun g_restored_maps;

// Walks to the portal at tile (col, row) of (area, map) and up through it; checks that the hero
// lands on `to` at its arrival point.
void then_portal(Run &r, const std::string &what, int area, int map, int col, int row, Landing to) {
    using namespace quest_bot;
    const int x = col * 16 + 8, y = (row + 1) * 16 - 1;
    then_go(r, what + ": to the portal", {area, map, x, y, 4}, 240);
    then(r, what + ": up through it", 20, [&r, to](mt_cpu *cpu, double el) {
        const Hero h = hero(cpu);
        if (h.addr != 0 && h.on(to.area, to.map) && field(cpu)) {
            release_all(r);
            return true;
        }
        if (!field(cpu)) { // a notice the portal's script opened: A through it
            hold(r, UP, false);
            hold(r, A, std::fmod(el, 0.8) < 0.1);
        } else {
            hold(r, A, false);
            hold(r, UP, std::fmod(el, 1.0) < 0.3);
        }
        return false;
    });
    then_do(r, what + ": the landing", [what, to](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const int ax = to.col * 16 + 8 + 16, ay = (to.row + 1) * 16 - 1;
        const std::string at = std::to_string(h.area) + "." + std::to_string(h.map) + " at " +
                               std::to_string(h.x) + "," + std::to_string(h.y);
        std::printf("restored maps: %s: landed %s (portal %d,%d: %d,%d)\n", what.c_str(),
                    at.c_str(), to.col, to.row, ax, ay);
        g_restored_maps.landed.push_back(at);
        const bool ok = h.on(to.area, to.map) && std::abs(h.x - ax) <= 24 &&
                        std::abs(h.y - ay) <= 24 && h.hp > 0;
        g_restored_maps.all_ok = g_restored_maps.all_ok && ok;
        check(ok, what + ": on " + std::to_string(to.area) + "." + std::to_string(to.map) +
                      " at portal (" + std::to_string(to.col) + ", " + std::to_string(to.row) +
                      ")'s arrival point");
    });
}

void restored_maps_run(Run &r, bool on) {
    using namespace quest_bot;
    r.settings.restored_content = on;
    g_restored_maps = RestoredMapsRun{};
    from_save(r);
    then_wait_field(r);
    const mt_restored_portal *p = mt_restored_portal_on(4, 3);
    check(p != nullptr && std::string(p->script) == "portal480" && p->kind == 2,
          "4.3's added portal is portal480, kind 2");
    if (p == nullptr) {
        then_finish(r);
        return;
    }
    then_do(r, "4.3's object list", [on, p](mt_cpu *cpu) {
        const std::vector<LiveObject> live = live_objects(cpu);
        const std::vector<LiveObject> file = file_objects(4, 3);
        const std::size_t skip = on ? 1 : 0;
        bool same = live.size() == file.size() + skip;
        for (std::size_t i = 0; same && i < file.size(); i++) {
            same = live[i + skip].cell == file[i].cell && live[i + skip].kind == file[i].kind &&
                   live[i + skip].name == file[i].name;
        }
        std::printf("4.3: %zu live objects, its .obj has %zu\n", live.size(), file.size());
        if (on) {
            check(!live.empty() && live[0].cell == p->cell && live[0].kind == p->kind &&
                      live[0].name == p->script,
                  "restored_content: 4.3's list starts with portal480, kind 2, at its cell");
            check(same, "then 43.obj's records, in order");
        } else {
            check(same, "restored_content off: 4.3's list is 43.obj's, record for record");
        }
    });
    if (!on) {
        // portal432 (4.3's portal 2) to 4.6's portal 0, as shipped.
        then_portal(r, "4.3 portal 2 (portal432)", 4, 3, 31, 5, {4, 6, 30, 24});
        then_check(r, "restored_content off: no object added",
                   [](mt_cpu *) { return mt_hooks_restored_maps_stats().portals == 0; });
        then_finish(r);
        return;
    }
    const int col = p->cell % 38, row = p->cell / 38;
    // In, up and back: every exit of 4.5 and 4.8 by walking.
    then_portal(r, "4.3 added portal (portal480)", 4, 3, col, row, {4, 5, 34, 24});
    then_portal(r, "4.5 portal 1 (portal450)", 4, 5, 22, 5, {4, 8, 3, 26});
    then_portal(r, "4.8 portal 0 (portal480)", 4, 8, 3, 26, {4, 5, 34, 24});
    then_portal(r, "4.5 portal 0 (portal451)", 4, 5, 34, 24, {4, 3, 31, 5});
    then_portal(r, "4.3 added portal again", 4, 3, col, row, {4, 5, 34, 24});
    then_portal(r, "4.5 portal 1 again", 4, 5, 22, 5, {4, 8, 3, 26});
    then_portal(r, "4.8 portal 1 (portal481)", 4, 8, 8, 5, {4, 10, 32, 33});
    then_do(r, "the restored maps' runs", [](mt_cpu *cpu) {
        const mt_restored_maps_stats s = mt_hooks_restored_maps_stats();
        std::printf("restored maps: %zu warps, the portal added %llu times\n",
                    g_restored_maps.landed.size(), static_cast<unsigned long long>(s.portals));
        check(g_restored_maps.all_ok && g_restored_maps.landed.size() == 7,
              "4.5 and 4.8 entered and left through each of their portals");
        check(s.portals == 2, "the portal added on each load of 4.3 (the start, the way back)");
        check(hero(cpu).hp > 0 && field(cpu), "the hero is alive in the field on 4.10");
    });
    then_finish(r);
}
void systems2_restored_maps(Run &r, u64) { restored_maps_run(r, true); }
void systems2_restored_maps_off(Run &r, u64) { restored_maps_run(r, false); }
void systems2_restored_maps_checks(Run &, u64) {}

// --- the compendium's NPCs, shops and recipes in the port (#220) ---------------------------------
//
// compendium_npcs: from the start of chapter 2 (tests/starts/chapter_2.json, equal to part L's
// save), five NPCs, two shops and two recipes of the compendium checked by play against the
// knowledge base's own records (bot_world.txt's npc, shop and recipe lines, which the compendium
// pages are built from; docs/testing/compendium-port-check.md):
//   - an NPC: the hero goes to the map the compendium names; its object shows there at the tile
//     the compendium gives (the live object, live_object), and up at it opens its chat or shop;
//   - a shop: opened from its NPC, the Buy tab's rows (item, price) are the compendium's stock
//     of that shop, row for row as a set, and of its kind (equipment or other items);
//   - a recipe: crafted at the smith through his lists; the materials, the meso fee and the
//     result the compendium gives are what leaves and enters the bag.

// The compendium's record of the NPC object `object` on (area, map), nullptr if none.
const world_map::Npc *kb_npc(int area, int map, const std::string &object) {
    for (const world_map::Npc &n : world_map::npcs()) {
        if (n.area == area && n.map == map && n.object == object) {
            return &n;
        }
    }
    return nullptr;
}

// Goes to the NPC, checks its live object against the compendium's tile, and presses up until
// its chat or its shop opens (left open for the next step).
void then_meet(Run &r, const std::string &name, int area, int map, const std::string &object) {
    const world_map::Npc *n = kb_npc(area, map, object);
    check(n != nullptr, name + ": the compendium places " + object + " on " + std::to_string(area) +
                            "." + std::to_string(map));
    if (n == nullptr) {
        return;
    }
    const int want_x = n->column * 16 + 8, want_y = (n->row + 1) * 16 - 1;
    quest_bot::then_go(r, name + " (" + object + ")", quest_bot::at_object(area, map, object, 6),
                       600);
    then_do(r, name + ": where the compendium puts it",
            [name, object, want_x, want_y, area, map](mt_cpu *cpu) {
                int x = -1, y = -1;
                const bool seen = quest_bot::live_object(cpu, object, x, y);
                check(hero(cpu).on(area, map) && seen && x == want_x && y == want_y,
                      name + " (" + object + ") stands on " + std::to_string(area) + "." +
                          std::to_string(map) + " at (" + std::to_string(x) + ", " +
                          std::to_string(y) + "), the compendium's tile gives (" +
                          std::to_string(want_x) + ", " + std::to_string(want_y) + ")");
            });
    then(r, name + ": up opens the chat or the shop", 20, [&r](mt_cpu *cpu, double el) {
        if (quest_bot::chatting(cpu) || game_mode(cpu) == 2) {
            hold(r, UP, false);
            return true;
        }
        hold(r, UP, std::fmod(el, 1.0) < 0.2);
        return false;
    });
    then_check(r, name + ": its chat or shop is open",
               [](mt_cpu *cpu) { return quest_bot::chatting(cpu) || game_mode(cpu) == 2; });
}

// The chat opened by then_meet, A through to the field (any row of a list).
void then_chat_closed(Run &r, const std::string &name, int area, int map,
                      const std::string &object) {
    quest_bot::then_talk_to(r, name + ": the chat closed", area, map, object, -1,
                            [](mt_cpu *) { return true; });
}

// The shop opened by then_meet: its Buy rows against the compendium's stock, then B to the field.
void then_shop_matches(Run &r, const std::string &name, int area, int map,
                       const std::string &object) {
    then_wait_for(r, name + ": the shop settled", 5, quest_bot::in_shop, 0.5);
    then_do(r, name + ": the Buy tab is the compendium's stock", [=](mt_cpu *cpu) {
        std::set<std::pair<i32, i32>> want;
        std::set<int> numbers, tables;
        for (const world_map::Shop &s : world_map::shops()) {
            if (s.chapter == hero(cpu).chapter && s.area == area && s.map == map &&
                s.npc == object) {
                want.insert({s.item, s.price});
                numbers.insert(s.shop);
                tables.insert(s.table);
            }
        }
        const Shop sh = shop(cpu);
        std::set<std::pair<i32, i32>> got;
        std::string rows;
        for (i32 i = 0; i < sh.rows; i++) {
            const ShopRow w = shop_row(cpu, sh, i);
            got.insert({w.id, w.price});
            rows += " " + std::to_string(w.id) + ":" + std::to_string(w.price);
        }
        const int table = tables.size() == 1 ? *tables.begin() : -1;
        std::printf("%s: shop %d, kind %d, %d rows:%s\n", name.c_str(),
                    numbers.size() == 1 ? *numbers.begin() : -1, sh.kind, sh.rows, rows.c_str());
        check(numbers.size() == 1 && !want.empty() && got == want &&
                  static_cast<std::size_t>(sh.rows) == want.size() &&
                  (sh.kind == 1) == (table == 1),
              name + ": " + std::to_string(sh.rows) +
                  " rows, each item and price as the "
                  "compendium's shop " +
                  std::to_string(numbers.empty() ? -1 : *numbers.begin()) + " (" +
                  std::to_string(want.size()) + " items) lists them");
    });
    then(r, name + ": B closes the shop", 10, [&r](mt_cpu *cpu, double el) {
        if (field(cpu) && game_mode(cpu) != 2) {
            hold(r, B, false);
            return true;
        }
        hold(r, B, std::fmod(el, 0.6) < 0.1);
        return false;
    });
}

// Crafts the compendium's recipe for equipment `item` of `chapter` at the smith (0.0) through his
// lists `choices`, and checks what left and entered the bag against the recipe.
void then_craft(Run &r, const std::string &name, i32 item, std::vector<int> choices,
                int chapter = 2) {
    const world_map::Recipe *rc = nullptr;
    for (const world_map::Recipe &c : world_map::recipes()) {
        if (c.chapter == chapter && c.table == 0 && c.item == item) {
            rc = &c;
        }
    }
    check(rc != nullptr && rc->area == 0 && rc->map == 0 && rc->npc == "smith",
          name + ": the compendium has its chapter-" + std::to_string(chapter) +
              " recipe at the smith on 0.0");
    if (rc == nullptr) {
        return;
    }
    std::vector<i32> items;
    for (const world_map::Material &m : rc->materials) {
        items.push_back(m.item);
    }
    auto before = std::make_shared<Counts>();
    then_do(r, name + ": before", [before, items, item](mt_cpu *cpu) {
        *before = counts(cpu, items, {item});
        std::printf("before %s\n", counts_text(*before).c_str());
    });
    quest_bot::then_talk_to(r, name + ": crafted at the smith", rc->area, rc->map, rc->npc,
                            std::move(choices), [before, item](mt_cpu *cpu) {
                                return quest_bot::equip_owned(cpu, hero(cpu), item) >
                                       before->equip[item];
                            });
    const world_map::Recipe want = *rc;
    then_do(r, name + ": after", [before, items, want, name](mt_cpu *cpu) {
        Counts now = counts(cpu, items, {want.item});
        bool ok = now.equip[want.item] == before->equip[want.item] + want.count &&
                  now.meso == before->meso - want.meso;
        std::string recipe = std::to_string(want.count) + " of equipment " +
                             std::to_string(want.item) + " for " + std::to_string(want.meso) +
                             " meso";
        for (const world_map::Material &m : want.materials) {
            ok = ok && m.table == 1 && now.items[m.item] == before->items[m.item] - m.count;
            recipe += ", " + std::to_string(m.count) + " of item " + std::to_string(m.item);
        }
        check(ok, name + ": the compendium's recipe (" + recipe + "): before" +
                      counts_text(*before) + "; after" + counts_text(now));
    });
}

void compendium_npcs(Run &r, u64) {
    from_save(r);
    then_wait_field(r);
    then_check(r, "chapter 2 on Orbis (4.1)",
               [](mt_cpu *cpu) { return hero(cpu).chapter == 2 && hero(cpu).on(4, 1); });
    // NPC 1: the Pirate on Orbis, whose list 1 takes the hero back to Henesys.
    then_meet(r, "the Pirate", 4, 1, "Pirate_Orbis");
    quest_bot::then_talk_to(r, "the Pirate: back to Henesys (list 1)", 4, 1, "Pirate_Orbis", 1,
                            [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    // NPC 2: Chief Stan (2.0).
    then_meet(r, "Chief Stan", 2, 0, "Stan");
    then_chat_closed(r, "Chief Stan", 2, 0, "Stan");
    // NPC 3 and shop 1: Mr. Wang's shop in Henesys (Etc20).
    then_meet(r, "Mr. Wang", 2, 0, "Etc20");
    then_shop_matches(r, "Mr. Wang's shop in Henesys", 2, 0, "Etc20");
    // Shop 2: Mr. Lee's equipment shop in Henesys (Equip20).
    then_meet(r, "Mr. Lee", 2, 0, "Equip20");
    then_shop_matches(r, "Mr. Lee's shop in Henesys", 2, 0, "Equip20");
    // NPC 4: Blackbull (the pig, Perion 1.0).
    then_meet(r, "Blackbull", 1, 0, "pig");
    then_chat_closed(r, "Blackbull", 1, 0, "pig");
    // NPC 5: Mr. Smith (0.0), then two of his recipes: the shoes' list (5), its first pair, and
    // the earrings' list (6), its first pair. His chat stays open for the first: any row of his
    // first list crafts something.
    then_meet(r, "Mr. Smith", 0, 0, "smith");
    then_craft(r, "Lappy Boots", 51, {5, 1});
    then_craft(r, "Lightning Earrings", 63, {6, 1});
    then_bag(r, "the end");
    then_finish(r);
}
void compendium_npcs_checks(Run &, u64) {}

// --- chapter 2's crafts, trades and shops (#299, docs/testing/chapter-2-quests.md) ------------

// One trade (side 78) at `npc` through its lists `choices`: 50 of item `cost` for 3 of item
// `result`, checked in the bag and in the quest manager before and after.
void then_trade(Run &r, const std::string &name, int area, int map, const std::string &npc,
                std::vector<int> choices, i32 cost, i32 result) {
    auto before = std::make_shared<Counts>();
    const std::vector<i32> items = {cost, result};
    then_do(r, name + ": before", [before, items](mt_cpu *cpu) {
        *before = counts(cpu, items, {});
        std::printf("before%s\n", counts_text(*before).c_str());
    });
    quest_bot::then_talk_to(
        r, name, area, map, npc, std::move(choices), [before, result](mt_cpu *cpu) {
            return item_count(cpu, hero(cpu), result) >= before->items[result] + 3;
        });
    then_do(r, name + ": after", [before, items, cost, result, name](mt_cpu *cpu) {
        Counts now = counts(cpu, items, {});
        const i32 state = quest_state(cpu, hero(cpu), 1, 78);
        check(now.items[cost] == before->items[cost] - 50 &&
                  now.items[result] == before->items[result] + 3 && now.meso == before->meso &&
                  state == 2,
              name + " (side 78:" + std::to_string(state) + "): 50 of item " +
                  std::to_string(cost) + " for 3 of item " + std::to_string(result) + "; before" +
                  counts_text(*before) + "; after" + counts_text(now));
    });
}

// Part crafts (#299, synthetic: tests/starts/raise/chapter_2_crafts.json over starts.chapter_2,
// the materials and meso every recipe and trade needs). Every trade of chapter 2 once: Ericsson
// (4.0, list 5, then 1-4) and the pig (1.0, lists 1-3); then every recipe of chapter 2's smith
// (0.0) once, his kind (1 hat, 2 overall, 3 dagger, 4 glove, 5 shoes, 6 earrings) then the row,
// each against the knowledge base's recipe (then_craft). Chapter 1's own Craft and Trade (#161)
// are these: in chapter 1 neither opens (they need main 40).
void chapter_2_crafts(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2 on 4.1", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 2 && h.on(4, 1);
    });
    then_bag(r, "the crafts' start");
    then_trade(r, "Ericsson: trade 1, 50 of item 54 for 3 of item 94", 4, 0, "Ericsson", {5, 1}, 54,
               94);
    then_trade(r, "Ericsson: trade 2, 50 of item 50 for 3 of item 100", 4, 0, "Ericsson", {5, 2},
               50, 100);
    then_trade(r, "Ericsson: trade 3, 50 of item 66 for 3 of item 95", 4, 0, "Ericsson", {5, 3}, 66,
               95);
    then_trade(r, "Ericsson: trade 4, 50 of item 68 for 3 of item 101", 4, 0, "Ericsson", {5, 4},
               68, 101);
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_trade(r, "pig: trade 1, 50 of item 103 for 3 of item 98", 1, 0, "pig", {1}, 103, 98);
    then_trade(r, "pig: trade 2, 50 of item 42 for 3 of item 93", 1, 0, "pig", {2}, 42, 93);
    then_trade(r, "pig: trade 3, 50 of item 112 for 3 of item 99", 1, 0, "pig", {3}, 112, 99);
    // The smith's rows in each kind are its recipes by result id (the knowledge base's order).
    const std::vector<std::tuple<const char *, i32, int, int>> recipes = {
        {"hat 3", 3, 1, 1},        {"hat 5", 5, 1, 2},        {"hat 7", 7, 1, 3},
        {"overall 15", 15, 2, 1},  {"overall 17", 17, 2, 2},  {"overall 19", 19, 2, 3},
        {"dagger 27", 27, 3, 1},   {"dagger 29", 29, 3, 2},   {"dagger 31", 31, 3, 3},
        {"glove 39", 39, 4, 1},    {"glove 41", 41, 4, 2},    {"glove 43", 43, 4, 3},
        {"shoes 51", 51, 5, 1},    {"shoes 52", 52, 5, 2},    {"shoes 55", 55, 5, 3},
        {"earrings 63", 63, 6, 1}, {"earrings 65", 65, 6, 2}, {"earrings 67", 67, 6, 3}};
    auto made = std::make_shared<int>(0);
    for (const auto &[name, item, kind, row] : recipes) {
        then_craft(r, std::string("smith: ") + name, item, {kind, row});
        then_do(r, std::string("smith: ") + name + ": side 77", [made](mt_cpu *cpu) {
            ++*made;
            check(quest_state(cpu, hero(cpu), 1, 77) == 2,
                  "the Craft is side 77, state " +
                      std::to_string(quest_state(cpu, hero(cpu), 1, 77)));
        });
    }
    then_do(r, "every recipe of chapter 2", [made](mt_cpu *) {
        int kb = 0;
        for (const world_map::Recipe &c : world_map::recipes()) {
            kb += c.chapter == 2 ? 1 : 0;
        }
        check(*made == kb && kb == 18, "crafted " + std::to_string(*made) +
                                           " recipes; the knowledge base has " +
                                           std::to_string(kb) + " in chapter 2");
    });
    then_bag(r, "the crafts' end");
    then_save(r);
    then_finish(r);
}
void chapter_2_crafts_checks(Run &, u64) {}

// The row of `id` in the shop's Sell tab: the hero's equipment (hero +0xD4) in an equipment
// shop, the other items (+0xD8) otherwise, up to the first empty entry (ADR 0102); -1 if absent.
i32 sell_tab_row(mt_cpu *cpu, const Shop &sh, i32 id) {
    const Hero h = hero(cpu);
    const u32 list = h.addr != 0 ? rd(cpu, h.addr + (sh.kind == 0 ? 0xD4 : 0xD8)) : 0;
    for (u32 i = 0; list != 0 && i < 48; i++) {
        const u32 it = rd(cpu, list + 4 * i);
        if (it == 0) {
            break;
        }
        if (rdi(cpu, it + 0x90) == id) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// Buys one of `id` (with `equip` a piece of equipment) at the shop NPC `npc` of (area, map), then
// sells one back on its Sell tab, by the pad alone: up opens the shop; on Buy, down to the row,
// A, A through the popups; right to Sell, down to the row, A (popup 6, how many: 1), A, A; B
// closes it. The bag and the meso are checked after each: the buy costs the shop's price (the
// knowledge base's), the sale brings meso in and takes one away.
void then_shop_use(Run &r, const std::string &name, int area, int map, const std::string &npc,
                   i32 id, bool equip) {
    struct S {
        quest_bot::Nav nav;
        double pressed = -10;
        i32 count = -1, meso = -1, price = -1;
        bool bought = false, sold = false, closing = false;
    };
    auto s = std::make_shared<S>();
    const quest_bot::Goal g = quest_bot::at_object(area, map, npc, 6);
    const auto owned = [equip, id](mt_cpu *cpu) {
        return equip ? quest_bot::equip_owned(cpu, hero(cpu), id) : item_count(cpu, hero(cpu), id);
    };
    const std::string what = std::string(equip ? "equipment " : "item ") + std::to_string(id);
    then(r, "shop: " + name + ": buy and sell one " + what, 300,
         [&r, s, g, id, equip, owned, name, what, area, map, npc](mt_cpu *cpu, double el) {
             const Hero h = hero(cpu);
             const auto press = [&](SDL_GamepadButton b) {
                 if (el - s->pressed >= 0.5) {
                     s->pressed = el;
                 }
                 hold(r, b, el - s->pressed < 0.1);
             };
             if (s->count < 0) {
                 s->count = owned(cpu);
                 s->meso = h.meso;
                 for (const world_map::Shop &w : world_map::shops()) {
                     if (w.chapter == h.chapter && w.area == area && w.map == map && w.npc == npc &&
                         w.item == id && w.table == (equip ? 0 : 1)) {
                         s->price = w.price;
                     }
                 }
                 std::printf("shop: %s: before: %d of %s, meso %d, price %d\n", name.c_str(),
                             s->count, what.c_str(), s->meso, s->price);
             }
             if (game_mode(cpu) == 2) {
                 hold(r, UP, false);
                 if (!quest_bot::in_shop(cpu)) { // a popup: how many, buy? / sell?, done
                     press(A);
                     return false;
                 }
                 const Shop sh = shop(cpu);
                 if (!s->bought && owned(cpu) == s->count + 1) {
                     s->bought = true;
                     check(h.meso == s->meso - s->price,
                           name + ": bought one " + what + " for " + std::to_string(s->price) +
                               " meso: " + std::to_string(s->count) + " -> " +
                               std::to_string(owned(cpu)) + ", meso " + std::to_string(s->meso) +
                               " -> " + std::to_string(h.meso));
                     s->meso = h.meso;
                 }
                 if (s->bought && !s->sold && owned(cpu) == s->count) {
                     s->sold = true;
                     check(h.meso > s->meso,
                           name + ": sold one " + what +
                               " on the Sell tab: " + std::to_string(s->count + 1) + " -> " +
                               std::to_string(owned(cpu)) + ", meso " + std::to_string(s->meso) +
                               " -> " + std::to_string(h.meso) + " (+" +
                               std::to_string(h.meso - s->meso) + ")");
                 }
                 if (s->sold || s->closing) {
                     press(B);
                     return false;
                 }
                 const i32 tab = s->bought ? 1 : 0;
                 i32 row = -1;
                 if (tab == 0) {
                     for (i32 i = 0; i < sh.rows; i++) {
                         row = shop_row(cpu, sh, i).id == id ? i : row;
                     }
                 } else if (sh.tab == 1) {
                     row = sell_tab_row(cpu, sh, id);
                 }
                 if (sh.tab != tab) {
                     press(tab == 1 ? RIGHT : LEFT);
                 } else if (row < 0 || (sh.kind == 0) != equip) {
                     check(false, name + ": no row of " + what + " on the " +
                                      (tab == 0 ? "Buy" : "Sell") + " tab");
                     s->closing = true;
                 } else if (sh.selected != row) {
                     press(sh.selected < row ? DOWN : UP);
                 } else {
                     press(A);
                 }
                 return false;
             }
             if (quest_bot::interrupted(r, cpu, el)) {
                 return false;
             }
             if (s->sold || s->closing) {
                 release_all(r);
                 return true;
             }
             if (!quest_bot::drive(r, cpu, el, s->nav, g)) {
                 return false;
             }
             press(UP);
             return false;
         });
    then_check(r, name + ": bought and sold one " + what,
               [s](mt_cpu *) { return s->bought && s->sold; });
}

// Part shops (#299, from part sides_f's save in area 4): the shops of 4.0 and 9.0 by the pad, each
// of its two shops opened, one item bought and one sold (then_shop_use), then the save.
void chapter_2_shops(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2", [](mt_cpu *cpu) { return hero(cpu).chapter == 2; });
    then_shop_use(r, "Etc40 (4.0)", 4, 0, "Etc40", 5, false);
    then_shop_use(r, "Equip40 (4.0)", 4, 0, "Equip40", 53, true);
    then_shop_use(r, "Etc90 (9.0)", 9, 0, "Etc90", 5, false);
    then_shop_use(r, "Equip90 (9.0)", 9, 0, "Equip90", 54, true);
    then_bag(r, "the shops' end");
    then_save(r);
    then_finish(r);
}
void chapter_2_shops_checks(Run &, u64) {}

// Part shop_6 (#299, from part F's save: chapter 3 on 6.0): the town of 6.0's two shops by the
// pad, as part shops does for 9.0 and 4.0.
void chapter_2_shop_6(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 3 on 6.0", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.on(6, 0);
    });
    then_shop_use(r, "Etc60 (6.0)", 6, 0, "Etc60", 5, false);
    then_shop_use(r, "Equip60 (6.0)", 6, 0, "Equip60", 6, true);
    then_bag(r, "the shops' end");
    then_finish(r);
}
void chapter_2_shop_6_checks(Run &, u64) {}

// --- chapter 2's side quests (#299, docs/testing/chapter-2-quests.md) --------------------------
//
// From part E's save (main 8 active, the hero in area 9), in parts by area. Each NPC's list
// value for a quest is the row the knowledge base's dialogue gives it: Skadru (9.0) 6 = 1, 29 =
// 2, 30 = 3; the SnowQueen (9.0) 5 = 1, 34 = 2; Jade (9.0) 31 = 1, 32 = 2, 38 = 3; Wilson (4.2) 22
// = 2, 11 and 12 = 3, 12 = 4, 28 = 5, 33 = 6; Barun (5.1) 10 = 3, 13 = 4, 25 = 5; Humanoid2 (4.1)
// 18 = 1, 23 = 3, 7 = 4; Heracle (4.0) 3 = 3, 9 = 4, 26 = 5, 27 = 6; Ericsson (4.0) 4 = 2, 19 =
// 3; Elma (4.0) and the Miner on 9.10 (Miner2) have no list.

// Every predicate of `all` holds.
Pred all_of(std::vector<Pred> all) {
    return [all](mt_cpu *cpu) {
        return std::all_of(all.begin(), all.end(), [cpu](const Pred &p) { return p(cpu); });
    };
}
Pred side_defeated(i32 n, i32 mob, i32 count) {
    return [n, mob, count](mt_cpu *cpu) { return quest_bot::defeats(cpu, 1, n, mob) >= count; };
}
// The side quests' items: then_sell keeps them.
const std::vector<i32> k_chapter_2_side_items = {105, 120, 114, 87, 111, 8,  35, 40, 44,
                                                 50,  51,  52,  53, 55,  56, 57, 60, 61,
                                                 62,  63,  64,  65, 67,  68, 71, 72, 115};

// The common start of a side part: the SP, the bag's materials sold and the potions stocked at
// the shop of `area` (4 or 9).
void chapter_2_side_start(Run &r, const std::string &part, int area) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2", [](mt_cpu *cpu) { return hero(cpu).chapter == 2; });
    then_spend_sp(r, part, k_chapter_2_build);
    const std::string shop = area == 9 ? "Etc90" : "Etc40";
    then_sell(r, "materials (" + shop + ")", area, 4, k_chapter_2_side_items);
    then_stock(r, "item 4 (" + shop + ")", area, 4, 200, 500, 600);
    then_stock(r, "item 7 (" + shop + ")", area, 7, 80, 500, 600);
    then_bag(r, part + "'s start");
}
void chapter_2_side_end(Run &r, const std::string &part) {
    then_do(r, part + "'s hero", [part](mt_cpu *cpu) {
        std::printf("%s: %s\n", part.c_str(), hero_text(cpu, hero(cpu)).c_str());
    });
    then_bag(r, part + "'s end");
    then_save(r);
    then_finish(r);
}

// Part sides_a (#299): area 9. Jade gives side 31 (her letter, item 114, for Wilson); Skadru
// side 6 (25 of item 64 from mob 34 on 9.1); the SnowQueen side 5 (item 72, later) and, once 6 is
// done, side 34 (30 of item 71 from mob 35 on 9.3).
void chapter_2_sides_a(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_a", 9);
    const FarmStyle fight = chapter_2_fight();
    then_talk_to(r, "Jade: side 31 given, item 114", 9, 0, "Jade", 1,
                 all_of({quest_is(1, 31, 1), has_item(114, 1)}));
    then_talk_to(r, "Skadru: side 6 given", 9, 0, "Skadru", 1, quest_is(1, 6, 1));
    then_talk_to(r, "SnowQueen: side 5 given", 9, 0, "SnowQueen", 1, quest_is(1, 5, 1));
    then_farm(r, "25 of item 64 from mob 34 on 9.1 (side 6)", 9, 1, {34}, ready(1, 6), 3600, fight);
    then_talk_to(r, "Skadru: side 6 done", 9, 0, "Skadru", 1, quest_is(1, 6, 2));
    then_talk_to(r, "SnowQueen: side 34 given", 9, 0, "SnowQueen", 2, quest_is(1, 34, 1));
    then_farm(r, "30 of item 71 from mob 35 on 9.3 (side 34)", 9, 3, {35}, ready(1, 34), 3600,
              fight);
    then_talk_to(r, "SnowQueen: side 34 done", 9, 0, "SnowQueen", 2, quest_is(1, 34, 2));
    then_quests_are(r, "part sides_a's quests", {{1, 5, 1}, {1, 6, 2}, {1, 31, 1}, {1, 34, 2}});
    chapter_2_side_end(r, "sides_a");
}
void chapter_2_sides_a_checks(Run &, u64) {}

// Part sides_b (#299): back through area 5 to area 4. Barun (5.1) side 25 (30 of item 63 from
// mob 25 on 5.3); Wilson (4.2) side 33 (Jade's letter) and 22 (item 111, the reward of 23);
// Humanoid2 (4.1) 18 and 23; Heracle (4.0) 3 (30 each of mobs 38 and 39) and 9 (done once
// Barun's 13 is); Elma's 20 (given in part A: 40 of item 55 from mob 40) and 21 (10 of item 8
// and 30 of item 56); Ericsson's 4 (25 of item 53 from mob 21) and 19 (50 of mob 22). Farms on
// 4.2, 4.3, 4.9 and 4.10.
void chapter_2_sides_b(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_b", 9);
    const FarmStyle fight = chapter_2_fight();
    then_talk_to(r, "Barun: side 25 given", 5, 1, "Barun", 5, quest_is(1, 25, 1), 900);
    then_farm(r, "30 of item 63 from mob 25 on 5.3 (side 25)", 5, 3, {25}, ready(1, 25), 3600,
              fight);
    then_talk_to(r, "Barun: side 25 done", 5, 1, "Barun", 5, quest_is(1, 25, 2));
    then_talk_to(r, "Wilson: Jade's letter, side 33 done", 4, 2, "Wilson", 6, quest_is(1, 33, 2),
                 900);
    then_talk_to(r, "Wilson: side 22 given", 4, 2, "Wilson", 2, quest_is(1, 22, 1));
    then_talk_to(r, "Humanoid2: side 18 given", 4, 1, "Humanoid2", 1, quest_is(1, 18, 1));
    then_talk_to(r, "Humanoid2: side 23 given", 4, 1, "Humanoid2", 3, quest_is(1, 23, 1));
    then_talk_to(r, "Heracle: side 3 given", 4, 0, "Heracle", 3, quest_is(1, 3, 1));
    then_talk_to(r, "Heracle: side 9 given", 4, 0, "Heracle", 4, quest_is(1, 9, 1));
    then_talk_to(r, "Ericsson: side 4 given", 4, 0, "Ericsson", 2, quest_is(1, 4, 1));
    then_check(r, "Elma's side 20 active (since part A)", quest_is(1, 20, 1));
    then_farm(r, "40 of item 55 (mob 40) and 25 of item 53 (mob 21) on 4.2 (sides 20, 4)", 4, 2,
              {40, 21}, all_of({ready(1, 20), ready(1, 4)}), 3600, fight);
    then_talk_to(r, "Elma: side 20 done", 4, 0, "Elma", -1, quest_is(1, 20, 2));
    then_talk_to(r, "Elma: side 21 given", 4, 0, "Elma", -1, quest_is(1, 21, 1));
    then_talk_to(r, "Ericsson: side 4 done", 4, 0, "Ericsson", 2, quest_is(1, 4, 2));
    then_talk_to(r, "Ericsson: side 19 given", 4, 0, "Ericsson", 3, quest_is(1, 19, 1));
    then_farm(r, "50 of mob 22 and mob 38 on 4.3 (sides 19, 3)", 4, 3, {22, 38}, ready(1, 19), 3600,
              fight);
    then_farm(r, "mobs 38 and 39, item 56 on 4.9 (sides 3, 21)", 4, 9, {38, 39},
              all_of({ready(1, 3), has_item(56, 30)}), 3600, fight);
    then_farm(r, "30 of item 57 from mob 26 on 4.10 (side 23)", 4, 10, {26}, ready(1, 23), 3600,
              fight);
    then_talk_to(r, "Heracle: side 3 done", 4, 0, "Heracle", 3, quest_is(1, 3, 2));
    then_talk_to(r, "Ericsson: side 19 done", 4, 0, "Ericsson", 3, quest_is(1, 19, 2));
    then_talk_to(r, "Humanoid2: side 23 done, item 111", 4, 1, "Humanoid2", 3,
                 all_of({quest_is(1, 23, 2), has_item(111, 1)}));
    then_talk_to(r, "Wilson: side 22 done (item 111)", 4, 2, "Wilson", 2, quest_is(1, 22, 2));
    then_talk_to(r, "Wilson: side 11 given", 4, 2, "Wilson", 3, quest_is(1, 11, 1));
    then_quests_are(r, "part sides_b's quests",
                    {{1, 3, 2},
                     {1, 4, 2},
                     {1, 9, 1},
                     {1, 11, 1},
                     {1, 18, 1},
                     {1, 19, 2},
                     {1, 20, 2},
                     {1, 21, 1},
                     {1, 22, 2},
                     {1, 23, 2},
                     {1, 25, 2},
                     {1, 33, 2}});
    chapter_2_side_end(r, "sides_b");
}
void chapter_2_sides_b_checks(Run &, u64) {}

// Part sides_c (#299): Heracle's 26 wants 20 each of chapter 1's caps (items 35, 40 and 44:
// mobs 6, 10 and 13 on 0.1, 1.1 and 3.1), so Pirate_Orbis takes the hero to Victoria Island and
// back. Then the pixies of 4.12-4.15: Heracle's 27 (20 each of mobs 24, 30 and 36), Barun's 10
// (30 of item 50 from mob 24) and 13 (30 of item 52 from mob 36), Humanoid2's 18 (30 of item 51
// from mob 30) and, after it, 7; Heracle ends 9 once 13 is done.
void chapter_2_sides_c(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_c", 4);
    const FarmStyle fight = chapter_2_fight();
    then_talk_to(r, "Heracle: side 26 given", 4, 0, "Heracle", 5, quest_is(1, 26, 1));
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    FarmStyle low = fight;
    low.restock = {};
    then_farm(r, "20 of item 35 from mob 6 on 0.1 (side 26)", 0, 1, {6}, has_item(35, 20), 1800,
              low);
    then_farm(r, "20 of item 40 from mob 10 on 1.1 (side 26)", 1, 1, {10}, has_item(40, 20), 1800,
              low);
    then_farm(r, "20 of item 44 from mob 13 on 3.1 (side 26)", 3, 1, {13}, ready(1, 26), 1800, low);
    then_talk_to(
        r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1,
        [](mt_cpu *cpu) { return hero(cpu).on(4, 1); }, 900);
    then_talk_to(r, "Heracle: side 26 done", 4, 0, "Heracle", 5, quest_is(1, 26, 2));
    then_talk_to(r, "Heracle: side 27 given", 4, 0, "Heracle", 6, quest_is(1, 27, 1));
    then_talk_to(r, "Barun: side 10 given", 5, 1, "Barun", 3, quest_is(1, 10, 1));
    then_farm(r, "mobs 24 and 30 on 4.12 (sides 10, 27, 18)", 4, 12, {24, 30},
              all_of({ready(1, 10), side_defeated(27, 24, 20), side_defeated(27, 30, 20)}), 3600,
              fight);
    then_talk_to(r, "Barun: side 10 done", 5, 1, "Barun", 3, quest_is(1, 10, 2));
    then_talk_to(r, "Barun: side 13 given", 5, 1, "Barun", 4, quest_is(1, 13, 1));
    then_farm(r, "mobs 36 and 30 on 4.14 (sides 13, 27, 18)", 4, 14, {36, 30},
              all_of({ready(1, 13), ready(1, 27), ready(1, 18)}), 3600, fight);
    then_talk_to(r, "Barun: side 13 done", 5, 1, "Barun", 4, quest_is(1, 13, 2));
    then_talk_to(r, "Heracle: side 27 done", 4, 0, "Heracle", 6, quest_is(1, 27, 2));
    then_talk_to(r, "Heracle: side 9 done", 4, 0, "Heracle", 4, quest_is(1, 9, 2));
    then_talk_to(r, "Humanoid2: side 18 done", 4, 1, "Humanoid2", 1, quest_is(1, 18, 2));
    then_talk_to(r, "Humanoid2: side 7 given", 4, 1, "Humanoid2", 4, quest_is(1, 7, 1));
    then_quests_are(
        r, "part sides_c's quests",
        {{1, 7, 1}, {1, 9, 2}, {1, 10, 2}, {1, 13, 2}, {1, 18, 2}, {1, 26, 2}, {1, 27, 2}});
    chapter_2_side_end(r, "sides_c");
}
void chapter_2_sides_c_checks(Run &, u64) {}

// Part sides_d (#299): Wilson's robot line in area 5: 11 (30 of item 60 from mob 18 on 5.0),
// 12 (30 of item 61 from mob 19) and 28 (30 of item 62 from mob 20 on 5.2), each back at Wilson.
void chapter_2_sides_d(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_d", 4);
    const FarmStyle fight = chapter_2_fight();
    then_farm(r, "30 of item 60 from mob 18 on 5.0 (side 11)", 5, 0, {18}, ready(1, 11), 3600,
              fight);
    then_talk_to(r, "Wilson: side 11 done", 4, 2, "Wilson", 3, quest_is(1, 11, 2), 900);
    then_talk_to(r, "Wilson: side 12 given", 4, 2, "Wilson", 3, quest_is(1, 12, 1));
    then_farm(r, "30 of item 61 from mob 19 on 5.0 (side 12)", 5, 0, {19}, ready(1, 12), 3600,
              fight);
    then_talk_to(r, "Wilson: side 12 done", 4, 2, "Wilson", 4, quest_is(1, 12, 2), 900);
    then_talk_to(r, "Wilson: side 28 given", 4, 2, "Wilson", 5, quest_is(1, 28, 1));
    then_farm(r, "30 of item 62 from mob 20 on 5.2 (side 28)", 5, 2, {20}, ready(1, 28), 3600,
              fight);
    then_talk_to(r, "Wilson: side 28 done", 4, 2, "Wilson", 5, quest_is(1, 28, 2), 900);
    then_quests_are(r, "part sides_d's quests", {{1, 11, 2}, {1, 12, 2}, {1, 28, 2}});
    chapter_2_side_end(r, "sides_d");
}
void chapter_2_sides_d_checks(Run &, u64) {}

// Part sides_e (#299): area 9 again. Jade ends 31 (33 is done) and gives 32 (30 of item 115 from
// mob 52 on 9.4); Skadru 29 (25 of item 67 from mob 47 on 9.6) and 30 (30 of item 68 from mob 48
// on 9.7); Humanoid2's 7 (30 of item 65 from mob 41 on 9.2); Elma's item 8, bought.
void chapter_2_sides_e(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_e", 4);
    const FarmStyle fight = chapter_2_fight();
    then_talk_to(r, "Jade: side 31 done", 9, 0, "Jade", 1, quest_is(1, 31, 2), 900);
    then_talk_to(r, "Jade: side 32 given", 9, 0, "Jade", 2, quest_is(1, 32, 1));
    then_talk_to(r, "Skadru: side 29 given", 9, 0, "Skadru", 2, quest_is(1, 29, 1));
    then_farm(r, "30 of item 65 from mob 41 on 9.2 (side 7)", 9, 2, {41}, ready(1, 7), 3600, fight);
    then_farm(r, "30 of item 115 from mob 52 on 9.4 (side 32)", 9, 4, {52}, ready(1, 32), 3600,
              fight);
    then_farm(r, "25 of item 67 from mob 47 on 9.6 (side 29)", 9, 6, {47}, ready(1, 29), 3600,
              fight);
    then_talk_to(r, "Skadru: side 29 done", 9, 0, "Skadru", 2, quest_is(1, 29, 2), 900);
    then_talk_to(r, "Skadru: side 30 given", 9, 0, "Skadru", 3, quest_is(1, 30, 1));
    then_farm(r, "30 of item 68 from mob 48 on 9.7 (side 30)", 9, 7, {48}, ready(1, 30), 3600,
              fight);
    // Elma's item 8 drops from mob 48 one time in a thousand (360 kills on 9.7 brought none);
    // Etc90 sells it.
    then_stock(r, "10 of item 8 for side 21 (Etc90)", 9, 8, 10, 500, 600);
    then_talk_to(r, "Skadru: side 30 done", 9, 0, "Skadru", 3, quest_is(1, 30, 2), 900);
    then_talk_to(r, "Jade: side 32 done", 9, 0, "Jade", 2, quest_is(1, 32, 2));
    then_quests_are(r, "part sides_e's quests",
                    {{1, 29, 2}, {1, 30, 2}, {1, 31, 2}, {1, 32, 2}, {1, 7, 1}});
    chapter_2_side_end(r, "sides_e");
}
void chapter_2_sides_e_checks(Run &, u64) {}

// Part sides_f (#299): the miner. Jade's 38 gives the supplies (item 87); 9.7's portal to 9.9
// opens while 38 is active; the climb through 9.9 to 9.10, where the Miner (Miner2) ends 39;
// Jade ends 38. Then the SnowQueen's 5: item 72 from mob 44 (level 108) in area 10 (9.7's
// other portal); and back in area 4, Elma ends 21 and Humanoid2 7.
void chapter_2_sides_f(Run &r, u64) {
    using namespace quest_bot;
    chapter_2_side_start(r, "sides_f", 9);
    const FarmStyle fight = chapter_2_fight();
    then_farm(r, "item 72 from mob 44 on 10.1 (side 5)", 10, 1, {44}, ready(1, 5), 3600, fight);
    then_talk_to(r, "SnowQueen: side 5 done", 9, 0, "SnowQueen", 1, quest_is(1, 5, 2), 1200);
    then_talk_to(r, "Elma: side 21 done", 4, 0, "Elma", -1, quest_is(1, 21, 2), 1200);
    then_talk_to(r, "Humanoid2: side 7 done", 4, 1, "Humanoid2", 4, quest_is(1, 7, 2));
    then_quests_are(r, "part sides_f's quests", {{1, 5, 2}, {1, 7, 2}, {1, 21, 2}});
    chapter_2_side_end(r, "sides_f");
}
void chapter_2_sides_f_checks(Run &, u64) {}

// Jade's 38 and the Miner's 39: the climb of 9.9 and 9.10 (#299, synthetic: tests/starts/raise/
// chapter_2_climb.json over part sides_f's save gives the Snowshoes' materials). Their steps are
// 40 px of air apart: the running jumps across land a few px from the far step's end, and on
// area 9's ice the landing slid the hero off it (9.10's platform 28, 360 tries in 1,800 s).
// Snowshoes (shoes 52, a chapter-2 recipe: 3 of item 93, 8 of 94, 2 of 99) turn the slide off,
// so the hero crafts and wears them first, as a player would.
void chapter_2_climb(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2, the Snowshoes' materials", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 2 && has_item(93, 3)(cpu) && has_item(94, 8)(cpu) &&
               has_item(99, 2)(cpu);
    });
    then_spend_sp(r, "climb", k_chapter_2_build);
    then_bag(r, "climb's start");
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1,
                 [](mt_cpu *cpu) { return hero(cpu).on(2, 0); });
    then_craft(r, "smith: the Snowshoes (shoes 52)", 52, {5, 2});
    then_equip(r, "the Snowshoes", 52, 4);
    then_check(r, "the Snowshoes worn", [](mt_cpu *cpu) { return worn(cpu, 4) == 52; });
    then_talk_to(
        r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1,
        [](mt_cpu *cpu) { return hero(cpu).on(4, 1); }, 900);
    then_talk_to(r, "Jade: side 38 given, item 87", 9, 0, "Jade", 3,
                 all_of({quest_is(1, 38, 1), has_item(87, 1)}));
    then_go(r, "the climb: 9.7 to 9.9 to 9.10", at_object(9, 10, "Miner2", 6), 1800);
    then_run_event(r, "9.7's portal to 9.9 (side 38 active)", MT_GAME_EVENT_MAP_CHANGE, 9, {9});
    then_run_event(r, "9.9's top to 9.10", MT_GAME_EVENT_MAP_CHANGE, 9, {10});
    then_talk_to(r, "Miner2 (9.10): side 39 done (item 87)", 9, 10, "Miner2", -1,
                 quest_is(1, 39, 2));
    then_talk_to(r, "Jade: side 38 done", 9, 0, "Jade", 3, quest_is(1, 38, 2), 1200);
    then_quests_are(r, "part climb's quests", {{1, 38, 2}, {1, 39, 2}});
    // Every chapter-2 side quest but 16 (the 2nd job, taken in chapter 1 by the chain's hero:
    // jobs_2_2_* and chapter_2_side_16_full) is done once the chain gets here; 42 is the 3rd job
    // (part B).
    std::vector<std::tuple<u32, u32, i32>> sides;
    for (const u32 n :
         {0u,  1u,  2u,  3u,  4u,  5u,  6u,  7u,  9u,  10u, 11u, 12u, 13u, 18u, 19u, 20u, 21u, 22u,
          23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u, 34u, 35u, 36u, 37u, 38u, 39u, 42u}) {
        sides.emplace_back(1u, n, 2);
    }
    then_quests_are(r, "chapter 2's 35 side quests but 16", sides);
    chapter_2_side_end(r, "climb");
}
void chapter_2_climb_checks(Run &, u64) {}

// Side 16, the 2nd job, in chapter 2 with a full equipment bag (#299, synthetic:
// tests/starts/raise/chapter_2_side_16_full.json over starts.jobs_ch2_rogue_30, the Rogue at
// level 30 on 0.0 of #302's jobs_2_2_* parts, with all 48 equipment slots used). The Darklord's
// list 6 (and 7) then ends in the bag-full notice (invenFullNotify, popup 8) and changes nothing.
// Lists 6 and 7 themselves are play.jobs_2_2_assassin_synthetic and _bandit_synthetic.
void chapter_2_side_16_full(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 2, a Rogue at level 30, 48 pieces of equipment", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 2 && job_at(0, 1)(cpu) && equip_count(cpu, h, 48) == 48;
    });
    then_talk_to(
        r, "Darklord (0.0): side 16, list 6, the bag full", 0, 0, "Darklord", std::vector<int>{6},
        [](mt_cpu *) { return true; }, 600, 1);
    then_do(r, "the bag full: no job, no reward", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        int jobs = 0;
        for (const mt_game_event &e : g_run_events) {
            jobs += e.kind == static_cast<u32>(MT_GAME_EVENT_JOB_ADVANCE) ? 1 : 0;
        }
        check(job_at(0, 1)(cpu) && quest_state(cpu, h, 1, 16) != 2 &&
                  equip_owned(cpu, h, 40) == 0 && jobs == 0,
              "branch " + std::to_string(rdi(cpu, h.addr + 0x88)) + " tier " +
                  std::to_string(rdi(cpu, h.addr + 0x8C)) +
                  ", side 16:" + std::to_string(quest_state(cpu, h, 1, 16)) + ", equipment 40: " +
                  std::to_string(equip_owned(cpu, h, 40)) + ", job events " + std::to_string(jobs));
    });
    then_finish(r);
}
void chapter_2_side_16_full_checks(Run &, u64) {}

// --- chapter 3's side quests, the climb of 6.1 and 6.2, crafts and trades (#301) ---------------
//
// From part D's save (chapter 3 on 9.0, main 7 active, the 4th job), in parts by place
// (docs/testing/chapter-3-quests.md). Each NPC's list value for a quest is the row its script
// gives it (regSelList): BrokerHan (6.0) 1 = 2, 3 = 3, 16 = 5, the trades 6; ZenumistChief (7.0)
// 5 = 6, 6 = 7, 8 = 8, 9 = 9, 13 = 10, 14 = 11; AlchadnoChief (8.0) 2 = 2, 17 = 3, 18 = 4;
// Humanoid3 (8.4) 10 = 2, 11 = 3; the smith (0.0) 12 = 1, the Craft 2; Alchester (9.0) 7 = 2,
// 26 = 3, 27 = 4, 28 = 5, 30 = 6, 31 = 7; Skadru (9.0) 20 = 1, 28 = 2, 29 = 3; the SnowQueen
// (9.0) 22 = 2, 23 = 3, 24 = 4; the Miner (9.0) 26 = 1; remem2 (6.2) has no list.

// The side quests' items: then_sell keeps them.
const std::vector<i32> k_chapter_3_side_items = {73,  74,  75,  76,  77,  78,  79,  80,
                                                 81,  82,  83,  84,  85,  117, 118, 119,
                                                 121, 122, 123, 124, 125, 126, 127};

// The common start of a side part of chapter 3: the SP, the bag's materials sold and the
// potions stocked at the shop of `area` (6 or 9).
void chapter_3_side_start(Run &r, const std::string &part, int area) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 3", [](mt_cpu *cpu) { return hero(cpu).chapter == 3; });
    then_spend_sp(r, part, k_chapter_2_build);
    const std::string shop = area == 9 ? "Etc90" : "Etc60";
    then_sell(r, "materials (" + shop + ")", area, 4, k_chapter_3_side_items);
    then_stock(r, "item 4 (" + shop + ")", area, 4, 200, 500, 600);
    then_stock(r, "item 7 (" + shop + ")", area, 7, 80, 500, 600);
    then_bag(r, part + "'s start");
}

Pred on_map(int area, int map) {
    return [area, map](mt_cpu *cpu) { return hero(cpu).on(area, map); };
}

// Part sides_a (#301): from part D's save on 9.0, to Magatia by Pirate_Orbis (4.1, list 2).
// BrokerHan's 3 (while 2 is active: 10,000 meso), 1 (his path: 15,000 meso) and 16 (his path: 30
// of mob 56 and 10,000 meso); AlchadnoChief ends 2 (side 4 is done) and gives 17;
// ZenumistChief's 5 (25 of item 83 from mob 58 on 7.2), then 6, which wants item 121: side 7's
// reward, for item 127 (mob 57 on 7.5, one drop in ten). 17: 30 of item 78 from mob 61 on 8.1.
void chapter_3_sides_a(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "sides_a", 9);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Pirate_Orbis (4.1): to 6.0 (list 2)", 4, 1, "Pirate_Orbis", 2, on_map(6, 0),
                 1200);
    then_talk_to(r, "BrokerHan: side 3 given", 6, 0, "BrokerHan", 3, quest_is(1, 3, 1));
    then_talk_to(r, "BrokerHan: side 3 done (10,000 meso)", 6, 0, "BrokerHan", 3,
                 quest_is(1, 3, 2));
    then_talk_to(r, "BrokerHan: side 1 given (his path)", 6, 0, "BrokerHan", 2, quest_is(1, 1, 1));
    then_talk_to(r, "BrokerHan: side 1 done (15,000 meso)", 6, 0, "BrokerHan", 2,
                 quest_is(1, 1, 2));
    then_talk_to(r, "BrokerHan: side 16 given (his path)", 6, 0, "BrokerHan", 5,
                 quest_is(1, 16, 1));
    then_talk_to(r, "AlchadnoChief: side 2 done", 8, 0, "AlchadnoChief", 2, quest_is(1, 2, 2));
    then_talk_to(r, "AlchadnoChief: side 17 given", 8, 0, "AlchadnoChief", 3, quest_is(1, 17, 1));
    then_talk_to(r, "ZenumistChief: side 5 given", 7, 0, "ZenumistChief", 6, quest_is(1, 5, 1));
    then_farm(r, "30 of mob 56 and 25 of item 83 (mob 58) on 7.2 (sides 16, 5)", 7, 2, {56, 58},
              all_of({ready(1, 16), ready(1, 5)}), 3600, fight);
    then_farm(r, "item 127 from mob 57 on 7.5 (side 7)", 7, 5, {57}, has_item(127, 1), 3600, fight);
    then_talk_to(r, "BrokerHan: side 16 done (10,000 meso)", 6, 0, "BrokerHan", 5,
                 quest_is(1, 16, 2));
    then_talk_to(r, "ZenumistChief: side 5 done", 7, 0, "ZenumistChief", 6, quest_is(1, 5, 2));
    then_talk_to(r, "ZenumistChief: side 6 given", 7, 0, "ZenumistChief", 7, quest_is(1, 6, 1));
    then_farm(r, "30 of item 78 from mob 61 on 8.1 (side 17)", 8, 1, {61}, ready(1, 17), 3600,
              fight);
    then_talk_to(r, "AlchadnoChief: side 17 done", 8, 0, "AlchadnoChief", 3, quest_is(1, 17, 2));
    then_talk_to(r, "AlchadnoChief: side 18 given", 8, 0, "AlchadnoChief", 4, quest_is(1, 18, 1));
    then_quests_are(r, "part sides_a's quests",
                    {{1, 1, 2},
                     {1, 2, 2},
                     {1, 3, 2},
                     {1, 5, 2},
                     {1, 6, 1},
                     {1, 16, 2},
                     {1, 17, 2},
                     {1, 18, 1}});
    chapter_2_side_end(r, "sides_a");
}
void chapter_3_sides_a_checks(Run &, u64) {}

// Part sides_b (#301): Alchester (9.0) gives 7 and ends it at once for item 127, with item 121,
// which ZenumistChief's 6 takes; then his 8 (30 of item 80 from mob 59 on 8.3) and 9, which
// wants Humanoid3's item 122 (side 11). Humanoid3 gives 10 while 9 is active (30 of item 79 from
// mob 60 on 8.2, for item 124) and then 11, which wants item 125 from the smith (side 12).
void chapter_3_sides_b(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "sides_b", 6);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Pirate_Magatia (6.0): to 4.1 (list 1)", 6, 0, "Pirate_Magatia", 1,
                 on_map(4, 1));
    then_talk_to(r, "Alchester (9.0): side 7 given", 9, 0, "Alchester", 2, quest_is(1, 7, 1), 1200);
    then_talk_to(r, "Alchester: side 7 done (item 127), item 121", 9, 0, "Alchester", 2,
                 all_of({quest_is(1, 7, 2), has_item(121, 1)}));
    then_talk_to(r, "Pirate_Orbis (4.1): to 6.0 (list 2)", 4, 1, "Pirate_Orbis", 2, on_map(6, 0),
                 1200);
    then_talk_to(r, "ZenumistChief: side 6 done (item 121)", 7, 0, "ZenumistChief", 7,
                 quest_is(1, 6, 2));
    then_talk_to(r, "ZenumistChief: side 8 given", 7, 0, "ZenumistChief", 8, quest_is(1, 8, 1));
    then_farm(r, "30 of item 80 from mob 59 on 8.3 (side 8)", 8, 3, {59}, ready(1, 8), 3600, fight);
    then_talk_to(r, "ZenumistChief: side 8 done", 7, 0, "ZenumistChief", 8, quest_is(1, 8, 2));
    then_talk_to(r, "ZenumistChief: side 9 given", 7, 0, "ZenumistChief", 9, quest_is(1, 9, 1));
    then_talk_to(r, "Humanoid3 (8.4): side 10 given", 8, 4, "Humanoid3", 2, quest_is(1, 10, 1));
    then_farm(r, "30 of item 79 from mob 60 on 8.2 (side 10)", 8, 2, {60}, ready(1, 10), 3600,
              fight);
    then_talk_to(r, "Humanoid3: side 10 done, item 124", 8, 4, "Humanoid3", 2,
                 all_of({quest_is(1, 10, 2), has_item(124, 1)}));
    then_talk_to(r, "Humanoid3: side 11 given", 8, 4, "Humanoid3", 3, quest_is(1, 11, 1));
    then_quests_are(r, "part sides_b's quests",
                    {{1, 6, 2}, {1, 7, 2}, {1, 8, 2}, {1, 9, 1}, {1, 10, 2}, {1, 11, 1}});
    chapter_2_side_end(r, "sides_b");
}
void chapter_3_sides_b_checks(Run &, u64) {}

// Part sides_c (#301): to the smith (0.0) by Pirate_Magatia and Pirate_Orbis: his 12 (item 124
// and 10,000 meso) gives item 125; back by the Pirate (2.0). Humanoid3 ends 11 with item 122,
// ZenumistChief ends 9 and gives 13 (30 of item 81 from mob 62 on 8.4, where AlchadnoChief's 18
// wants 40 of mob 62), ends 13 and gives 14, which wants item 85 (side 34, the climb).
void chapter_3_sides_c(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "sides_c", 6);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Pirate_Magatia (6.0): to 4.1 (list 1)", 6, 0, "Pirate_Magatia", 1,
                 on_map(4, 1));
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1, on_map(2, 0));
    then_talk_to(r, "smith (0.0): side 12 given", 0, 0, "smith", 1, quest_is(1, 12, 1), 900);
    then_talk_to(r, "smith: side 12 done (item 124, 10,000 meso), item 125", 0, 0, "smith", 1,
                 all_of({quest_is(1, 12, 2), has_item(125, 1)}));
    then_talk_to(r, "Pirate (2.0): back to 4.1", 2, 0, "Pirate", -1, on_map(4, 1), 900);
    then_talk_to(r, "Pirate_Orbis (4.1): to 6.0 (list 2)", 4, 1, "Pirate_Orbis", 2, on_map(6, 0));
    then_talk_to(r, "Humanoid3: side 11 done (item 125), item 122", 8, 4, "Humanoid3", 3,
                 all_of({quest_is(1, 11, 2), has_item(122, 1)}), 900);
    then_talk_to(r, "ZenumistChief: side 9 done (item 122)", 7, 0, "ZenumistChief", 9,
                 quest_is(1, 9, 2));
    then_talk_to(r, "ZenumistChief: side 13 given", 7, 0, "ZenumistChief", 10, quest_is(1, 13, 1));
    then_farm(r, "40 of mob 62 and 30 of item 81 on 8.4 (sides 18, 13)", 8, 4, {62},
              all_of({ready(1, 18), ready(1, 13)}), 3600, fight);
    then_talk_to(r, "AlchadnoChief: side 18 done", 8, 0, "AlchadnoChief", 4, quest_is(1, 18, 2));
    then_talk_to(r, "ZenumistChief: side 13 done", 7, 0, "ZenumistChief", 10, quest_is(1, 13, 2));
    then_talk_to(r, "ZenumistChief: side 14 given", 7, 0, "ZenumistChief", 11, quest_is(1, 14, 1));
    then_quests_are(r, "part sides_c's quests",
                    {{1, 9, 2}, {1, 11, 2}, {1, 12, 2}, {1, 13, 2}, {1, 14, 1}, {1, 18, 2}});
    chapter_2_side_end(r, "sides_c");
}
void chapter_3_sides_c_checks(Run &, u64) {}

// Part climb (#301): 6.0's portal to 6.1 opens while side 14 is active. 6.1 and 6.2 are a climb
// to remem2 at 6.2's top, who gives side 34 and ends it on one talk with item 85. The part saves
// on 6.0 with 14 still active, for part side_14 (the Bandit's reward) and side_14_assassin.
void chapter_3_climb(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "climb", 6);
    then_check(r, "side 14 active, no item 85", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return quest_state(cpu, h, 1, 14) == 1 && item_count(cpu, h, 85) == 0;
    });
    then_talk_to(r, "remem2 (6.2): the climb, side 34 done, item 85", 6, 2, "remem2", -1,
                 all_of({quest_is(1, 34, 2), has_item(85, 1)}), 1800);
    then_run_event(r, "6.0's portal to 6.1 (side 14 active)", MT_GAME_EVENT_MAP_CHANGE, 6, {1});
    then_run_event(r, "6.1's top to 6.2", MT_GAME_EVENT_MAP_CHANGE, 6, {2});
    then_go(r, "back down to 6.0", at_object(6, 0, "BrokerHan", 6), 900);
    then_quests_are(r, "part climb's quests", {{1, 14, 1}, {1, 34, 2}});
    chapter_2_side_end(r, "climb");
}
void chapter_3_climb_checks(Run &, u64) {}

// ZenumistChief ends side 14 for item 85 with a weapon by the hero's job (getHeroClass): claw 44
// for an Assassin (branch 1), dagger 32 otherwise.
void then_side_14(Run &r, i32 weapon) {
    using namespace quest_bot;
    auto before = std::make_shared<i32>(0);
    then_do(r, "side 14: before",
            [before, weapon](mt_cpu *cpu) { *before = equip_owned(cpu, hero(cpu), weapon); });
    then_talk_to(r, "ZenumistChief: side 14 done (item 85)", 7, 0, "ZenumistChief", 11,
                 quest_is(1, 14, 2), 900);
    then_do(r, "side 14's reward", [before, weapon](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 other = weapon == 44 ? 32 : 44;
        check(equip_owned(cpu, h, weapon) == *before + 1 && item_count(cpu, h, 85) == 0,
              "branch " + std::to_string(rdi(cpu, h.addr + 0x88)) + ": equipment " +
                  std::to_string(weapon) + " " + std::to_string(*before) + " -> " +
                  std::to_string(equip_owned(cpu, h, weapon)) + ", equipment " +
                  std::to_string(other) + " " + std::to_string(equip_owned(cpu, h, other)) +
                  ", item 85 " + std::to_string(item_count(cpu, h, 85)));
    });
}

// Part side_14 (#301): the chain's Bandit (branch 2) hands side 14 in: dagger 32.
void chapter_3_side_14(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "side_14", 6);
    then_side_14(r, 32);
    then_quests_are(r, "part side_14's quests", {{1, 14, 2}, {1, 34, 2}});
    chapter_2_side_end(r, "side_14");
}
void chapter_3_side_14_checks(Run &, u64) {}

// Side 14's other offer (#301, synthetic: tests/starts/raise/chapter_3_side_14_assassin.json over
// part sides_c's save makes the hero an Assassin, branch 1, with side 14 not yet offered). The
// reward is set when ZenumistChief offers the quest (getHeroClass), so the Assassin takes the
// offer, climbs 6.1 and 6.2 for item 85 and hands it in: claw 44.
void chapter_3_side_14_assassin(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "an Assassin, side 14 not offered", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && rdi(cpu, h.addr + 0x88) == 1 && quest_state(cpu, h, 1, 14) == 0;
    });
    then_talk_to(r, "ZenumistChief: side 14 given", 7, 0, "ZenumistChief", 11, quest_is(1, 14, 1));
    then_talk_to(r, "remem2 (6.2): the climb, side 34 done, item 85", 6, 2, "remem2", -1,
                 all_of({quest_is(1, 34, 2), has_item(85, 1)}), 1800);
    then_side_14(r, 44);
    then_quests_are(r, "side 14 done by an Assassin", {{1, 14, 2}, {1, 34, 2}});
    then_finish(r);
}
void chapter_3_side_14_assassin_checks(Run &, u64) {}

// Area 10's side parts (#301, synthetic: tests/starts/raise/chapter_3_sides_10.json over part
// side_14's save raises the hero to level 99 by the game's level-up rules, as part E). Area 10's
// mobs are level 98 to 130; the played level-72 hero of part E fell 23 times there in an hour.
// sides_10_a: Skadru's 20 (30 of item 73 from mob 43 on 10.0) and the SnowQueen's 22 (30 of mob
// 43), 23 (33 of mob 44 on 10.1) and 24 (30 each of item 75, mob 33 on 10.2, and item 123, mob
// 37 on 10.1).
void chapter_3_sides_10_a(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 3, level 99", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.level == 99;
    });
    then_spend_sp(r, "sides_10_a", k_chapter_2_build);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Pirate_Magatia (6.0): to 4.1 (list 1)", 6, 0, "Pirate_Magatia", 1,
                 on_map(4, 1));
    then_sell(r, "materials (Etc90)", 9, 4, k_chapter_3_side_items, 1200);
    then_stock(r, "item 4 (Etc90)", 9, 4, 200, 500, 600);
    then_stock(r, "item 7 (Etc90)", 9, 7, 80, 500, 600);
    then_talk_to(r, "Skadru (9.0): side 20 given", 9, 0, "Skadru", 1, quest_is(1, 20, 1));
    then_talk_to(r, "SnowQueen (9.0): side 22 given", 9, 0, "SnowQueen", 2, quest_is(1, 22, 1));
    then_farm(r, "30 of mob 43 and 30 of item 73 on 10.0 (sides 22, 20)", 10, 0, {43},
              all_of({ready(1, 22), ready(1, 20)}), 3600, fight);
    then_talk_to(r, "Skadru: side 20 done", 9, 0, "Skadru", 1, quest_is(1, 20, 2), 900);
    then_talk_to(r, "SnowQueen: side 22 done", 9, 0, "SnowQueen", 2, quest_is(1, 22, 2));
    then_talk_to(r, "SnowQueen: side 23 given", 9, 0, "SnowQueen", 3, quest_is(1, 23, 1));
    then_farm(r, "33 of mob 44 on 10.1 (side 23)", 10, 1, {44}, ready(1, 23), 3600, fight);
    then_talk_to(r, "SnowQueen: side 23 done", 9, 0, "SnowQueen", 3, quest_is(1, 23, 2), 900);
    then_talk_to(r, "SnowQueen: side 24 given", 9, 0, "SnowQueen", 4, quest_is(1, 24, 1));
    then_farm(r, "30 of item 123 from mob 37 on 10.1 (side 24)", 10, 1, {37}, has_item(123, 30),
              3600, fight);
    then_farm(r, "30 of item 75 from mob 33 on 10.2 (side 24)", 10, 2, {33}, ready(1, 24), 3600,
              fight);
    then_talk_to(r, "SnowQueen: side 24 done", 9, 0, "SnowQueen", 4, quest_is(1, 24, 2), 900);
    then_quests_are(r, "part sides_10_a's quests",
                    {{1, 20, 2}, {1, 22, 2}, {1, 23, 2}, {1, 24, 2}});
    chapter_2_side_end(r, "sides_10_a");
}
void chapter_3_sides_10_a_checks(Run &, u64) {}

// Part sides_10_b (#301, synthetic: from sides_10_a's save, the raised hero): the Miner's 26
// (after 24), ended by Alchester; his 27 (30 of item 74 from mob 29 on 10.2) and 28, ended by
// Skadru; Skadru's 29 (60 of item 76 from mob 54 on 10.2); Alchester's 30 (60 of mob 54).
void chapter_3_sides_10_b(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "sides_10_b", 9);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Miner (9.0): side 26 given", 9, 0, "Miner", 1, quest_is(1, 26, 1));
    then_talk_to(r, "Alchester: side 26 done", 9, 0, "Alchester", 3, quest_is(1, 26, 2));
    then_talk_to(r, "Alchester: side 27 given", 9, 0, "Alchester", 4, quest_is(1, 27, 1));
    then_farm(r, "30 of item 74 from mob 29 on 10.2 (side 27)", 10, 2, {29}, ready(1, 27), 3600,
              fight);
    then_talk_to(r, "Alchester: side 27 done", 9, 0, "Alchester", 4, quest_is(1, 27, 2), 900);
    then_talk_to(r, "Alchester: side 28 given", 9, 0, "Alchester", 5, quest_is(1, 28, 1));
    then_talk_to(r, "Skadru: side 28 done", 9, 0, "Skadru", 2, quest_is(1, 28, 2));
    then_talk_to(r, "Skadru: side 29 given", 9, 0, "Skadru", 3, quest_is(1, 29, 1));
    then_farm(r, "60 of item 76 from mob 54 on 10.2 (side 29)", 10, 2, {54}, ready(1, 29), 3600,
              fight);
    then_talk_to(r, "Skadru: side 29 done", 9, 0, "Skadru", 3, quest_is(1, 29, 2), 900);
    then_talk_to(r, "Alchester: side 30 given", 9, 0, "Alchester", 6, quest_is(1, 30, 1));
    then_farm(r, "60 of mob 54 on 10.2 (side 30)", 10, 2, {54}, ready(1, 30), 3600, fight);
    then_talk_to(r, "Alchester: side 30 done", 9, 0, "Alchester", 6, quest_is(1, 30, 2), 900);
    then_quests_are(r, "part sides_10_b's quests",
                    {{1, 26, 2}, {1, 27, 2}, {1, 28, 2}, {1, 29, 2}, {1, 30, 2}});
    chapter_2_side_end(r, "sides_10_b");
}
void chapter_3_sides_10_b_checks(Run &, u64) {}

// Part sides_10_c (#301, synthetic: from sides_10_b's save): Alchester's 31 (60 of item 77 from
// mob 55 on 10.3, level 130), then every side quest of chapter 3 the chain can take.
void chapter_3_sides_10_c(Run &r, u64) {
    using namespace quest_bot;
    chapter_3_side_start(r, "sides_10_c", 9);
    const FarmStyle fight = chapter_3_fight();
    then_talk_to(r, "Alchester: side 31 given", 9, 0, "Alchester", 7, quest_is(1, 31, 1));
    then_farm(r, "60 of item 77 from mob 55 on 10.3 (side 31)", 10, 3, {55}, ready(1, 31), 7200,
              fight);
    then_talk_to(r, "Alchester: side 31 done", 9, 0, "Alchester", 7, quest_is(1, 31, 2), 900);
    then_quests_are(r, "part sides_10_c's quests", {{1, 31, 2}});
    // Every side quest of chapter 3 the chain can take (the knowledge base's 33 but 42, the 3rd
    // job: the chain's hero took it in chapter 2).
    then_quests_are(r, "chapter 3's side quests but 42",
                    {{1, 0, 2},  {1, 1, 2},  {1, 2, 2},  {1, 3, 2},  {1, 4, 2},  {1, 5, 2},
                     {1, 6, 2},  {1, 7, 2},  {1, 8, 2},  {1, 9, 2},  {1, 10, 2}, {1, 11, 2},
                     {1, 12, 2}, {1, 13, 2}, {1, 14, 2}, {1, 15, 2}, {1, 16, 2}, {1, 17, 2},
                     {1, 18, 2}, {1, 20, 2}, {1, 22, 2}, {1, 23, 2}, {1, 24, 2}, {1, 25, 2},
                     {1, 26, 2}, {1, 27, 2}, {1, 28, 2}, {1, 29, 2}, {1, 30, 2}, {1, 31, 2},
                     {1, 34, 2}, {1, 58, 2}});
    chapter_2_side_end(r, "sides_10_c");
}
void chapter_3_sides_10_c_checks(Run &, u64) {}

// Part crafts (#301, synthetic: tests/starts/raise/chapter_3_crafts.json over part D's save, the
// materials and meso every recipe and trade of chapter 3 needs, and an empty equipment bag for
// the 18 results). Every trade of chapter 3 once: Ericsson (4.0, lists 1-4: in chapter 3 he has no
// quest, so his trades open at once), BrokerHan (6.0, list 6, then 1-3) and the pig (1.0, lists
// 1-3); then every recipe of chapter 3's smith (0.0) once, the kind, then the row (with no side 12
// to offer, his quest list is skipped and the kinds show at once), each against the knowledge
// base's recipe (then_craft).
void chapter_3_crafts(Run &r, u64) {
    using namespace quest_bot;
    from_save(r);
    then_check(r, "chapter 3 on 9.0", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        return h.chapter == 3 && h.on(9, 0);
    });
    then_bag(r, "the crafts' start");
    then_trade(r, "Ericsson: trade 1, 50 of item 54 for 3 of item 94", 4, 0, "Ericsson", {1}, 54,
               94);
    then_trade(r, "Ericsson: trade 2, 50 of item 50 for 3 of item 100", 4, 0, "Ericsson", {2}, 50,
               100);
    then_trade(r, "Ericsson: trade 3, 50 of item 66 for 3 of item 95", 4, 0, "Ericsson", {3}, 66,
               95);
    then_trade(r, "Ericsson: trade 4, 50 of item 68 for 3 of item 101", 4, 0, "Ericsson", {4}, 68,
               101);
    then_talk_to(r, "Pirate_Orbis (4.1): to 6.0 (list 2)", 4, 1, "Pirate_Orbis", 2, on_map(6, 0));
    then_trade(r, "BrokerHan: trade 1, 50 of item 79 for 3 of item 96", 6, 0, "BrokerHan", {6, 1},
               79, 96);
    then_trade(r, "BrokerHan: trade 2, 50 of item 81 for 3 of item 102", 6, 0, "BrokerHan", {6, 2},
               81, 102);
    then_trade(r, "BrokerHan: trade 3, 50 of item 74 for 3 of item 97", 6, 0, "BrokerHan", {6, 3},
               74, 97);
    then_talk_to(r, "Pirate_Magatia (6.0): to 4.1 (list 1)", 6, 0, "Pirate_Magatia", 1,
                 on_map(4, 1));
    then_talk_to(r, "Pirate_Orbis (4.1): to 2.0 (list 1)", 4, 1, "Pirate_Orbis", 1, on_map(2, 0));
    then_trade(r, "pig: trade 1, 50 of item 103 for 3 of item 98", 1, 0, "pig", {1}, 103, 98);
    then_trade(r, "pig: trade 2, 50 of item 42 for 3 of item 93", 1, 0, "pig", {2}, 42, 93);
    then_trade(r, "pig: trade 3, 50 of item 112 for 3 of item 99", 1, 0, "pig", {3}, 112, 99);
    // The smith's rows in each kind are its recipes by result id (the knowledge base's order).
    const std::vector<std::tuple<const char *, i32, int, int>> recipes = {
        {"hat 7", 7, 1, 1},        {"hat 9", 9, 1, 2},        {"hat 11", 11, 1, 3},
        {"overall 19", 19, 2, 1},  {"overall 21", 21, 2, 2},  {"overall 23", 23, 2, 3},
        {"dagger 31", 31, 3, 1},   {"dagger 33", 33, 3, 2},   {"dagger 35", 35, 3, 3},
        {"glove 43", 43, 4, 1},    {"glove 45", 45, 4, 2},    {"glove 47", 47, 4, 3},
        {"shoes 55", 55, 5, 1},    {"shoes 57", 57, 5, 2},    {"shoes 59", 59, 5, 3},
        {"earrings 67", 67, 6, 1}, {"earrings 69", 69, 6, 2}, {"earrings 71", 71, 6, 3}};
    auto made = std::make_shared<int>(0);
    for (const auto &[name, item, kind, row] : recipes) {
        then_craft(r, std::string("smith: ") + name, item, {kind, row}, 3);
        then_do(r, std::string("smith: ") + name + ": side 77", [made](mt_cpu *cpu) {
            ++*made;
            check(quest_state(cpu, hero(cpu), 1, 77) == 2,
                  "the Craft is side 77, state " +
                      std::to_string(quest_state(cpu, hero(cpu), 1, 77)));
        });
    }
    then_do(r, "every recipe of chapter 3", [made](mt_cpu *) {
        int kb = 0;
        for (const world_map::Recipe &c : world_map::recipes()) {
            kb += c.chapter == 3 ? 1 : 0;
        }
        check(*made == kb && kb == 18, "crafted " + std::to_string(*made) +
                                           " recipes; the knowledge base has " +
                                           std::to_string(kb) + " in chapter 3");
    });
    then_bag(r, "the crafts' end");
    then_save(r);
    then_finish(r);
}
void chapter_3_crafts_checks(Run &, u64) {}

// --- map pages in the running port (#221, docs/compendium/kb.md) --------------------------------
//
// map_page: the compendium's map pages say where each portal leads, and when (the Connections
// table). From a built start state (MT_FROM_SAVE, tests/starts/map_page_*.json) the hero walks
// to the portal object of kind MT_MAP_PAGE_PORTAL (0-3: 3 is a hidden portal, docs/formats/
// obj.md) on the save's map and holds Up there for 8 s. OUT_DIR/map_page.txt gets one line:
//   from <area>.<map> chapter <c> portal <kind> <name> landed <area>.<map> at <x> <y>
// or "... landed none" when no map change came. tests/test_compendium_world_port.py compares
// it with the page's row for that portal: an "always" row names the map the port went to; a
// conditional row either names it or the run stayed put (its condition unmet in the start).
struct MapPageRun {
    int area = -1, map = -1, chapter = -1, kind = -1;
    std::string name;
    int x = 0, y = 0;
    bool placed = false; // the start puts the hero beside the portal: no route there
    bool left = false;
    int to_area = -1, to_map = -1, to_x = 0, to_y = 0;
};
std::shared_ptr<MapPageRun> g_map_page;

int map_page_setup(Run &r) {
    g_map_page = std::make_shared<MapPageRun>();
    MapPageRun &k = *g_map_page;
    const char *env = std::getenv("MT_MAP_PAGE_PORTAL");
    if (env == nullptr || std::sscanf(env, "%d", &k.kind) != 1 || k.kind < 0 || k.kind > 3) {
        std::printf("FAIL: MT_MAP_PAGE_PORTAL must name a portal kind, 0 to 3\n");
        return 1;
    }
    const std::vector<std::uint8_t> theif = read_save(r, "theif0");
    const std::vector<std::uint8_t> quest = read_save(r, "quest0");
    if (theif.size() != 1124 || quest.empty()) {
        std::printf("FAIL: MT_FROM_SAVE gave no theif0 and quest0\n");
        return 1;
    }
    k.area = theif[0x41C]; // docs/formats/save.md
    k.map = theif[0x41D];
    k.chapter = quest[0];
    k.placed = theif[0x41E] != 0xFF || theif[0x41F] != 0xFF; // x -1: the map's default spawn
    // The last object of that kind, as the map change 0x868D5 picks an arrival point.
    for (const world_map::Object &o : world_map::get(k.area, k.map).objects) {
        if (o.kind == k.kind) {
            k.name = o.name.empty() ? "-" : o.name;
            k.x = o.x;
            k.y = o.y;
        }
    }
    if (k.name.empty()) {
        std::printf("FAIL: map %d.%d has no portal of kind %d\n", k.area, k.map, k.kind);
        return 1;
    }
    std::printf("map_page: %d.%d chapter %d, portal %d (%s) at (%d, %d)\n", k.area, k.map,
                k.chapter, k.kind, k.name.c_str(), k.x, k.y);
    return 0;
}

void map_page(Run &r, u64) {
    const MapPageRun &k = *g_map_page;
    from_save(r);
    then_wait_field(r);
    if (!k.placed) {
        quest_bot::then_go(r, "portal " + std::to_string(k.kind) + " (" + k.name + ")",
                           {k.area, k.map, k.x, k.y, 4}, 180);
    }
    then(r, "Up at the portal (8 s)", 30, [&r](mt_cpu *cpu, double el) {
        MapPageRun &m = *g_map_page;
        const Hero h = hero(cpu);
        if (!m.left && h.addr != 0 && !h.on(m.area, m.map) && h.area >= 0) {
            m.left = true; // the first turn on the new map: where the warp put the hero
            m.to_area = h.area;
            m.to_map = h.map;
            m.to_x = h.x;
            m.to_y = h.y;
        }
        if (m.left) {
            release_all(r);
            return true;
        }
        if (!field(cpu)) { // a notice or a chat the portal's script opened: A through it
            hold(r, UP, false);
            hold(r, A, std::fmod(el, 0.8) < 0.1);
        } else {
            // A placed start steps the last pixels on its own platform (the portal's tile is
            // within 8 px: obj.md); Up once there.
            const int d = m.x - h.x;
            hold(r, A, false);
            hold(r, RIGHT, m.placed && d > 4);
            hold(r, LEFT, m.placed && d < -4);
            hold(r, UP, std::abs(d) <= 6 && std::fmod(el, 1.5) < 0.4);
        }
        if (el >= 8) {
            release_all(r);
            return true;
        }
        return false;
    });
    // A shut portal's script can leave a notice open (popup 17): A closes it.
    then(r, "the field (A through any notice)", 20,
         [&r](mt_cpu *cpu, double el) { return !quest_bot::interrupted(r, cpu, el); });
    then_do(r, "the portal's outcome", [&r](mt_cpu *cpu) {
        const MapPageRun &m = *g_map_page;
        char line[256];
        if (m.left) {
            std::snprintf(line, sizeof line,
                          "from %d.%d chapter %d portal %d %s landed %d.%d at %d %d\n", m.area,
                          m.map, m.chapter, m.kind, m.name.c_str(), m.to_area, m.to_map, m.to_x,
                          m.to_y);
        } else {
            std::snprintf(line, sizeof line, "from %d.%d chapter %d portal %d %s landed none\n",
                          m.area, m.map, m.chapter, m.kind, m.name.c_str());
        }
        std::printf("map_page: %s", line);
        std::ofstream(r.out / "map_page.txt", std::ios::trunc) << line;
        check(field(cpu) && hero(cpu).hp > 0, "the hero is alive in the field afterwards");
    });
    then_finish(r);
}
void map_page_checks(Run &, u64) {}

// --- Korean (#303, docs/testing/systems-sweep-1.md) ----------------------------------------------
//
// korean_*: MAPLETHIEF_LANGUAGE=ko. The terms are agreed by the pad (the data directory has no
// CTerms), then a part of the game. Every text the game draws reaches the CG text observer
// (mt_cg_set_text_observer, after the draw): a character no font has a glyph for (where the
// device drew a box) or a single line running past its bitmap (cut) fails the run, and so does
// a dialogue line that is not whole UTF-8 (Text_Wrap cut a character). Checkpoints: the screen
// after the terms, the first field, each NPC chat's first and last page (a quest offer's Accept
// is on its last), each shop; each goes to OUT_DIR/ko_<n>_<name>.png and its FNV-1a with the
// texts drawn since the checkpoint before to OUT_DIR/checkpoints.txt. The screens are compared
// with our own recorded renders: MT_KOREAN_BASE=<an earlier run's OUT_DIR> (CTest: a directory
// kept beside the outputs, filled on the first run) makes every checkpoint's screen equal the
// recorded one's, the run being deterministic on the virtual clock.

void then_character_select(Run &r); // below, with the slots

struct KoreanRun {
    u64 draws = 0, hangul = 0, missing = 0, clipped = 0, box_lines = 0, broken = 0;
    std::set<std::string> since; // texts drawn since the last checkpoint
    std::string last_box;
    std::string key; // a text a box must show (the English line of the key ko.lproj lacks)
    u64 key_shown = 0, raw_key = 0; // how often a box showed it, and the key itself
    u64 key_cut = 0;                // boxes whose lines, joined, are the key, more than one line
    std::string key_lines;          // the last such box's lines, '|' after each
    std::map<std::string, std::string> base; // the recorded checkpoints: name -> FNV-1a
    fs::path base_dir;                       // MT_KOREAN_BASE
    bool recording = false;
    std::vector<std::string> names;
    u64 compared = 0, differ = 0;
    std::ofstream file;
    // The automatic checkpoints' state.
    i32 chat_page = -2, chat_pages = 0;
    double chat_since = -1;
    bool shop = false, field_seen = false;
    double shop_since = -1;
};
KoreanRun g_ko;

void korean_observe(void *, const mt_cg_drawn_text *t) {
    KoreanRun &k = g_ko;
    k.draws++;
    const std::string s = t->utf8;
    if (!k.since.insert(s).second) {
        return;
    }
    whole_utf8(s, &k.hangul);
    if (t->missing > 0) {
        k.missing++;
        std::printf("korean: FAIL: %u character(s) without a glyph in \"%s\"\n", t->missing,
                    s.c_str());
    }
    if (t->newlines == 0 && t->right > t->limit + 0.5f) {
        k.clipped++;
        std::printf("korean: FAIL: \"%s\" ends at %.1f points, its bitmap is %.0f: cut\n",
                    s.c_str(), static_cast<double>(t->right), static_cast<double>(t->limit));
    }
}

int korean_setup(Run &r) {
    g_ko = KoreanRun{};
    const char *lang = std::getenv("MAPLETHIEF_LANGUAGE");
    if (lang == nullptr || std::string(lang) != "ko") {
        std::printf("FAIL: korean_* runs with MAPLETHIEF_LANGUAGE=ko\n");
        return 1;
    }
    // The terms are part of the run: a save copied in (MT_FROM_SAVE) brings none.
    fs::remove(r.data / "saves" / "CTerms");
    if (const char *b = std::getenv("MT_KOREAN_BASE"); b != nullptr && *b != 0) {
        g_ko.base_dir = b;
        std::ifstream in(g_ko.base_dir / "checkpoints.txt");
        for (std::string line; std::getline(in, line);) {
            std::istringstream f(line); // "name hash count", then the texts indented
            std::string name, hash;
            if (!line.empty() && line[0] != ' ' && f >> name >> hash) {
                g_ko.base[name] = hash;
            }
        }
        g_ko.recording = g_ko.base.empty();
        std::printf("korean: %zu recorded checkpoint(s) in %s%s\n", g_ko.base.size(), b,
                    g_ko.recording ? ": this run records them" : "");
    }
    g_ko.file.open(r.out / "checkpoints.txt", std::ios::trunc);
    mt_cg_set_text_observer(korean_observe, nullptr);
    return 0;
}

void korean_checkpoint(Run &r, mt_cpu *cpu, const std::string &what) {
    KoreanRun &k = g_ko;
    const std::string name = std::to_string(k.names.size()) + "_" + what;
    k.names.push_back(name);
    u64 hash = 0;
    const bool shot = save_shot(r, cpu, "ko_" + name, &hash);
    check(shot, "korean: checkpoint " + name + " has a screen");
    const std::string h = fe::hex64(hash);
    k.file << name << ' ' << h << ' ' << k.since.size() << '\n';
    for (const std::string &t : k.since) {
        k.file << "  " << t << '\n';
    }
    k.file.flush();
    std::printf("korean: checkpoint %s: %s, %zu text(s) drawn since the last\n", name.c_str(),
                h.c_str(), k.since.size());
    k.since.clear();
    if (const auto it = k.base.find(name); it != k.base.end()) {
        k.compared++;
        if (it->second != h) {
            k.differ++;
            std::printf("korean: FAIL: checkpoint %s's screen %s, recorded %s\n", name.c_str(),
                        h.c_str(), it->second.c_str());
        }
    } else if (!k.base.empty()) {
        k.differ++;
        std::printf("korean: FAIL: checkpoint %s was not recorded\n", name.c_str());
    }
}

// Every turn: the box lines, and the automatic checkpoints (a chat's first and last page once
// shown for 0.5 s, a shop once shown for 1 s, the first field).
void korean_turn(Run &r, mt_cpu *cpu) {
    KoreanRun &k = g_ko;
    const ChatBoxView v = chat_box_view(cpu);
    if (!v.kind.empty() && v.text != k.last_box) {
        k.last_box = v.text;
        if (!k.key.empty() && v.text.find(k.key) != std::string::npos) {
            k.key_shown++;
            std::printf("korean: the box shows the English line (%s ...)\n", k.key.c_str());
        }
        k.raw_key += v.text.find("Chapter3_Quest_Zenumist_Chief") != std::string::npos ? 1 : 0;
        std::string whole;
        for (const char c : v.text) {
            whole += c != '|' ? std::string(1, c) : std::string();
        }
        if (v.lines > 1 && whole == "Chapter3_Quest_Zenumist_Chief.dialog.1") {
            k.key_cut++;
            k.key_lines = v.text;
        }
        for (std::size_t a = 0, b = 0; (b = v.text.find('|', a)) != std::string::npos; a = b + 1) {
            k.box_lines++;
            if (!whole_utf8(v.text.substr(a, b - a))) {
                k.broken++;
                std::printf("korean: FAIL: a %s box line is not whole UTF-8\n", v.kind.c_str());
            }
        }
    }
    if (!k.field_seen && field(cpu)) {
        k.field_seen = true;
        korean_checkpoint(r, cpu, "field");
    }
    const quest_bot::Chat c = quest_bot::chatting(cpu) ? quest_bot::chat(cpu) : quest_bot::Chat{};
    if (c.addr == 0) {
        k.chat_page = -2;
    } else if (c.page != k.chat_page) {
        k.chat_page = c.page;
        k.chat_pages = c.pages;
        k.chat_since = r.t;
    } else if (k.chat_since >= 0 && r.t - k.chat_since >= 0.5) {
        k.chat_since = -1;
        if (c.page == 0 || c.last_page()) {
            korean_checkpoint(r, cpu, c.page == 0 ? "chat" : "chat_end");
        }
    }
    const bool shop = quest_bot::in_shop(cpu);
    if (shop && !k.shop) {
        k.shop_since = r.t;
    } else if (shop && k.shop_since >= 0 && r.t - k.shop_since >= 1.0) {
        k.shop_since = -1;
        korean_checkpoint(r, cpu, "shop");
    }
    k.shop = shop;
}

// The terms by the pad: the focus starts on the agreement, A agrees (CTerms is written).
void then_korean_terms(Run &r) {
    then_wait_for(
        r, "the Korean terms, the focus on the agreement", 20,
        [&r](mt_cpu *) { return focused(r, "releaseAgreementButton:"); }, 1.0);
    then_do(r, "the terms, a checkpoint",
            [&r](mt_cpu *cpu) { korean_checkpoint(r, cpu, "terms"); });
    then_press(r, A);
    then_wait_for(r, "CTerms written", 5, [&r](mt_cpu *) {
        std::error_code ec;
        return fs::file_size(r.data / "saves" / "CTerms", ec) == 2;
    });
}

void korean_attach(Run &r) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        korean_turn(r, cpu);
    };
    then_korean_terms(r);
}

void korean_checks(Run &r, u64) {
    mt_cg_set_text_observer(nullptr, nullptr);
    g_ko.file.close();
    const KoreanRun &k = g_ko;
    std::printf("korean: %llu draws, %llu Hangul syllables in distinct texts, %llu box lines, %zu "
                "checkpoints (%llu compared with the recorded ones)\n",
                static_cast<unsigned long long>(k.draws), static_cast<unsigned long long>(k.hangul),
                static_cast<unsigned long long>(k.box_lines), k.names.size(),
                static_cast<unsigned long long>(k.compared));
    check(k.draws > 0 && k.hangul > 0, "the game drew Korean text");
    check(k.missing == 0, "no drawn character lacks a glyph (no box)");
    check(k.clipped == 0, "no drawn line runs past its bitmap");
    check(k.broken == 0, "every box line is whole UTF-8");
    check(k.box_lines > 0, "boxes were read");
    check(k.differ == 0, "every checkpoint's screen equals the recorded one");
    if (!k.key.empty()) {
        check(k.key_shown > 0, "a box showed the English line of the key ko.lproj lacks");
    }
    // The first run with an empty base records it, if its own checks held.
    if (k.recording && k.missing == 0 && k.clipped == 0 && k.broken == 0 && !k.names.empty()) {
        std::error_code ec;
        fs::create_directories(k.base_dir, ec);
        fs::copy_file(r.out / "checkpoints.txt", k.base_dir / "checkpoints.txt",
                      fs::copy_options::overwrite_existing, ec);
        check(!ec, "korean: recorded " + std::to_string(k.names.size()) + " checkpoints in " +
                       k.base_dir.string());
    }
}

// A built save's slot 0 after the terms: the character select, A on the slot, Start.
void then_korean_load(Run &r) {
    g_play.anywhere = true; // the play starts in the save's field
    then_character_select(r);
    then_press(r, A); // the slot is chosen and the focus goes on to Start (ADR 0102)
    then_wait_for(r, "the focus on Start", 5, [&r](mt_cpu *) { return focused(r, "clickStart:"); });
    then_wait(r, 1.0);
    then_press(r, A);
    then_wait_for(r, "the game", 30, field, 1.0);
}

// Tutorial, chapter 1's part a (Mr. Wang's shop, Stan's and the Dark Lord's quests), in Korean.
void korean_chapter_1_a(Run &r, u64) {
    korean_attach(r);
    chapter_1_a_steps(r);
}

// Chapter 2's first offer (tests/starts/chapter_2.json built in Korean: part L's end): Heracle
// on 4.0 gives Fighting Monsters (main 0), accepted.
void korean_offer_2(Run &r, u64) {
    using namespace quest_bot;
    korean_attach(r);
    then_korean_load(r);
    then_talk_to(r, "Heracle: Fighting Monsters (main 0) offered and taken", 4, 0, "Heracle", -1,
                 main_quest(0, 1));
    then_finish(r);
}

// Chapter 3's first offer (tests/starts/korean_ch3.json): the Zenumist President on 7.0 ends
// main 40 and gives Join the Zenumist Association (main 0), accepted.
void korean_offer_3(Run &r, u64) {
    using namespace quest_bot;
    korean_attach(r);
    then_korean_load(r);
    then_talk_to(r, "the Zenumist President: main 40 done, main 0 offered and taken", 7, 0,
                 "ZenumistChief", -1, main_quests(40, 0));
    then_finish(r);
}

// The key ko.lproj lacks (re/original-bugs.md #15; tests/starts/korean_ch3_key.json): with main
// 0-3 and every side quest he gives done, the Zenumist President's only line is
// Chapter3_Quest_Zenumist_Chief.dialog.1, shown as the field is entered. With fix_original_bugs
// (korean_missing_key) the line is the English table's: the box shows it whole, and never the
// key. Without it (korean_missing_key_original) the box gets the key, one 38-character word
// wider than the box; Text_Wrap's runaway on it (#419) is cut by the wrap guard, so the run
// lives and the box shows the key cut across lines, the first cut after its "C" as the
// device's wrap does, and the lines joined are the key.
void korean_missing_key_run(Run &r, bool fixed) {
    using namespace quest_bot;
    r.settings.fix_original_bugs = fixed;
    if (fixed) {
        const std::vector<std::uint8_t> raw =
            read_file((fs::path(MT_BUNDLE_DIR) / "en.lproj/Scripts.strings").string());
        mt_ns::PlistValue table;
        std::string why;
        const mt_ns::PlistValue *line = nullptr;
        if (mt_ns::plist_parse(raw.data(), raw.size(), &table, &why)) {
            line = table.find(u"Chapter3_Quest_Zenumist_Chief.dialog.1");
        }
        check(line != nullptr && line->kind == mt_ns::PlistValue::string,
              "en.lproj/Scripts.strings has the key ko.lproj lacks");
        // The box's first line: the English text up to its first space.
        const std::string english = line != nullptr ? utf8_of(line->s) : std::string();
        g_ko.key = english.substr(0, english.find(' '));
    }
    korean_attach(r);
    then_korean_load(r);
    if (fixed) {
        then_talk_to(r, "the Zenumist President: the line without a Korean text", 7, 0,
                     "ZenumistChief", -1, [](mt_cpu *) { return g_ko.key_shown > 0; });
        then_do(r, "the key never shown",
                [](mt_cpu *) { check(g_ko.raw_key == 0, "no box showed the key itself"); });
    } else {
        const std::uint64_t guarded = mt_hooks_text_wrap_guarded();
        then_talk_to(r, "the Zenumist President: the key, cut across lines", 7, 0, "ZenumistChief",
                     -1, [](mt_cpu *) { return g_ko.key_cut > 0; });
        then_do(r, "the key cut by the wrap guard", [guarded](mt_cpu *) {
            std::printf("korean: the box's lines: %s\n", g_ko.key_lines.c_str());
            check(g_ko.key_lines.rfind("C|", 0) == 0,
                  "the key's first line is its \"C\", as the device's wrap cuts it");
            check(mt_hooks_text_wrap_guarded() > guarded, "the wrap guard cut the key");
        });
    }
    then_finish(r);
}

void korean_missing_key(Run &r, u64) { korean_missing_key_run(r, true); }
void korean_missing_key_original(Run &r, u64) { korean_missing_key_run(r, false); }

// --- Slots 2 and 3 (#303, docs/testing/systems-sweep-1.md) ---------------------------------------
//
// slots_create, on a data directory holding only a built slot 0 (tests/starts/systems_slot0):
// twice the character select's New (its popup's Yes), the intro and the tutorial (its maps
// cannot be saved on) to Stan in Henesys, Red Potions drunk (one in the second slot, two in the
// third, so the two saves differ), Save on the option tab, and the option tab's exit to the
// main menu. Each slot's map, x and Red Potions go to OUT_DIR/slots.txt. slots_reload, a fresh
// process on the same data directory: each new slot chosen and started, the hero where and as
// it was saved; then both deleted (Delete, its alert's Yes): their files are gone, slot 0's are
// as built.

struct SlotState {
    i32 area = -1, map = -1, x = 0, red = -1;
};
struct SlotsRun {
    std::vector<std::uint8_t> slot0, quest0; // slot 0 as the run found it
};
SlotsRun g_slots;

std::string slot_file(const char *kind, int slot) { return kind + std::to_string(slot); }

// OUT_DIR/slots.txt: "slot area map x red" per saved slot.
std::map<int, SlotState> saved_slots(const Run &r) {
    std::map<int, SlotState> out;
    std::ifstream in(r.out / "slots.txt");
    int slot = 0;
    SlotState s;
    while (in >> slot >> s.area >> s.map >> s.x >> s.red) {
        out[slot] = s;
    }
    return out;
}

int slots_setup(Run &r) {
    g_slots = SlotsRun{};
    g_slots.slot0 = read_save(r, "theif0");
    g_slots.quest0 = read_save(r, "quest0");
    if (g_slots.slot0.size() != 1124) {
        std::printf("skipped: no slot 0 in %s (starts.systems_slot0 first)\n",
                    r.data.string().c_str());
        return 77;
    }
    return 0;
}

// The main menu's Play to the character select (B first on the promo banner, if it shows).
void then_character_select(Run &r) {
    then(r, "the main menu, the focus on Play", 30, [&r](mt_cpu *, double el) {
        bool banner = false;
        for (const fe::FocusTarget &f : r.focus.targets()) {
            banner = banner || f.action == "clickADClose:";
        }
        hold(r, B, banner && std::fmod(el, 1.0) < 0.1);
        return !banner && el > 1.0 && focused(r, "clickPlay:");
    });
    then_press(r, A);
    then_wait_for(
        r, "the character select", 10, [&r](mt_cpu *) { return focused(r, "chooseCharacterOne:"); },
        0.5);
}

// The option tab's exit (item 3), popup 14's Yes: the main menu.
void then_exit_to_menu(Run &r) {
    then_wait_field(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 1 && game_mode(cpu) == 1; });
    then_press(r, LEFT);
    then_wait_for(r, "the option tab", 5, [](mt_cpu *cpu) { return menu_tab(cpu) == 5; });
    for (int i = 0; i < 3; i++) {
        then_press(r, DOWN, 0.1, 0.6);
    }
    then_press(r, A);
    then_wait_for(r, "exit? (popup 14)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 14; });
    then_wait_for(r, "Yes picked", 3, exit_yes_picked);
    then_press(r, A);
    then_wait_for(
        r, "the main menu (the game left)", 20, [&r](mt_cpu *) { return focused(r, "clickPlay:"); },
        1.0);
}

void then_new_slot(Run &r, int slot) {
    then_character_select(r);
    then_focus(r, "clickNew:");
    then_press(r, A);
    then_wait_for(r, "New asks (the engine's popup on panel 7)", 5,
                  [](mt_cpu *cpu) { return mt_input_panel() == 7 && popup_id(cpu) >= 0; });
    then_press(r, A);
    then_wait_for(r, "slot " + std::to_string(slot) + "'s files", 10, [&r, slot](mt_cpu *) {
        return read_save(r, slot_file("theif", slot).c_str()).size() == 1124 &&
               read_save(r, slot_file("quest", slot).c_str()).size() == 4781;
    });
    then_tutorial(r);
    then_check(r, "the game plays slot " + std::to_string(slot),
               [slot](mt_cpu *cpu) { return rdi(cpu, data_mgr(cpu) + 0x4E4) == slot; });
    auto red = std::make_shared<i32>(0);
    then_do(r, "the Red Potions", [red](mt_cpu *cpu) { *red = item_count(cpu, hero(cpu), 3); });
    for (int i = 0; i < slot; i++) {
        then_press(r, Y, 0.1, 1.0); // quick slot 1: a Red Potion
    }
    then_check(r, std::to_string(slot) + " Red Potion(s) drunk",
               [red, slot](mt_cpu *cpu) { return item_count(cpu, hero(cpu), 3) == *red - slot; });
    then_save(r, [&r, slot](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        const i32 red_now = item_count(cpu, h, 3);
        std::printf("slot %d saved: map %d.%d x %d, %d Red Potions\n", slot, h.area, h.map, h.x,
                    red_now);
        std::ofstream(r.out / "slots.txt", std::ios::app)
            << slot << ' ' << h.area << ' ' << h.map << ' ' << h.x << ' ' << red_now << '\n';
    });
    then_exit_to_menu(r);
}

void slots_create(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    g_play.anywhere = true;
    fs::remove(r.out / "slots.txt");
    then_new_slot(r, 1);
    then_new_slot(r, 2);
    then_do(r, "slot 0 untouched", [&r](mt_cpu *) {
        check(read_save(r, "theif0") == g_slots.slot0 && read_save(r, "quest0") == g_slots.quest0,
              "slot 0's files are as built");
        check(saved_slots(r).size() == 2, "two slots saved");
    });
    then_finish(r);
}
void slots_create_checks(Run &, u64) {}

const char *const k_slot_actions[3] = {
    "chooseCharacterOne:", "chooseCharacterTwo:", "chooseCharacterThree:"};

void then_load_slot(Run &r, int slot) {
    then_character_select(r);
    then_focus(r, k_slot_actions[slot]);
    then_press(r, A); // chosen: the focus goes on to Start (ADR 0102)
    then_wait_for(r, "the focus on Start", 5, [&r](mt_cpu *) { return focused(r, "clickStart:"); });
    then_wait(r, 1.0);
    then_press(r, A);
    then_wait_for(r, "the game", 30, field, 1.0);
    then_check(
        r, "slot " + std::to_string(slot) + " loads as it was saved", [&r, slot](mt_cpu *cpu) {
            const Hero h = hero(cpu);
            const SlotState s = saved_slots(r)[slot];
            std::printf("slot %d loaded: map %d.%d x %d, %d Red Potions; saved map %d.%d "
                        "x %d, %d\n",
                        slot, h.area, h.map, h.x, item_count(cpu, h, 3), s.area, s.map, s.x, s.red);
            return rdi(cpu, data_mgr(cpu) + 0x4E4) == slot && h.on(s.area, s.map) &&
                   std::abs(h.x - s.x) <= 8 && item_count(cpu, h, 3) == s.red;
        });
    then_exit_to_menu(r);
}

void then_delete_slot(Run &r, int slot) {
    then_character_select(r);
    then_focus(r, k_slot_actions[slot]);
    then_press(r, A);
    then_focus(r, "clickDelete:");
    then_press(r, A);
    then_wait_for(r, "Delete asks, the focus on No", 5,
                  [&r](mt_cpu *) { return focused(r, "cancelAlert:"); });
    then_focus(r, "okDeleteAlert:");
    then_press(r, A);
    then_wait_for(r, "slot " + std::to_string(slot) + "'s files deleted", 5, [&r, slot](mt_cpu *) {
        std::error_code ec;
        return !fs::exists(r.data / "saves" / slot_file("theif", slot), ec) &&
               !fs::exists(r.data / "saves" / slot_file("quest", slot), ec);
    });
    then_press(r, B); // Back: the main menu
    then_wait_for(r, "the main menu", 5, [&r](mt_cpu *) { return focused(r, "clickPlay:"); }, 0.5);
}

void slots_reload(Run &r, u64) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    g_play.anywhere = true;
    then_load_slot(r, 1);
    then_load_slot(r, 2);
    then_delete_slot(r, 1);
    then_delete_slot(r, 2);
    then_do(r, "slot 0 untouched", [&r](mt_cpu *) {
        check(read_save(r, "theif0") == g_slots.slot0 && read_save(r, "quest0") == g_slots.quest0,
              "slot 0's files are as built");
    });
    then_finish(r);
}
void slots_reload_checks(Run &, u64) {}

// --- A full inventory and the level cap (#303, docs/testing/systems-sweep-1.md) ------------------

// The hero's bag: the etc items' (id, count) by slot and the equipment ids by slot (-1 empty).
struct Bag {
    std::vector<std::pair<i32, i32>> etc;
    std::vector<i32> equip;
    i32 etc_used() const {
        return static_cast<i32>(
            std::count_if(etc.begin(), etc.end(), [](const auto &e) { return e.first >= 0; }));
    }
    i32 equip_used() const {
        return static_cast<i32>(
            std::count_if(equip.begin(), equip.end(), [](i32 id) { return id >= 0; }));
    }
};
Bag bag_of(mt_cpu *cpu) {
    Bag b;
    const Hero h = hero(cpu);
    for (i32 i = 0; i < 48; i++) {
        const EtcItem e = etc_item(cpu, h, i);
        b.etc.emplace_back(e.id, e.count);
    }
    const u32 equips = h.addr != 0 ? rd(cpu, h.addr + 0xD4) : 0;
    for (i32 i = 0; i < 48; i++) {
        const u32 it = equips != 0 ? rd(cpu, equips + 4 * static_cast<u32>(i)) : 0;
        b.equip.push_back(it != 0 ? rdi(cpu, it + 0x90) : -1);
    }
    return b;
}

// inventory_full_synthetic (tests/starts/systems_inventory_full.json): a Bandit on 2.3 with 48
// equipment items and 48 etc stacks, none of them a Red Snail's drop. Hunting there, a drop the
// hero touches is refused (the drop's +0x10C) and stays, and the bag is unchanged; meso still
// comes in. Then Mr. Wang's Sell tab (Henesys): one stack sold, its price times its count paid,
// and the bag's order afterwards.
struct InventoryRun {
    Bag before;
    i32 sold_id = -1, sold_count = 0;
    u64 refused = 0;
};
InventoryRun g_inv;

// Sells the etc stack in bag slot `slot` at Mr. Wang's (Henesys): goes there, up opens the
// shop, RB the Sell tab, down to the row, A through its popups, B closes the shop. Checks that
// the stack left the bag, the meso it paid, and the order of the rest.
void then_sell(Run &r, i32 slot) {
    using namespace quest_bot;
    struct S {
        Nav nav;
        double pressed = -10;
        bool closing = false;
        Bag before;
        i32 meso = 0;
        bool sold = false;
    };
    auto st = std::make_shared<S>();
    const Goal g = shop_goal(2, 3);
    then(r, "sell bag slot " + std::to_string(slot) + " at Mr. Wang's", 300,
         [&r, st, g, slot](mt_cpu *cpu, double el) {
             const auto press = [&](SDL_GamepadButton b) {
                 if (el - st->pressed >= 0.5) {
                     st->pressed = el;
                 }
                 hold(r, b, el - st->pressed < 0.1);
             };
             const Hero h = hero(cpu);
             if (game_mode(cpu) == 2) {
                 hold(r, UP, false);
                 if (!in_shop(cpu)) {
                     press(A); // a popup: how many, sell?, sold
                     return false;
                 }
                 const Shop sh = shop(cpu);
                 if (st->before.etc.empty()) {
                     st->before = bag_of(cpu);
                     st->meso = h.meso;
                     const EtcItem e = etc_item(cpu, h, slot);
                     g_inv.sold_id = e.id;
                     g_inv.sold_count = e.count;
                 }
                 if (!st->closing && bag_of(cpu).etc_used() < st->before.etc_used()) {
                     st->closing = true;
                 }
                 if (st->closing) {
                     press(B);
                 } else if (sh.tab != 1) {
                     press(RB);
                 } else if (sh.selected != slot) {
                     press(sh.selected < slot ? DOWN : UP);
                 } else {
                     press(A);
                 }
                 return false;
             }
             if (interrupted(r, cpu, el)) {
                 return false;
             }
             if (st->closing) {
                 release_all(r);
                 const Bag now = bag_of(cpu);
                 const i32 paid = h.meso - st->meso;
                 std::printf("sold: item %d x %d: %d meso paid; etc stacks %d -> %d\n",
                             g_inv.sold_id, g_inv.sold_count, paid, st->before.etc_used(),
                             now.etc_used());
                 std::string order;
                 for (const auto &[id, count] : now.etc) {
                     order += " " + std::to_string(id);
                 }
                 std::printf("the bag after the sale:%s\n", order.c_str());
                 // The rest in their order, the sold stack's slot taken by the next.
                 std::vector<std::pair<i32, i32>> want = st->before.etc;
                 want.erase(want.begin() + slot);
                 want.emplace_back(-1, 0);
                 check(item_count(cpu, h, g_inv.sold_id) == 0, "the sold stack left the bag");
                 // Slot 10 holds item 45, one of it; item_etc lists its price as 7 meso.
                 check(g_inv.sold_id == 45 && g_inv.sold_count == 1 && paid == 7,
                       "the sale paid item_etc's price, 7 meso, for one item 45");
                 check(now.etc == want, "the rest keep their order, the gap closed");
                 check(now.equip == st->before.equip, "the equipment is unchanged");
                 return true;
             }
             if (!drive(r, cpu, el, st->nav, g)) {
                 return false;
             }
             press(UP);
             return false;
         });
}

void inventory_full(Run &r, u64) {
    using namespace quest_bot;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        for (const Drop &d : drops(cpu)) {
            g_inv.refused += d.refused ? 1 : 0;
        }
    };
    g_play.anywhere = true;
    then_load_slot0(r);
    then_do(r, "the full bag", [](mt_cpu *cpu) {
        g_inv.before = bag_of(cpu);
        std::printf("bag: %d etc stacks, %d equipment items\n", g_inv.before.etc_used(),
                    g_inv.before.equip_used());
        check(g_inv.before.etc_used() == 48 && g_inv.before.equip_used() == 48,
              "the bag is full: 48 etc stacks and 48 equipment items");
    });
    then_farm(
        r, "Red Snails on 2.3 until a drop is refused", 2, 3, {2},
        [](mt_cpu *) { return g_inv.refused > 0; }, 900);
    then_wait(r, 2.0);
    then_do(r, "the refused drop", [](mt_cpu *cpu) {
        const Bag now = bag_of(cpu);
        u64 lying = 0;
        for (const Drop &d : drops(cpu)) {
            lying += d.refused && d.state == 0 ? 1 : 0;
        }
        std::printf("refused drops: %llu turns seen, %llu lying refused\n",
                    static_cast<unsigned long long>(g_inv.refused),
                    static_cast<unsigned long long>(lying));
        check(lying > 0, "a refused drop stays on the ground");
        check(now.etc == g_inv.before.etc && now.equip == g_inv.before.equip,
              "the bag is unchanged by the refused pick-up");
    });
    then_sell(r, 10);
    then_finish(r);
}
void inventory_full_checks(Run &, u64) {}

// level_cap_synthetic (tests/starts/systems_level_99.json): a level-99 Bandit with 71,735 EXP,
// 5 short of level 99's need in hero.dat (71,740), kills Red Snails on 2.3. CHero::setExp
// 0x6654D calls CHero::LevelUp 0x65089 when the need is reached, which returns at once above
// level 98, and then sets EXP to 0: the level stays 99, EXP drops to 0 and counts up again.
// Then the way to Henesys (2.0), a map change, reports the total ranking (99 x 999000 + EXP) /
// 1000 (CMapleWorld::changeMap 0x81369), which gamecenter.json keeps.
struct CapRun {
    u64 kills0 = 0;
    i32 last_exp = -1, resets = 0, reset_to = -1, level_max = 0;
};
CapRun g_cap;

void level_cap(Run &r, u64) {
    using namespace quest_bot;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) {
        monitor(r, cpu);
        const Hero h = hero(cpu);
        if (h.addr == 0) {
            return;
        }
        CapRun &c = g_cap;
        if (c.last_exp >= 0 && h.exp < c.last_exp) {
            c.resets++;
            c.reset_to = h.exp;
            std::printf("level cap: EXP %d -> %d at level %d\n", c.last_exp, h.exp, h.level);
        }
        c.last_exp = h.exp;
        c.level_max = std::max(c.level_max, h.level);
    };
    g_play.anywhere = true;
    then_load_slot0(r);
    then_do(r, "level 99", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        g_cap.kills0 = g_play.kills;
        check(h.level == 99 && h.exp == 71735, "the hero is level 99 with 71,735 EXP");
    });
    then_farm(
        r, "Red Snails on 2.3 at level 99", 2, 3, {2},
        [](mt_cpu *cpu) { return g_cap.resets > 0 && hero(cpu).exp > 0; }, 900);
    then_do(r, "EXP at the cap", [](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        std::printf("level cap: %llu kills, level %d (most %d), EXP %d, %d reset(s) to %d\n",
                    static_cast<unsigned long long>(g_play.kills - g_cap.kills0), h.level,
                    g_cap.level_max, h.exp, g_cap.resets, g_cap.reset_to);
        check(h.level == 99 && g_cap.level_max == 99, "the level stays 99");
        check(g_cap.resets == 1 && g_cap.reset_to == 0,
              "reaching level 99's need sets EXP to 0, once");
        check(h.exp > 0 && h.exp < 71740, "EXP counts up again from 0");
    });
    auto exp = std::make_shared<i32>(-1);
    then_go(r, "Henesys (2.0)", at_object(2, 0, "Stan", 30), 300);
    then_do(r, "the score", [exp](mt_cpu *cpu) {
        static const char *const k_ranking = "com.nexonmobile.maplestory.thief.totalranking";
        *exp = hero(cpu).exp;
        const std::int64_t want = (99ll * 999000 + *exp) / 1000;
        mt_gamekit_entry e[8];
        const u32 n = mt_gamekit_entries(k_ranking, e, 8);
        std::printf("gamecenter.json: %u entr%s", n, n == 1 ? "y" : "ies");
        bool found = false;
        for (u32 i = 0; i < n && i < 8; i++) {
            std::printf("; %s best %lld last %lld (%lld reports)", e[i].player,
                        static_cast<long long>(e[i].best), static_cast<long long>(e[i].last),
                        static_cast<long long>(e[i].reports));
            found = found || e[i].last == want;
        }
        std::printf("\n");
        check(found, "gamecenter.json's last total ranking is " + std::to_string(want) +
                         " (level 99, " + std::to_string(*exp) + " EXP) after the map change");
    });
    then_finish(r);
}
void level_cap_checks(Run &, u64) {}

// --- Quick slots and the trash (#303, docs/testing/systems-sweep-1.md) ---------------------------
//
// quick_slots_trash_synthetic (tests/starts/systems_carry.json): a Bandit with Double Step
// (skill 3) and Nimble Body (4) learned, by the pad only through the port's pick up and drop
// (ADR 0129, #140): Double Step picked up on the skill tab and dropped on quick slot 3, then
// used from it in the field (LB: MP paid); a stack of Snail Shells taken to the trash, the
// game's popup 20 declined (B: the shells stay), then taken again and accepted (A: gone).

constexpr i32 k_double_step = 3, k_shells = 32;

// One press of `b` at a time (0.1 s down, 0.4 s up) until `done`.
void then_press_until(Run &r, const std::string &what, double timeout,
                      std::function<SDL_GamepadButton(mt_cpu *)> next, Pred done) {
    auto at = std::make_shared<double>(-10);
    then(r, what, timeout, [&r, next, done, at](mt_cpu *cpu, double el) {
        if (el - *at < 0.5) {
            if (el - *at >= 0.1) {
                release_all(r);
            }
            return false;
        }
        if (done(cpu)) {
            release_all(r);
            return true;
        }
        *at = el;
        hold(r, next(cpu), true);
        return false;
    });
}

// The carry's drop spot moved to quick slot n (up / down over the slot column), then A drops.
void then_drop_on_slot(Run &r, i32 n) {
    then_press_until(
        r, "the carry on quick slot " + std::to_string(n), 10,
        [&r, n](mt_cpu *) {
            const fe::FocusTarget *t = r.focus.carry_target();
            return t == nullptr || t->action != "slot" ? RIGHT
                   : t->order < static_cast<u32>(n)    ? UP // the slot column: up to slot 4
                                                       : DOWN;
        },
        [&r, n](mt_cpu *) {
            const fe::FocusTarget *t = r.focus.carry_target();
            return t != nullptr && t->action == "slot" && t->order == static_cast<u32>(n);
        });
    then_press(r, A);
}

// The index of etc item `id` in the hero's item list (the item grid's cell), or -1.
i32 bag_cell(const mt_cpu *cpu, i32 id) {
    const u32 items = rd(cpu, hero(cpu).addr + 0xD8);
    for (u32 i = 0; items != 0 && i < 48; i++) {
        const u32 it = rd(cpu, items + 4 * i);
        if (it != 0 && rdi(cpu, it + 0x90) == id) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}
// The item grid's cursor cell: CDataMgr +0xD8 plus 6 cells a scrolled row.
i32 item_cursor(mt_cpu *cpu) {
    const u32 mgr = data_mgr(cpu);
    const u32 scroll = rd(cpu, mgr + 0x10C);
    return rdi(cpu, mgr + 0xD8) + 6 * (scroll != 0 ? rdi(cpu, scroll + 4) : 0);
}

// The item grid open (the popup menu, right, A) with its cursor on etc item `id`, from the field.
void then_item_cell(Run &r, i32 id) {
    then_press(r, START, 0.1, 0.6);
    then_wait_for(r, "the popup menu", 5, [](mt_cpu *cpu) { return game_mode(cpu) == 1; }, 0.3);
    then_press(r, RIGHT);
    then_press(r, A);
    then_wait_for(r, "the item grid", 5, [](mt_cpu *cpu) { return menu_sub(cpu) == 1; }, 0.3);
    then_press_until(
        r, "the cursor on item " + std::to_string(id), 20,
        [id](mt_cpu *cpu) { return item_cursor(cpu) < bag_cell(cpu, id) ? RIGHT : LEFT; },
        [id](mt_cpu *cpu) {
            return bag_cell(cpu, id) >= 0 && item_cursor(cpu) == bag_cell(cpu, id);
        });
}

// The shells taken to the trash: pick up (X), left to the trash, A: the game's popup 20.
void then_shells_to_trash(Run &r) {
    then_item_cell(r, k_shells);
    then_press(r, X);
    then_check(r, "picked up: the carry is on", [&r](mt_cpu *) { return r.focus.carrying(); });
    then_press_until(
        r, "the carry on the trash", 10, [](mt_cpu *) { return LEFT; },
        [&r](mt_cpu *) {
            const fe::FocusTarget *t = r.focus.carry_target();
            return t != nullptr && t->action == "trash";
        });
    then_press(r, A);
    then_wait_for(r, "the trash popup (20)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 20; });
}

void quick_slots_trash(Run &r, u64) {
    using namespace quest_bot;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    g_play.anywhere = true;
    then_load_slot0(r);
    // Double Step onto quick slot 3: the skill tab, its row, X picks it up, the drop on slot 3.
    auto k = std::make_shared<MenuPad>();
    then(r, "the skill tab on Double Step's row", 30, [&r, k](mt_cpu *cpu, double el) {
        if (!k->ready(r, el)) {
            return false;
        }
        if (!at_skill_tab(r, cpu, el, *k)) {
            return false;
        }
        const u32 m = data_mgr(cpu);
        const i32 row = skill_row(cpu, hero(cpu), k_double_step);
        const i32 cur = rdi(cpu, m + 0xD4) + rdi(cpu, rd(cpu, m + 0x10C) + 4);
        if (cur == row) {
            return true;
        }
        k->press(r, el, cur < row ? DOWN : UP);
        return false;
    });
    then_press(r, X);
    then_check(r, "Double Step picked up", [&r](mt_cpu *) { return r.focus.carrying(); });
    then_drop_on_slot(r, 3);
    then_check(r, "quick slot 3 holds Double Step", [&r](mt_cpu *cpu) {
        std::printf("quick slots: 3 holds skill %d; %llu picks, %llu drops\n",
                    quick_slot_skill(cpu, hero(cpu), 3),
                    static_cast<unsigned long long>(r.focus.stats().picks),
                    static_cast<unsigned long long>(r.focus.stats().drops));
        return !r.focus.carrying() && quick_slot_skill(cpu, hero(cpu), 3) == k_double_step;
    });
    then_back_to_field(r, "back to the field");
    // Used from the slot: LB, the skill's MP paid.
    auto mp = std::make_shared<i32>(0);
    then_do(r, "MP before", [mp](mt_cpu *cpu) { *mp = hero(cpu).mp; });
    then_press(r, LB, 0.1, 1.0);
    then_check(r, "LB used Double Step from quick slot 3: MP paid", [mp](mt_cpu *cpu) {
        std::printf("Double Step from slot 3: MP %d -> %d\n", *mp, hero(cpu).mp);
        return hero(cpu).mp < *mp;
    });
    // The trash, declined: B on popup 20, the shells stay.
    auto shells = std::make_shared<i32>(0);
    then_do(r, "the shells", [shells](mt_cpu *cpu) {
        *shells = item_count(cpu, hero(cpu), k_shells);
        check(*shells > 0, "the bag has Snail Shells");
    });
    then_shells_to_trash(r);
    then_press(r, B);
    then_wait_for(r, "popup 20 closed", 5, [](mt_cpu *cpu) { return popup_id(cpu) != 20; });
    then_check(r, "declined: the shells stay",
               [shells](mt_cpu *cpu) { return item_count(cpu, hero(cpu), k_shells) == *shells; });
    then_back_to_field(r, "back to the field");
    // Accepted: A on popup 20, the shells are gone.
    then_shells_to_trash(r);
    then_press(r, A);
    then_wait_for(r, "accepted: the shells are gone", 5, [](mt_cpu *cpu) {
        return popup_id(cpu) != 20 && item_count(cpu, hero(cpu), k_shells) == 0;
    });
    then_back_to_field(r, "back to the field");
    then_finish(r);
}
void quick_slots_trash_checks(Run &, u64) {}

// --- the host-driven tests (#432, ADR 0242, docs/testing/run-coverage.md) ------------------
//
// host_events: the game functions no play can enter (re/coverage-gaps.md's `unreachable` class),
// each entered from the host with the state it needs, for the run coverage (mac-coverage,
// MAPLETHIEF_COVERAGE). A coupon file with two coupons is written before the boot; the new game
// and the tutorial lead to the field; then, from the turn hook:
//   - a push token: the app delegate gets application:didRegisterForRemoteNotificationsWith
//     DeviceToken: with 32 bytes, and CDataMgr+0x58C holds them as 64 hex digits;
//   - a memory warning: the EAGLViewController (0x11A97C) gets didReceiveMemoryWarning and
//     viewDidUnload, as UIKit sends them to a controller whose view is in no window;
//   - an audio interruption (mt_audio_session_interruption): the listener the game gave
//     AudioSessionInitialize begins one (the session goes inactive) and ends it (active again);
//   - the death popup's grade revival: PopupKeyPadView's button action, which sends
//     clickGradeRevivalButton: to the TouchKeyPadView; the field is back without a popup;
//   - a coupon: CDataMgr::DeleteCoupon(0) empties the first and moves the second up;
//   - the teardown, last: a save state of the field, then each destructor and dealloc on its
//     object, the state restored before each: applicationWillTerminate:, every ObjC class's
//     dealloc (the live object, or a new one where the game made none), and every destructor
//     of the class's vtable on a live object of the class found on the heap, or on a zeroed
//     object with the class's vtables where the field has none. Each one returns; the run then
//     ends without another turn.
constexpr u32 k_eagl_controller = 0x11A97C; // the EAGLViewController (objc-shell.md)
constexpr u32 k_delete_coupon = 0x491E1;    // CDataMgr::DeleteCoupon(this, n) (formats/save.md)
constexpr u32 k_coupon_names = 0x118;       // CDataMgr: char *text_a[50]
constexpr u32 k_push_token = 0x58C;         // CDataMgr: the token as hex (re/network.md)
// Ivars (work/re/objc.json): EAGLView's touchKeyPadView, mainMenuView, m_pINAPMgr,
// m_pGameCenterManager; TouchKeyPadView's panels.
constexpr u32 k_view_keypad = 84, k_view_main_menu = 104, k_view_inap = 140, k_view_gc = 144;
constexpr u32 k_keypad_map = 48, k_keypad_popup = 52, k_keypad_npc = 56, k_keypad_market = 60,
              k_keypad_cash = 64, k_keypad_help = 68;
constexpr const char *k_coupons[2] = {"Coupon A", "Coupon B"};

// A destructor: its function pointer, the class's primary vtable (address point), for a
// secondary base's thunk the base's offset in the object and its vtable (work/re/vtables.json),
// and the constructor (this only) that makes an object where the field has none and a zeroed
// one will not do (the destructor follows a member pointer the constructor sets).
struct Destructor {
    u32 fn;
    const char *name;
    u32 vtable;
    u32 base = 0, base_vtable = 0;
    u32 ctor = 0;
};
constexpr u32 k_vt_state_adaptor = 0x119548, k_vt_floating_bg = 0x119918,
              k_vt_state_game = 0x119B78, k_vt_state_main_menu = 0x119C08,
              k_vt_state_title = 0x119C98;
const Destructor k_destructors[] = {
    {0xB015, "IStateAdaptor::~IStateAdaptor_deleting@thunk-4", k_vt_state_adaptor, 4, 0x1195B0},
    {0xB031, "IStateAdaptor::~IStateAdaptor_deleting", k_vt_state_adaptor},
    {0xB069, "IStateAdaptor::~IStateAdaptor", k_vt_state_adaptor},
    {0xB0A1, "IStateAdaptor::~IStateAdaptor@thunk-4", k_vt_state_adaptor, 4, 0x1195B0},
    {0xB169, "IState::~IState", 0xFC6B8},
    {0xB195, "IState::~IState_deleting", 0xFC6B8},
    {0xB221, "UtilAdaptor::~UtilAdaptor", 0xFC724},
    {0xB255, "UtilAdaptor::~UtilAdaptor_deleting", 0xFC724},
    {0xC75D, "CMMapTileSet::~CMMapTileSet", 0x119600},
    {0xCBD5, "CMMap::~CMMap", 0x11961C},
    {0xEAD1, "CMPopup::~CMPopup_deleting", 0x119638},
    {0xED69, "CMPopup::~CMPopup", 0x119638},
    {0x19689, "CMResMgr::~CMResMgr", 0x119654},
    {0x1BB31, "CMScrollBar::~CMScrollBar", 0x119670},
    {0x1C6B5, "CMSocket::~CMSocket_deleting", 0x11968C},
    {0x1C7B5, "CMSocket::~CMSocket", 0x11968C},
    {0x1DB19, "CMSoundPlayer::~CMSoundPlayer", 0x1196A4},
    {0x1DDD9, "CMSoundPlayer::~CMSoundPlayer_deleting", 0x1196A4},
    {0x1E6E1, "CMLayer::~CMLayer", 0x1196C0},
    {0x1F58D, "CMFrame::~CMFrame", 0x1196DC},
    {0x1FB89, "CMAction::~CMAction", 0x1196F8},
    {0x20C2D, "CMSprite::~CMSprite", 0x119714},
    {0x216D1, "CMSpriteImage::~CMSpriteImage", 0x119730},
    {0x2210D, "CMTimer::~CMTimer_deleting", 0x11974C},
    {0x22139, "CMTimer::~CMTimer", 0x11974C},
    {0x24E05, "CMUtil::~CMUtil_deleting", 0x119764},
    {0x24E39, "CMUtil::~CMUtil", 0x119764},
    {0x264D1, "SkyCanvas::~SkyCanvas_deleting", 0x119780},
    {0x264FD, "SkyCanvas::~SkyCanvas", 0x119780},
    {0x26785, "Build::~Build", 0x119798},
    {0x267B1, "Build::~Build_deleting", 0x119798},
    {0x26DA9, "CAniManager::~CAniManager", 0x1197B0, 0, 0, 0x26F21},
    {0x31C19, "CSelItem::~CSelItem", 0x1197C8},
    {0x31CD9, "CMsg::~CMsg", 0x1197E0},
    {0x361E5, "CChatBox::~CChatBox", 0x1197F8},
    {0x367F1, "CFloating::~CFloating", 0x119814},
    {0x36CA1, "CDamageFloatings::~CDamageFloatings", 0x119830},
    {0x5AD61, "CDataMgr::~CDataMgr_deleting", 0x119848},
    {0x5ADCD, "CDataMgr::~CDataMgr", 0x119848},
    {0x5CCA1, "CDropItems::~CDropItems", 0x119864},
    {0x5CFDD, "CFloatingEff::~CFloatingEff", 0x119888},
    {0x5D249, "CFloatingBGAlgorithm::~CFloatingBGAlgorithm", 0x1198A8},
    {0x5D275, "CFloatingBGAlgorithm::~CFloatingBGAlgorithm_deleting", 0x1198A8},
    {0x5D2A1, "CFloatingBGFlowerAlgorithm::~CFloatingBGFlowerAlgorithm", 0x1198D8},
    {0x5DA39, "CFloatingBGImage::~CFloatingBGImage", 0x119900},
    {0x5DD51, "CFloatingBG::~CFloatingBG_deleting@thunk-36", k_vt_floating_bg, 36, 0x11992C},
    {0x5E06D, "CFloatingBG::~CFloatingBG", k_vt_floating_bg},
    {0x5E36D, "CFloatingBG::~CFloatingBG@thunk-36", k_vt_floating_bg, 36, 0x11992C},
    {0x5ED7D, "CEffectFloatings::~CEffectFloatings", 0x119968},
    {0x68575, "CHero::~CHero", 0x119988},
    {0x69EED, "CItem::~CItem", 0x1199B8},
    {0x6B4BD, "CLoaderThread::~CLoaderThread", 0x1199E0},
    {0x6C5DD, "CMapArms::~CMapArms", 0x1199FC},
    {0x6DB59, "CMapleObject::~CMapleObject_deleting", 0x119A18},
    {0x6DB8D, "CMapleObject::~CMapleObject", 0x119A18},
    {0x6DEA1, "CMapleWorld::~CMapleWorld", 0x119A40},
    {0x6ED15, "CMobArms::~CMobArms", 0x119A58},
    {0x742E9, "CMob::~CMob", 0x119A78},
    {0x77421, "CMobs::~CMobs", 0x119AA0},
    {0x7789D, "CNotifyText::~CNotifyText", 0x119ABC},
    {0x78741, "CNotifyTextList::~CNotifyTextList", 0x119AD8},
    {0x79949, "CPopup::~CPopup", 0x117874},
    {0x7997D, "CPopup::~CPopup_deleting", 0x117874},
    {0x79A21, "CRequire::~CRequire", 0x119AF4},
    {0x79B99, "CReward::~CReward", 0x119B0C},
    {0x7A5DD, "CQuest::~CQuest", 0x119B24},
    {0x7AB81, "CQuestList::~CQuestList", 0x119B40},
    {0x80315, "CSkill::~CSkill", 0x119B5C},
    {0x92FF9, "CStateGame::~CStateGame_deleting@thunk-4", k_vt_state_game, 4, 0x119BE0},
    {0x93869, "CStateGame::~CStateGame", k_vt_state_game},
    {0x940BD, "CStateGame::~CStateGame@thunk-4", k_vt_state_game, 4, 0x119BE0},
    {0x97455, "CStateMainMenu::~CStateMainMenu_deleting@thunk-4", k_vt_state_main_menu, 4,
     0x119C74},
    {0x97809, "CStateMainMenu::~CStateMainMenu", k_vt_state_main_menu},
    {0x97BA1, "CStateMainMenu::~CStateMainMenu@thunk-4", k_vt_state_main_menu, 4, 0x119C74},
    {0x987AD, "CStateTitle::~CStateTitle_deleting@thunk-4", k_vt_state_title, 4, 0x119D00},
    {0x98809, "CStateTitle::~CStateTitle", k_vt_state_title},
    {0x98849, "CStateTitle::~CStateTitle@thunk-4", k_vt_state_title, 4, 0x119D00},
    {0x99069, "DataReader::~DataReader", 0x119D1C},
    {0x9D415, "Lua::~Lua", 0x119D38},
    {0x9D7A1, "Stringtable::~Stringtable", 0x119D54},
    {0x9E0B1, "Symbol::~Symbol", 0x119D6C},
    {0x9E79D, "Table::~Table", 0x119D84},
    {0x9F07D, "TFunc::~TFunc", 0x119DB4},
    {0x9F97D, "Undump::~Undump", 0x119DD0},
    {0xC0B41, "SVectorClass::~SVectorClass", 0x119DEC},
    {0xC0B6D, "SVectorClass::~SVectorClass_deleting", 0x119DEC},
};
constexpr u32 k_zeroed_object = 0x4000; // a zeroed object's bytes: more than any class's

struct HostEvents {
    std::vector<std::uint8_t> field;    // the save state the teardown restores before each step
    int live = 0, made = 0, zeroed = 0; // destructors on a live, a constructed, a zeroed object
    int fresh = 0;                      // deallocs on a new object (the game made none)
};
HostEvents g_host;

int host_events_setup(Run &r) {
    // cupon (formats/save.md): three best scores, then 50 entries of 0x77 bytes, text_a first.
    std::vector<char> cupon(12 + 50 * 0x77, 0);
    for (std::size_t i = 0; i < 2; i++) {
        std::memcpy(cupon.data() + 12 + i * 0x77, k_coupons[i], std::strlen(k_coupons[i]));
    }
    fs::create_directories(r.data / "saves");
    std::ofstream(r.data / "saves" / "cupon", std::ios::binary)
        .write(cupon.data(), static_cast<std::streamsize>(cupon.size()));
    return 0;
}

std::string guest_text(mt_cpu *cpu, u32 addr) {
    std::string s;
    while (addr != 0 && cpu->mem[addr] != 0 && s.size() < 256) {
        s += static_cast<char>(cpu->mem[addr++]);
    }
    return s;
}

u32 send(mt_cpu *cpu, u32 self, const char *selector, u32 arg = 0) {
    return mt_objc_send1(cpu, self, selector, arg);
}
u32 objc_class(const char *name) { return mt_objc_class_named(mt_objc_current(), name); }
u32 app_delegate(mt_cpu *cpu) {
    return mt_objc_send0(cpu, mt_objc_send0(cpu, objc_class("UIApplication"), "sharedApplication"),
                         "delegate");
}
// The EAGLView, as the engine reaches it: +[EAGLView_Instance Instance].objView (the delegate's
// glView outlet is never set: re/objc-shell.md).
u32 gl_view(mt_cpu *cpu) {
    return mt_objc_send0(cpu, mt_objc_send0(cpu, objc_class("EAGLView_Instance"), "Instance"),
                         "objView");
}
u32 keypad(mt_cpu *cpu) { return rd(cpu, gl_view(cpu) + k_view_keypad); }

void push_token(mt_cpu *cpu) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    const u32 bytes = mt_heap_malloc(mem, 32);
    std::string hex;
    for (u32 i = 0; i < 32; i++) {
        cpu->mem[bytes + i] = static_cast<std::uint8_t>(i * 37 + 5);
        char two[3];
        std::snprintf(two, sizeof two, "%02x", cpu->mem[bytes + i]);
        hex += two;
    }
    const u32 data = mt_objc_send2(cpu, mt_objc_send0(cpu, objc_class("NSData"), "alloc"),
                                   "initWithBytes:length:", bytes, 32);
    mt_heap_free(mem, bytes);
    const u32 delegate = app_delegate(cpu);
    const u32 app = mt_objc_send0(cpu, objc_class("UIApplication"), "sharedApplication");
    mt_objc_send2(cpu, delegate,
                  "application:didRegisterForRemoteNotificationsWithDeviceToken:", app, data);
    mt_objc_release(cpu, data);
    check(guest_text(cpu, data_mgr(cpu) + k_push_token) == hex,
          "push token: CDataMgr+0x58C holds the 32 bytes as hex");
}

void memory_warning(mt_cpu *cpu) {
    const u32 vc = rd(cpu, k_eagl_controller);
    check(vc != 0, "memory warning: the EAGLViewController exists");
    send(cpu, vc, "didReceiveMemoryWarning");
    send(cpu, vc, "viewDidUnload");
}

void audio_interruption(mt_cpu *cpu) {
    check(mt_audio_session_interruption(cpu, 1) == 1, "interruption: the listener is called");
    check(mt_audio_get_stats().session_active == 0,
          "interruption: SoundPlayer_BeginInterruption deactivated the session");
    mt_audio_session_interruption(cpu, 0);
    check(mt_audio_get_stats().session_active == 1,
          "interruption: SoundPlayer_EndInterruption activated it again");
}

void delete_coupon(mt_cpu *cpu) {
    const u32 dm = data_mgr(cpu);
    auto name = [cpu, dm](u32 i) { return guest_text(cpu, rd(cpu, dm + k_coupon_names + 4 * i)); };
    check(name(0) == k_coupons[0] && name(1) == k_coupons[1],
          "coupon: LoadCupon read the two coupons");
    const u32 args[2] = {dm, 0};
    mt_call_guest(cpu, k_delete_coupon, args, 2, k_delete_coupon);
    check(name(0) == k_coupons[1] && name(1).empty(),
          "coupon: DeleteCoupon(0) moved the second coupon up and emptied its entry");
}

// A live object of `d`'s class (the newest: the highest address), or 0: a heap block whose
// vtable words are the class's; else such words inside a live block (a member object) or in the
// image's writable segments (a static object).
bool has_vtables(const mt_cpu *cpu, u32 a, const Destructor &d) {
    return rd(cpu, a) == d.vtable && (d.base == 0 || rd(cpu, a + d.base) == d.base_vtable);
}
u32 live_object(mt_cpu *cpu, const Destructor &d) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    mt_heap_stats st{};
    mt_heap_get_stats(mem, &st);
    u32 found = 0;
    for (u32 a = MT_GUEST_HEAP_BASE; a + d.base + 4 <= st.top; a += MT_HEAP_ALIGN) {
        if (has_vtables(cpu, a, d) && mt_heap_usable_size(mem, a) != 0) {
            found = a;
        }
    }
    if (found != 0) {
        return found;
    }
    for (u32 a = MT_GUEST_HEAP_BASE; a + d.base + 4 <= st.top; a += 4) {
        if (!has_vtables(cpu, a, d)) {
            continue;
        }
        // The block holding it: the nearest live block start below, if it reaches past it.
        for (u32 b = a & ~(MT_HEAP_ALIGN - 1); b >= MT_GUEST_HEAP_BASE && a - b < 0x10000;
             b -= MT_HEAP_ALIGN) {
            if (const u32 n = mt_heap_usable_size(mem, b); n != 0) {
                found = a + d.base + 4 <= b + n ? a : found;
                break;
            }
        }
    }
    if (found != 0) {
        return found;
    }
    u32 count = 0;
    const mt_segment *segs = mt_mem_segments(mem, &count);
    for (u32 i = 0; i < count; i++) {
        if ((segs[i].initprot & MT_PROT_WRITE) == 0) {
            continue;
        }
        for (u32 a = segs[i].vmaddr; a + d.base + 4 <= segs[i].vmaddr + segs[i].vmsize; a += 4) {
            if (has_vtables(cpu, a, d)) {
                found = a;
            }
        }
    }
    return found;
}

void restore_field(Run &r, mt_cpu *cpu) {
    mt_snapshot_expect ex = mt_snapshot_expect_default();
    ex.rebase_clock = guest_rebase;
    ex.user = &r.clock;
    char why[512] = {};
    if (mt_snapshot_restore(cpu, g_host.field.data(), g_host.field.size(), &ex, nullptr, why,
                            sizeof why) != 0) {
        check(false, std::string("teardown: the field's state restores: ") + why);
    }
}

// The teardown's steps, one a turn (each restore takes a while: a turn stays short for the hang
// watchdog). The first saves the field's state; each step runs its destructor or dealloc and,
// but for the last, restores the field before the game's next turn.
struct TeardownStep {
    std::string what;
    std::function<void(mt_cpu *)> run;
};
std::vector<TeardownStep> teardown_steps(Run &r, mt_cpu *cpu) {
    std::vector<TeardownStep> steps;
    // applicationWillTerminate: deletes the state and CMResMgr (re/program-flow.md).
    steps.push_back({"applicationWillTerminate:", [](mt_cpu *c) {
                         check(mt_uikit_lifecycle_event(c, MT_UIKIT_WILL_TERMINATE) == 1,
                               "teardown: applicationWillTerminate: was sent");
                     }});
    // The ObjC objects: the game's own where it has one, else a new one.
    const u32 view = gl_view(cpu), pad = keypad(cpu);
    const std::vector<std::pair<const char *, u32>> objc = {
        {"MapleThiefAppDelegate", app_delegate(cpu)},
        {"EAGLView", view},
        {"EAGLViewController", rd(cpu, k_eagl_controller)},
        {"TouchKeyPadView", pad},
        {"MainMenuView", rd(cpu, view + k_view_main_menu)},
        {"MapKeyPadView", rd(cpu, pad + k_keypad_map)},
        {"PopupKeyPadView", rd(cpu, pad + k_keypad_popup)},
        {"NpcKeyPadView", rd(cpu, pad + k_keypad_npc)},
        {"MarketKeyPadView", rd(cpu, pad + k_keypad_market)},
        {"CashShopKeyPadView", rd(cpu, pad + k_keypad_cash)},
        {"HelpScrollKeyPadView", rd(cpu, pad + k_keypad_help)},
        {"InAppPurchaseManager", rd(cpu, view + k_view_inap)},
        {"GameCenterManager", rd(cpu, view + k_view_gc)},
        {"TermsView", 0},
    };
    for (const auto &[cls_name, live_obj] : objc) {
        const std::string cls = cls_name;
        const u32 live = live_obj;
        steps.push_back({"-[" + cls + " dealloc]", [cls, live](mt_cpu *c) {
                             u32 obj = live;
                             if (obj == 0) {
                                 obj = mt_objc_send0(c, objc_class(cls.c_str()), "alloc");
                                 g_host.fresh++;
                             }
                             check(obj != 0 && mt_objc_class_of(mt_objc_current(), obj) ==
                                                   objc_class(cls.c_str()),
                                   "teardown: a " + cls +
                                       (live != 0 ? " of the game's" : " (new)"));
                             mt_objc *rt = mt_objc_current();
                             const u32 pool = mt_objc_pool_push(rt);
                             mt_objc_send0(c, obj, "dealloc");
                             mt_objc_pool_pop_on(c, rt, pool);
                         }});
    }
    // The C++ objects: a live one of the class on the heap, else a zeroed one with its vtables.
    for (const Destructor &d : k_destructors) {
        steps.push_back({d.name, [&d](mt_cpu *c) {
                             mt_mem *mem = mt_mem_from_cpu(c);
                             u32 obj = live_object(c, d);
                             const bool live = obj != 0;
                             if (live) {
                                 g_host.live++;
                             } else {
                                 obj = mt_heap_calloc(mem, 1, k_zeroed_object);
                                 if (d.ctor != 0) {
                                     mt_call_guest(c, d.ctor, &obj, 1, d.ctor);
                                     g_host.made++;
                                 } else {
                                     std::memcpy(c->mem + obj, &d.vtable, 4);
                                     if (d.base != 0) {
                                         std::memcpy(c->mem + obj + d.base, &d.base_vtable, 4);
                                     }
                                     g_host.zeroed++;
                                 }
                             }
                             std::printf("teardown: %s on a %s object\n", d.name,
                                         live          ? "live"
                                         : d.ctor != 0 ? "new"
                                                       : "zeroed");
                             std::fflush(stdout);
                             const u32 self = obj + d.base;
                             mt_call_guest(c, d.fn, &self, 1, d.fn);
                         }});
    }
    (void)r;
    return steps;
}

// One teardown step a turn; true after the last, which ends the run without another turn.
bool teardown(Run &r, mt_cpu *cpu) {
    static std::vector<TeardownStep> steps;
    static std::size_t next = 0;
    if (g_host.field.empty()) {
        mt_snapshot_meta meta = mt_snapshot_meta_default();
        meta.flags = MT_SNAPSHOT_STORE;
        std::uint8_t *bytes = nullptr;
        std::size_t size = 0;
        char why[512] = {};
        if (mt_snapshot_save(cpu, &meta, &bytes, &size, why, sizeof why) != 0) {
            check(false, std::string("teardown: the field's state saves: ") + why);
            r.on_turn = nullptr;
            r.done = true;
            return true;
        }
        g_host.field.assign(bytes, bytes + size);
        std::free(bytes);
        steps = teardown_steps(r, cpu);
        next = 0;
        // A destructor may leave the game anywhere: restore wherever it is.
        const mt_snapshot_config any{0, 0, 0};
        mt_snapshot_configure(&any);
    }
    steps[next].run(cpu);
    if (++next < steps.size()) {
        restore_field(r, cpu);
        return false;
    }
    check(true, "teardown: " + std::to_string(steps.size()) +
                    " steps returned: " + std::to_string(std::size(k_destructors)) +
                    " destructors (" + std::to_string(g_host.live) + " on live objects, " +
                    std::to_string(g_host.made) + " on constructed ones, " +
                    std::to_string(g_host.zeroed) + " on zeroed ones), " +
                    std::to_string(steps.size() - 1 - std::size(k_destructors)) + " deallocs (" +
                    std::to_string(g_host.fresh) + " on new objects)");
    mt_snapshot_configure(nullptr);
    release_all(r);
    r.on_turn = nullptr;
    r.done = true;
    return true;
}

// A host step in its own autorelease pool, as UIKit runs every event: what the sends autorelease
// is released before the next turn (a save state waits for an empty pool).
void then_host(Run &r, std::string what, std::function<void(mt_cpu *)> fn) {
    then_do(r, std::move(what), [fn](mt_cpu *cpu) {
        mt_objc *rt = mt_objc_current();
        const u32 pool = mt_objc_pool_push(rt);
        fn(cpu);
        mt_objc_pool_pop_on(cpu, rt, pool);
    });
}

void host_events(Run &r, u64) {
    g_host = HostEvents{};
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_wait_field(r);
    then_host(r, "a push token", push_token);
    then_host(r, "a memory warning", memory_warning);
    then_host(r, "an audio interruption", audio_interruption);
    then_host(r, "the grade revival's button", [](mt_cpu *cpu) {
        // The panel is made when a popup first needs it: a new one if the game has none yet.
        u32 popup = rd(cpu, keypad(cpu) + k_keypad_popup);
        if (popup == 0) {
            popup = mt_objc_send0(cpu, objc_class("PopupKeyPadView"), "alloc");
        }
        send(cpu, popup, "releaseGradeRevivalButton:");
    });
    then_wait_for(
        r, "the field again, no popup", 10,
        [](mt_cpu *cpu) { return field(cpu) && popup_id(cpu) == -1; }, 1.0);
    then_host(r, "a coupon deleted", delete_coupon);
    then_wait(r, 0.5);
    then(r, "the teardown", 120, [&r](mt_cpu *cpu, double) {
        char why[256] = {};
        if (g_host.field.empty() && mt_snapshot_safe(cpu, why, sizeof why) != 1) {
            return false;
        }
        return teardown(r, cpu);
    });
}
void host_events_checks(Run &, u64) {}

constexpr Mode k_modes[] = {
    {"first_map", first_map, first_map_checks, k_default_ticks},
    {"regress_record", regress_record, regress_record_checks, 1, false, regress_record_setup},
    {"regress", regress, regress_checks, 1, false, regress_setup},
    {"save_state", save_state, save_state_checks, 1, false, save_state_setup},
    {"save_state_census", save_state_census, save_state_census_checks, 1, false,
     save_state_census_setup},
    {"quick_state", quick_state, quick_state_checks, 1, false, quick_state_setup},
    {"practice_state", practice_state, practice_state_checks, 1},
    {"recovery_crash", recovery_crash, recovery_crash_checks, 1, false, recovery_crash_setup},
    {"recovery_resume", recovery_resume, recovery_resume_checks, 1, true, recovery_resume_setup},
    {"recovery_off", recovery_off, recovery_off_checks, 1, false, recovery_off_setup},
    {"save_quit", save_quit, save_quit_checks, 1},
    {"save_relaunch", save_relaunch, save_relaunch_checks, 1, true},
    {"save_migrate", save_relaunch, save_migrate_checks, 1, false, save_migrate_setup},
    {"config_speed", config_speed, config_speed_checks, 1},
    {"game_speed", game_speed, game_speed_checks, 1},
    {"cash_shop_meso", cash_shop_meso, cash_shop_checks, 1},
    {"cash_shop_off", cash_shop_off, cash_shop_checks, 1},
    {"cash_buttons_hidden", cash_buttons_hidden, cash_shop_checks, 1},
    {"chapter_1_a", chapter_1_a, chapter_1_a_checks, 1},
    {"chapter_1_b", chapter_1_b, chapter_1_b_checks, 1},
    {"chapter_1_c", chapter_1_c, chapter_1_c_checks, 1},
    {"chapter_1_d", chapter_1_d, chapter_1_d_checks, 1},
    {"chapter_1_e", chapter_1_e, chapter_1_e_checks, 1},
    {"chapter_1_f", chapter_1_f, chapter_1_f_checks, 1},
    {"chapter_1_g", chapter_1_g, chapter_1_g_checks, 1},
    {"chapter_1_h", chapter_1_h, chapter_1_h_checks, 1},
    {"chapter_1_i", chapter_1_i, chapter_1_i_checks, 1},
    {"chapter_1_j", chapter_1_j, chapter_1_j_checks, 1},
    {"chapter_1_k", chapter_1_k, chapter_1_k_checks, 1},
    {"chapter_1_l", chapter_1_l, chapter_1_l_checks, 1},
    {"chapter_1_m", chapter_1_m, chapter_1_m_checks, 1},
    {"chapter_2_a", chapter_2_a, chapter_2_a_checks, 1},
    {"chapter_2_b", chapter_2_b, chapter_2_b_checks, 1},
    {"chapter_2_c", chapter_2_c, chapter_2_c_checks, 1},
    {"chapter_2_d", chapter_2_d, chapter_2_d_checks, 1},
    {"chapter_2_e", chapter_2_e, chapter_2_e_checks, 1},
    {"chapter_2_f", chapter_2_f, chapter_2_f_checks, 1},
    {"chapter_2_sides_a", chapter_2_sides_a, chapter_2_sides_a_checks, 1},
    {"chapter_2_sides_b", chapter_2_sides_b, chapter_2_sides_b_checks, 1},
    {"chapter_2_sides_c", chapter_2_sides_c, chapter_2_sides_c_checks, 1},
    {"chapter_2_sides_d", chapter_2_sides_d, chapter_2_sides_d_checks, 1},
    {"chapter_2_sides_e", chapter_2_sides_e, chapter_2_sides_e_checks, 1},
    {"chapter_2_sides_f", chapter_2_sides_f, chapter_2_sides_f_checks, 1},
    {"chapter_2_crafts", chapter_2_crafts, chapter_2_crafts_checks, 1},
    {"chapter_2_side_16_full", chapter_2_side_16_full, chapter_2_side_16_full_checks, 1},
    {"chapter_2_climb", chapter_2_climb, chapter_2_climb_checks, 1},
    {"chapter_2_shops", chapter_2_shops, chapter_2_shops_checks, 1},
    {"chapter_2_shop_6", chapter_2_shop_6, chapter_2_shop_6_checks, 1},
    {"chapter_3_a", chapter_3_a, chapter_3_a_checks, 1},
    {"chapter_3_b", chapter_3_b, chapter_3_b_checks, 1},
    {"chapter_3_c", chapter_3_c, chapter_3_c_checks, 1},
    {"chapter_3_d", chapter_3_d, chapter_3_d_checks, 1},
    {"chapter_3_e", chapter_3_e, chapter_3_e_checks, 1},
    {"chapter_3_e_skipped", chapter_3_e_skipped, chapter_3_e_checks, 1},
    {"chapter_3_postgame_quit", chapter_3_postgame_quit, chapter_3_postgame_quit_checks, 1},
    {"chapter_3_postgame_relaunch", chapter_3_postgame_relaunch, chapter_3_postgame_relaunch_checks,
     1, true},
    {"chapter_3_postgame_reload", chapter_3_postgame_reload, chapter_3_postgame_reload_checks, 1,
     true},
    {"chapter_3_sides_a", chapter_3_sides_a, chapter_3_sides_a_checks, 1},
    {"chapter_3_sides_b", chapter_3_sides_b, chapter_3_sides_b_checks, 1},
    {"chapter_3_sides_c", chapter_3_sides_c, chapter_3_sides_c_checks, 1},
    {"chapter_3_climb", chapter_3_climb, chapter_3_climb_checks, 1},
    {"chapter_3_side_14", chapter_3_side_14, chapter_3_side_14_checks, 1},
    {"chapter_3_side_14_assassin", chapter_3_side_14_assassin, chapter_3_side_14_assassin_checks,
     1},
    {"chapter_3_sides_10_a", chapter_3_sides_10_a, chapter_3_sides_10_a_checks, 1},
    {"chapter_3_sides_10_b", chapter_3_sides_10_b, chapter_3_sides_10_b_checks, 1},
    {"chapter_3_sides_10_c", chapter_3_sides_10_c, chapter_3_sides_10_c_checks, 1},
    {"chapter_3_crafts", chapter_3_crafts, chapter_3_crafts_checks, 1},
    {"texture_dump", first_map, texture_dump_checks, k_default_ticks, false, texture_dump_setup},
    {"texture_pack", first_map, texture_pack_checks, k_default_ticks, false, texture_pack_setup},
    {"confirm_guard", confirm_guard, confirm_guard_checks, 1},
    {"game_options", game_options, game_options_checks, 1},
    {"autosave_scene", autosave_scene, autosave_scene_checks, 1},
    {"autosave_map", autosave_map, autosave_map_checks, 1, false, autosave_map_setup},
    {"autosave_quit", autosave_quit, autosave_quit_checks, 1},
    {"autosave_exit", autosave_exit, autosave_exit_checks, 1},
    {"autosave_death_exit", autosave_death_exit, autosave_death_exit_checks, 1},
    {"autosave_reload", autosave_reload, autosave_reload_checks, 1, true},
    {"popup_menu_tabs", popup_menu_tabs, popup_menu_tabs_checks, 1},
    {"popup_menu_down", popup_menu_down, popup_menu_down_checks, 1},
    {"popup_menu_shoulders", popup_menu_shoulders, popup_menu_shoulders_checks, 1},
    {"widescreen_off", widescreen_flow, widescreen_off_checks, 2400, false, widescreen_off_setup},
    {"widescreen_on", widescreen_flow, widescreen_on_checks, 2400, false, widescreen_on_setup},
    {"widescreen_fill_on", widescreen_flow, widescreen_on_checks, 2400, false,
     widescreen_fill_on_setup},
    {"widescreen_scene_off", widescreen_scene, widescreen_scene_off_checks, 1, false,
     widescreen_scene_off_setup},
    {"widescreen_scene_on", widescreen_scene, widescreen_scene_on_checks, 1, false,
     widescreen_scene_on_setup},
    {"popup_menu_scroll", popup_menu_scroll, popup_menu_scroll_checks, 1, false,
     popup_menu_scroll_setup},
    {"screen_shake", screen_shake, screen_shake_checks, 1, false, screen_shake_setup},
    {"flashes", flashes, flashes_checks, 1, false, flashes_setup},
    {"widescreen_pad_off", widescreen_pad, widescreen_pad_checks, 1, false,
     widescreen_pad_off_setup},
    {"widescreen_pad_on", widescreen_pad, widescreen_pad_checks, 1, false, widescreen_pad_on_setup},
    {"widescreen_pad_fill_on", widescreen_pad, widescreen_pad_checks, 1, false,
     widescreen_pad_fill_on_setup},
    {"widescreen_filters", widescreen_filters, widescreen_filters_checks, 1, false,
     widescreen_filters_setup},
    {"auto_potion", auto_potion, auto_potion_checks, 1},
    {"turbo_attack", turbo_attack, turbo_attack_checks, 1},
    {"ui_animations_normal", ui_animations_normal, ui_animations_checks, 1},
    {"ui_animations_fast", ui_animations_fast, ui_animations_checks, 1},
    {"ui_animations_instant", ui_animations_instant, ui_animations_checks, 1},
    {"chat_text", chat_text, chat_text_checks, 1},
    {"speech_dialogue", speech_dialogue, speech_dialogue_checks, 1},
    {"speech_notices", speech_notices, speech_notices_checks, 1},
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
    {"jobs_1_1", jobs_1_1, jobs_checks, 1},
    {"jobs_1_2_assassin", jobs_1_2_assassin, jobs_checks, 1},
    {"jobs_1_2_bandit", jobs_1_2_bandit, jobs_checks, 1},
    {"jobs_2_2_assassin", jobs_2_2_assassin, jobs_checks, 1},
    {"jobs_2_2_bandit", jobs_2_2_bandit, jobs_checks, 1},
    {"jobs_2_3_assassin", jobs_2_3_assassin, jobs_checks, 1},
    {"jobs_2_3_bandit", jobs_2_3_bandit, jobs_checks, 1},
    {"jobs_3_1", jobs_3_1, jobs_checks, 1},
    {"jobs_3_2_assassin", jobs_3_2_assassin, jobs_checks, 1},
    {"jobs_3_2_bandit", jobs_3_2_bandit, jobs_checks, 1},
    {"jobs_3_4_assassin", jobs_3_4_assassin, jobs_checks, 1},
    {"jobs_3_4_bandit", jobs_3_4_bandit, jobs_checks, 1},
    {"skills_assassin", skills_assassin, skills_assassin_checks, 1},
    {"skills_bandit", skills_bandit, skills_bandit_checks, 1},
    {"skills_weapon_assassin", skills_weapon_assassin, skills_weapon_checks, 1},
    {"skills_weapon_bandit", skills_weapon_bandit, skills_weapon_checks, 1},
    {"systems2_cash_tiles", systems2_cash_tiles, systems2_cash_checks, 1},
    {"systems2_cash_restores", systems2_cash_restores, systems2_cash_checks, 1, false,
     systems2_cash_restores_setup},
    {"systems2_scrolls_success", systems2_scrolls_success, systems2_scrolls_checks, 1},
    {"systems2_scrolls_failure", systems2_scrolls_failure, systems2_scrolls_checks, 1},
    {"systems2_pigmy", systems2_pigmy, systems2_pigmy_checks, 1},
    {"systems2_portal620", systems2_portal620, systems2_portal620_checks, 1},
    {"systems2_portal620_fixed", systems2_portal620_fixed, systems2_portal620_checks, 1},
    {"systems2_boss_knockback", systems2_boss_knockback, systems2_boss_knockback_checks, 1},
    {"systems2_boss_knockback_fixed", systems2_boss_knockback_fixed, systems2_boss_knockback_checks,
     1},
    {"systems2_restored_shops_0", systems2_restored_shops_0, systems2_restored_shops_checks, 1},
    {"systems2_restored_shops_0_off", systems2_restored_shops_0_off, systems2_restored_shops_checks,
     1},
    {"systems2_restored_shops_9", systems2_restored_shops_9, systems2_restored_shops_checks, 1},
    {"systems2_restored_shops_9_off", systems2_restored_shops_9_off, systems2_restored_shops_checks,
     1},
    {"systems2_restored_carnival", systems2_restored_carnival, systems2_restored_carnival_checks,
     1},
    {"systems2_restored_carnival_off", systems2_restored_carnival_off,
     systems2_restored_carnival_checks, 1},
    {"systems2_restored_carnival_4", systems2_restored_carnival_4,
     systems2_restored_carnival_checks, 1},
    {"systems2_restored_carnival_6", systems2_restored_carnival_6,
     systems2_restored_carnival_checks, 1},
    {"systems2_restored_maps", systems2_restored_maps, systems2_restored_maps_checks, 1},
    {"systems2_restored_maps_off", systems2_restored_maps_off, systems2_restored_maps_checks, 1},
    {"compendium_npcs", compendium_npcs, compendium_npcs_checks, 1},
    {"map_page", map_page, map_page_checks, 1, false, map_page_setup},
    {"korean_chapter_1_a", korean_chapter_1_a, korean_checks, 1, false, korean_setup},
    {"korean_offer_2", korean_offer_2, korean_checks, 1, false, korean_setup},
    {"korean_offer_3", korean_offer_3, korean_checks, 1, false, korean_setup},
    {"korean_missing_key", korean_missing_key, korean_checks, 1, false, korean_setup},
    {"korean_missing_key_original", korean_missing_key_original, korean_checks, 1, false,
     korean_setup},
    {"slots_create", slots_create, slots_create_checks, 1, false, slots_setup},
    {"slots_reload", slots_reload, slots_reload_checks, 1, true, slots_setup},
    {"inventory_full", inventory_full, inventory_full_checks, 1},
    {"level_cap", level_cap, level_cap_checks, 1},
    {"quick_slots_trash", quick_slots_trash, quick_slots_trash_checks, 1},
    {"host_events", host_events, host_events_checks, 1, false, host_events_setup},
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
    const bool part_checkpoints = playthrough_setup(r);
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE));
    if (status == 0) {
        common_checks(r, goal);
        mode->checks(r, goal);
        if (part_checkpoints) {
            playthrough_checks(r);
        }
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
