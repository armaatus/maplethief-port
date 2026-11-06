// The game's lists by the keys (ADR 0102, platform/src/list_keys.h) with synthetic data only:
// the NPC shop's cursor and drag (shop_list_step), its rows and keys on a synthetic guest (a
// shop, a game and hero with an item list, a MarketKeyPadView's drag, the tabs' key scrolls),
// then the popup menu grids' moves (grid_step, grid_follow) and keys on a synthetic data
// manager and grid widget of 4 columns, and the hooks on both handlers.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "list_keys.h"
#include "maplethief/hooks.h"
#include "maplethief/recomp/calls.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr i32 kUp = -1, kDown = -2, kLeft = -3, kRight = -4, kOk = -5;

// A flat guest memory.
struct Guest {
    static constexpr u32 kSize = 0x120000;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(kSize, 0);
    mt_cpu cpu{};
    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = kSize;
        put(0x108C64, 0xFFFFFFFFu); // no popup open
    }
    void put(u32 addr, u32 v) { std::memcpy(mem.data() + addr, &v, 4); }
    void putf(u32 addr, float f) { std::memcpy(mem.data() + addr, &f, 4); }
    void put8(u32 addr, std::uint8_t v) { mem[addr] = v; }
    i32 get(u32 addr) const {
        i32 v = 0;
        std::memcpy(&v, mem.data() + addr, 4);
        return v;
    }
    float getf(u32 addr) const {
        float f = 0;
        std::memcpy(&f, mem.data() + addr, 4);
        return f;
    }
};

// The shop: Buy rows, the cursors, the key scrolls; the game, hero and its first item list.
struct ShopGuest : Guest {
    static constexpr u32 kShop = 0x10000, kScrolls = 0x11000, kScroll0 = 0x11100,
                         kScroll1 = 0x11200, kGame = 0x12000, kHero = 0x13000, kList = 0x14000,
                         kMarket = 0x15000;
    explicit ShopGuest(i32 buy_rows, i32 sell_rows) {
        put(kShop + 0x5C, 0);
        put(kShop + 0x60, 0xFFFFFFFFu);
        put(kShop + 0x64, 0xFFFFFFFFu);
        put(kShop + 0x90, 0);
        put(kShop + 0x98, static_cast<u32>(buy_rows));
        put(kShop + 0x114, kScrolls);
        put(kScrolls, kScroll0);
        put(kScrolls + 4, kScroll1);
        put(kGame + 0x30, kHero);
        put(kHero + 0xD4, kList);
        for (i32 i = 0; i < sell_rows; i++) {
            put(kList + 4 * static_cast<u32>(i), 0x20000 + 0x100 * static_cast<u32>(i));
        }
    }
    i32 cursor(i32 tab) const { return get(kShop + 0x60 + 4 * static_cast<u32>(tab)); }
    // The drag Shop_Paint reads: moved.y + scroll.y.
    float drag() const { return getf(kMarket + 64 + 4) + getf(kMarket + 56 + 4); }
    bool key(i32 k) { return fe::shop_list_key(&cpu, kShop, k, kGame, kMarket); }
};

} // namespace

