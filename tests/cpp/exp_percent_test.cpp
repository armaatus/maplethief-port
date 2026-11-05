// The EXP percentage beside the HUD's bar (#191, exp_percent.h, ADR 0109) with synthetic data
// only: a fake guest laid out as the game keeps the hero and hero.dat (CStateGame +0x30, CHero
// +0x80 / +0x90, the table at *0x11A9A8) read back, hostile values refused and nothing written,
// the figure's text (rounded down, clamped, no overflow), the box's place at 1x to 8x and in fit
// mode (right of the plate's left end, left of the level badge, above the plate), the pixels, the
// layer on a software renderer (drawn anew only when the text changes, nothing for an empty
// text) and the setting's row. The real game's side (the figure equal to the hero's fields and
// hero.dat on the first maps) is play_real_test's hud_extras mode.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "autosave.h"
#include "exp_percent.h"
#include "minimap.h"
#include "scaling.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

// Guest memory as a few byte regions; a span must lie inside one of them.
struct FakeGuest {
    struct Region {
        u32 base;
        std::vector<u8> bytes;
    };
    std::vector<Region> regions;
    u8 *at(u32 addr, u32 n) {
        for (Region &r : regions) {
            if (addr >= r.base &&
                std::uint64_t{addr} + n <= std::uint64_t{r.base} + r.bytes.size()) {
                return r.bytes.data() + (addr - r.base);
            }
        }
        return nullptr;
    }
    void add(u32 base, u32 size) { regions.push_back({base, std::vector<u8>(size)}); }
    void put32(u32 addr, u32 v) {
        u8 *p = at(addr, 4);
        REQUIRE(p != nullptr);
        std::memcpy(p, &v, 4);
    }
    void put16(u32 addr, std::uint16_t v) {
        u8 *p = at(addr, 2);
        REQUIRE(p != nullptr);
        std::memcpy(p, &v, 2);
    }
    fe::GuestSpan span() {
        return [this](u32 addr, u32 n) -> const u8 * { return n == 0 ? nullptr : at(addr, n); };
    }
};

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kHero = 0x01002000,
              kTable = 0x01003000;
constexpr int kRows = 100, kStride = 18; // hero.dat's shape

// The EXP level n needs in the synthetic table: 15 * n * n (level 1: 15).
std::int32_t need_of(int level) { return 15 * level * level; }

// The field with a level-`level` hero holding `exp`, and a hero.dat of 100 rows (keys 1-100,
// the EXP column at +1, unaligned as in the shipped file).
FakeGuest field(int level, std::int32_t exp) {
    FakeGuest g;
    g.add(fe::kAutosaveCanvas, 4);
    g.add(fe::kAutosaveGameMode, 4);
    g.add(fe::kExpHeroTable, 4);
    g.add(kCanvas, 0x100);
    g.add(kState, 0x100);
    g.add(kHero, 0x200);
    g.add(kTable, 6 + kRows * kStride);
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(kState + fe::kMinimapStateHero, kHero);
    g.put32(kHero + fe::kExpHeroLevel, static_cast<u32>(level));
    g.put32(kHero + fe::kExpHeroExp, static_cast<u32>(exp));
    g.put32(fe::kExpHeroTable, kTable);
    g.put16(kTable + 0, 13);
    g.put16(kTable + 2, kRows);
    g.put16(kTable + 4, kStride);
    for (int r = 0; r < kRows; r++) {
        const u32 row = kTable + 6 + static_cast<u32>(r * kStride);
        *g.at(row, 1) = static_cast<u8>(r + 1);
        const std::int32_t need = need_of(r + 1);
        std::memcpy(g.at(row + 1, 4), &need, 4);
        *g.at(row + 9, 1) = 1; // other columns: not read
    }
    return g;
}

bool whole(float v) { return v == std::floor(v); }

} // namespace

