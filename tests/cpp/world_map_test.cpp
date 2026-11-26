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
    CHECK(world_map::links().size() == 9);
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
    const auto read = [&s](int kind, int number) { return state(s, kind, number); };
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