TEST_CASE("list keys: the shop's cursor moves by item and the drag keeps its row in view") {
    // Five rows shown, 38 points each: the drag runs from 38 x (5 - rows) to 0.
    CHECK(fe::shop_list_clamp(9, 10) == 0);
    CHECK(fe::shop_list_clamp(9, -500) == -152);
    CHECK(fe::shop_list_clamp(3, -20) == 0); // a short list never scrolls
    // No cursor: the first row wholly in view.
    fe::ShopListView v = fe::shop_list_step(9, {-1, 0}, 1);
    CHECK((v.cursor == 0 && v.drag == 0));
    v = fe::shop_list_step(9, {-1, -40}, 1);
    CHECK((v.cursor == 2 && v.drag == -40));
    v = fe::shop_list_step(9, {12, 0}, -1); // out of range: as none
    CHECK((v.cursor == 0 && v.drag == 0));
    // Down through the rows in view, then past them: the list scrolls a row at a time.
    v = {0, 0};
    for (i32 i = 1; i <= 4; i++) {
        v = fe::shop_list_step(9, v, 1);
        CHECK((v.cursor == i && v.drag == 0));
    }
    v = fe::shop_list_step(9, v, 1);
    CHECK((v.cursor == 5 && v.drag == -38));
    for (i32 i = 0; i < 6; i++) {
        v = fe::shop_list_step(9, v, 1);
    }
    CHECK((v.cursor == 8 && v.drag == -152)); // the last row, at the bottom
    // Up: the drag stays until the cursor's row leaves the top.
    v = fe::shop_list_step(9, v, -1);
    v = fe::shop_list_step(9, v, -1);
    v = fe::shop_list_step(9, v, -1);
    v = fe::shop_list_step(9, v, -1);
    CHECK((v.cursor == 4 && v.drag == -152));
    v = fe::shop_list_step(9, v, -1);
    CHECK((v.cursor == 3 && v.drag == -114));
    // A drag a touch left between rows is clamped and the cursor's row shown wholly.
    v = fe::shop_list_step(9, {2, -100}, -1);
    CHECK((v.cursor == 1 && v.drag == -38));
    // An empty list: no cursor.
    v = fe::shop_list_step(0, {3, 0}, 1);
    CHECK(v.cursor == -1);
}

TEST_CASE("list keys: the shop's keys on a guest: the cursor, the drag and the key scroll") {
    ShopGuest g(8, 6);
    CHECK(fe::shop_list_rows(&g.cpu, ShopGuest::kShop, ShopGuest::kGame) == 8);
    for (i32 i = 0; i < 6; i++) {
        CHECK(g.key(kDown));
    }
    CHECK(g.cursor(0) == 5);
    CHECK(g.drag() == -38.0f);
    CHECK(fe::shop_list_drag(&g.cpu, ShopGuest::kMarket) == -38);
    CHECK(g.get(ShopGuest::kScroll0 + 4) == 1);     // the first row in view
    CHECK(g.get(ShopGuest::kScroll0 + 0x14) == 38); // and in points
    // A touch's scroll point adds to the drag; the key folds it into the moved point.
    g.putf(ShopGuest::kMarket + 56 + 4, -38.0f);
    CHECK(fe::shop_list_drag(&g.cpu, ShopGuest::kMarket) == -76);
    CHECK(g.key(kDown));
    CHECK(g.cursor(0) == 6);
    CHECK(g.drag() == -76.0f); // row 6 in view: the drag as it was
    CHECK(g.getf(ShopGuest::kMarket + 56 + 4) == -38.0f);
    CHECK(g.key(kDown));
    CHECK(g.cursor(0) == 7);
    CHECK(g.drag() == -114.0f);
    CHECK(g.getf(ShopGuest::kMarket + 56 + 4) == 0.0f);
    CHECK(g.key(kDown)); // the last row: it stays
    CHECK(g.cursor(0) == 7);
    // Not a direction, a popup open or a bad shop: the original's.
    CHECK_FALSE(g.key(kOk));
    CHECK_FALSE(g.key('6'));
    g.put(0x108C64, 5);
    CHECK_FALSE(g.key(kDown));
    g.put(0x108C64, 0xFFFFFFFFu);
    CHECK_FALSE(fe::shop_list_key(&g.cpu, Guest::kSize - 8, kDown, ShopGuest::kGame, 0));
    CHECK(g.cursor(0) == 7);

    // The Sell tab: the hero's list up to its first empty entry; its own cursor and key scroll.
    g.put(ShopGuest::kShop + 0x5C, 1);
    CHECK(fe::shop_list_rows(&g.cpu, ShopGuest::kShop, ShopGuest::kGame) == 6);
    CHECK(fe::shop_list_tab_changed(&g.cpu, ShopGuest::kShop, 0, ShopGuest::kMarket));
    CHECK(g.drag() == 0.0f);
    CHECK_FALSE(fe::shop_list_tab_changed(&g.cpu, ShopGuest::kShop, 1, ShopGuest::kMarket));
    for (i32 i = 0; i < 9; i++) {
        CHECK(g.key(kDown));
    }
    CHECK(g.cursor(1) == 5);
    CHECK(g.cursor(0) == 7);
    CHECK(g.drag() == -38.0f);
    CHECK(g.get(ShopGuest::kScroll1 + 4) == 1);
    // The other list (kind 1) unset: no rows, no cursor.
    g.put(ShopGuest::kShop + 0x90, 1);
    CHECK(fe::shop_list_rows(&g.cpu, ShopGuest::kShop, ShopGuest::kGame) == 0);
    CHECK(g.key(kDown));
    CHECK(g.cursor(1) == -1);
    // No market view (no key pad): the cursor still moves.
    g.put(ShopGuest::kShop + 0x90, 0);
    CHECK(fe::shop_list_key(&g.cpu, ShopGuest::kShop, kDown, ShopGuest::kGame, 0));
    CHECK(g.cursor(1) == 0);
}

