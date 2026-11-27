// The bot's world tables as world_map.h reads them (#297): mtport.botworld's format, from a
// synthetic file (tests/cpp/data/bot_world/bot_world.txt). The generator's side is
// tests/test_botworld.py.
#include "world_map.h"

#include <doctest/doctest.h>

#include <map>
#include <utility>

namespace {

using States = std::map<std::pair<int, int>, int>;

int state(const States &s, int kind, int number) {
    const auto it = s.find({kind, number});
    return it == s.end() ? 0 : it->second;
}

const world_map::Link &link(int chapter, int area, int map, int kind) {
    for (const world_map::Link &l : world_map::links()) {
        if (l.chapter == chapter && l.area == area && l.map == map && l.kind == kind) {
            return l;
        }
    }
    FAIL("no link");
    return world_map::links().front();
}

} // namespace

TEST_CASE("the tables load every link and shop") {
    CHECK(world_map::links().size() == 10);
    const world_map::Link &l = link(2, 0, 1, 1);
    REQUIRE(l.gate.size() == 2);
    REQUIRE(l.gate[0].size() == 1);
    CHECK(l.gate[0][0].known);
    CHECK(l.gate[0][0].kind == 1);
    CHECK(l.gate[0][0].number == 25);
    CHECK(l.gate[0][0].op == "!=");
    CHECK(l.gate[0][0].value == 1);
    REQUIRE(l.gate[1].size() == 2);
    CHECK_FALSE(l.gate[1][0].known);
    CHECK(l.gate[1][1].op == ">=");
    CHECK(link(1, 0, 0, 0).gate.empty());
}

TEST_CASE("a gate opens with the quest states") {
    States s;
    const auto read = [&s](int kind, int number, bool require) {
        return require ? 0 : state(s, kind, number);
    };
    const world_map::Link &perion = link(1, 0, 1, 1);
    CHECK_FALSE(world_map::open(perion, read));
    s[{0, 9}] = 1;
    CHECK_FALSE(world_map::open(perion, read));
    s[{0, 9}] = 2;
    CHECK(world_map::open(perion, read));
    CHECK(world_map::open(link(1, 0, 0, 0), read));
    // An alternative with a term the bot can't test is shut; the other may hold.
    const world_map::Link &two = link(2, 0, 1, 1);
    s[{1, 25}] = 1;
    s[{1, 3}] = 2;
    CHECK_FALSE(world_map::open(two, read));
    s[{1, 25}] = 2;
    CHECK(world_map::open(two, read));
    CHECK_FALSE(world_map::open(link(2, 0, 1, 2), read));
}

TEST_CASE("a requirement term opens until the quest's requirements are met") {
    States s{{{0, 8}, 1}};
    bool met = false;
    const auto read = [&](int kind, int number, bool require) {
        return require ? (met ? 1 : 0) : state(s, kind, number);
    };
    const world_map::Link &boss = link(2, 0, 3, 1);
    REQUIRE(boss.gate.size() == 1);
    CHECK(boss.gate[0][0].require);
    CHECK_FALSE(boss.gate[0][1].require);
    CHECK(world_map::open(boss, read));
    met = true;
    CHECK_FALSE(world_map::open(boss, read));
    met = false;
    s[{0, 8}] = 2;
    CHECK_FALSE(world_map::open(boss, read));
}

TEST_CASE("routes use the chapter's links and skip the closed ones") {
    CHECK(world_map::next_portal(1, 0, 0, 0, 2) == 0);
    CHECK(world_map::next_portal(1, 0, 1, 0, 2) == 1);
    // 0.1's portal 1 shut: round by 0.3.
    CHECK(world_map::next_portal(1, 0, 1, 0, 2, {{1, 1}}) == 2);
    CHECK(world_map::next_portal(1, 0, 1, 0, 2, {{1, 1}, {1, 2}}) == -1);
    // Chapter 2 has no link from 0.1 back to 0.0, nor 0.3 to 0.2.
    CHECK(world_map::next_portal(2, 0, 1, 0, 0) == -1);
    CHECK(world_map::next_portal(2, 0, 0, 0, 3) == 0);
    CHECK(world_map::next_portal(3, 0, 0, 0, 1) == -1);
}

