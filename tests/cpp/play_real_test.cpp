// The first maps played for 15 game-minutes (#76). The recompiled game boots from work/ as
// MapleThief does with --fixed-step 1 (the virtual clock, ADR 0044) on a fresh data directory,
// and a small bot plays it through the frontend's own input layer with a virtual Xbox 360 pad
// (game_driver.h: Gamepads -> Actions -> the runtime's actions), reading the hero, the mobs and
// the drops from guest memory (guest_state.h; read only) and the game events of the observed
// functions (ADR 0053). Local only: it needs work/gen (python -m recomp all), the slice and the
// extracted bundle, and exits 77 (skipped) without them.
//
//   play_real_test MODE OUT_DIR [TICKS]      (first_map: CTest play.first_map)
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
#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#if defined(MT_HAVE_GENERATED_CODE)

#include <map>
#include <set>

#include "game_driver.h"
#include "maplethief/events.h"
#include "maplethief/gfx.h"

namespace {

using namespace game_driver;

constexpr u64 k_default_ticks = 18000; // 15 game-minutes at 50 ms a tick
constexpr u64 k_minute_ticks = 1200;   // a game-minute at 50 ms

// What the monitor has seen (every turn while the hero is in the field).
struct Play {
    u64 play_ticks = 0; // the goal: game ticks since the hero first stood in the field
    bool started = false;
    u64 start_ticks = 0, start_gl = 0;
    double start_t = 0;
    // Per game-minute.
    u64 minute = 0, minute_ticks = 0, minute_gl = 0;
    double minute_t = 0;
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
        if (!(field(cpu) && h.on(2, 8))) {
            return;
        }
        p.started = true;
        p.start_ticks = p.minute_ticks = r.ticks;
        p.start_gl = p.minute_gl = r.gl_frames;
        p.start_t = p.minute_t = r.t;
        p.first = p.last = h;
        std::printf("play: the hero stands in the field of map 2.8 at %.2f s (tick %llu): %s\n",
                    r.t, static_cast<unsigned long long>(r.ticks), hero_text(cpu, h).c_str());
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
        check(lit, "minute " + std::to_string(p.minute) + ": the screen is not black");
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

// The modes: the tasks a mode queues on the Run, and its checks after common_checks.
struct Mode {
    const char *name;
    void (*queue)(Run &, u64);
    void (*checks)(Run &, u64);
};
constexpr Mode k_modes[] = {
    {"first_map", first_map, first_map_checks},
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
    const u64 goal = argc == 4 ? std::strtoull(argv[3], nullptr, 10) : k_default_ticks;
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
    fs::remove_all(r.out);
    fs::create_directories(r.data);
    // The pads' events reach the test without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    g_run = &r;
    r.focus.attach(r.actions);
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