TEST_CASE("list keys: grid moves follow the cells as drawn") {
    // Ten cells in rows of four, 72 points apart; the last row has two.
    std::vector<fe::GridCell> cells;
    for (i32 i = 0; i < 10; i++) {
        cells.push_back({20 + 72 * (i % 4), 60 + 70 * (i / 4), 64, 64});
    }
    CHECK(fe::grid_step(cells, 0, kRight) == 1);
    CHECK(fe::grid_step(cells, 3, kRight) == 4); // on to the next row
    CHECK(fe::grid_step(cells, 9, kRight) == -1);
    CHECK(fe::grid_step(cells, 0, kLeft) == -1);
    CHECK(fe::grid_step(cells, 4, kLeft) == 3);
    CHECK(fe::grid_step(cells, 1, kDown) == 5); // one row, not six cells
    CHECK(fe::grid_step(cells, 5, kDown) == 9);
    CHECK(fe::grid_step(cells, 7, kDown) == 9); // the nearest of the short row
    CHECK(fe::grid_step(cells, 9, kDown) == -1);
    CHECK(fe::grid_step(cells, 6, kUp) == 2);
    CHECK(fe::grid_step(cells, 2, kUp) == -1);
    CHECK(fe::grid_step(cells, 2, kOk) == -1);
    CHECK(fe::grid_step(cells, 17, kLeft) == 9); // out of range: the nearest end
    CHECK(fe::grid_step(cells, -1, kDown) == 0);
    CHECK(fe::grid_step({}, 0, kDown) == -1);

    // The drag: unchanged with the cell in view, else just enough, within [lo, 0].
    CHECK(fe::grid_follow(100, 64, 50, 250, -200, 0) == 0);
    CHECK(fe::grid_follow(260, 64, 50, 250, -200, 0) == -74);
    CHECK(fe::grid_follow(260, 64, 50, 250, -200, -100) == -100);
    CHECK(fe::grid_follow(60, 64, 50, 250, -200, -100) == -10);
    CHECK(fe::grid_follow(600, 64, 50, 250, -200, 0) == -200);
    CHECK(fe::grid_follow(260, 64, 50, 250, 30, 0) == 0); // content shorter than the area
}

