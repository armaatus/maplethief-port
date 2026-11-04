// The port's minimap (#176, minimap.h, ADR 0098) with synthetic data only: a fake guest laid out
// as the game keeps its map (CStateGame +0x2C, the CMMap's .att grid and objects) read back,
// hostile values refused, the layout in a bar or over the picture at 3:2, 16:9, a tall window
// and widescreen, the corner with the key pad at the edges (every aspect, render scale and safe
// area; the compass moved clear of it), the dots, a golden image of a synthetic grid, the layer on
// a software renderer (off draws nothing; the grid uploaded once a map), and the setting's row. The
// real game's side (the grid equal to map/23.att, the dot following the hero on 2.3) is
// play_real_test's minimap mode.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "autosave.h"
#include "golden.h"
#include "minimap.h"
#include "present.h"
#include "scaling.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace golden = maplethief::golden;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using fe::Minimap;

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
    void put16(u32 addr, std::int16_t v) {
        u8 *p = at(addr, 2);
        REQUIRE(p != nullptr);
        std::memcpy(p, &v, 2);
    }
    fe::GuestSpan span() {
        return [this](u32 addr, u32 n) -> const u8 * { return n == 0 ? nullptr : at(addr, n); };
    }
};

constexpr u32 kCanvas = 0x01000000, kState = 0x01001000, kHero = 0x01002000, kMap = 0x01003000,
              kAtt = 0x01004000, kObjArray = 0x01010000, kObjs = 0x01011000;
constexpr int kCols = 40, kRows = 24; // 20 x 12 tiles: 320 x 192 map pixels

u8 &cell(FakeGuest &g, int x, int y) { return *g.at(kAtt + static_cast<u32>(y * kCols + x), 1); }

// A small map: the ground (row 22), a ledge (row 12, columns 6-20) with a ladder from it to the
// ground (columns 10-11, its top step 0x06), a wall (column 30, its top 0x12), a rope's cap
// (0x08) at the top of columns 34-35 and the rope under it. Objects: a portal, a hidden portal,
// two NPCs (one hidden) and a mob spawn. The hero stands on the ground at x 100.
FakeGuest field_map() {
    FakeGuest g;
    g.add(fe::kAutosaveCanvas, 4);
    g.add(fe::kAutosaveGameMode, 4);
    g.add(kCanvas, 0x100);
    g.add(kState, 0x100);
    g.add(kHero, 0x100);
    g.add(kMap, 0x100);
    g.add(kAtt, kCols * kRows);
    g.add(kObjArray, 4 * 5);
    g.add(kObjs, 12 * 5);
    g.put32(fe::kAutosaveCanvas, kCanvas);
    g.put32(fe::kAutosaveGameMode, 0);
    g.put32(kCanvas + 0x0C, kState);
    g.put32(kState, fe::kAutosaveGameVptr);
    g.put32(kState + fe::kMinimapStateMap, kMap);
    g.put32(kState + fe::kMinimapStateHero, kHero);
    g.put32(kHero + 0x04, 100);
    g.put32(kHero + 0x08, 22 * 8 - 1);
    g.put32(kMap + fe::kMapTileW, 16);
    g.put32(kMap + fe::kMapTileH, 16);
    g.put32(kMap + fe::kMapCellW, 8);
    g.put32(kMap + fe::kMapCellH, 8);
    g.put32(kMap + fe::kMapTilesWide, kCols / 2);
    g.put32(kMap + fe::kMapAttW, kCols);
    g.put32(kMap + fe::kMapAttH, kRows);
    g.put32(kMap + fe::kMapAtt, kAtt);
    g.put32(kMap + fe::kMapAttSize, kCols * kRows);
    for (int y = 0; y < kRows; y++) {
        for (int x = 0; x < kCols; x++) {
            cell(g, x, y) = 0x01;
        }
    }
    for (int x = 0; x < kCols; x++) {
        cell(g, x, 22) = 0x02;
    }
    for (int x = 6; x <= 20; x++) {
        cell(g, x, 12) = 0x02;
    }
    for (int y = 13; y <= 21; y++) {
        cell(g, 10, y) = cell(g, 11, y) = 0x04;
        cell(g, 30, y) = 0x10;
    }
    cell(g, 10, 12) = cell(g, 11, 12) = 0x06;
    cell(g, 30, 13) = 0x12;
    cell(g, 34, 2) = cell(g, 35, 2) = 0x08;
    for (int y = 3; y <= 10; y++) {
        cell(g, 34, y) = cell(g, 35, y) = 0x04;
    }
    // Objects: {tile x, tile y, kind, visible}.
    const int objs[5][4] = {
        {2, 10, 0, 1}, {5, 10, 3, 1}, {8, 5, 7, 1}, {12, 5, 8, 0}, {15, 10, 60, 1}};
    g.put32(kMap + fe::kMapObjects, kObjArray);
    g.put32(kMap + fe::kMapObjectCount, 5);
    for (u32 i = 0; i < 5; i++) {
        const u32 o = kObjs + 12 * i;
        g.put32(kObjArray + 4 * i, o);
        g.put16(o, static_cast<std::int16_t>(objs[i][1] * (kCols / 2) + objs[i][0]));
        g.put16(o + 2, static_cast<std::int16_t>(objs[i][2]));
        g.put16(o + 4, -1);
        g.put16(o + 6, static_cast<std::int16_t>(objs[i][3]));
    }
    return g;
}

