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
//                                             autosave_*: CTests play.autosave_*, #150;
//                                             widescreen_off, then widescreen_on with
//                                             MT_WIDESCREEN_BASE: CTests play.widescreen_off,
//                                             play.widescreen_on, #86; widescreen_pad_off,
//                                             then widescreen_pad_on with
//                                             MT_WIDESCREEN_PAD_BASE: CTests
//                                             play.widescreen_pad_off, play.widescreen_pad_on,
//                                             #168; minimap: CTest play.minimap,
//                                             #176)
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

#include <map>
#include <set>
#include <tuple>

#include "autosave.h"
#include "game_driver.h"
#include "game_speed.h"
#include "lifecycle.h"
#include "live_settings.h"
#include "maplethief/cashshop.h"
#include "maplethief/events.h"
#include "maplethief/gfx.h"
#include "maplethief/hooks.h"
#include "maplethief/saves.h"
#include "maplethief/texpack.h"
#include "maplethief/widescreen.h"
#include "minimap.h"
#include "price_line.h"
#include "quest_bot.h"
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
              std::to_string(goal) + " wanted: " + std::to_string(goal * 50 / 60000.0) +
              " game-minutes at 50 ms)");
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
    fe::configure_save_location(fe::Settings{}, r.data.string(), "");
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
            fs::copy_file(r.data / "Documents" / name, r.out / (std::string("saved_") + name),
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
    for (const auto &e : fs::directory_iterator(r.data / "Documents", ec)) {
        leftovers |= e.path().string().find(MT_SAVES_TMP_SUFFIX) != std::string::npos;
    }
    check(!leftovers, "no temporary save file is left in Documents/");
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
              std::string("after the relaunch Documents/") + name + " holds the bytes saved (" +
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
    std::ofstream out(r.data / "Documents" / "theif0", std::ios::binary | std::ios::trunc);
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
        std::ofstream q(r.data / "Documents" / "quest0", std::ios::binary | std::ios::trunc);
        q.write(reinterpret_cast<const char *>(quest.data()),
                static_cast<std::streamsize>(quest.size()));
        std::printf("quest0's chapter set to 3 (Balog's scripts)\n");
        if (!q) {
            return 1;
        }
    }
    return out ? 0 : 1;
}

void autosave_map(Run &r, u64) {
    g_play.anywhere = true;
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    // then_load_slot0 up to Start; then A through whatever scene the map starts with.
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
    auto scenes = std::make_shared<int>(0);
    auto since = std::make_shared<double>(-1);
    const std::string where = std::to_string(g_map_area) + "." + std::to_string(g_map_map);
    then(r, "the field of map " + where + " (A through any scene)", 180,
         [&r, scenes, since](mt_cpu *cpu, double el) {
             const bool box = mt_input_panel() == 3 || mt_input_panel() == 5;
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
    fe::configure_save_location(fe::Settings{}, r.data.string(), "");
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
    for (const auto &e : fs::directory_iterator(r.data / "Documents", ec)) {
        leftovers |= e.path().string().find(MT_SAVES_TMP_SUFFIX) != std::string::npos;
    }
    check(!leftovers, "no temporary save file is left in Documents/ (the atomic replacement)");
}

void autosave_reload(Run &r, u64) {
    for (const char *name : {"theif0", "quest0"}) {
        const std::vector<std::uint8_t> saved = read_file(r.out / (std::string("saved_") + name));
        check(!saved.empty() && read_save(r, name) == saved,
              std::string("after the relaunch Documents/") + name + " holds the autosaved bytes");
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
    fs::create_directories(r.data / "Documents");
    std::ofstream(r.data / "Documents" / "config", std::ios::binary) << k_bad_config;
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

// The receipt list `name` in Documents/: its entry count (i16), -1 if the file is absent.
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
                   loc.documents_dir = (r.data / "Documents").string();
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
    std::ofstream out(r.data / "Documents" / "theif0", std::ios::binary | std::ios::trunc);
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
void wide_shot(Run &r, mt_cpu *cpu, const std::string &name, bool pad = false) {
    if (pad) {
        fe::apply_keypad_layout();
    }
    const double now = mt_cf_absolute_time();
    if (!r.screen.compose(cpu, now) || r.screen.gl_pixels().empty()) {
        return;
    }
    const u32 gw = r.screen.gl_width(), gh = r.screen.gl_height();
    const u32 view = std::min(r.screen.gl_view(), gw);
    constexpr u32 w = MT_WIDESCREEN_WIDE, h = MT_WIDESCREEN_HEIGHT;
    if (gh != h) {
        return; // render scale 1 only
    }
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
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
constexpr Mode k_modes[] = {
    {"first_map", first_map, first_map_checks, k_default_ticks},
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
    {"autosave_reload", autosave_reload, autosave_reload_checks, 1, true},
    {"popup_menu_tabs", popup_menu_tabs, popup_menu_tabs_checks, 1},
    {"widescreen_off", widescreen_flow, widescreen_off_checks, 2400, false, widescreen_off_setup},
    {"widescreen_on", widescreen_flow, widescreen_on_checks, 2400, false, widescreen_on_setup},
    {"popup_menu_scroll", popup_menu_scroll, popup_menu_scroll_checks, 1, false,
     popup_menu_scroll_setup},
    {"screen_shake", screen_shake, screen_shake_checks, 1, false, screen_shake_setup},
    {"widescreen_pad_off", widescreen_pad, widescreen_pad_checks, 1, false,
     widescreen_pad_off_setup},
    {"widescreen_pad_on", widescreen_pad, widescreen_pad_checks, 1, false, widescreen_pad_on_setup},
    {"auto_potion", auto_potion, auto_potion_checks, 1},
    {"turbo_attack", turbo_attack, turbo_attack_checks, 1},
    {"ui_animations_normal", ui_animations_normal, ui_animations_checks, 1},
    {"ui_animations_fast", ui_animations_fast, ui_animations_checks, 1},
    {"ui_animations_instant", ui_animations_instant, ui_animations_checks, 1},
    {"minimap", minimap, minimap_checks, 600},
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
    if (mode->relaunch && !fs::is_directory(r.data / "Documents")) {
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
        if (!fs::is_directory(data / "Documents")) {
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