namespace {

// The data manager, its key scroll, and the item grid's widget with `n` cells in rows of 4.
struct GridGuest : Guest {
    static constexpr u32 kMgr = 0x10000, kScroll = 0x11000, kWidget = 0x12000, kCells = 0x13000,
                         kCell0 = 0x14000;
    explicit GridGuest(i32 n) {
        put(kMgr + 0x10C, kScroll);
        put(0xF14B8, 0x11F00);
        put(0x11F00, kWidget);
        put(kWidget + 0x00, kCells);
        put(kWidget + 0x04, 48);
        put(kWidget + 0x08, static_cast<u32>(n));
        put(kWidget + 0x20, 50);  // rect y
        put(kWidget + 0x28, 220); // rect h: the list shows y 50 to 270
        put(kWidget + 0x38, 70 * ((static_cast<u32>(n) + 3) / 4)); // the content's height
        put(kWidget + 0x48, 220); // the touch area's: the drag's clamp [220 - content, 0]
        put8(kWidget + 0xB1, 1);
        for (i32 i = 0; i < n; i++) {
            const u32 c = kCell0 + 0x100 * static_cast<u32>(i);
            put(kCells + 4 * static_cast<u32>(i), c);
            put(c + 0x08, 20); // the list's offset
            put(c + 0x0C, 50);
            put(c + 0x20, static_cast<u32>(72 * (i % 4))); // the cell's rect
            put(c + 0x24, static_cast<u32>(70 * (i / 4)));
            put(c + 0x28, 64);
            put(c + 0x2C, 64);
        }
    }
    i32 cursor() const { return get(kMgr + 0xD8); }
    i32 drag() const { return get(kWidget + 0x6C); }
    bool key(i32 k, bool *moved = nullptr) { return fe::grid_key(&cpu, kMgr, 1, k, moved); }
};

int g_originals = 0;
void fake_original(mt_cpu *) { g_originals++; }

} // namespace

TEST_CASE("list keys: the item grid's keys on a guest: by the cells drawn, the drag follows") {
    GridGuest g(14); // rows of 4, 4, 4, 2 at y 50, 120, 190, 260; the list shows 50 to 270
    bool moved = false;
    CHECK(g.key(kDown, &moved));
    CHECK((g.cursor() == 4 && moved));
    CHECK(g.key(kDown));
    CHECK((g.cursor() == 8 && g.drag() == 0)); // the third row ends at 254: in view
    g.put(GridGuest::kWidget + 0x74, 900);     // a fling
    CHECK(g.key(kDown));
    CHECK((g.cursor() == 12 && g.drag() == -54)); // the last row's bottom at the list's
    CHECK(g.get(GridGuest::kWidget + 0x74) == 0);
    CHECK(g.key(kRight));
    CHECK(g.cursor() == 13);
    CHECK(g.key(kRight, &moved)); // the last cell: it stays
    CHECK((g.cursor() == 13 && !moved));
    CHECK(g.key(kDown, &moved));
    CHECK((g.cursor() == 13 && !moved));
    CHECK(g.key(kUp));
    CHECK((g.cursor() == 9 && g.drag() == -54));
    CHECK(g.key(kUp));
    CHECK((g.cursor() == 5 && g.drag() == -54));
    CHECK(g.key(kUp));
    CHECK((g.cursor() == 1 && g.drag() == 0)); // the top row back in view
    CHECK_FALSE(g.key(kUp));                   // up from the top row: the original's (the tab bar)
    CHECK_FALSE(g.key(kOk));

    // The handlers' cursor with the key scroll moved: as a cell index, the key scroll at 0.
    g.put(GridGuest::kScroll + 4, 1);
    g.put(GridGuest::kMgr + 0xD8, 2);
    CHECK(g.key(kLeft));
    CHECK((g.cursor() == 7 && g.get(GridGuest::kScroll + 4) == 0));
    // The original's cursor past the cells in use: the last one.
    g.put(GridGuest::kMgr + 0xD8, 20);
    CHECK(g.key(kLeft));
    CHECK(g.cursor() == 13);

    // The shortcut panel or a quick slot choice open, the grid hidden: the original's.
    g.put8(GridGuest::kMgr + 0xCC, 1);
    CHECK_FALSE(g.key(kLeft));
    g.put8(GridGuest::kMgr + 0xCC, 0);
    g.put8(GridGuest::kMgr + 0xBC, 1);
    CHECK_FALSE(g.key(kLeft));
    g.put8(GridGuest::kMgr + 0xBC, 0);
    g.put8(GridGuest::kWidget + 0xB1, 0);
    CHECK_FALSE(g.key(kLeft));
    g.put8(GridGuest::kWidget + 0xB1, 1);
    // The equipment grid with no widget: the original's.
    CHECK_FALSE(fe::grid_key(&g.cpu, GridGuest::kMgr, 2, kLeft));
    CHECK(g.cursor() == 13);
}