TEST_CASE("the shop that sells an item") {
    const world_map::Shop *s = world_map::shop_selling(2, 1, 6);
    REQUIRE(s != nullptr);
    CHECK(s->npc == "Etc20");
    CHECK(s->map == 0);
    CHECK(s->price == 30);
    REQUIRE(world_map::shop_selling(3, 0, 53) != nullptr);
    CHECK(world_map::shop_selling(3, 0, 53)->npc == "Equip30");
    CHECK(world_map::shop_selling(3, 1, 53) == nullptr); // the other table
    CHECK(world_map::shop_selling(1, 1, 3) == nullptr);  // no shop in the area
    REQUIRE(world_map::shop_selling(2, 1, 3, 2) != nullptr);
    CHECK(world_map::shop_selling(2, 1, 3, 2)->npc == "Etc21");
    CHECK(world_map::shop_selling(2, 1, 6, 2) == nullptr);
}

TEST_CASE("a term the format does not know is unknown") {
    CHECK_FALSE(world_map::parse_term("?").known);
    CHECK_FALSE(world_map::parse_term("x0.1==2").known);
    CHECK_FALSE(world_map::parse_term("q0").known);
    const world_map::Term t = world_map::parse_term("q1.7<2");
    CHECK(t.known);
    CHECK(t.kind == 1);
    CHECK(t.number == 7);
    CHECK(t.op == "<");
    CHECK(t.value == 2);
    CHECK_FALSE(t.require);
    const world_map::Term r = world_map::parse_term("r1.37==0");
    CHECK(r.known);
    CHECK(r.require);
    CHECK(r.number == 37);
}

// A synthetic grid like 4.3's: a floor, a rope hanging 24 px above it, and a ledge whose end is
// 16 px from the rope, 60 px above the floor. The ledge is reached by a rope jump: up the rope,
// then hop off sideways.
TEST_CASE("a ledge beside a rope is reached by a rope jump") {
    namespace wm = world_map;
    wm::Map m;
    m.w = 30;
    m.h = 20;
    m.att.assign(static_cast<std::size_t>(m.w * m.h), wm::k_air);
    auto set = [&m](int c, int r, wm::u8 v) { m.att[static_cast<std::size_t>(r * m.w + c)] = v; };
    for (int c = 0; c < m.w; c++) {
        set(c, 18, wm::k_foothold); // the floor: y 143
    }
    for (int c = 2; c <= 10; c++) {
        set(c, 10, wm::k_foothold); // the ledge: y 79, x 16..87
    }
    for (int r = 6; r <= 14; r++) { // the rope: x 104, y 48..119
        set(12, r, wm::k_ladder);
        set(13, r, wm::k_ladder);
    }
    wm::build(m);
    REQUIRE(m.platforms.size() == 2);
    REQUIRE(m.ladders.size() == 1);
    const int ledge = m.platforms[0].row == 10 ? 0 : 1, floor = 1 - ledge;
    bool hop = false;
    for (const wm::Edge &e : m.edges[static_cast<std::size_t>(floor)]) {
        hop = hop || (e.move == wm::Move::rope_jump && e.to == ledge && e.dir < 0 && e.x == 104);
    }
    CHECK(hop);
    const std::vector<wm::Step> path = wm::route(m, floor, 200, ledge);
    REQUIRE(path.size() == 1);
    CHECK(path[0].edge.move == wm::Move::rope_jump);
}

