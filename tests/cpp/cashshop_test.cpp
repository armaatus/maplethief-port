// The offline cash shop's pricing (#156, ADR 0072, maplethief/cashshop.h) as a table: the level
// curve, the escalation and its cap, the cool-down, the floors; the counters file; the products;
// and the port's price line (platform/src/price_line.h). Pure functions only: the running cash
// shop on a synthetic guest is stubs_test.cpp's.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/cashshop.h"
#include "price_line.h"
#include "settings.h"

namespace {

using i64 = std::int64_t;
namespace fe = maplethief::frontend;

const mt_cash_knobs *k = mt_cash_default_knobs();

// hero.dat's EXP to the next level (docs/re/cash-shop.md).
struct Level {
    int level;
    std::int32_t exp;
};
constexpr Level k_levels[] = {{1, 10},     {5, 40},     {10, 200},   {20, 1980},
                              {30, 5350},  {40, 10300}, {50, 16830}, {58, 23200},
                              {59, 24060}, {60, 24950}, {70, 34650}, {99, 71740}};

} // namespace

TEST_CASE("cash shop: the products, their ids and titles") {
    CHECK(std::string(mt_cash_product_suffix(MT_CASH_POW)) == "powpackage");
    CHECK(mt_cash_product_suffix(MT_CASH_PRODUCT_COUNT) == nullptr);
    CHECK(mt_cash_product_from_id("com.nexonmobile.maplestory.thief.ComebackPack") ==
          MT_CASH_COMEBACK);
    CHECK(mt_cash_product_from_id("exppack2") == MT_CASH_EXP80);
    CHECK(mt_cash_product_from_id("com.nexonmobile.maplestory.thief.nothing") == -1);
    CHECK(mt_cash_product_from_id(nullptr) == -1);
    CHECK(mt_cash_product_is_revive(MT_CASH_POW) == 1);
    CHECK(mt_cash_product_is_revive(MT_CASH_EXP40) == 0);
    for (std::uint32_t p = 0; p < MT_CASH_PRODUCT_COUNT; p++) {
        CHECK(mt_cash_product_from_id(mt_cash_product_suffix(p)) == static_cast<int>(p));
        CHECK(mt_cash_product_title(p) != nullptr);
    }
}

TEST_CASE("cash shop pricing: a level's income and the anchors at level 59") {
    CHECK(mt_cash_level_income(k, 23200) == 38048); // 1.64 meso per EXP
    CHECK(mt_cash_level_income(k, -5) == 0);
    // ADR 0072's anchors: a POW! Package (30 Power Elixirs, 39,000 at the NPC shops) and the
    // Comeback Pack (79,000) at level 59 (24,060 EXP), whose income is nearest 39,000.
    CHECK(mt_cash_price(k, MT_CASH_POW, 24060, 0, 0) == 39460);
    CHECK(mt_cash_price(k, MT_CASH_COMEBACK, 24060, 0, 0) == 78920);
    CHECK(mt_cash_price(k, MT_CASH_EXP40, 24060, 0, 0) == 39460);
    CHECK(mt_cash_price(k, MT_CASH_EXP80, 24060, 0, 0) == 78920);
    CHECK(mt_cash_price(k, MT_CASH_SP_MARBLE, 24060, 0, 0) == 49330);
    CHECK(mt_cash_price(k, MT_CASH_INITIALIZE_SP, 24060, 0, 0) == 10000); // its resale: 9,865
    // The meso packs are never sold.
    for (std::uint32_t p = MT_CASH_MESOPACK3; p <= MT_CASH_MESOPACK6; p++) {
        CHECK(mt_cash_price(k, p, 23200, 0, 0) == -1);
    }
    CHECK(mt_cash_price(k, MT_CASH_PRODUCT_COUNT, 23200, 0, 0) == -1);
}

TEST_CASE("cash shop pricing: the level curve") {
    // Every product's price grows with the level (never falls), and is a fixed share of a
    // level's income once above its floors.
    for (std::uint32_t p : {MT_CASH_INITIALIZE_SP, MT_CASH_SP_MARBLE, MT_CASH_EXP40, MT_CASH_EXP80,
                            MT_CASH_POW, MT_CASH_COMEBACK}) {
        i64 last = 0;
        for (const Level &l : k_levels) {
            const i64 price = mt_cash_price(k, p, l.exp, 0, 0);
            INFO("product " << p << " level " << l.level);
            CHECK(price >= last);
            CHECK(price % 10 == 0);
            CHECK(price >= 100);
            last = price;
        }
    }
    // The table (default knobs, no steps, no meso).
    struct Row {
        int level;
        i64 exp40, pow, marble;
    };
    const Row rows[] = {
        {1, 100, 9000, 5000},      // the minimum, the resale floors
        {10, 330, 9000, 5000},     // 328
        {30, 8780, 9000, 10970},   // 8,774
        {40, 16900, 16900, 21120}, // 16,892
        {70, 56830, 56830, 71040}, // 56,826
        {99, 117660, 117660, 147070},
    };
    for (const Row &r : rows) {
        std::int32_t exp = 0;
        for (const Level &l : k_levels) {
            exp = l.level == r.level ? l.exp : exp;
        }
        INFO("level " << r.level);
        CHECK(mt_cash_price(k, MT_CASH_EXP40, exp, 0, 0) == r.exp40);
        CHECK(mt_cash_price(k, MT_CASH_POW, exp, 0, 0) == r.pow);
        CHECK(mt_cash_price(k, MT_CASH_SP_MARBLE, exp, 0, 0) == r.marble);
    }
}