fe::Placement placed(int w, int h) { return fe::place(fe::ScaleMode::kInteger, w, h, 480, 320); }

} // namespace

TEST_CASE("minimap: the game's grid, hero, portals and NPCs read from a synthetic guest") {
    FakeGuest g = field_map();
    const fe::MinimapView v = fe::read_minimap(g.span());
    REQUIRE(v.shown);
    CHECK(v.cols == kCols);
    CHECK(v.rows == kRows);
    CHECK(v.map_w() == 320);
    CHECK(v.map_h() == 192);
    REQUIRE(v.grid.size() == static_cast<std::size_t>(kCols * kRows));
    CHECK(v.grid[22 * kCols + 5] == 0x02);
    CHECK(v.grid[12 * kCols + 10] == 0x06);
    CHECK(v.hero_x == 100);
    CHECK(v.hero_y == 175);
    // The portal and the visible NPC; not the hidden portal (3), the hidden NPC or the mob spawn.
    REQUIRE(v.markers.size() == 2);
    CHECK(v.markers[0] == fe::MinimapMarker{fe::MinimapMarker::kPortal, 2 * 16 + 8, 11 * 16 - 1});
    CHECK(v.markers[1] == fe::MinimapMarker{fe::MinimapMarker::kNpc, 8 * 16 + 8, 6 * 16 - 1});
}