// Steps 32 px wide and 32 px apart, like 5.3's: a running jump from a step's end carries the hero
// 42 px, past the next step's far end, so it leaves from further back and comes down on it.
TEST_CASE("a jump onto a narrow step leaves from far enough back to land on it") {
    namespace wm = world_map;
    wm::Map m;
    m.w = 30;
    m.h = 20;
    m.att.assign(static_cast<std::size_t>(m.w * m.h), wm::k_air);
    auto set = [&m](int c, int r, wm::u8 v) { m.att[static_cast<std::size_t>(r * m.w + c)] = v; };
    for (int c = 0; c < m.w; c++) {
        set(c, 18, wm::k_foothold); // the floor: y 143
    }
    for (int c = 4; c <= 7; c++) {
        set(c, 14, wm::k_foothold); // the low step: y 111, x 32..63
    }
    for (int c = 8; c <= 11; c++) {
        set(c, 10, wm::k_foothold); // the high step: y 79, x 64..95
    }
    wm::build(m);
    REQUIRE(m.platforms.size() == 3);
    int low = -1, high = -1;
    for (std::size_t i = 0; i < m.platforms.size(); i++) {
        const int row = m.platforms[i].row;
        if (row == 14) {
            low = static_cast<int>(i);
        } else if (row == 10) {
            high = static_cast<int>(i);
        }
    }
    const wm::Platform &h = m.platforms[static_cast<std::size_t>(high)];
    bool jump = false;
    for (const wm::Edge &e : m.edges[static_cast<std::size_t>(low)]) {
        if (e.move == wm::Move::jump && e.to == high) {
            jump = true;
            CHECK(e.dir == 1);
            CHECK(e.x + wm::jump_travel(32) >= h.x0() + 4);
            CHECK(e.x + wm::jump_travel(32) <= h.x1() - 4);
        }
    }
    CHECK(jump);
    CHECK(wm::jump_travel(32) == 42);
    CHECK(wm::jump_travel(0) == 49);
}

// A rope hanging from a high platform, 64 px above the floor (too high to grab from it) and
// 11 px beside a ledge 48 px below the top (too high to jump), like 5.1's rope to portal510: the
// ledge reaches the top by a rope grab (a jump toward the rope, up in the air), and the top comes
// back down by a rope jump.
TEST_CASE("a rope beside a ledge is caught in a jump from it") {
    namespace wm = world_map;
    wm::Map m;
    m.w = 30;
    m.h = 20;
    m.att.assign(static_cast<std::size_t>(m.w * m.h), wm::k_air);
    auto set = [&m](int c, int r, wm::u8 v) { m.att[static_cast<std::size_t>(r * m.w + c)] = v; };
    for (int c = 0; c < m.w; c++) {
        set(c, 18, wm::k_foothold); // the floor: y 143
    }
    for (int c = 12; c <= 20; c++) {
        set(c, 3, wm::k_foothold); // the top: y 23, x 96..167
    }
    for (int c = 2; c <= 10; c++) {
        set(c, 9, wm::k_foothold); // the ledge: y 71, x 16..87
    }
    for (int r = 3; r <= 9; r++) { // the rope: x 104, y 24..79, from the top's surface
        const auto v = static_cast<wm::u8>(r == 3 ? wm::k_ladder | wm::k_foothold : wm::k_ladder);
        set(12, r, v);
        set(13, r, v);
    }
    wm::build(m);
    REQUIRE(m.platforms.size() == 3);
    REQUIRE(m.ladders.size() == 1);
    int top = -1, ledge = -1, floor = -1;
    for (std::size_t i = 0; i < m.platforms.size(); i++) {
        const int row = m.platforms[i].row;
        (row == 3 ? top : row == 9 ? ledge : floor) = static_cast<int>(i);
    }
    bool down = false;
    for (const wm::Edge &e : m.edges[static_cast<std::size_t>(top)]) {
        down = down || (e.move == wm::Move::rope_jump && e.to == ledge);
    }
    CHECK(down);
    const std::vector<wm::Step> up = wm::route(m, ledge, 40, top);
    REQUIRE(up.size() == 1);
    CHECK(up[0].edge.move == wm::Move::rope_grab);
    CHECK(up[0].edge.dir > 0);
    CHECK(up[0].edge.x == 84); // 3 px in from the ledge's near end
    CHECK(wm::route(m, floor, 40, top).empty());
}
