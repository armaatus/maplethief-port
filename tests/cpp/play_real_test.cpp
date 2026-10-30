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
//                                             confirm_guard: CTest play.confirm_guard, #147)
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

#include "game_driver.h"
#include "maplethief/cashshop.h"
#include "maplethief/events.h"
#include "maplethief/gfx.h"
#include "maplethief/saves.h"
#include "maplethief/texpack.h"
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

void first_map(Run &r, u64 goal) {
    attach_pad(r);
    r.on_turn = [&r](mt_cpu *cpu) { monitor(r, cpu); };
    then_new_game(r);
    then_tutorial(r);
    then_do(r, "a shot of Henesys", [&r](mt_cpu *cpu) { save_shot(r, cpu, "henesys"); });
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
    {"cash_shop_meso", cash_shop_meso, cash_shop_checks, 1},
    {"cash_shop_off", cash_shop_off, cash_shop_checks, 1},
    {"chapter_1_a", chapter_1_a, chapter_1_a_checks, 1},
    {"chapter_1_b", chapter_1_b, chapter_1_b_checks, 1},
    {"chapter_1_c", chapter_1_c, chapter_1_c_checks, 1},
    {"chapter_1_d", chapter_1_d, chapter_1_d_checks, 1},
    {"texture_dump", first_map, texture_dump_checks, k_default_ticks, false, texture_dump_setup},
    {"texture_pack", first_map, texture_pack_checks, k_default_ticks, false, texture_pack_setup},
    {"confirm_guard", confirm_guard, confirm_guard_checks, 1},
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