TEST_CASE("list keys: the hooks on the shop's and the grids' handlers") {
    mt_hook_reset();
    fe::install_shop_list_keys();
    fe::install_grid_keys();
    CHECK(fe::shop_list_keys_installed());
    CHECK(mt_hook_enabled(MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY) != 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_POPUP_MENU_EQUIP_TAB_ON_KEY) != 0);

    // The grid: a key the hook leaves runs the original; one it takes does not.
    GridGuest g(6);
    g_originals = 0;
    g.cpu.r[0] = GridGuest::kMgr;
    g.cpu.r[1] = static_cast<u32>(kOk);
    CHECK(mt_hook_entry(&g.cpu, MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY, fake_original) == 0);
    g.put(GridGuest::kMgr + 0xD8, 5);
    g.cpu.r[0] = GridGuest::kMgr;
    g.cpu.r[1] = static_cast<u32>(kRight); // the last cell: no move, so no guest call
    const fe::GridStats before = fe::grid_stats();
    CHECK(mt_hook_entry(&g.cpu, MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY, fake_original) != 0);
    CHECK(g_originals == 0);
    CHECK(fe::grid_stats().keys == before.keys + 1);

    // The shop: a tab key runs the original, then the drag goes back to the top.
    ShopGuest s(8, 3);
    const fe::ShopListStats tabs = fe::shop_list_stats();
    s.cpu.r[0] = ShopGuest::kShop;
    s.cpu.r[1] = '6';
    s.cpu.r[2] = ShopGuest::kGame;
    CHECK(mt_hook_entry(&s.cpu, MT_HOOK_FN_SHOP_ON_KEY, [](mt_cpu *cpu) {
              g_originals++;
              const u32 sell = 1;
              std::memcpy(cpu->mem + ShopGuest::kShop + 0x5C, &sell, 4);
          }) != 0);
    CHECK(g_originals == 1);
    CHECK(fe::shop_list_stats().tabs == tabs.tabs + 1);

    fe::install_shop_list_keys(false);
    CHECK_FALSE(fe::shop_list_keys_installed());
    fe::uninstall_shop_list_keys();
    fe::uninstall_grid_keys();
    CHECK(mt_hook_enabled(MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY) == 0);
    mt_hook_reset();
}

TEST_CASE("list keys: a page moves the shop's cursor by the rows in view, kept in view") {
    // Twelve rows, five shown: the drag runs from -266 to 0.
    fe::ShopListView v = fe::shop_list_page(12, {0, 0}, 1);
    CHECK((v.cursor == 5 && v.drag == -190)); // the view paged with it: row 5 at the top
    v = fe::shop_list_page(12, v, 1);
    CHECK((v.cursor == 10 && v.drag == -266)); // the drag clamped at the list's end
    v = fe::shop_list_page(12, v, 1);
    CHECK((v.cursor == 11 && v.drag == -266)); // the last row
    v = fe::shop_list_page(12, v, 1);
    CHECK((v.cursor == 11 && v.drag == -266)); // it stays
    v = fe::shop_list_page(12, v, -1);
    CHECK((v.cursor == 6 && v.drag == -76));
    v = fe::shop_list_page(12, v, -1);
    CHECK((v.cursor == 1 && v.drag == 0));
    v = fe::shop_list_page(12, v, -1);
    CHECK((v.cursor == 0 && v.drag == 0));
    // No cursor: from the first row wholly in view.
    v = fe::shop_list_page(12, {-1, -40}, 1);
    CHECK((v.cursor == 7 && v.drag == -230));
    // A short list: the last row, no scroll; an empty one: no cursor.
    v = fe::shop_list_page(3, {0, 0}, 1);
    CHECK((v.cursor == 2 && v.drag == 0));
    CHECK(fe::shop_list_page(0, {2, 0}, 1).cursor == -1);
    // Every page of every list keeps the cursor's row wholly in view, within the clamp.
    for (i32 rows = 1; rows <= 30; rows++) {
        for (i32 from = -1; from < rows; from++) {
            for (const i32 drag : {0, -19, -38, -500}) {
                for (const int dir : {-1, 1}) {
                    const fe::ShopListView p = fe::shop_list_page(rows, {from, drag}, dir);
                    const i32 y = p.cursor * fe::kShopRowHeight + p.drag;
                    CAPTURE(rows);
                    CAPTURE(from);
                    CAPTURE(drag);
                    CAPTURE(dir);
                    CHECK((p.cursor >= 0 && p.cursor < rows));
                    CHECK((y >= 0 &&
                           y + fe::kShopRowHeight <= fe::kShopRowsShown * fe::kShopRowHeight));
                    CHECK(p.drag == fe::shop_list_clamp(rows, p.drag));
                    if (from >= 0) {
                        CHECK(p.cursor == std::clamp(from + dir * fe::kShopRowsShown, 0, rows - 1));
                    }
                }
            }
        }
    }
}