TEST_CASE("cash shop pricing: escalation, its cap and the floors") {
    // x1.5 a step: 38,048 -> 57,072 -> 85,608 -> 128,412 -> 192,618, then capped at 4 steps.
    const i64 expected[] = {38050, 57080, 85610, 128420, 192620, 192620, 192620};
    for (std::uint32_t s = 0; s < 7; s++) {
        INFO("steps " << s);
        CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 0, s) == expected[s]);
    }
    CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 0, 1000000) == 192620);
    // The wealth floor: never below 10 % of the hero's meso.
    CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 1000000, 0) == 100000);
    CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 380480, 0) == 38050);
    CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 380490, 0) == 38050); // 38,049
    CHECK(mt_cash_price(k, MT_CASH_EXP40, 200, 50000, 0) == 5000);
    CHECK(mt_cash_price(k, MT_CASH_EXP40, 200, -50, 0) == 330); // no negative meso
    // The resale floor: a purchase never sells back at a profit.
    CHECK(mt_cash_price(k, MT_CASH_INITIALIZE_SP, 10, 0, 0) == 10000);
    // Other knobs: no floors, a gentler step, a lower cap.
    mt_cash_knobs soft = *k;
    for (std::uint32_t &r : soft.resale) {
        r = 0;
    }
    soft.min_price = 0;
    soft.step_pct = 120;
    soft.max_steps = 2;
    CHECK(mt_cash_price(&soft, MT_CASH_POW, 10, 0, 0) == 20);         // 16.4
    CHECK(mt_cash_price(&soft, MT_CASH_POW, 200, 0, 5) == 480);       // 328 x 1.44 = 472.32
    CHECK(mt_cash_price(nullptr, MT_CASH_POW, 23200, 0, 0) == 38050); // the defaults
}

TEST_CASE("cash shop counters: steps, deaths, purchases and the cool-down") {
    mt_cash_counters c;
    mt_cash_counters_reset(&c, 12);
    CHECK(c.level == 12);
    CHECK(mt_cash_steps(&c, MT_CASH_POW) == 0u);
    mt_cash_counters_death(&c); // the first fall at a level: the base price
    CHECK(mt_cash_steps(&c, MT_CASH_POW) == 0u);
    mt_cash_counters_bought(&c, MT_CASH_POW);
    CHECK(c.paid_revives == 1u);
    CHECK(mt_cash_steps(&c, MT_CASH_POW) == 1u);
    mt_cash_counters_death(&c); // and the next: one for the death, one for the paid revive
    CHECK(mt_cash_steps(&c, MT_CASH_POW) == 2u);
    CHECK(mt_cash_steps(&c, MT_CASH_COMEBACK) == 2u); // the revives share their steps
    CHECK(mt_cash_price(k, MT_CASH_POW, 23200, 0, mt_cash_steps(&c, MT_CASH_POW)) >
          mt_cash_price(k, MT_CASH_POW, 23200, 0, 0));
    // Other products count their own purchases at this level.
    mt_cash_counters_bought(&c, MT_CASH_EXP40);
    mt_cash_counters_bought(&c, MT_CASH_EXP40);
    CHECK(mt_cash_steps(&c, MT_CASH_EXP40) == 2u);
    CHECK(mt_cash_steps(&c, MT_CASH_EXP80) == 0u);
    CHECK(c.paid_revives == 1u);
    mt_cash_counters_bought(&c, MT_CASH_PRODUCT_COUNT); // ignored
    CHECK(mt_cash_steps(&c, MT_CASH_PRODUCT_COUNT) == 0u);
    // A level-up: everything cools down.
    mt_cash_counters_reset(&c, 13);
    CHECK((c.level == 13 && c.deaths == 0u && c.paid_revives == 0u));
    CHECK(mt_cash_steps(&c, MT_CASH_EXP40) == 0u);
}