TEST_CASE("exp percent: the hero's level, EXP and the level's need read back") {
    FakeGuest g = field(7, 300);
    const std::vector<FakeGuest::Region> before = g.regions;
    const fe::ExpView v = fe::read_exp(g.span());
    CHECK(v.shown);
    CHECK(v.level == 7);
    CHECK(v.exp == 300);
    CHECK(v.needed == need_of(7));
    // Read only: not a byte of the guest changed.
    REQUIRE(g.regions.size() == before.size());
    for (std::size_t i = 0; i < before.size(); i++) {
        CHECK(g.regions[i].bytes == before[i].bytes);
    }
    // The figure follows the guest from frame to frame: more EXP, then a level-up.
    g.put32(kHero + fe::kExpHeroExp, 600);
    CHECK(fe::read_exp(g.span()).exp == 600);
    g.put32(kHero + fe::kExpHeroLevel, 8);
    g.put32(kHero + fe::kExpHeroExp, 5);
    const fe::ExpView up = fe::read_exp(g.span());
    CHECK(up.level == 8);
    CHECK(up.needed == need_of(8));
    // The first and last rows.
    CHECK(fe::read_exp(field(1, 0).span()).needed == 15);
    CHECK(fe::read_exp(field(100, 0).span()).needed == need_of(100));
}

TEST_CASE("exp percent: outside the field or with hostile values, no figure") {
    SUBCASE("another game mode (the popup menu, the map screen)") {
        FakeGuest g = field(5, 10);
        g.put32(fe::kAutosaveGameMode, 1);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
        g.put32(fe::kAutosaveGameMode, 3);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
    }
    SUBCASE("not a CStateGame") {
        FakeGuest g = field(5, 10);
        g.put32(kState, 0x119B00);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
    }
    SUBCASE("no state, no hero") {
        FakeGuest g = field(5, 10);
        g.put32(kState + fe::kMinimapStateHero, 0);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
        g.put32(kCanvas + 0x0C, 0);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
    }
    SUBCASE("the hero outside guest memory") {
        FakeGuest g = field(5, 10);
        g.put32(kState + fe::kMinimapStateHero, 0x02000000);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
    }
    SUBCASE("a level the table has no row for, or out of the key's range") {
        for (const std::int32_t level : {0, -1, 101, 255, 256, 1 << 20}) {
            CAPTURE(level);
            FakeGuest g = field(5, 10);
            g.put32(kHero + fe::kExpHeroLevel, static_cast<u32>(level));
            CHECK_FALSE(fe::read_exp(g.span()).shown);
        }
    }
    SUBCASE("no table, a table outside guest memory or larger than its buffer") {
        FakeGuest g = field(5, 10);
        g.put32(fe::kExpHeroTable, 0);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
        g.put32(fe::kExpHeroTable, 0x03000000);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
        FakeGuest h = field(5, 10);
        h.put16(kTable + 2, kRows + 1); // one row past the buffer
        CHECK_FALSE(fe::read_exp(h.span()).shown);
    }
    SUBCASE("a header out of the caps: no rows, a stride too small or too large") {
        for (const auto [rows, stride] :
             {std::pair{0, kStride}, std::pair{kRows, 4}, std::pair{kRows, 257},
              std::pair{2000, kStride}}) {
            CAPTURE(rows);
            CAPTURE(stride);
            FakeGuest g = field(5, 10);
            g.put16(kTable + 2, static_cast<std::uint16_t>(rows));
            g.put16(kTable + 4, static_cast<std::uint16_t>(stride));
            CHECK_FALSE(fe::read_exp(g.span()).shown);
        }
    }
    SUBCASE("a level whose need is 0 or negative") {
        FakeGuest g = field(5, 10);
        const std::int32_t zero = 0, negative = -40;
        std::memcpy(g.at(kTable + 6 + 4 * kStride + 1, 4), &zero, 4);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
        std::memcpy(g.at(kTable + 6 + 4 * kStride + 1, 4), &negative, 4);
        CHECK_FALSE(fe::read_exp(g.span()).shown);
    }
}