TEST_CASE("list keys: a page on the shop's list on a guest, and from the frontend") {
    ShopGuest g(12, 0);
    CHECK(
        fe::shop_list_page_key(&g.cpu, ShopGuest::kShop, 1, ShopGuest::kGame, ShopGuest::kMarket));
    CHECK(g.cursor(0) == 5);
    CHECK(g.drag() == -190.0f);
    CHECK(g.get(ShopGuest::kScroll0 + 4) == 5); // the first row in view
    CHECK(g.get(ShopGuest::kScroll0 + 0x14) == 190);
    g.put(0x108C64, 5); // a popup open: nothing
    CHECK_FALSE(
        fe::shop_list_page_key(&g.cpu, ShopGuest::kShop, 1, ShopGuest::kGame, ShopGuest::kMarket));
    CHECK(g.cursor(0) == 5);
    // list_page: the shop's list owns the focus on panel 6 in game mode 2 without a popup.
    g.put(0xF1454, 0x10F00);
    g.put(0x10F00, ShopGuest::kShop);
    g.put(0x11AA00, 2);
    CHECK_FALSE(fe::list_page(&g.cpu, 6, 1, false)); // the popup still open
    g.put(0x108C64, 0xFFFFFFFFu);
    CHECK(fe::list_page(&g.cpu, 6, 1, false));
    CHECK_FALSE(fe::list_page(&g.cpu, 7, 1, false)); // another panel
    CHECK_FALSE(fe::list_page(&g.cpu, 1, 1, false));
    g.put(0x11AA00, 0); // the field
    CHECK_FALSE(fe::list_page(&g.cpu, 6, 1, false));
    g.put(0x11AA00, 2);
    g.put(ShopGuest::kShop + 0x5C, 3); // a bad tab
    CHECK_FALSE(fe::list_page(&g.cpu, 6, 1, false));
    g.put(ShopGuest::kShop + 0x5C, 0);
    // Applied where the cursor cannot move (page up on row 0): paged, no guest call.
    g.put(ShopGuest::kShop + 0x60, 0);
    const fe::PageStats before = fe::page_stats();
    CHECK(fe::list_page(&g.cpu, 6, -1, true));
    CHECK(g.cursor(0) == 0);
    CHECK(fe::page_stats().pages == before.pages + 1);
    CHECK(fe::page_stats().moved == before.moved);
    // No game image: nothing.
    CHECK_FALSE(fe::list_page(nullptr, 6, 1, false));
}