TEST_CASE("minimap: only in the field, and nothing from hostile guest values") {
    SUBCASE("not the field: a menu's game mode") {
        FakeGuest g = field_map();
        g.put32(fe::kAutosaveGameMode, 3);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("not a CStateGame") {
        FakeGuest g = field_map();
        g.put32(kState, 0x119000);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("no map or no hero") {
        FakeGuest g = field_map();
        g.put32(kState + fe::kMinimapStateMap, 0);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
        FakeGuest h = field_map();
        h.put32(kState + fe::kMinimapStateHero, 0);
        CHECK_FALSE(fe::read_minimap(h.span()).shown);
    }
    SUBCASE("a grid side past the cap") {
        FakeGuest g = field_map();
        g.put32(kMap + fe::kMapAttW, 5000);
        g.put32(kMap + fe::kMapAttSize, 0x7FFFFFFF);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
        g.put32(kMap + fe::kMapAttW, 0xFFFFFFFF); // -1
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("a buffer smaller than the grid") {
        FakeGuest g = field_map();
        g.put32(kMap + fe::kMapAttSize, kCols * kRows - 1);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("a grid that runs past mapped memory") {
        FakeGuest g = field_map();
        g.put32(kMap + fe::kMapAttH, kRows + 1);
        g.put32(kMap + fe::kMapAttSize, kCols * (kRows + 1));
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
        g.put32(kMap + fe::kMapAttH, kRows);
        g.put32(kMap + fe::kMapAtt, 0x02000000);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("a cell size of 0") {
        FakeGuest g = field_map();
        g.put32(kMap + fe::kMapCellW, 0);
        CHECK_FALSE(fe::read_minimap(g.span()).shown);
    }
    SUBCASE("objects: a huge count, unmapped entries and a bad tile size cost only the markers") {
        FakeGuest g = field_map();
        g.put32(kMap + fe::kMapObjectCount, 0x7FFFFFFF); // read up to the cap, the rest unmapped
        g.put32(kObjArray + 4, 0x03000000);              // an object that is not memory
        fe::MinimapView v = fe::read_minimap(g.span());
        REQUIRE(v.shown);
        CHECK(v.markers.size() == 2);
        g.put32(kMap + fe::kMapTileW, 0);
        v = fe::read_minimap(g.span());
        CHECK(v.shown);
        CHECK(v.markers.empty());
    }
}

TEST_CASE("minimap: the layout in the bar, over the picture, tall and wide") {
    SUBCASE("off: nothing") {
        CHECK_FALSE(
            fe::minimap_layout(Minimap::kOff, placed(960, 640), placed(960, 640).rect, 320, 192)
                .shown);
    }
    SUBCASE("overlay at 3:2: right of the compass, at the top, a tile at most 2 points") {
        const fe::Placement p = placed(960, 640); // 2x
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kOverlay, p, p.rect, 320, 192);
        REQUIRE(l.shown);
        CHECK_FALSE(l.in_border);
        CHECK(l.scale == doctest::Approx(0.25));
        CHECK(l.box.x == 92);
        CHECK(l.box.y == 8);
        CHECK(l.box.w == 80);
        CHECK(l.box.h == 48);
    }
    SUBCASE("a wide map is fitted to the overlay's 150 x 64 points") {
        const fe::Placement p = placed(480, 320);
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kOverlay, p, p.rect, 3200, 640);
        REQUIRE(l.shown);
        CHECK(l.box.w == 150);
        CHECK(l.box.h <= 64);
    }
    SUBCASE("border at 3:2: no bar, so over the picture") {
        const fe::Placement p = placed(960, 640);
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kBorder, p, p.rect, 320, 192);
        REQUIRE(l.shown);
        CHECK_FALSE(l.in_border);
        CHECK(l.box.x == 92);
    }
    SUBCASE("border at 16:9: the left bar's top") {
        const fe::Placement p = placed(1920, 1080); // 3x: 1440 x 960 at (240, 60)
        REQUIRE(p.rect.x == 240);
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kBorder, p, p.rect, 320, 192);
        REQUIRE(l.shown);
        CHECK(l.in_border);
        CHECK(l.scale == doctest::Approx(0.375));
        CHECK(l.box.w == 120);
        CHECK(l.box.h == 72);
        CHECK(l.box.x == 60);
        CHECK(l.box.y == 72);
        CHECK(l.box.x + l.box.w <= p.rect.x);
    }
    SUBCASE("border on a tall window: the bar below") {
        const fe::Placement p = placed(1080, 1920);
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kBorder, p, p.rect, 320, 192);
        REQUIRE(l.shown);
        CHECK(l.in_border);
        CHECK(l.box.y >= p.rect.y + p.rect.h);
        CHECK(l.box.x == p.rect.x + std::round(fe::kMinimapMargin * p.rect.w / 480));
    }
    SUBCASE("border with widescreen filling the window: over the screen image, as at 3:2") {
        const fe::Placement p = placed(1920, 1080);
        const fe::Rect wide{0, p.rect.y, 1920, p.rect.h}; // the wide picture across the window
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kBorder, p, wide, 320, 192);
        const fe::MinimapLayout o = fe::minimap_layout(Minimap::kOverlay, p, wide, 320, 192);
        REQUIRE(l.shown);
        CHECK_FALSE(l.in_border);
        CHECK(l.box.x == o.box.x);
        CHECK(l.box.x == p.rect.x + std::round(fe::kMinimapOverlayX * 3));
        CHECK(l.box.y == p.rect.y + 12);
    }
    SUBCASE("an empty map or placement: nothing") {
        const fe::Placement p = placed(960, 640);
        CHECK_FALSE(fe::minimap_layout(Minimap::kOverlay, p, p.rect, 0, 192).shown);
        CHECK_FALSE(
            fe::minimap_layout(Minimap::kOverlay, fe::Placement{}, fe::Rect{}, 320, 192).shown);
    }
}

