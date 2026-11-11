// A bot that plays the quests of chapter 1 (#77) on top of game_driver.h: it goes anywhere in
// the chapter's world (world_map.h: the platforms, ladders and moves of each map, the portal
// links between maps), talks to NPCs and picks their quest rows, and hunts given mobs until a
// quest's requirement holds. Only the virtual pad, like the rest of the driver; guest memory is
// only read (guest_state.h). Header-only: included once by play_real_test.cpp.
#ifndef MAPLETHIEF_TESTS_QUEST_BOT_H
#define MAPLETHIEF_TESTS_QUEST_BOT_H

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "game_driver.h"
#include "world_map.h"

namespace quest_bot {

using namespace game_driver;
namespace wm = world_map;

// --- the chat box ------------------------------------------------------------------------------
//
// The dialogue box is the world's +0x5C (CChatBox; the script natives' msg): mode +0x08 (0 OK,
// 1 / 3 Accept / Refuse, 2 a list of rows), the page shown +0x10 and the pages +0x14, the rows
// +0x20 (regSelList: key +0x04, value +0x08), the selected row +0x30 (the NPC panel's
// questSelector, copied every paint; getEvtIndex 0x31E10 returns the selected row's value).
struct Chat {
    u32 addr = 0;
    i32 mode = -1, page = 0, pages = 0, sel = 0;
    std::vector<i32> values;
    bool last_page() const { return page >= pages - 1; }
};
inline Chat chat(const mt_cpu *cpu) {
    Chat c;
    const Hero h = hero(cpu);
    c.addr = h.world != 0 ? rd(cpu, h.world + 0x5C) : 0;
    if (c.addr == 0) {
        return c;
    }
    c.mode = rdi(cpu, c.addr + 0x08);
    c.page = rdi(cpu, c.addr + 0x10);
    const u32 pages = rd(cpu, c.addr + 0x14);
    c.pages = pages != 0 ? static_cast<i32>(std::min(rd(cpu, pages + 4), 100u)) : 0;
    c.sel = rdi(cpu, c.addr + 0x30);
    for (const u32 row : list_items(cpu, rd(cpu, c.addr + 0x20), 10)) {
        c.values.push_back(row != 0 ? rdi(cpu, row + 0x08) : -1);
    }
    return c;
}
inline bool chatting(const mt_cpu *cpu) { return mt_input_panel() == 5 || game_mode(cpu) == 4; }

// The hero's flags (+0x0C): 0x01 in the air (falling), 0x02 jumping, 0x04 on a ladder or rope,
// 0x10 dead (CHero::KeyJump 0x61651, the probe on the first maps).
inline bool standing(const Hero &h) { return h.addr != 0 && (h.flags & 0x17) == 0; }
inline bool on_ladder(const Hero &h) { return (h.flags & 0x04) != 0; }

inline bool trace_on() { return std::getenv("MT_NAV_TRACE") != nullptr; }

// --- going places ------------------------------------------------------------------------------

// Where to go: map (area, map), x on the platform under (x, y), within `tol` px.
struct Goal {
    i32 area = -1, map = -1, x = 0, y = 0, tol = 6;
};

// The navigator's state between turns: the move in progress and what failed.
struct Nav {
    bool acting = false;
    wm::Edge edge{};
    int from = -1, from_y = 0, map_key = -1;
    double start = 0;
    bool left = false; // the hero left the ground during the move
    int last_x = INT32_MIN, last_y = INT32_MIN;
    double still = 0, drink = -10;
    double moved = 0, jumped = -10, settled = -1, stuck_since = -1;
    int idle_x = INT32_MIN, idle_y = INT32_MIN;
    double idle = 0;
    std::map<std::tuple<int, int, int, int>, int> fails; // (map, from, to, move) -> count
    std::set<std::tuple<int, int, int, int>> banned;
    std::string said; // the last trace line (printed once)
};

inline void say(Nav &n, const std::string &s) {
    if (s != n.said) {
        n.said = s;
        std::printf("nav: %s\n", s.c_str());
        std::fflush(stdout);
    }
}

// The cheapest route that avoids the banned moves: banned edges are removed from a copy.
inline std::vector<wm::Step> route(const wm::Map &m, const Nav &n, int key, int from, int x,
                                   int to) {
    if (n.banned.empty()) {
        return wm::route(m, from, x, to);
    }
    wm::Map copy = m;
    for (std::size_t i = 0; i < copy.edges.size(); i++) {
        auto &es = copy.edges[i];
        es.erase(std::remove_if(es.begin(), es.end(),
                                [&](const wm::Edge &e) {
                                    return n.banned.count({key, static_cast<int>(i), e.to,
                                                           static_cast<int>(e.move)}) != 0;
                                }),
                 es.end());
    }
    return wm::route(copy, from, x, to);
}

// Holds the d-pad toward x (0: let go). Where the hero does not move for 0.4 s, a jump with the
// direction held (a step, a small wall).
inline void walk_toward(Run &r, Nav &n, const Hero &h, double el, int dir) {
    if (h.x != n.last_x) {
        n.last_x = h.x;
        n.moved = el;
    }
    if (dir == 0) {
        n.moved = el;
    }
    hold(r, LEFT, dir < 0);
    hold(r, RIGHT, dir > 0);
    if (dir != 0 && el - n.moved > 0.4 && el - n.jumped > 1.0) {
        n.jumped = el;
    }
    hold(r, A, dir != 0 && el - n.jumped < 0.1);
}

inline void end_move(Run &r, Nav &n, const wm::Map &m, const Hero &h, int key) {
    release_all(r);
    n.acting = false;
    // A climb leaves x where it was: walking on starts afresh, not as a walk stuck for the whole
    // climb (walk_toward would jump at once, off a short platform's end).
    n.last_x = INT32_MIN;
    const int now = m.platform_at(h.x, h.y, 2);
    const auto id = std::make_tuple(key, n.from, n.edge.to, static_cast<int>(n.edge.move));
    if (now == n.edge.to) {
        n.fails.erase(id);
        return;
    }
    const int f = ++n.fails[id];
    if (trace_on()) {
        std::printf("nav: %s from platform %d to %d failed (now on %d at %d,%d), %d time(s)\n",
                    wm::move_name(n.edge.move), n.from, n.edge.to, now, h.x, h.y, f);
    }
    if (f >= 3) {
        n.banned.insert(id);
        std::printf("nav: map %d.%d: %s from platform %d to %d banned after 3 failures\n", h.area,
                    h.map, wm::move_name(n.edge.move), n.from, n.edge.to);
    }
}

// One turn of the move in progress (n.acting).
inline void act(Run &r, Nav &n, const wm::Map &m, const Hero &h, double el, int key) {
    const double t = el - n.start;
    const bool stand = standing(h);
    static u64 traced = 0;
    if (std::getenv("MT_ACT_TRACE") != nullptr && r.ticks != traced) {
        traced = r.ticks;
        std::printf("act %s %.2f: x %d y %d state %d flags %X held", wm::move_name(n.edge.move), t,
                    h.x, h.y, h.state, h.flags);
        for (const SDL_GamepadButton b : r.held) {
            std::printf(" %s", SDL_GetGamepadStringForButton(b));
        }
        std::printf("\n");
    }
    n.left = n.left || !stand;
    switch (n.edge.move) {
    case wm::Move::jump: {
        // A running jump keeps its direction in the air until the hero is over the near end of
        // the platform it jumps to (let go at once, a jump up a step across a gap fell short:
        // 2.2's platform 4 to 2), then lets go so it does not run past a short one.
        const wm::Platform &q = m.platforms[static_cast<std::size_t>(n.edge.to)];
        const int over = (n.edge.dir < 0 ? q.x1() - 8 : q.x0() + 8) - h.x;
        const bool carry = n.edge.dir != 0 && over * n.edge.dir > 0;
        if (t < 0.12) {
            hold(r, LEFT, n.edge.dir < 0);
            hold(r, RIGHT, n.edge.dir > 0);
            hold(r, A, true);
        } else {
            hold(r, A, false);
            if (!stand) {
                hold(r, LEFT, carry && n.edge.dir < 0);
                hold(r, RIGHT, carry && n.edge.dir > 0);
            }
        }
        if ((n.left && stand && t > 0.25) || t > 2.5) {
            end_move(r, n, m, h, key);
        }
        break;
    }
    case wm::Move::fall:
        hold(r, LEFT, !n.left && n.edge.dir < 0);
        hold(r, RIGHT, !n.left && n.edge.dir > 0);
        // Down a stair the hero never leaves the ground: done on the next platform.
        if ((n.left && stand) || (stand && t > 0.1 && m.platform_at(h.x, h.y, 2) == n.edge.to) ||
            t > 3.0) {
            end_move(r, n, m, h, key);
        }
        break;
    case wm::Move::climb_up:
    case wm::Move::climb_down: {
        // The game reads one key at a time: after the jump's A is let go, a held up no longer
        // climbs. Where the hero hangs on the ladder without moving, the key is let go for a
        // tick and pressed again.
        const bool up = n.edge.move == wm::Move::climb_up;
        if (h.y != n.last_y) {
            n.last_y = h.y;
            n.still = el;
        }
        const bool again = on_ladder(h) && el - n.still > 0.3 && std::fmod(el - n.still, 0.6) < 0.1;
        hold(r, up ? UP : DOWN, !again);
        hold(r, A,
             up && !on_ladder(h) && stand && std::fmod(t, 1.5) >= 0.1 && std::fmod(t, 1.5) < 0.2);
        if ((stand && !on_ladder(h) && (up ? h.y < n.from_y - 8 : h.y > n.from_y + 8)) || t > 15) {
            end_move(r, n, m, h, key);
        }
        break;
    }
    case wm::Move::walk: // up a stair step: on in the edge's direction
        hold(r, LEFT, n.edge.dir < 0);
        hold(r, RIGHT, n.edge.dir > 0);
        if ((stand && m.platform_at(h.x, h.y, 2) != n.from && t > 0.1) || t > 1.5) {
            end_move(r, n, m, h, key);
        }
        break;
    }
}

// One turn toward goal g. True once the hero stands within g.tol of g.x on g's platform. On
// another map it goes to the portal toward g's map (links()) and up through it; `closed` lists
// portal links it may not take ((area * 100 + map, kind)).
inline bool drive(Run &r, mt_cpu *cpu, double el, Nav &n, const Goal &g,
                  const std::vector<std::pair<int, int>> &closed = {}) {
    const Hero h = hero(cpu);
    if (h.addr == 0 || h.area < 0) {
        return false;
    }
    const int key = h.area * 100 + h.map;
    if (key != n.map_key) {
        release_all(r);
        Nav fresh;
        fresh.fails = std::move(n.fails);
        fresh.banned = std::move(n.banned);
        fresh.map_key = key;
        n = std::move(fresh);
    }
    // A hero that stays put for 10 s: say so once with its state (a stall to diagnose), and let
    // go of everything for a moment.
    if (h.x != n.idle_x || h.y != n.idle_y) {
        n.idle_x = h.x;
        n.idle_y = h.y;
        n.idle = el;
    } else if (el - n.idle > 10) {
        std::printf("nav: map %d.%d: the hero has not moved for 10 s at %d,%d (state %d flags %X, "
                    "acting %d %s)\n",
                    h.area, h.map, h.x, h.y, h.state, h.flags, n.acting ? 1 : 0,
                    wm::move_name(n.edge.move));
        n.idle = el;
        n.acting = false;
        n.stuck_since = -1;
        release_all(r);
        return false;
    }
    const wm::Map &m = wm::get(h.area, h.map);
    if (!m.ok) {
        say(n, "no map data for " + std::to_string(h.area) + "." + std::to_string(h.map));
        return false;
    }
    int tx = g.x, ty = g.y, tol = g.tol;
    bool enter = false;
    if (!h.on(g.area, g.map)) {
        // The portals whose scripts check a quest (script/chapter1/portal*.out) are closed
        // until it allows them: portal201 (2.0) main 9 done, portal212 (2.1) side 25 active,
        // portal240 (2.4) side 7 done, portal371 (3.7) main 18 active. Chapter 1's quest
        // numbers: a later chapter runs common/'s portal scripts, and its quests are others.
        std::vector<std::pair<int, int>> shut = closed;
        const auto gate = [&](int area, int map, int kind, u32 sort, u32 n, i32 state) {
            if (h.chapter == 1 && quest_state(cpu, h, sort, n) != state) {
                shut.push_back({area * 100 + map, kind});
            }
        };
        gate(2, 0, 1, 0, 9, 2);
        gate(2, 1, 2, 1, 25, 1);
        gate(2, 4, 2, 1, 7, 2);
        gate(3, 7, 1, 0, 18, 1);
        const int kind = wm::next_portal(h.area, h.map, g.area, g.map, shut);
        const wm::Object *p = kind >= 0 ? m.portal(kind) : nullptr;
        if (p == nullptr) {
            say(n, "no way from " + std::to_string(h.area) + "." + std::to_string(h.map) + " to " +
                       std::to_string(g.area) + "." + std::to_string(g.map));
            return false;
        }
        tx = p->x;
        ty = p->y;
        tol = 5;
        enter = true;
    }
    if (n.acting) {
        act(r, n, m, h, el, key);
        return false;
    }
    // A Red Potion (Y, quick slot 1) under 40 % of HP, standing, the d-pad let go.
    if (el - n.drink > 1.5 && h.hp * 5 < h.max_hp * 2 && standing(h) && item_count(cpu, h, 3) > 0) {
        n.drink = el;
    }
    if (el - n.drink < 0.3) {
        release_all(r);
        hold(r, Y, el - n.drink >= 0.1 && el - n.drink < 0.2);
        return false;
    }
    hold(r, Y, false);
    if (on_ladder(h)) { // on a ladder with no move in progress: up it
        hold(r, LEFT, false);
        hold(r, RIGHT, false);
        hold(r, UP, true);
        return false;
    }
    hold(r, UP, false);
    hold(r, DOWN, false);
    if (!standing(h)) {
        hold(r, LEFT, false);
        hold(r, RIGHT, false);
        hold(r, A, false);
        return false;
    }
    const int here = m.platform_at(h.x, h.y, 2);
    int there = m.platform_at(tx, ty, 10);
    if (there < 0) {
        there = m.platform_below(tx, ty - 10);
    }
    if (here < 0 || there < 0) {
        say(n, "off the platforms at " + std::to_string(h.x) + "," + std::to_string(h.y) +
                   " (map " + std::to_string(key) + ", goal platform " + std::to_string(there) +
                   ")");
        walk_toward(r, n, h, el, tx > h.x ? 1 : -1);
        return false;
    }
    if (here == there) {
        const int d = tx - h.x;
        if (std::abs(d) <= tol) {
            walk_toward(r, n, h, el, 0);
            hold(r, A, false);
            if (n.settled < 0) {
                n.settled = el;
            }
            if (enter) {
                hold(r, UP, std::fmod(el - n.settled, 1.0) < 0.3);
                return false;
            }
            return el - n.settled >= 0.3 && std::abs(tx - h.x) <= tol + 4;
        }
        n.settled = -1;
        walk_toward(r, n, h, el, d > 0 ? 1 : -1);
        return false;
    }
    n.settled = -1;
    const std::vector<wm::Step> path = route(m, n, key, here, h.x, there);
    if (path.empty()) {
        say(n, "no route on map " + std::to_string(key) + " from platform " + std::to_string(here) +
                   " to " + std::to_string(there) + "; walking toward x " + std::to_string(tx));
        walk_toward(r, n, h, el, std::abs(tx - h.x) > 4 ? (tx > h.x ? 1 : -1) : 0);
        return false;
    }
    const wm::Edge &e = path.front().edge;
    const int drop = m.platforms[static_cast<std::size_t>(e.to)].y() - h.y;
    if (e.move == wm::Move::walk || (e.move == wm::Move::fall && drop <= wm::k_cell)) {
        // A stair up or down: on past the platform's end, no stop.
        walk_toward(r, n, h, el, e.x + e.dir * 16 > h.x ? 1 : -1);
        return false;
    }
    const bool ladder = e.move == wm::Move::climb_up || e.move == wm::Move::climb_down;
    const int step_tol = ladder ? 2 : 4;
    const int d = e.x - h.x;
    if (trace_on()) {
        say(n, "map " + std::to_string(h.area) + "." + std::to_string(h.map) + ": platform " +
                   std::to_string(here) + " -> " + std::to_string(there) + " (" +
                   std::to_string(path.size()) + " moves): " + wm::move_name(e.move) + " at x " +
                   std::to_string(e.x) + " to " + std::to_string(e.to));
    }
    // A running jump off a platform's end starts on the run, a few px early: stopping first slid
    // the hero past the end (2.2's platform 4: off it, and down two floors). Within one tick's
    // walk (7 px) of the edge's x: from 11 px early the jump from 1.0's platform 7 to 5 (33 px
    // across, 16 up) passed the far end 1 px short.
    const bool on_the_run = e.move == wm::Move::jump && e.dir != 0 && d * e.dir >= 0 &&
                            std::abs(d) <= 7 && h.state != 2;
    if (std::abs(d) > step_tol && !on_the_run) {
        n.stuck_since = -1;
        walk_toward(r, n, h, el, d > 0 ? 1 : -1);
        return false;
    }
    // At the start of the move: let go, then do it.
    walk_toward(r, n, h, el, 0);
    hold(r, A, false);
    if (h.state != 2 && !on_the_run) { // still sliding to a stop (or a state that never ends)
        if (n.stuck_since < 0) {
            n.stuck_since = el;
        }
        if (el - n.stuck_since < 0.6) {
            return false;
        }
        if (trace_on()) {
            std::printf("nav: starting the move in state %d flags %X\n", h.state, h.flags);
        }
    }
    n.stuck_since = -1;
    n.acting = true;
    n.edge = e;
    n.from = here;
    n.from_y = h.y;
    n.start = el;
    n.left = false;
    n.last_y = h.y;
    n.still = el;
    return false;
}

// Everything that stops the hero from walking: popups (the death popup's OK is Return to town,
// a notice's OK closes it), the NPC chat and scenes (A through them). True while it handles one.
inline bool interrupted(Run &r, mt_cpu *cpu, double el) {
    if (field(cpu)) {
        return false;
    }
    hold(r, LEFT, false);
    hold(r, RIGHT, false);
    hold(r, UP, false);
    hold(r, DOWN, false);
    hold(r, X, false);
    hold(r, A, std::fmod(el, 0.8) < 0.1);
    return true;
}

// Goes to g; done when the hero stands there.
inline void then_go(Run &r, std::string what, Goal g, double timeout = 300,
                    std::vector<std::pair<int, int>> closed = {}) {
    auto n = std::make_shared<Nav>();
    then(r, "go: " + what, timeout, [&r, g, n, closed](mt_cpu *cpu, double el) {
        if (interrupted(r, cpu, el)) {
            return false;
        }
        if (drive(r, cpu, el, *n, g, closed)) {
            release_all(r);
            return true;
        }
        return false;
    });
}

// The position of the object named `name` on (area, map), as a goal (an NPC: stand on it).
inline Goal at_object(int area, int map, const std::string &name, int tol = 6) {
    const wm::Map &m = wm::get(area, map);
    const wm::Object *o = m.object(name);
    if (o == nullptr) {
        std::printf("FAIL: no object %s on map %d.%d\n", name.c_str(), area, map);
        g_failures++;
        return {area, map, 0, 0, tol};
    }
    return {area, map, o->x, o->y, tol};
}

// The map's live objects (CMMap, *(*0xF13C0); docs/formats/obj.md): the width in tiles +0x20,
// the objects +0x48 (each: cell +0, kind +2, visible +6), their count +0x4C, their script names
// +0x54. A scene can move an object (its commands 37-41 rewrite the cell): the boss map's
// Sabitrama (Sabi2, 3.8) stands elsewhere after Malady's closing scene than the file puts him.
// The anchor of the visible object named `name` on the current map, as at_object gives it.
inline bool live_object(const mt_cpu *cpu, const std::string &name, int &x, int &y) {
    const u32 map = rd(cpu, rd(cpu, 0xF13C0));
    const i32 w = map != 0 ? rdi(cpu, map + 0x20) : 0;
    const u32 objs = map != 0 ? rd(cpu, map + 0x48) : 0;
    const u32 names = map != 0 ? rd(cpu, map + 0x54) : 0;
    const i32 n = map != 0 ? std::min(rdi(cpu, map + 0x4C), 200) : 0;
    for (i32 k = 0; w > 0 && objs != 0 && names != 0 && k < n; k++) {
        const u32 o = rd(cpu, objs + 4 * static_cast<u32>(k));
        const u32 s = rd(cpu, names + 4 * static_cast<u32>(k));
        if (o == 0 || s == 0 || s + name.size() + 1 > cpu->mem_size ||
            std::memcmp(cpu->mem + s, name.c_str(), name.size() + 1) != 0) {
            continue;
        }
        const i32 cell = rd16(cpu, o);
        if (rd16(cpu, o + 6) == 0 || cell < 0) {
            return false;
        }
        x = (cell % w) * 16 + 8;
        y = (cell / w + 1) * 16 - 1;
        return true;
    }
    return false;
}

// --- talking -----------------------------------------------------------------------------------

// Talks to the NPC `npc` on (area, map) until `done` holds in the field: goes there, up to talk,
// A through the pages; on a list of rows it picks the row whose value is `choose` (any row
// when -1), with up / down, and A. Accept / Refuse: A (Accept). With a list of choices, the
// n-th list of one chat takes the n-th (the last for the lists after it): an NPC whose list
// leads to another (the chapter-2 smith: the kind, then the item).
inline void then_talk_to(Run &r, std::string what, int area, int map, const std::string &npc,
                         std::vector<int> choices, Pred done, double timeout = 300) {
    struct S {
        Nav nav;
        double chat = -1, field = -1, pressed = -10;
        int talks = 0;
        std::size_t lists = 0; // the lists answered in this chat
    };
    auto s = std::make_shared<S>();
    const Goal g = at_object(area, map, npc, 6);
    if (choices.empty()) {
        choices.push_back(-1);
    }
    then(r, "talk: " + what, timeout, [&r, s, g, npc, choices, done](mt_cpu *cpu, double el) {
        if (chatting(cpu)) {
            hold(r, UP, false);
            hold(r, LEFT, false);
            hold(r, RIGHT, false);
            s->field = -1;
            if (s->chat < 0) {
                s->chat = el;
                s->talks++;
                s->lists = 0;
            }
            const Chat c = chat(cpu);
            const bool down = std::find(r.held.begin(), r.held.end(), A) != r.held.end() ||
                              std::find(r.held.begin(), r.held.end(), DOWN) != r.held.end() ||
                              std::find(r.held.begin(), r.held.end(), UP) != r.held.end();
            if (down) {
                if (el - s->pressed >= 0.1) {
                    hold(r, A, false);
                    hold(r, DOWN, false);
                    hold(r, UP, false);
                }
                return false;
            }
            if (el - s->chat < 0.8 || el - s->pressed < 0.6) {
                return false;
            }
            SDL_GamepadButton b = A;
            if (c.mode == 2 && c.last_page() && !c.values.empty()) {
                const int choose = choices[std::min(s->lists, choices.size() - 1)];
                int want = 0;
                for (std::size_t i = 0; i < c.values.size(); i++) {
                    if (c.values[i] == choose) {
                        want = static_cast<int>(i);
                    }
                }
                b = c.sel < want ? DOWN : (c.sel > want ? UP : A);
                s->lists += b == A ? 1 : 0;
                if (trace_on()) {
                    std::printf("talk: rows %zu, selected %d, want %d (value %d)\n",
                                c.values.size(), c.sel, want, choose);
                }
            }
            s->pressed = el;
            hold(r, b, true);
            return false;
        }
        s->chat = -1;
        hold(r, A, false);
        hold(r, DOWN, false);
        if (interrupted(r, cpu, el)) {
            s->field = -1;
            return false;
        }
        if (s->field < 0) {
            s->field = el;
        }
        if (el - s->field >= 0.5 && done(cpu)) {
            release_all(r);
            std::printf("talk done after %d talk(s): %s\n", s->talks, where(cpu).c_str());
            return true;
        }
        Goal at = g; // where the NPC stands now, if a scene moved it
        if (hero(cpu).on(g.area, g.map)) {
            live_object(cpu, npc, at.x, at.y);
        }
        if (!drive(r, cpu, el, s->nav, at)) {
            return false;
        }
        // At the NPC: up (one press every 2 s) opens the chat.
        const double f = el - s->field;
        hold(r, UP, f >= 0.5 && std::fmod(f - 0.5, 2.0) < 0.3);
        return false;
    });
}

inline void then_talk_to(Run &r, std::string what, int area, int map, const std::string &npc,
                         int choose, Pred done, double timeout = 300) {
    then_talk_to(r, std::move(what), area, map, npc, std::vector<int>{choose}, std::move(done),
                 timeout);
}

// --- shops -------------------------------------------------------------------------------------

// The shop (game mode 2) on the key pad's panel 6, no popup.
inline bool in_shop(mt_cpu *cpu) {
    return mt_input_panel() == 6 && game_mode(cpu) == 2 && popup_id(cpu) == -1;
}

// Buys other item `id` (its row on the Buy tab) one at a time from the shop NPC `npc` on (area,
// map) until the hero carries `want` of it or has less than `keep` meso left after one more: goes
// there, up opens the shop, down to the row, A (popup 5 / 3: how many, buy?), A (popup 111:
// bought), A; B closes the shop.
// The stock-up's state between turns.
struct Stock {
    Nav nav;
    double pressed = -10;
    bool closing = false;
};

// The count of equipment `id` the hero carries or wears.
inline i32 equip_owned(mt_cpu *cpu, const Hero &h, i32 id);

// One turn of buying `id` from the shop NPC at g, one at a time, until the hero has `want` (an
// other item, or with `equip` a piece of equipment carried or worn) or less than `keep` meso would
// be left. True once done (the shop closed again).
inline bool stock_turn(Run &r, mt_cpu *cpu, double el, Stock &s, const Goal &g, i32 id, i32 want,
                       i32 keep, bool equip = false) {
    const Hero h = hero(cpu);
    const bool enough = (equip ? equip_owned(cpu, h, id) : item_count(cpu, h, id)) >= want;
    const auto press = [&](SDL_GamepadButton b) {
        if (el - s.pressed >= 0.5) {
            s.pressed = el;
        }
        hold(r, b, el - s.pressed < 0.1);
    };
    if (game_mode(cpu) == 2) {
        hold(r, UP, false);
        if (in_shop(cpu)) {
            const Shop sh = shop(cpu);
            i32 row = -1;
            for (i32 i = 0; i < sh.rows && (sh.kind == 1) != equip; i++) {
                row = shop_row(cpu, sh, i).id == id ? i : row;
            }
            const ShopRow w = shop_row(cpu, sh, row);
            if (s.closing || enough || row < 0 || h.meso - w.price < keep) {
                if (!s.closing && row < 0) {
                    std::printf("stock: the shop sells no item %d\n", id);
                }
                s.closing = true;
                press(B);
            } else if (sh.selected != row) {
                press(sh.selected < row ? DOWN : UP);
            } else {
                press(A);
            }
        } else { // a popup: how many, buy?, bought
            press(A);
        }
        return false;
    }
    if (interrupted(r, cpu, el)) {
        return false;
    }
    if (enough || s.closing) {
        release_all(r);
        std::printf("stock done: %d of %s %d, meso %d\n",
                    equip ? equip_owned(cpu, h, id) : item_count(cpu, h, id),
                    equip ? "equipment" : "item", id, h.meso);
        return true;
    }
    if (!drive(r, cpu, el, s.nav, g)) {
        return false;
    }
    press(UP);
    return false;
}

inline void then_stock(Run &r, std::string what, int area, int map, const std::string &npc, i32 id,
                       i32 want, i32 keep = 0, double timeout = 240) {
    auto s = std::make_shared<Stock>();
    const Goal g = at_object(area, map, npc, 6);
    then(r, "stock: " + what, timeout, [&r, s, g, id, want, keep](mt_cpu *cpu, double el) {
        return stock_turn(r, cpu, el, *s, g, id, want, keep);
    });
}

// --- equipment ---------------------------------------------------------------------------------

// The item_equip id worn in slot `slot` (0 hat, 1 weapon, 2 overall, 3 earrings, 4 shoes;
// hero +0xDC[5]), -1 for none; the equipment inventory's slot holding `id` (+0xD4[48]), -1.
inline i32 worn(mt_cpu *cpu, i32 slot) {
    const Hero h = hero(cpu);
    const u32 eq = h.addr != 0 ? rd(cpu, h.addr + 0xDC) : 0;
    const u32 it = eq != 0 ? rd(cpu, eq + 4 * static_cast<u32>(slot)) : 0;
    return it != 0 ? rdi(cpu, it + 0x90) : -1;
}
inline i32 bag_slot(mt_cpu *cpu, i32 id) {
    const Hero h = hero(cpu);
    const u32 inv = h.addr != 0 ? rd(cpu, h.addr + 0xD4) : 0;
    for (u32 i = 0; inv != 0 && i < 48; i++) {
        const u32 it = rd(cpu, inv + 4 * i);
        if (it != 0 && rdi(cpu, it + 0x90) == id) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// The cell of equipment `id` in the equip tab's list: the list shows the carried items without
// the empty inventory slots between them. -1 if it is not carried.
inline i32 bag_cell(mt_cpu *cpu, i32 id) {
    const Hero h = hero(cpu);
    const u32 inv = h.addr != 0 ? rd(cpu, h.addr + 0xD4) : 0;
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

// Wears equipment `id` (into item_equip slot `slot`) by the popup menu: Start, right to the
// equip tab (2), A opens its list, left / right to the item's cell, A and A wear it; B until
// the field. (On the item tab, A twice on an item asks to discard it: never there.)
inline void then_equip(Run &r, std::string what, i32 id, i32 slot) {
    struct S {
        double pressed = -10;
    };
    auto s = std::make_shared<S>();
    then(r, "equip: " + what, 60, [&r, s, id, slot](mt_cpu *cpu, double el) {
        const auto press = [&](SDL_GamepadButton b) {
            if (el - s->pressed >= 0.5) {
                s->pressed = el;
                hold(r, b, true);
            }
        };
        if (el - s->pressed >= 0.1) {
            release_all(r);
        }
        if (el - s->pressed < 0.5) {
            return false;
        }
        const bool wearing = worn(cpu, slot) == id;
        if (field(cpu)) {
            if (wearing || bag_slot(cpu, id) < 0) {
                std::printf("equip: item %d, slot %d wears %d\n", id, slot, worn(cpu, slot));
                return true;
            }
            press(START);
            return false;
        }
        if (game_mode(cpu) != 1 || popup_id(cpu) != -1) {
            press(A); // something else: on through it
            return false;
        }
        if (wearing || bag_slot(cpu, id) < 0) {
            press(B);
            return false;
        }
        if (menu_tab(cpu) != 2) {
            press(menu_sub(cpu) != 0 ? B : RIGHT);
            return false;
        }
        if (menu_sub(cpu) == 0) {
            press(A);
            return false;
        }
        // The cursor (CDataMgr +0xDC, a cell of the list) stays where it was left between
        // openings: moved to the item's cell, then A opens the item's sub-list (+0xBC) and A on
        // its first entry wears it.
        const u32 m = data_mgr(cpu);
        if (cpu->mem[m + 0xBC] == 0) {
            const i32 cursor = rdi(cpu, m + 0xDC);
            const i32 cell = bag_cell(cpu, id);
            if (cursor != cell) {
                press(cursor < cell ? RIGHT : LEFT);
                return false;
            }
        }
        press(A);
        return false;
    });
}

// Buys equipment `id` from the equip shop NPC `npc` on (area, map), keeping at least `keep` meso,
// and wears it in item_equip slot `slot`. Short of meso it buys nothing and goes on.
inline void then_gear(Run &r, std::string what, int area, int map, const std::string &npc, i32 id,
                      i32 slot, i32 keep = 0, double timeout = 240) {
    auto s = std::make_shared<Stock>();
    const Goal g = at_object(area, map, npc, 6);
    then(r, "gear: " + what, timeout, [&r, s, g, id, keep](mt_cpu *cpu, double el) {
        return stock_turn(r, cpu, el, *s, g, id, 1, keep, true);
    });
    then_equip(r, what, id, slot);
}

// --- the skill tab and the quick slots ---------------------------------------------------------
//
// Hero (docs/formats/save.md): free SP +0xBC; the skills +0x114, slot i: id +0x08 + 8 i (-1 when
// unused), level +0x0C + 8 i, the skill tab's rows in order; quick slot n {kind +0xE0 + 8 n, id
// +0xE4 + 8 n}, kind 2 a skill. The tab's state, in CDataMgr (PopupMenu_SkillTabOnKey 0x54051):
// the row +0xD4 plus the keys' scroll *(+0x10C) +4; the sub-list open +0xBC, its entries +0xC4,
// its row +0xC8; choosing a quick slot +0xCC, the slot +0xD0 (docs/ANALYSIS.md).
struct SkillSlot {
    i32 id = -1, level = 0;
};
inline std::vector<SkillSlot> skill_slots(const mt_cpu *cpu, const Hero &h) {
    std::vector<SkillSlot> out;
    const u32 skills = h.addr != 0 ? rd(cpu, h.addr + 0x114) : 0;
    for (u32 i = 0; skills != 0 && i < 13; i++) {
        out.push_back({rdi(cpu, skills + 0x08 + 8 * i), rdi(cpu, skills + 0x0C + 8 * i)});
    }
    return out;
}
inline i32 skill_level(const mt_cpu *cpu, const Hero &h, i32 id) {
    for (const SkillSlot &s : skill_slots(cpu, h)) {
        if (s.id == id) {
            return s.level;
        }
    }
    return 0;
}
inline i32 free_sp(const mt_cpu *cpu, const Hero &h) {
    return h.addr != 0 ? rdi(cpu, h.addr + 0xBC) : 0;
}
// The skill on quick slot n, -1 for none.
inline i32 quick_slot_skill(const mt_cpu *cpu, const Hero &h, u32 n) {
    return h.addr != 0 && rdi(cpu, h.addr + 0xE0 + 8 * n) == 2 ? rdi(cpu, h.addr + 0xE4 + 8 * n)
                                                               : -1;
}

// Presses in menus: a button held 0.1 s, the next press 0.5 s later.
struct MenuPad {
    double pressed = -10;
    bool ready(Run &r, double el) {
        if (el - pressed >= 0.1) {
            release_all(r);
        }
        return el - pressed >= 0.5;
    }
    void press(Run &r, double el, SDL_GamepadButton b) {
        pressed = el;
        hold(r, b, true);
    }
};

// One turn toward the skill tab (3) of the popup menu open, from the field or another tab.
// True when it is open and the bot may press there. A popup is answered with A (OK).
inline bool at_skill_tab(Run &r, mt_cpu *cpu, double el, MenuPad &k) {
    if (field(cpu)) {
        k.press(r, el, START);
        return false;
    }
    if (game_mode(cpu) != 1 || popup_id(cpu) != -1) {
        k.press(r, el, A);
        return false;
    }
    if (menu_tab(cpu) != 3) {
        k.press(r, el, menu_sub(cpu) != 0 ? B : RIGHT);
        return false;
    }
    if (menu_sub(cpu) != 3) {
        k.press(r, el, A);
        return false;
    }
    return true;
}

// The row of skill `id` on the skill tab (its slot), -1 if the hero has none.
inline i32 skill_row(const mt_cpu *cpu, const Hero &h, i32 id) {
    const std::vector<SkillSlot> slots = skill_slots(cpu, h);
    for (std::size_t i = 0; i < slots.size(); i++) {
        if (slots[i].id == id) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// Moves the skill tab's row to `row`, or (at it) presses A. True when the sub-list is open.
inline bool on_skill_row(Run &r, mt_cpu *cpu, double el, MenuPad &k, i32 row) {
    const u32 m = data_mgr(cpu);
    if (cpu->mem[m + 0xBC] != 0) {
        return true;
    }
    const i32 cur = rdi(cpu, m + 0xD4) + rdi(cpu, rd(cpu, m + 0x10C) + 4);
    k.press(r, el, cur < row ? DOWN : cur > row ? UP : A);
    return false;
}

// Spends the hero's free SP on the skill tab, one point at a time, in `plan`'s order: each entry
// (skill id, the level to reach, within the skill's max_level). Done when no entry needs a point
// or no SP is left.
inline void then_spend_sp(Run &r, std::string what, std::vector<std::pair<i32, i32>> plan) {
    auto k = std::make_shared<MenuPad>();
    then(r, "skills: " + what, 120, [&r, k, plan](mt_cpu *cpu, double el) {
        if (!k->ready(r, el)) {
            return false;
        }
        const Hero h = hero(cpu);
        i32 want = -1;
        for (const auto &[id, level] : plan) {
            if (want < 0 && free_sp(cpu, h) > 0 && skill_row(cpu, h, id) >= 0 &&
                skill_level(cpu, h, id) < level) {
                want = id;
            }
        }
        if (want < 0) {
            if (field(cpu)) {
                std::string levels;
                for (const auto &[id, level] : plan) {
                    levels += " skill " + std::to_string(id) + " level " +
                              std::to_string(skill_level(cpu, h, id));
                }
                std::printf("skills done: SP %d,%s\n", free_sp(cpu, h), levels.c_str());
                return true;
            }
            k->press(r, el, popup_id(cpu) != -1 ? A : B);
            return false;
        }
        if (!at_skill_tab(r, cpu, el, *k)) {
            return false;
        }
        if (on_skill_row(r, cpu, el, *k, skill_row(cpu, h, want))) {
            k->press(r, el, A); // the sub-list's first entry: level up
        }
        return false;
    });
}

// Puts skill `id` (learned) on quick slot n (1-4: the short keys Y, B, LB, RB) by the skill tab:
// A on its row opens the sub-list, its second-to-last entry assigns, left / right choose the slot
// and A places it, B back to the field. A slot that holds it stays as it is (assigning it again
// would clear it).
inline void then_quick_slot(Run &r, std::string what, i32 id, u32 n) {
    auto k = std::make_shared<MenuPad>();
    then(r, "quick slot: " + what, 60, [&r, k, id, n](mt_cpu *cpu, double el) {
        if (!k->ready(r, el)) {
            return false;
        }
        const Hero h = hero(cpu);
        const u32 m = data_mgr(cpu);
        if (quick_slot_skill(cpu, h, n) == id) {
            if (field(cpu)) {
                std::printf("quick slot %u holds skill %d\n", n, id);
                return true;
            }
            k->press(r, el, popup_id(cpu) != -1 ? A : B);
            return false;
        }
        if (!at_skill_tab(r, cpu, el, *k)) {
            return false;
        }
        if (cpu->mem[m + 0xCC] != 0) { // choosing the slot: left / right, A
            const i32 slot = rdi(cpu, m + 0xD0);
            k->press(r, el,
                     slot < static_cast<i32>(n)   ? RIGHT
                     : slot > static_cast<i32>(n) ? LEFT
                                                  : A);
            return false;
        }
        if (!on_skill_row(r, cpu, el, *k, skill_row(cpu, h, id))) {
            return false;
        }
        const i32 last = static_cast<i32>(list_items(cpu, rd(cpu, m + 0xC4), 8).size()) - 2;
        const i32 row = rdi(cpu, m + 0xC8);
        k->press(r, el, row < last ? DOWN : row > last ? UP : A);
        return false;
    });
}

// --- hunting -----------------------------------------------------------------------------------

// What then_farm restocks: buys `id` up to `want` (keeping `keep` meso) when the hero has fewer
// than `below` of it and at least `min_meso`.
struct Restock {
    i32 id = 3, want = 100, keep = 500, below = 1, min_meso = 1500;
};
// How then_farm fights. The defaults are the plain attack (X) and Red Potions restocked at the
// area's shop when they run out; a skill button (`skill`) is pressed instead of X with at least
// `min_mp` MP; a Red Potion under hp_pct % of HP, a Blue Potion under mp_pct % of MP.
// A flying boss (`reach_up` above 6): a mob counts as on the hero's floor while its anchor is up
// to `reach_up` px above the hero's feet (its body is that tall), and the hero follows it along
// the floor it stands on instead of over the platforms.
struct FarmStyle {
    SDL_GamepadButton skill = SDL_GAMEPAD_BUTTON_INVALID;
    i32 min_mp = 0, hp_pct = 33, mp_pct = 25, reach_up = 6;
    std::vector<Restock> restock{Restock{}};
};

// Hunts mobs of `ids` on map (area, map) until `done`: goes to the map (from wherever, after a
// fall too: the death popup's OK returns to town), then each turn
//   - a Red Potion (Y, quick slot 1) under a third of HP, a Blue Potion (B, slot 2) under a
//     quarter of MP, with the d-pad let go;
//   - whatever mob is in reach on the hero's floor (it would knock the hero back), else the
//     nearest mob of `ids` on the floor: walked to, faced and attacked with X;
//   - else the nearest drop on the floor: walked onto (the game picks it up on contact);
//   - else the nearest mob of `ids` anywhere on the map, gone to over the platforms (drive).
// `style` adds an attack skill on a quick slot's button, restocking and the potion thresholds.
inline void then_farm(Run &r, std::string what, int area, int map, std::vector<i32> ids, Pred done,
                      double timeout, FarmStyle style = {}) {
    struct S {
        Nav nav;
        int facing = 1, falls = 0;
        double attack = -10, drink = -10, turn = -10, report = 0;
        SDL_GamepadButton potion = Y;
        bool away = false;
        u32 chase = 0;
        Stock stock;
        bool restocking = false;
        double restock_after = -1e9;
        std::size_t restock_index = 0;
    };
    auto s = std::make_shared<S>();
    // Out of Red Potions with meso to spare: more from the area's town shop (Etc00 in Kerning
    // City, Etc10 in Perion, Etc20 in Henesys, Etc30 on 3.0), then back.
    static const char *const k_shops[] = {"Etc00", "Etc10", "Etc20", "Etc30"};
    const Goal shop_at = area >= 0 && area <= 3 ? at_object(area, 0, k_shops[area]) : Goal{};
    then(r, "farm: " + what, timeout,
         [&r, s, area, map, ids, done, shop_at, style](mt_cpu *cpu, double el) {
             const Hero h = hero(cpu);
             if (done(cpu)) {
                 release_all(r);
                 std::printf("farm over at %.2f s (%d falls): %s\n", r.t, s->falls,
                             hero_text(cpu, h).c_str());
                 return true;
             }
             if (h.addr != 0 && h.hp <= 0 && !s->away) {
                 s->away = true;
                 s->falls++;
                 std::printf("farm: the hero fell at %.2f s (%s)\n", r.t, where(cpu).c_str());
             }
             if (!s->restocking && shop_at.area >= 0 && h.addr != 0 && h.hp > 0 &&
                 el >= s->restock_after) {
                 for (std::size_t i = 0; i < style.restock.size() && !s->restocking; i++) {
                     const Restock &w = style.restock[i];
                     if (item_count(cpu, h, w.id) < w.below && h.meso >= w.min_meso) {
                         s->restocking = true;
                         s->stock = Stock{};
                         s->restock_index = i;
                         std::printf("farm: restock item %d at %.2f s with %d meso: to the shop\n",
                                     w.id, r.t, h.meso);
                     }
                 }
             }
             if (s->restocking) {
                 const Restock &w = style.restock[s->restock_index];
                 if (stock_turn(r, cpu, el, s->stock, shop_at, w.id, w.want, w.keep)) {
                     s->restocking = false;
                     s->restock_after = el + 120;
                 }
                 return false;
             }
             if (interrupted(r, cpu, el)) {
                 return false;
             }
             s->away = h.hp <= 0;
             if (el - s->report >= (trace_on() ? 10 : 60)) {
                 s->report = el;
                 std::printf("farm %.0f s: %s\n", el, hero_text(cpu, h).c_str());
                 if (trace_on()) {
                     for (const Mob &m : mobs(cpu)) {
                         std::printf("farm: mob %d at %d,%d HP %d\n", m.id, m.x, m.y, m.hp);
                     }
                 }
             }
             if (!h.on(area, map)) {
                 const wm::Map &m = wm::get(area, map);
                 const Goal g{area, map, m.objects.empty() ? 0 : m.objects.front().x,
                              m.objects.empty() ? 0 : m.objects.front().y, 8};
                 drive(r, cpu, el, s->nav, g);
                 return false;
             }
             // A potion.
             const bool low_hp = h.hp * 100 < h.max_hp * style.hp_pct;
             const bool low_mp = h.mp * 100 < h.max_mp * style.mp_pct;
             if (el - s->drink > 1.5 && (low_hp || low_mp) && standing(h)) {
                 s->drink = el;
                 s->potion = low_hp ? Y : B;
                 s->nav.acting = false;
             }
             if (el - s->drink < 0.3) {
                 const double d = el - s->drink;
                 release_all(r);
                 hold(r, s->potion, d >= 0.1 && d < 0.2);
                 return false;
             }
             hold(r, Y, false);
             hold(r, B, false);
             if (s->nav.acting) { // a move between platforms goes on to its end
                 drive(r, cpu, el, s->nav, {area, map, h.x, h.y, 8});
                 return false;
             }
             const auto wanted = [&ids](i32 id) {
                 return ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end();
             };
             const std::vector<Mob> all = mobs(cpu);
             const Mob *target = nullptr;
             if (standing(h)) {
                 for (const int pass : {0, 1}) {
                     for (const Mob &m : all) {
                         const int dist = std::abs(m.x - h.x);
                         if (m.hp <= 0 || m.y - h.y > 6 || h.y - m.y > style.reach_up ||
                             (pass == 0 && dist > 50) || (pass == 1 && !wanted(m.id)) ||
                             dist > 500) {
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
             }
             if (target != nullptr) {
                 const int dx = target->x - h.x;
                 int go = 0;
                 if (std::abs(dx) > 30) {
                     go = dx > 0 ? 1 : -1;
                 } else if (s->facing != (dx >= 0 ? 1 : -1) && el - s->turn > 0.6) {
                     s->turn = el; // a short step to face it
                     s->facing = dx >= 0 ? 1 : -1;
                 }
                 const bool turning = el - s->turn < 0.05;
                 if (go != 0) {
                     s->facing = go;
                 }
                 const int dir = go != 0 ? go : (turning ? s->facing : 0);
                 walk_toward(r, s->nav, h, el, dir);
                 if (go == 0 && !turning && el - s->attack > 0.45) {
                     s->attack = el;
                 }
                 const bool skill =
                     style.skill != SDL_GAMEPAD_BUTTON_INVALID && h.mp >= style.min_mp;
                 hold(r, X, go == 0 && !skill && el - s->attack < 0.1);
                 if (style.skill != SDL_GAMEPAD_BUTTON_INVALID) {
                     hold(r, style.skill, go == 0 && skill && el - s->attack < 0.1);
                 }
                 return false;
             }
             hold(r, X, false);
             if (style.skill != SDL_GAMEPAD_BUTTON_INVALID) {
                 hold(r, style.skill, false);
             }
             // A drop on the floor.
             int aim = INT32_MIN;
             for (const Drop &d : drops(cpu)) {
                 if (d.state == 0 && !d.refused && std::abs(d.y - h.y) <= 12 &&
                     std::abs(d.x - h.x) <= 300 &&
                     (aim == INT32_MIN || std::abs(d.x - h.x) < std::abs(aim - h.x))) {
                     aim = d.x;
                 }
             }
             if (aim != INT32_MIN && standing(h)) {
                 walk_toward(r, s->nav, h, el, std::abs(aim - h.x) > 3 ? (aim > h.x ? 1 : -1) : 0);
                 return false;
             }
             // The nearest wanted mob anywhere on the map.
             const Mob *far = nullptr;
             for (const Mob &m : all) {
                 if (m.hp > 0 && wanted(m.id) &&
                     (far == nullptr || std::abs(m.x - h.x) + 2 * std::abs(m.y - h.y) <
                                            std::abs(far->x - h.x) + 2 * std::abs(far->y - h.y))) {
                     far = &m;
                 }
             }
             if (far == nullptr) { // none alive: wait where the hero is
                 walk_toward(r, s->nav, h, el, 0);
                 hold(r, A, false);
                 return false;
             }
             if (style.reach_up > 6) { // a flying boss: under it, on this floor
                 walk_toward(r, s->nav, h, el,
                             std::abs(far->x - h.x) > 8 ? (far->x > h.x ? 1 : -1) : 0);
                 return false;
             }
             drive(r, cpu, el, s->nav, {area, map, far->x, far->y, 24});
             return false;
         });
}

// --- quest predicates ----------------------------------------------------------------------------

inline Pred quest_is(i32 sort, u32 n, i32 state) {
    return [sort, n, state](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), static_cast<u32>(sort), n) == state;
    };
}
inline Pred quest_at_least(i32 sort, u32 n, i32 state) {
    return [sort, n, state](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), static_cast<u32>(sort), n) >= state;
    };
}

// The count of equipment `id` (item_equip) in the hero's equipment inventory (+0xD4[48]).
inline i32 equip_count(mt_cpu *cpu, const Hero &h, i32 id) {
    const u32 inv = h.addr != 0 ? rd(cpu, h.addr + 0xD4) : 0;
    i32 n = 0;
    for (u32 i = 0; inv != 0 && i < 48; i++) {
        const u32 it = rd(cpu, inv + 4 * i);
        n += it != 0 && rdi(cpu, it + 0x90) == id ? 1 : 0;
    }
    return n;
}

inline i32 equip_owned(mt_cpu *cpu, const Hero &h, i32 id) {
    i32 n = equip_count(cpu, h, id);
    for (i32 slot = 0; slot < 5; slot++) {
        n += worn(cpu, slot) == id ? 1 : 0;
    }
    return n;
}

// Every requirement of active quest (sort, n) met, as checkRequire (0x7B151) sees it: the
// items in the inventory, the defeat counts, the meso.
inline bool requirements_met(mt_cpu *cpu, i32 sort, i32 n) {
    const Hero h = hero(cpu);
    for (const ActiveQuest &a : active_quests(cpu, h)) {
        if (a.sort != sort || a.num != n) {
            continue;
        }
        for (const Requirement &q : a.reqs) {
            const bool met = q.kind == 1        ? q.progress >= q.needed
                             : q.kind == 2      ? h.meso >= q.meso
                             : q.item_kind == 1 ? item_count(cpu, h, q.item) >= q.items
                                                : equip_count(cpu, h, q.item) >= q.items;
            if (!met) {
                return false;
            }
        }
        return true;
    }
    return false;
}
inline Pred ready(i32 sort, i32 n) {
    return [sort, n](mt_cpu *cpu) { return requirements_met(cpu, sort, n); };
}
// The defeat count of mob `mob` in active quest (sort, n), or -1.
inline i32 defeats(mt_cpu *cpu, i32 sort, i32 n, i32 mob) {
    for (const ActiveQuest &a : active_quests(cpu, hero(cpu))) {
        for (const Requirement &q : a.reqs) {
            if (a.sort == sort && a.num == n && q.kind == 1 && q.target == mob) {
                return q.progress;
            }
        }
    }
    return -1;
}
inline Pred has_item(i32 id, i32 count) {
    return [id, count](mt_cpu *cpu) { return item_count(cpu, hero(cpu), id) >= count; };
}

} // namespace quest_bot

#endif // MAPLETHIEF_TESTS_QUEST_BOT_H