TEST_CASE("list keys: a page on a grid moves by the rows in view and keeps the cell in view") {
    // Rows 70 points apart, 64 high, in a view of 220: three rows wholly in view.
    std::vector<fe::GridCell> cells;
    for (i32 i = 0; i < 30; i++) {
        cells.push_back({20 + 72 * (i % 4), 50 + 70 * (i / 4), 64, 64});
    }
    CHECK(fe::grid_rows_in_view(cells, 220) == 3);
    CHECK(fe::grid_rows_in_view(cells, 64) == 1);
    CHECK(fe::grid_rows_in_view(cells, 10) == 1);
    CHECK(fe::grid_rows_in_view({cells[0], cells[1]}, 220) == 3); // one row: by its height
    CHECK(fe::grid_page(cells, 1, 1, 3) == 13);
    CHECK(fe::grid_page(cells, 25, 1, 3) == 29); // the last row there is (two cells)
    CHECK(fe::grid_page(cells, 29, 1, 3) == 29);
    CHECK(fe::grid_page(cells, 17, -1, 3) == 5);
    CHECK(fe::grid_page(cells, 5, -1, 3) == 1); // stops at the top row, never leaves the grid
    CHECK(fe::grid_page(cells, 40, -1, 3) == 29);
    CHECK(fe::grid_page({}, 0, 1, 3) == -1);
    // The drag pages with the cursor, clamped, the cell wholly in view.
    CHECK(fe::grid_page_drag(cells[1], cells[13], 50, 270, -340, 0) == -210);
    CHECK(fe::grid_page_drag(cells[13], cells[25], 50, 270, -340, -210) == -340);
    CHECK(fe::grid_page_drag(cells[29], cells[17], 50, 270, -340, -340) == -130);

    GridGuest g(30); // the list shows 50 to 270; content 560: the drag runs from -340 to 0
    g.put(GridGuest::kMgr + 0xD8, 1);
    bool moved = false;
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, 1, &moved));
    CHECK((g.cursor() == 13 && g.drag() == -210 && moved));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, 1));
    CHECK((g.cursor() == 25 && g.drag() == -340));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, 1));
    CHECK((g.cursor() == 29 && g.drag() == -340));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, 1, &moved));
    CHECK((g.cursor() == 29 && !moved));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, -1));
    CHECK((g.cursor() == 17 && g.drag() == -130));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, -1));
    CHECK((g.cursor() == 5 && g.drag() == 0));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, -1));
    CHECK((g.cursor() == 1 && g.drag() == 0));
    CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, -1, &moved));
    CHECK((g.cursor() == 1 && !moved)); // the top row: it stays in the grid
    // Every page from every cell keeps the cursor's cell wholly in view.
    for (i32 from = 0; from < 30; from++) {
        for (const int dir : {-1, 1}) {
            for (const i32 drag : {0, -100, -340}) {
                g.put(GridGuest::kMgr + 0xD8, static_cast<u32>(from));
                g.put(GridGuest::kWidget + 0x6C, static_cast<u32>(drag));
                CHECK(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, dir));
                const fe::GridCell &c = cells[static_cast<std::size_t>(g.cursor())];
                CAPTURE(from);
                CAPTURE(dir);
                CAPTURE(drag);
                CHECK(c.y + g.drag() >= 50);
                CHECK(c.y + c.h + g.drag() <= 270);
                CHECK((g.drag() <= 0 && g.drag() >= -340));
            }
        }
    }
    // The shortcut panel open: not a page.
    g.put8(GridGuest::kMgr + 0xCC, 1);
    CHECK_FALSE(fe::grid_page_key(&g.cpu, GridGuest::kMgr, 1, 1));
    g.put8(GridGuest::kMgr + 0xCC, 0);
    // list_page: the grid owns the focus on panel 1, game mode 1, its tab open.
    g.put(0x11A984, GridGuest::kMgr);
    g.put(0x11AA00, 1);
    g.put(GridGuest::kMgr + 0xA8, 1);
    CHECK(fe::list_page(&g.cpu, 1, 1, false));
    CHECK_FALSE(fe::list_page(&g.cpu, 6, 1, false));
    g.put(GridGuest::kMgr + 0xA8, 2); // the equipment tab: no widget here
    CHECK_FALSE(fe::list_page(&g.cpu, 1, 1, false));
    g.put(GridGuest::kMgr + 0xA8, 0); // the tab bar
    CHECK_FALSE(fe::list_page(&g.cpu, 1, 1, false));
    g.put(GridGuest::kMgr + 0xA8, 1);
    g.put8(GridGuest::kWidget + 0xB1, 0); // the grid hidden
    CHECK_FALSE(fe::list_page(&g.cpu, 1, 1, false));
}