TEST_CASE("exp percent: the text, in hundredths rounded down and clamped") {
    CHECK(fe::exp_percent_text(0, 15) == "EXP 0.00%");
    CHECK(fe::exp_percent_text(4217, 10000) == "EXP 42.17%");
    CHECK(fe::exp_percent_text(1, 3) == "EXP 33.33%");
    CHECK(fe::exp_percent_text(2, 3) == "EXP 66.66%");
    CHECK(fe::exp_percent_text(9, 10) == "EXP 90.00%");
    CHECK(fe::exp_percent_text(9999, 10000) == "EXP 99.99%");
    CHECK(fe::exp_percent_text(99999, 100000) == "EXP 99.99%");
    // The game never holds a full level (setExp levels up), but a figure never reads 100.
    CHECK(fe::exp_percent_text(15, 15) == "EXP 99.99%");
    CHECK(fe::exp_percent_text(40, 15) == "EXP 99.99%");
    CHECK(fe::exp_percent_text(-5, 15) == "EXP 0.00%");
    // Large figures (late levels) without overflow.
    CHECK(fe::exp_percent_text(2000000000, 2100000000) == "EXP 95.23%");
    // No need: no text.
    CHECK(fe::exp_percent_text(5, 0).empty());
    CHECK(fe::exp_percent_text(5, -1).empty());
}

TEST_CASE("exp percent: the box above the plate, left of the level badge, at every scale") {
    const std::string widest = "EXP 99.99%";
    CHECK(fe::exp_percent_units_w(widest) == 61); // 10 glyphs of 6, less a gap, and the backing
    for (int k = 1; k <= 8; k++) {
        const fe::Rect image{37, 11, 480.0f * static_cast<float>(k), 320.0f * static_cast<float>(k)};
        CAPTURE(k);
        CHECK(fe::exp_percent_pixels(image) == k);
        const fe::Rect r = fe::exp_percent_rect(image, widest);
        // Whole pixels at an integer scale, the texture's size exactly.
        CHECK(whole(r.x));
        CHECK(whole(r.y));
        CHECK(whole(r.w));
        CHECK(whole(r.h));
        const float sp = static_cast<float>(k);
        const float left = (r.x - image.x) / sp, right = (r.x + r.w - image.x) / sp;
        const float top = (r.y - image.y) / sp, bottom = (r.y + r.h - image.y) / sp;
        CHECK(right == fe::kExpPercentRight);
        CHECK(bottom == fe::kExpPercentBottom);
        CHECK(left >= fe::kExpPercentLeft);
        // A glyph point is at most a screen point: never taller than the plate's gap above.
        CHECK(top >= fe::kExpPercentBottom - 9);
        CHECK(top < bottom);
    }
    // At 1x the glyphs are one pixel a point; at 4x three pixels.
    CHECK(fe::exp_percent_rect(fe::Rect{0, 0, 480, 320}, widest).w == 61);
    CHECK(fe::exp_percent_rect(fe::Rect{0, 0, 1920, 1280}, widest).w == 61 * 3);
    // A fit scale (a 1366 x 768 window's 1152 x 768 picture): whole pixels, still clear.
    const fe::Rect fit{107, 0, 1152, 768};
    const fe::Rect r = fe::exp_percent_rect(fit, widest);
    CHECK(whole(r.x));
    CHECK(whole(r.w));
    const float sp = fit.w / 480.0f;
    CHECK((r.x - fit.x) / sp >= fe::kExpPercentLeft);
    CHECK((r.x + r.w - fit.x) / sp <= fe::kExpPercentRight + 0.5f);
    CHECK((r.y + r.h - fit.y) / sp <= fe::kExpPercentBottom + 0.5f);
    // No text, no image: no box.
    CHECK(fe::exp_percent_rect(fe::Rect{0, 0, 480, 320}, "").w == 0);
    CHECK(fe::exp_percent_rect(fe::Rect{}, widest).w == 0);
}

