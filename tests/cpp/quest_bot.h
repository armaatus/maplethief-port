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
    case wm::Move::jump:
        if (t < 0.12) {
            hold(r, LEFT, n.edge.dir < 0);
            hold(r, RIGHT, n.edge.dir > 0);
            hold(r, A, true);
        } else {
            hold(r, A, false);
            if (!stand) {
                hold(r, LEFT, false);
                hold(r, RIGHT, false);
            }
        }
        if ((n.left && stand && t > 0.25) || t > 2.5) {
            end_move(r, n, m, h, key);
        }
        break;
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
        // portal240 (2.4) side 7 done, portal371 (3.7) main 18 active.
        std::vector<std::pair<int, int>> shut = closed;
        const auto gate = [&](int area, int map, int kind, u32 sort, u32 n, i32 state) {
            if (quest_state(cpu, h, sort, n) != state) {
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
    if (std::abs(d) > step_tol) {
        n.stuck_since = -1;
        walk_toward(r, n, h, el, d > 0 ? 1 : -1);
        return false;
    }
    // At the start of the move: let go, then do it.
    walk_toward(r, n, h, el, 0);
    hold(r, A, false);
    if (h.state != 2) { // still sliding to a stop (or in a state that never ends: go anyway)
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

// --- talking -----------------------------------------------------------------------------------

// Talks to the NPC `npc` on (area, map) until `done` holds in the field: goes there, up to talk,
// A through the pages; on a list of rows it picks the row whose value is `choose` (any row
// when -1), with up / down, and A. Accept / Refuse: A (Accept).
inline void then_talk_to(Run &r, std::string what, int area, int map, const std::string &npc,
                         int choose, Pred done, double timeout = 300) {
    struct S {
        Nav nav;
        double chat = -1, field = -1, pressed = -10;
        int talks = 0;
    };
    auto s = std::make_shared<S>();
    const Goal g = at_object(area, map, npc, 6);
    then(r, "talk: " + what, timeout, [&r, s, g, choose, done](mt_cpu *cpu, double el) {
        if (chatting(cpu)) {
            hold(r, UP, false);
            hold(r, LEFT, false);
            hold(r, RIGHT, false);
            s->field = -1;
            if (s->chat < 0) {
                s->chat = el;
                s->talks++;
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
                int want = 0;
                for (std::size_t i = 0; i < c.values.size(); i++) {
                    if (c.values[i] == choose) {
                        want = static_cast<int>(i);
                    }
                }
                b = c.sel < want ? DOWN : (c.sel > want ? UP : A);
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
        if (!drive(r, cpu, el, s->nav, g)) {
            return false;
        }
        // At the NPC: up (one press every 2 s) opens the chat.
        const double f = el - s->field;
        hold(r, UP, f >= 0.5 && std::fmod(f - 0.5, 2.0) < 0.3);
        return false;
    });
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

// One turn of buying item `id` from the shop NPC at g, one at a time, until the hero has `want`
// or less than `keep` meso would be left. True once done (the shop closed again).
inline bool stock_turn(Run &r, mt_cpu *cpu, double el, Stock &s, const Goal &g, i32 id, i32 want,
                       i32 keep) {
    const Hero h = hero(cpu);
    const bool enough = item_count(cpu, h, id) >= want;
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
            for (i32 i = 0; i < sh.rows && sh.kind == 1; i++) {
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
        std::printf("stock done: %d of item %d, meso %d\n", item_count(cpu, h, id), id, h.meso);
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

// Wears equipment `id` (into item_equip slot `slot`) by the popup menu: Start, right to the
// equip tab (2), A opens its list, right to the item's place in the list, A wears it; B until
// the field. (On the item tab, A twice on an item asks to discard it: never there.)
inline void then_equip(Run &r, std::string what, i32 id, i32 slot) {
    struct S {
        double pressed = -10;
        int moved = 0;
        bool entered = false;
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
                std::printf("equip: weapon %d, slot %d wears %d\n", id, slot, worn(cpu, slot));
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
            s->entered = false;
            press(A);
            return false;
        }
        if (!s->entered) { // the list's first item chosen
            s->entered = true;
            s->moved = 0;
            press(A);
            return false;
        }
        if (s->moved < bag_slot(cpu, id)) {
            s->moved++;
            press(RIGHT);
            return false;
        }
        press(A);
        return false;
    });
}

// --- hunting -----------------------------------------------------------------------------------

// Hunts mobs of `ids` on map (area, map) until `done`: goes to the map (from wherever, after a
// fall too: the death popup's OK returns to town), then each turn
//   - a Red Potion (Y, quick slot 1) under a third of HP, a Blue Potion (B, slot 2) under a
//     quarter of MP, with the d-pad let go;
//   - whatever mob is in reach on the hero's floor (it would knock the hero back), else the
//     nearest mob of `ids` on the floor: walked to, faced and attacked with X;
//   - else the nearest drop on the floor: walked onto (the game picks it up on contact);
//   - else the nearest mob of `ids` anywhere on the map, gone to over the platforms (drive).
inline void then_farm(Run &r, std::string what, int area, int map, std::vector<i32> ids, Pred done,
                      double timeout) {
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
    };
    auto s = std::make_shared<S>();
    // Out of Red Potions with meso to spare: more from the area's town shop (Etc00 in Kerning
    // City, Etc10 in Perion, Etc20 in Henesys), then back.
    static const char *const k_shops[] = {"Etc00", "Etc10", "Etc20"};
    const Goal shop_at = area >= 0 && area <= 2 ? at_object(area, 0, k_shops[area]) : Goal{};
    then(r, "farm: " + what, timeout,
         [&r, s, area, map, ids, done, shop_at](mt_cpu *cpu, double el) {
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
                 item_count(cpu, h, 3) == 0 && h.meso >= 1500 && el >= s->restock_after) {
                 s->restocking = true;
                 s->stock = Stock{};
                 std::printf("farm: out of Red Potions at %.2f s with %d meso: to the shop\n", r.t,
                             h.meso);
             }
             if (s->restocking) {
                 if (stock_turn(r, cpu, el, s->stock, shop_at, 3, 100, 500)) {
                     s->restocking = false;
                     s->restock_after = el + 120;
                 }
                 return false;
             }
             if (interrupted(r, cpu, el)) {
                 return false;
             }
             s->away = h.hp <= 0;
             if (el - s->report >= 60) {
                 s->report = el;
                 std::printf("farm %.0f s: %s\n", el, hero_text(cpu, h).c_str());
             }
             if (!h.on(area, map)) {
                 const wm::Map &m = wm::get(area, map);
                 const Goal g{area, map, m.objects.empty() ? 0 : m.objects.front().x,
                              m.objects.empty() ? 0 : m.objects.front().y, 8};
                 drive(r, cpu, el, s->nav, g);
                 return false;
             }
             // A potion.
             if (el - s->drink > 1.5 && (h.hp * 3 < h.max_hp || h.mp * 4 < h.max_mp) &&
                 standing(h)) {
                 s->drink = el;
                 s->potion = h.hp * 3 < h.max_hp ? Y : B;
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
                         if (m.hp <= 0 || std::abs(m.y - h.y) > 6 || (pass == 0 && dist > 50) ||
                             (pass == 1 && !wanted(m.id)) || dist > 500) {
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
                 hold(r, X, go == 0 && el - s->attack < 0.1);
                 return false;
             }
             hold(r, X, false);
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
