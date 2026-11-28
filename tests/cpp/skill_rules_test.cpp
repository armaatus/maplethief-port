// The skill rules of skill_rules.h (#302) on synthetic skill.dat rows and the worked examples of
// docs/re/mechanics.md: the row parser (offsets, a bad stride refused), MP / @etc / @dmg /
// duration by level, one hit's x and range per skill, the stars a use takes and the hits it lands
// on one mob. The real game's side is play_real_test's skills_* modes.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "skill_rules.h"

namespace {

using namespace skill_rules;

void put16(std::vector<std::uint8_t> &b, std::size_t at, i32 v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

// A table of `rows` made-up rows: row i has id i and its fields from i.
std::vector<std::uint8_t> table(int rows, int stride = k_skill_stride) {
    std::vector<std::uint8_t> b(6 + static_cast<std::size_t>(rows) * stride, 0);
    put16(b, 2, rows);
    put16(b, 4, stride);
    for (int i = 0; i < rows; i++) {
        const std::size_t o = 6 + static_cast<std::size_t>(i) * stride;
        b[o] = static_cast<std::uint8_t>(i);
        b[o + 0x13] = 1;
        b[o + 0x14] = 3;
        b[o + 0x15] = 2;
        b[o + 0x16] = 10;
        put16(b, o + 0x19, 10 + i);
        put16(b, o + 0x1B, 6);
        put16(b, o + 0x1D, 500);
        put16(b, o + 0x1F, 50);
        put16(b, o + 0x21, 900);
        put16(b, o + 0x23, -10);
        put16(b, o + 0x25, 220);
        put16(b, o + 0x27, 25);
    }
    return b;
}

SkillRow row(i32 id, i32 dmg, i32 dmg_per) {
    SkillRow s;
    s.id = id;
    s.damage_base = dmg;
    s.damage_per_level = dmg_per;
    return s;
}

} // namespace

TEST_CASE("skill.dat rows are read at the documented offsets") {
    const std::map<i32, SkillRow> t = parse_skills(table(3));
    REQUIRE(t.size() == 3);
    const SkillRow &s = t.at(2);
    CHECK(s.branch == 1);
    CHECK(s.tier == 3);
    CHECK(s.type == 2);
    CHECK(s.max_level == 10);
    CHECK(s.damage_base == 12);
    CHECK(s.damage_per_level == 6);
    CHECK(s.time_base == 500);
    CHECK(s.time_per_level == 50);
    CHECK(s.etc_base == 900);
    CHECK(s.etc_per_level == -10);
    CHECK(s.mp_base == 220);
    CHECK(s.mp_per_level == 25);
}

TEST_CASE("a table with another stride or a short body is refused") {
    CHECK(parse_skills(table(3, 296)).empty());
    std::vector<std::uint8_t> b = table(3);
    b.pop_back();
    CHECK(parse_skills(b).empty());
    CHECK(parse_skills({0, 0, 0}).empty());
}

TEST_CASE("the values grow by level from level 1, @etc in tenths") {
    // The note's example: mp_base 6, mp_per_level 2 costs 14 MP at level 5; etc 50 and 10 give 9.
    SkillRow s;
    s.mp_base = 6;
    s.mp_per_level = 2;
    s.etc_base = 50;
    s.etc_per_level = 10;
    CHECK(mp_cost(s, 1) == 6);
    CHECK(mp_cost(s, 5) == 14);
    CHECK(etc_value(s, 5) == 9);
    // Meso Guard's falling @etc: 900 - 10 per level stays positive, in tenths.
    SkillRow g;
    g.etc_base = 900;
    g.etc_per_level = -10;
    CHECK(etc_value(g, 2) == 89);
    // A negative value is not divided.
    g.etc_base = -5;
    g.etc_per_level = 0;
    CHECK(etc_value(g, 1) == -5);
    SkillRow c;
    c.damage_base = 10;
    c.damage_per_level = 6;
    c.time_base = 500;
    c.time_per_level = 50;
    CHECK(window_damage(c, 2) == 16);
    CHECK(duration(c, 2) == 550);
}

TEST_CASE("one hit's x and range follow the note's examples") {
    // A = 40, the dagger swing: x = 40 x 4 / 5 = 32, 24 to 41.
    const SkillRow swing = row(0, 0, 0);
    CHECK(hit_x(swing, 0, 40, 0) == 32);
    CHECK(hit_range(32).lo == 24);
    CHECK(hit_range(32).hi == 41);
    // The basic throw with a 4-ATT star: A = 44, x = 35 - 3 = 32. The star counts only for star
    // skills.
    const SkillRow throw_ = row(1, 0, 0);
    CHECK(hit_x(throw_, 0, 40, 4) == 32);
    CHECK(hit_x(swing, 0, 40, 4) == 32);
    // A level 5 Double Step at A = 40: p = 60 + 5 x 5 = 85, x = 32 + 27 - 3 = 56, 41 to 73.
    const SkillRow ds = row(3, 60, 5);
    CHECK(hit_x(ds, 5, 40, 0) == 56);
    CHECK(hit_range(56).lo == 41);
    CHECK(hit_range(56).hi == 73);
}

TEST_CASE("each attack skill adds its percentage and its constant") {
    // A = 30: x = 30 x 4 / 5 = 24 before the skill.
    CHECK(hit_x(row(2, 30, 11), 2, 30, 0) == 24 + 24 * 52 / 100 - 5);
    CHECK(hit_x(row(7, 20, 20), 2, 30, 0) == 24 + 24 * 60 / 100 + 2);
    CHECK(hit_x(row(10, 65, 31), 2, 30, 0) == 24 + 24 * 127 / 100 + 4);
    CHECK(hit_x(row(16, 5, 6), 2, 30, 0) == 24 + 24 * 17 / 100 + 2);
    CHECK(hit_x(row(23, 40, 12), 2, 30, 0) == 24 + 24 * 64 / 100 + 3);
    // Other skills leave x as it is; a negative x becomes 10, zero stays zero.
    CHECK(hit_x(row(12, 10, 6), 2, 30, 0) == 24);
    CHECK(hit_x(row(2, 0, 0), 0, 1, 0) == 10);
    CHECK(hit_x(row(0, 0, 0), 0, 0, 0) == 0);
}

TEST_CASE("stars per use and hits on one mob") {
    CHECK(stars_used(1, false) == 1);
    CHECK(stars_used(1, true) == 2);
    CHECK(stars_used(2, false) == 2);
    CHECK(stars_used(2, true) == 4);
    CHECK(stars_used(7, true) == 1);
    CHECK(stars_used(10, true) == 3);
    CHECK(stars_used(3, false) == 0);
    SkillRow savage = row(16, 5, 6);
    savage.etc_base = 35;
    savage.etc_per_level = 2;
    CHECK(hits_on_one(savage, 2, false) == 3);
    CHECK(hits_on_one(row(3, 0, 0), 1, false) == 2);
    CHECK(hits_on_one(row(2, 0, 0), 1, true) == 4);
    CHECK(hits_on_one(row(10, 0, 0), 1, true) == 1);
}

TEST_CASE("a turn's loss fits k hits, normal or critical") {
    // x = 32: one hit 24-41, critical 48-82.
    CHECK(loss_fits(30, 32, 1, false));
    CHECK(loss_fits(60, 32, 1, false));
    CHECK_FALSE(loss_fits(23, 32, 1, false));
    CHECK_FALSE(loss_fits(83, 32, 1, false));
    CHECK(loss_fits(150, 32, 2, false));
    CHECK_FALSE(loss_fits(165, 32, 2, false));
    // A killing blow can be anything up to the most.
    CHECK(loss_fits(5, 32, 1, true));
    CHECK_FALSE(loss_fits(83, 32, 1, true));
}