TEST_CASE("exp percent: the pixels, the label in the HUD's EXP green and the figure in white") {
    const std::string t = "EXP 42.17%";
    for (const int unit : {1, 2, 3}) {
        CAPTURE(unit);
        const std::vector<u8> px = fe::render_exp_percent(t, unit);
        const int w = fe::exp_percent_units_w(t) * unit, h = fe::kExpPercentUnitsH * unit;
        REQUIRE(px.size() == static_cast<std::size_t>(w * h * 4));
        // The corners are the backing (premultiplied: 0x10 * 0xB0 / 255 ...).
        const u8 *corner = px.data();
        CHECK(corner[3] == 0xB0);
        CHECK(corner[0] < 0x10);
        long green = 0, white = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            green += px[i] == 0xB6 && px[i + 1] == 0xDA && px[i + 2] == 0x00 ? 1 : 0;
            white += px[i] == 0xF0 && px[i + 1] == 0xF2 && px[i + 2] == 0xF5 ? 1 : 0;
            CHECK(px[i + 3] >= 0xB0); // the backing everywhere: opaque text or the backing
        }
        CHECK(green > 0);
        CHECK(white > green); // seven glyphs of figure against three of label
    }
    // The same text gives the same bytes (no state between calls).
    CHECK(fe::render_exp_percent(t, 2) == fe::render_exp_percent(t, 2));
    CHECK(fe::render_exp_percent(t, 2) != fe::render_exp_percent("EXP 42.18%", 2));
}

TEST_CASE("exp percent: the layer on a software renderer") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE(SDL_CreateWindowAndRenderer("exp_percent_test", 960, 640, 0, &window, &renderer));
    {
        fe::ExpPercentLayer layer;
        const fe::Rect image{0, 0, 960, 640};
        SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
        SDL_RenderClear(renderer);
        // No text: nothing drawn.
        CHECK(layer.draw(renderer, image, ""));
        CHECK(layer.draws() == 0);
        CHECK(layer.draw(renderer, image, "EXP 42.17%"));
        CHECK(layer.draw(renderer, image, "EXP 42.17%"));
        CHECK(layer.draws() == 2);
        CHECK(layer.redraws() == 1); // the same text: the texture kept
        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE(read != nullptr);
        SDL_Surface *s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(read);
        REQUIRE(s != nullptr);
        const fe::Rect r = fe::exp_percent_rect(image, "EXP 42.17%");
        const auto at = [s](int x, int y) {
            return static_cast<const u8 *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch +
                   static_cast<std::size_t>(x) * 4;
        };
        // Inside the box: the backing darkens the magenta; outside it: untouched.
        const u8 *in = at(static_cast<int>(r.x) + 1, static_cast<int>(r.y) + 1);
        CHECK(in[0] < 255);
        const u8 *out = at(static_cast<int>(r.x) - 2, static_cast<int>(r.y) - 2);
        CHECK(out[0] == 255);
        CHECK(out[1] == 0);
        CHECK(out[2] == 255);
        SDL_DestroySurface(s);
        // A new figure: drawn anew; a new size: a new texture.
        CHECK(layer.draw(renderer, image, "EXP 42.18%"));
        CHECK(layer.redraws() == 2);
        CHECK(layer.draw(renderer, fe::Rect{0, 0, 480, 320}, "EXP 42.18%"));
        CHECK(layer.redraws() == 3);
        layer.stop();
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

TEST_CASE("exp percent: the setting's row (Video; on in Enhanced only)") {
    fe::Settings s;
    CHECK_FALSE(s.exp_percent);
    std::string error;
    REQUIRE(fe::apply_setting(&s, "exp_percent", "on", &error));
    CHECK(s.exp_percent);
    REQUIRE(fe::apply_setting(&s, "exp_percent", "off", &error));
    CHECK_FALSE(s.exp_percent);
    CHECK_FALSE(fe::apply_setting(&s, "exp_percent", "maybe", &error));
    const fe::SettingInfo *row = fe::find_setting("exp_percent");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kVideo);
    CHECK(std::string(row->presets.original) == "off");
    CHECK(std::string(row->presets.original_controller) == "off");
    CHECK(std::string(row->presets.enhanced) == "on");
    CHECK(std::string(row->flag) == "--exp-percent");
}