namespace {

// A window with widescreen as the Presenter places it (present.cpp): the 568-point picture
// placed (integer mode scales its points), the screen image its middle 480 points, at render
// scale n, inside the insets' safe area.
struct WideWindow {
    fe::Placement image;
    fe::Rect wide;
};
WideWindow wide_window(int w, int h, int n = 1, fe::Insets insets = {},
                       fe::ScaleMode mode = fe::ScaleMode::kInteger) {
    WideWindow out;
    out.image = fe::place(mode, w, h, 568 * n, 320 * n, insets, 1, 1, 568);
    out.wide = out.image.rect;
    out.image.rect = fe::wide_part(out.wide, 568, 480);
    return out;
}

bool overlaps(const fe::Rect &a, const fe::Rect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

} // namespace

TEST_CASE("minimap: the anchor, and the corner with the key pad at the edges") {
    using fe::MinimapAnchor;
    SUBCASE("the anchor: the corner only with widescreen and the HUD at the edges") {
        CHECK(fe::minimap_anchor(fe::Widescreen::kAuto, fe::WidescreenHud::kEdges) ==
              MinimapAnchor::kCorner);
        CHECK(fe::minimap_anchor(fe::Widescreen::kAuto, fe::WidescreenHud::kNarrow) ==
              MinimapAnchor::kCompass);
        CHECK(fe::minimap_anchor(fe::Widescreen::kOff, fe::WidescreenHud::kEdges) ==
              MinimapAnchor::kCompass);
        CHECK(fe::minimap_anchor(fe::Widescreen::kOff, fe::WidescreenHud::kNarrow) ==
              MinimapAnchor::kCompass);
    }
    SUBCASE("the visible view: the wide picture, its middle on a narrower map, else the image") {
        const WideWindow ww = wide_window(1920, 1080); // 3x: 1704 x 960 at (108, 60)
        REQUIRE(ww.wide.x == 108);
        REQUIRE(ww.image.rect.x == 240);
        const fe::Rect all = fe::minimap_visible(ww.image, ww.wide, 568, 568);
        CHECK((all.x == 108 && all.w == 1704 && all.y == 60 && all.h == 960));
        const fe::Rect mid = fe::minimap_visible(ww.image, ww.wide, 568, 520);
        CHECK((mid.x == 108 + 3 * 24 && mid.w == 3 * 520));
        const fe::Rect narrow = fe::minimap_visible(ww.image, ww.wide, 568, 480);
        CHECK((narrow.x == ww.image.rect.x && narrow.w == ww.image.rect.w));
        const fe::Placement p = placed(960, 640);
        const fe::Rect off = fe::minimap_visible(p, p.rect, 0, 480);
        CHECK((off.x == p.rect.x && off.w == p.rect.w));
    }
    SUBCASE("16:9 filled by the wide picture: the top left corner, the compass right of it") {
        const WideWindow ww = wide_window(1920, 1080);
        const fe::Rect vis = fe::minimap_visible(ww.image, ww.wide, 568, 568);
        // Map 2.3's size: 960 x 480 map pixels, 0.1 points each (48 points high).
        const fe::MinimapLayout l = fe::minimap_layout(Minimap::kOverlay, MinimapAnchor::kCorner,
                                                       ww.image, ww.wide, vis, 960, 480);
        REQUIRE(l.shown);
        CHECK(l.corner);
        CHECK_FALSE(l.in_border);
        CHECK(l.box.x == 108 + 12);
        CHECK(l.box.y == 60 + 12);
        CHECK(l.box.w == 288);
        CHECK(l.box.h == 144);
        CHECK(l.points.x == doctest::Approx(-40));
        CHECK(l.points.y == doctest::Approx(4));
        CHECK(l.points.w == doctest::Approx(96));
        CHECK(l.points.h == doctest::Approx(48));
        CHECK(l.compass_dx == 60);
        const std::vector<fe::InputRedirect> r = fe::minimap_touch_redirects(l);
        REQUIRE(r.size() == 2);
        CHECK((r[0].from.x == 60 && r[0].from.w == 52 && r[0].to.x == 0 && r[0].to.w == 52));
        CHECK((r[1].from.x == l.points.x && r[1].from.w == l.points.w && r[1].to.x == 0));
        // The frame: a point (3 pixels) just outside the box.
        const std::vector<fe::Rect> f = fe::minimap_frame(l);
        REQUIRE(f.size() == 4);
        CHECK((f[0].x == l.box.x - 3 && f[0].y == l.box.y - 3 && f[0].h == 3));
        CHECK((f[0].w == l.box.w + 6 && f[3].x == l.box.x + l.box.w && f[3].w == 3));
    }
    SUBCASE("compass and narrow keep the place right of the compass, at 3:2 and wide") {
        const WideWindow ww = wide_window(1920, 1080);
        const fe::Rect vis = fe::minimap_visible(ww.image, ww.wide, 568, 568);
        const fe::MinimapLayout c = fe::minimap_layout(Minimap::kOverlay, MinimapAnchor::kCompass,
                                                       ww.image, ww.wide, vis, 960, 480);
        const fe::MinimapLayout old =
            fe::minimap_layout(Minimap::kOverlay, ww.image, ww.wide, 960, 480);
        REQUIRE(c.shown);
        CHECK_FALSE(c.corner);
        CHECK(c.compass_dx == 0);
        CHECK(fe::minimap_touch_redirects(c).empty());
        CHECK(c.box.x == ww.image.rect.x + 3 * fe::kMinimapOverlayX);
        CHECK((c.box.x == old.box.x && c.box.y == old.box.y && c.box.w == old.box.w));
    }
    SUBCASE("border with the corner: in the bar when it has room, else the corner") {
        const WideWindow ultra = wide_window(2560, 1080); // 21:9: bars beside the wide picture
        const fe::Rect vis = fe::minimap_visible(ultra.image, ultra.wide, 568, 568);
        const fe::MinimapLayout b = fe::minimap_layout(Minimap::kBorder, MinimapAnchor::kCorner,
                                                       ultra.image, ultra.wide, vis, 960, 480);
        REQUIRE(b.shown);
        CHECK(b.in_border);
        CHECK_FALSE(b.corner);
        CHECK(b.compass_dx == 0);
        CHECK(b.box.x + b.box.w <= ultra.wide.x);
        const WideWindow full = wide_window(1920, 1080);
        const fe::MinimapLayout c =
            fe::minimap_layout(Minimap::kBorder, MinimapAnchor::kCorner, full.image, full.wide,
                               fe::minimap_visible(full.image, full.wide, 568, 568), 960, 480);
        CHECK(c.corner);
        CHECK(c.box.x == full.wide.x + 12);
    }
    SUBCASE("every aspect, render scale, scale mode, view and safe area: the corner holds") {
        struct Case {
            int w, h, n;
            fe::Insets insets;
            fe::ScaleMode mode;
        };
        const Case cases[] = {
            {960, 640, 1, {}, fe::ScaleMode::kInteger},              // 3:2
            {1920, 1080, 2, {}, fe::ScaleMode::kInteger},            // 16:9
            {1920, 1200, 1, {}, fe::ScaleMode::kInteger},            // 16:10
            {2560, 1080, 3, {}, fe::ScaleMode::kFit},                // 21:9
            {1200, 1000, 1, {}, fe::ScaleMode::kSmooth},             // taller than 3:2
            {1280, 720, 1, {}, fe::ScaleMode::kFit},                 // 16:9, not a whole multiple
            {2532, 1170, 3, {0, 141, 63, 141}, fe::ScaleMode::kFit}, // a notched phone
            {2532, 1170, 1, {0, 141, 63, 141}, fe::ScaleMode::kInteger},
        };
        const std::int32_t maps[][2] = {{960, 480}, {3200, 640}, {320, 192}, {600, 2400}};
        for (const Case &cs : cases) {
            const WideWindow ww = wide_window(cs.w, cs.h, cs.n, cs.insets, cs.mode);
            REQUIRE(ww.image.rect.w > 0);
            const float k = ww.image.rect.w / 480;
            for (const std::uint32_t view : {568u, 540u, 480u}) {
                const fe::Rect vis = fe::minimap_visible(ww.image, ww.wide, 568, view);
                for (const auto &map : maps) {
                    CAPTURE(cs.w);
                    CAPTURE(cs.h);
                    CAPTURE(view);
                    CAPTURE(map[0]);
                    const fe::MinimapLayout l =
                        fe::minimap_layout(Minimap::kOverlay, MinimapAnchor::kCorner, ww.image,
                                           ww.wide, vis, map[0], map[1]);
                    REQUIRE(l.shown);
                    REQUIRE(l.corner);
                    // At the visible view's top left, a margin in, inside the safe area.
                    const float m = std::round(fe::kMinimapMargin * k);
                    CHECK(l.box.x == std::max(vis.x, ww.image.safe.x) + m);
                    CHECK(l.box.y == std::max(vis.y, ww.image.safe.y) + m);
                    CHECK(l.box.x >= ww.image.safe.x);
                    CHECK(l.box.x + l.box.w <= vis.x + vis.w);
                    // Whole pixels, at most 150 x 48 points: above the game's messages.
                    CHECK(l.box.w == std::floor(l.box.w));
                    CHECK(l.points.w <= fe::kMinimapCornerW + 0.01f);
                    CHECK(l.points.y + l.points.h <= 52 + 0.01f);
                    // The compass moved right of the box (and its frame), short of the quest note.
                    const fe::Rect compass{ww.image.rect.x + l.compass_dx * k, ww.image.rect.y,
                                           fe::kMinimapCompassSide * k,
                                           fe::kMinimapCompassSide * k};
                    CHECK_FALSE(overlaps(compass, l.box));
                    for (const fe::Rect &f : fe::minimap_frame(l)) {
                        CHECK_FALSE(overlaps(compass, f));
                    }
                    CHECK(l.compass_dx >= 0);
                    CHECK(l.compass_dx + fe::kMinimapCompassSide <= fe::kMinimapCornerRight + 1);
                }
            }
        }
    }
}

TEST_CASE("minimap: the hero's dot follows the hero, markers and dots stay in the box") {
    FakeGuest g = field_map();
    const fe::Placement p = placed(960, 640);
    fe::MinimapView v = fe::read_minimap(g.span());
    const fe::MinimapLayout l =
        fe::minimap_layout(Minimap::kOverlay, p, p.rect, v.map_w(), v.map_h());
    const fe::Rect a = fe::minimap_hero_dot(v, l);
    // 4 points at 2x: 8 pixels, centred at (100, 175 - 8) * 0.25 from the box.
    CHECK(a.w == 8);
    CHECK(a.x + a.w / 2 == doctest::Approx(l.box.x + 100 * 0.25).epsilon(0.01));
    CHECK(a.y + a.h / 2 == doctest::Approx(l.box.y + 167 * 0.25).epsilon(0.02));
    g.put32(kHero + 0x04, 260);
    v = fe::read_minimap(g.span());
    const fe::Rect b = fe::minimap_hero_dot(v, l);
    CHECK(b.x - a.x == doctest::Approx(160 * 0.25));
    CHECK(b.y == a.y);
    // Past the map's edge: clamped into the box.
    g.put32(kHero + 0x04, 100000);
    v = fe::read_minimap(g.span());
    const fe::Rect c = fe::minimap_hero_dot(v, l);
    CHECK(c.x + c.w == l.box.x + l.box.w);
    const std::vector<fe::MinimapDot> dots = fe::minimap_dots(v, l);
    REQUIRE(dots.size() == 4); // the portal, the NPC, the hero's edge and fill
    for (const fe::MinimapDot &d : dots) {
        CHECK(d.rect.x >= l.box.x);
        CHECK(d.rect.y >= l.box.y);
        CHECK(d.rect.x + d.rect.w <= l.box.x + l.box.w);
        CHECK(d.rect.y + d.rect.h <= l.box.y + l.box.h);
    }
}

TEST_CASE("minimap: a synthetic grid drawn (golden), thin floors kept when reduced") {
    FakeGuest g = field_map();
    const fe::MinimapView v = fe::read_minimap(g.span());
    const fe::Placement p = placed(1920, 1280); // 4x: 0.5 pixels a map pixel, 160 x 96
    const fe::MinimapLayout l =
        fe::minimap_layout(Minimap::kOverlay, p, p.rect, v.map_w(), v.map_h());
    REQUIRE(l.box.w == 160);
    REQUIRE(l.box.h == 96);
    const std::vector<u8> px = fe::render_minimap(v, l);
    REQUIRE(px.size() == 160u * 96 * 4);
    golden::Image img(160, 96);
    img.bytes() = px;
    const auto r = golden::check_golden(
        img, std::filesystem::path(MT_GOLDEN_DATA_DIR) / "minimap_synthetic.png",
        "minimap_synthetic");
    CHECK_MESSAGE(r.passed, r.message);
    // Spot checks: the ground (row 22: y 88-91) and the ladder (columns 10-11: x 40-47, rows
    // 13-21) and the wall (column 30: x 120-123).
    auto rgba = [&](int x, int y) {
        const u8 *q = &px[(static_cast<std::size_t>(y) * 160 + x) * 4];
        return fe::MinimapColor{q[0], q[1], q[2], q[3]};
    };
    auto same = [](fe::MinimapColor a, fe::MinimapColor b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    CHECK(same(rgba(150, 89), fe::kMinimapFoothold));
    CHECK(same(rgba(42, 70), fe::kMinimapLadder));
    CHECK(same(rgba(121, 70), fe::kMinimapWall));
    CHECK(same(rgba(150, 20), fe::kMinimapBack));
    // Reduced to an eighth (a cell under a pixel), the one-cell floors still show in full.
    fe::MinimapLayout small = l;
    small.scale = 1.0f / 64;
    small.box.w = 5;
    small.box.h = 3;
    const std::vector<u8> tiny = fe::minimap_grid_pixels(v, small);
    REQUIRE(tiny.size() == 5u * 3 * 4);
    for (int x = 0; x < 5; x++) {
        const u8 *q = &tiny[(2 * 5 + x) * 4]; // the bottom row: cells 16-23, the ground at 22
        CHECK(q[0] == fe::kMinimapFoothold.r);
    }
}

TEST_CASE("minimap: the layer on a software renderer, off draws nothing") {
    SDL_Surface *surface = SDL_CreateSurface(960, 640, SDL_PIXELFORMAT_ABGR8888);
    REQUIRE(surface != nullptr);
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    REQUIRE(renderer != nullptr);
    SDL_SetRenderDrawColor(renderer, 10, 20, 30, 255);
    SDL_RenderClear(renderer);
    auto read = [&] {
        SDL_Surface *s = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE(s != nullptr);
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(s);
        std::vector<u8> out(static_cast<u8 *>(c->pixels),
                            static_cast<u8 *>(c->pixels) +
                                static_cast<std::size_t>(c->pitch) * c->h);
        SDL_DestroySurface(c);
        return out;
    };
    const std::vector<u8> before = read();
    FakeGuest g = field_map();
    const fe::Placement p = placed(960, 640);
    fe::MinimapView v = fe::read_minimap(g.span());
    fe::MinimapLayer layer;
    // Off (the layout not shown) and a view not shown: byte for byte as before.
    CHECK(layer.draw(renderer, v,
                     fe::minimap_layout(Minimap::kOff, p, p.rect, v.map_w(), v.map_h())));
    CHECK(layer.draw(renderer, fe::MinimapView{},
                     fe::minimap_layout(Minimap::kOverlay, p, p.rect, v.map_w(), v.map_h())));
    CHECK(read() == before);
    CHECK(layer.stats().draws == 0);
    // On: the hero's fill at its dot, the draw colour kept; the grid uploaded once a map.
    const fe::MinimapLayout l =
        fe::minimap_layout(Minimap::kOverlay, p, p.rect, v.map_w(), v.map_h());
    REQUIRE(layer.draw(renderer, v, l));
    g.put32(kHero + 0x04, 200);
    v = fe::read_minimap(g.span());
    REQUIRE(layer.draw(renderer, v, l));
    CHECK(layer.stats().draws == 2);
    CHECK(layer.stats().uploads == 1);
    Uint8 r = 0, gg = 0, b = 0, a = 0;
    SDL_GetRenderDrawColor(renderer, &r, &gg, &b, &a);
    CHECK((r == 10 && gg == 20 && b == 30 && a == 255));
    const std::vector<u8> after = read();
    const fe::Rect dot = fe::minimap_hero_dot(v, l);
    const std::size_t at = (static_cast<std::size_t>(dot.y + dot.h / 2) * surface->pitch) +
                           (static_cast<std::size_t>(dot.x + dot.w / 2) * 4);
    CHECK(after[at] == fe::kMinimapHero.r);
    CHECK(after[at + 1] == fe::kMinimapHero.g);
    // Outside the box: untouched.
    CHECK(std::memcmp(&after[0], &before[0], 4 * 80) == 0);
    // A new map: uploaded again.
    cell(g, 0, 0) = 0x02;
    v = fe::read_minimap(g.span());
    REQUIRE(layer.draw(renderer, v, l));
    CHECK(layer.stats().uploads == 2);
    layer.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
}

TEST_CASE("minimap: the setting, off by default and in every preset") {
    const fe::Settings defaults;
    CHECK(defaults.minimap == Minimap::kOff);
    const fe::SettingInfo *row = fe::find_setting("minimap");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kVideo);
    CHECK(std::string(row->presets.original) == "off");
    CHECK(std::string(row->presets.original_controller) == "off");
    CHECK(std::string(row->presets.enhanced) == "off");
    CHECK(std::string(row->flag) == "--minimap");
    fe::Settings s;
    std::string why;
    REQUIRE(fe::apply_setting_list(&s, "minimap = border", &why));
    CHECK(s.minimap == Minimap::kBorder);
    REQUIRE(fe::apply_setting_list(&s, "minimap = overlay", &why));
    CHECK(s.minimap == Minimap::kOverlay);
    CHECK_FALSE(fe::apply_setting_list(&s, "minimap = big", &why));
}