TEST_CASE("cash shop counters: the file's text, read back, and bad files") {
    mt_cash_counters c;
    mt_cash_counters_reset(&c, 31);
    c.deaths = 3;
    c.paid_revives = 2;
    c.bought[MT_CASH_POW] = 2;
    c.bought[MT_CASH_EXP80] = 1;
    char text[1024];
    const std::size_t n = mt_cash_counters_format(&c, text, sizeof text);
    CHECK(n == std::strlen(text));
    const std::string s(text, n);
    CHECK(s.rfind("# MapleThief", 0) == 0);
    CHECK(s.find("\nlevel = 31\n") != std::string::npos);
    CHECK(s.find("\ndeaths = 3\n") != std::string::npos);
    CHECK(s.find("\npowpackage = 2\n") != std::string::npos);
    CHECK(s.find("mesopack") == std::string::npos); // never sold, never counted
    mt_cash_counters back;
    REQUIRE(mt_cash_counters_parse(text, n, &back) == 0);
    CHECK(std::memcmp(&back, &c, sizeof c) == 0);
    // Truncated output stays NUL-terminated.
    char tiny[16];
    CHECK(mt_cash_counters_format(&c, tiny, sizeof tiny) == 15u);
    CHECK(std::strlen(tiny) == 15u);
    // Hand edits: blanks, CRLF, unknown keys, bad values and comments are skipped.
    const std::string edited = "  # comment\r\nlevel=7\r\n deaths =  2 \nbogus = 4\n"
                               "paid_revives = -1\nexppack1 = x\nComebackPack = 5";
    REQUIRE(mt_cash_counters_parse(edited.data(), edited.size(), &back) == 0);
    CHECK((back.level == 7 && back.deaths == 2u && back.paid_revives == 0u));
    CHECK(back.bought[MT_CASH_EXP40] == 0u);
    CHECK(back.bought[MT_CASH_COMEBACK] == 5u);
    // Nothing recognised: -1, and the counters are reset.
    const std::string junk = "\x01\x02 garbage\n= 3\n";
    CHECK(mt_cash_counters_parse(junk.data(), junk.size(), &back) == -1);
    CHECK((back.level == 0 && back.deaths == 0u));
    CHECK(mt_cash_counters_parse(nullptr, 0, &back) == -1);
    CHECK(mt_cash_counters_parse("level = 99999999999", 19, &back) == -1); // overflow
}

TEST_CASE("cash shop price line: meso text and the lines of each screen") {
    CHECK(fe::meso_text(0) == "0");
    CHECK(fe::meso_text(999) == "999");
    CHECK(fe::meso_text(1000) == "1,000");
    CHECK(fe::meso_text(192620) == "192,620");
    CHECK(fe::meso_text(1234567) == "1,234,567");
    CHECK(fe::meso_text(-38050) == "-38,050");

    mt_cash_view v{};
    CHECK(fe::price_lines(v).empty()); // nothing open (or the shop off)
    v.screen = MT_CASH_SCREEN_DEATH_POPUP;
    v.meso = 12345;
    v.count = 2;
    v.quotes[0] = {MT_CASH_POW, 38050, 0};
    v.quotes[1] = {MT_CASH_COMEBACK, 76100, 0};
    std::vector<std::string> lines = fe::price_lines(v);
    REQUIRE(lines.size() == 2u);
    CHECK(lines[0] == "Revive prices in meso (you have 12,345)");
    CHECK(lines[1] == "POW! Package 38,050   Comeback Pack 76,100");

    v.screen = MT_CASH_SCREEN_SHOP;
    v.count = 5;
    v.quotes[0] = {MT_CASH_INITIALIZE_SP, 10000, 0};
    v.quotes[1] = {MT_CASH_SP_MARBLE, 47560, 0};
    v.quotes[2] = {MT_CASH_EXP40, 57080, 1};
    v.quotes[3] = {MT_CASH_EXP80, 76100, 0};
    v.quotes[4] = {MT_CASH_MESOPACK3, -1, 0};
    lines = fe::price_lines(v);
    REQUIRE(lines.size() == 4u);
    CHECK(lines[0] == "Cash shop prices in meso (you have 12,345)");
    CHECK(lines[1] == "Initialize SP 10,000   SP Marble SP+1 47,560");
    CHECK(lines[2] == "EXP Explosion 40% 57,080   EXP Explosion 80% 76,100");
    CHECK(lines[3] == "meso packs: not sold");
    for (const std::string &l : lines) {
        CHECK(l.size() <= 56u); // fits a 480-pixel window at 8 pixels a glyph
    }
}

TEST_CASE("settings: cash_shop is off or meso, an Assist off in every preset (#156)") {
    fe::Settings s;
    CHECK(s.cash_shop == fe::CashShop::kOff); // the default
    const fe::SettingInfo *row = fe::find_setting("cash_shop");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kAssists);
    CHECK(fe::find_setting_flag("--cash-shop") == row);
    CHECK(row->game_only);
    CHECK(row->value.parse(row->value, "meso", &s));
    CHECK(s.cash_shop == fe::CashShop::kMeso);
    CHECK(row->value.print(row->value, s) == "meso");
    CHECK_FALSE(row->value.parse(row->value, "dollars", &s));
    CHECK(s.cash_shop == fe::CashShop::kMeso);
    CHECK(row->value.parse(row->value, "OFF", &s));
    CHECK(s.cash_shop == fe::CashShop::kOff);
    for (fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        CHECK(std::string(row->presets[p]) == "off");
    }
}
