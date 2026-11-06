// The game's lists by the keys (ADR 0102): the NPC shop's list and the popup menu's item and
// equipment grids. The directions move the cursor one item (one cell, one row of the grid) at a
// time and the drawn list scrolls so the cursor's row stays in view.
//
// THE SHOP.
//
// The shop (game mode 2, the key pad's panel 6) takes its keys in Shop_OnKey 0x46DB9 (shop, key,
// game). The device never sends it up or down (MarketKeyPadView has no arrows; the list is
// dragged and tapped), and that key path is broken past the third row:
//
//   - The cursor (shop +0x60 + 4 x tab, an item index, -1 for none) moves by 1 up to row 2,
//     then the handler moves the tab's key scroll (+0x114[tab]: KeyScroll_Down 0x1B859 on +4,
//     the first row shown) instead, leaving the cursor where it was: it treats the cursor as a
//     row of the view, but the paint, the buy and the sell read it as an item index.
//   - Shop_Paint 0x56E91 draws the list by MarketKeyPadView's drag (movedTrackingPoint.y plus
//     scrollTrackingPoint.y, clamped to [38 x (5 - rows), 0]) and writes that view back into the
//     key scroll (+4 = -drag / 38) every frame, so a key scroll is undone at the next paint.
//
// So down stopped on the third row of Mr. Wang's six (the Buy tab) and of any Sell list longer
// than five. With the hook (always on: the keys never reach the shop on the device), up and down
// with no popup open move the cursor by one item within the tab's rows (the paint's count: the
// shop's rows on Buy, the hero's items in a row on Sell; from no cursor either key chooses the
// first row in view), and set the drag so that row is wholly in view (the five 38-point rows
// from y 62), as a touch drag would. Every other key, and up and down over a popup (its
// amounts), run the original; when one of them changes the tab (left, right, the tabs' '4' and
// '6'), the drag, which both tabs share, goes back to the top, as the new tab's cursor (-1) is.
//
// THE ITEM AND EQUIPMENT GRIDS. The popup menu's item tab (CDataMgr::keyPopupItemInven 0x5285D,
// cursor CDataMgr +0xD8, grid widget 0xF14B8) and equipment tab (PopupMenu_EquipTabOnKey 0x519F9,
// +0xDC, 0xF14C0) take the keys as a grid of 6 columns with 6 (items) or 3 (equipment) rows in
// view, past which they move the key scroll *(CDataMgr +0x10C) (the cursor then plus 6 x its
// first row). The grids drawn have 4 columns and scroll by the widget's touch drag (+0x6C) only.
// So down skipped a row and a half, and past the rows in view the cursor left the drawn grid,
// the drag unmoved. With the hooks, the directions with neither the shortcut panel (+0xCC) nor
// the quick slot choice (+0xBC) open move the cursor over the grid's cells (the widget's cells in
// use, +8) as drawn: left and right one cell, up and down to the nearest cell of the row above or
// below; the cursor is the cell's index, with the key scroll kept at 0, so OK uses or equips that
// cell's item as before; and the drag follows the cursor's cell as #166's does for the skill
// list. Up from the top row runs the original, which leaves the grid for the tab bar.
//
// PAGING (ADR 0114). With the port's page action held (LT in menus, Shift on the keyboard), up
// and down page these lists instead of stepping: the cursor moves by the rows in view (the
// shop's 5; a grid's rows wholly inside its rect) and the view moves with it, as far as the
// list allows, the cursor's row kept wholly in view. A grid's page up stops at the top row (it
// never leaves the grid for the tab bar). MenuInput (menu_input.h) asks list_page whether a
// list owns the focus, drops the direction then and applies the page at a turn with the
// runtime's queue empty; the guest's key handlers never see it.
#ifndef MAPLETHIEF_PLATFORM_LIST_KEYS_H
#define MAPLETHIEF_PLATFORM_LIST_KEYS_H

#include <cstdint>
#include <vector>

#include "maplethief/recomp/recomp.h"

namespace maplethief::frontend {

// The list's geometry (Shop_Paint): rows of 38 points, 5 in view.
constexpr std::int32_t kShopRowHeight = 38;
constexpr std::int32_t kShopRowsShown = 5;

// One up (-1) or down (+1) on a list of `rows` rows with the cursor at `cursor` (-1: none) and
// the drawn list at `drag` (points, <= 0): the cursor and the drag after it. The drag changes
// only as far as the cursor's row needs to be wholly in view, within [38 x (5 - rows), 0].
struct ShopListView {
    std::int32_t cursor = -1;
    std::int32_t drag = 0;
};
ShopListView shop_list_step(std::int32_t rows, ShopListView now, int dir);
// The drag clamped as Shop_Paint clamps it.
std::int32_t shop_list_clamp(std::int32_t rows, std::int32_t drag);

// A page up (-1) or down (+1): the cursor (from none: the first row wholly in view) moves by
// kShopRowsShown rows within the list, the drag by as many rows' points, clamped, then only as
// far as the cursor's row needs to be wholly in view.
ShopListView shop_list_page(std::int32_t rows, ShopListView now, int dir);

// The list's rows on the shop's tab now, as Shop_Paint counts them: tab 0 the shop's rows (+0x98),
// tab 1 the hero's items in a row (game +0x30, its list +0xD4 in a shop of kind 0, else +0xD8),
// at most 48.
std::int32_t shop_list_rows(const mt_cpu *cpu, std::uint32_t shop, std::uint32_t game);
// The drawn list's drag in MarketKeyPadView `market` (movedTrackingPoint.y +
// scrollTrackingPoint.y).
std::int32_t shop_list_drag(const mt_cpu *cpu, std::uint32_t market);

// A key of Shop_OnKey on the shop's list (the hook's work without the original): up (-1) or
// down (-2) with no popup open move the cursor and set `market`'s drag. False for any other key
// and over a popup: the original then runs. `market` 0: nothing to scroll (the cursor moves).
bool shop_list_key(mt_cpu *cpu, std::uint32_t shop, std::int32_t key, std::uint32_t game,
                   std::uint32_t market);
// A page (dir -1 up, +1 down) on the shop's list, as shop_list_key without the original's
// timers: false over a popup, on a bad shop or tab (nothing written).
bool shop_list_page_key(mt_cpu *cpu, std::uint32_t shop, int dir, std::uint32_t game,
                        std::uint32_t market);
// After the original ran for any other key: if it changed the tab from `tab_before`, the shared
// drag goes back to the top. False if the tab did not change.
bool shop_list_tab_changed(mt_cpu *cpu, std::uint32_t shop, std::int32_t tab_before,
                           std::uint32_t market);

// The hook on Shop_OnKey (ADR 0069): installed by the game at start (game.cpp) and by the
// real-asset drivers; uninstall puts the original back.
void install_shop_list_keys(bool on = true);
void uninstall_shop_list_keys();
bool shop_list_keys_installed();
struct ShopListStats {
    std::uint64_t keys = 0;    // up / down handled on the list
    std::uint64_t scrolls = 0; // of them, the drag moved
    std::uint64_t tabs = 0;    // tab changes whose drag went back to the top
};
ShopListStats shop_list_stats();

// A grid cell as drawn (the widget's cell rect plus the list's offset), screen points.
struct GridCell {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
};
// The cell after a direction key (-1 up, -2 down, -3 left, -4 right) from cell `from` (out of
// range: the nearest end, whatever the key), or -1 if there is none that way: left / right the
// previous / next cell; up / down the nearest cell of the nearest row above / below, by its
// centre's x.
int grid_step(const std::vector<GridCell> &cells, int from, std::int32_t key);
// The drag (within [min(lo, 0), 0]) that shows the cell at y (height h) wholly inside
// [top, bottom], starting from `drag`: unchanged when it already is.
std::int32_t grid_follow(std::int32_t y, std::int32_t h, std::int32_t top, std::int32_t bottom,
                         std::int32_t lo, std::int32_t drag);
// The rows of `cells` wholly inside a view `view_h` points high: the rows' pitch (the smallest
// gap between two rows' tops; the cell's height with one row), at least 1.
int grid_rows_in_view(const std::vector<GridCell> &cells, std::int32_t view_h);
// A page up (dir -1) or down (+1) from cell `from` (out of range: the nearest end): `rows` rows
// that way by grid_step, stopping at the last row there is; `from` itself at the end.
int grid_page(const std::vector<GridCell> &cells, int from, int dir, int rows);
// The drag after a page from cell `from` to cell `to`: moved by the cursor's move (the view
// pages with it) within [min(lo, 0), 0], then grid_follow for `to` inside [top, bottom].
std::int32_t grid_page_drag(const GridCell &from, const GridCell &to, std::int32_t top,
                            std::int32_t bottom, std::int32_t lo, std::int32_t drag);
// A page on the item (`tab` 1) or equipment (2) grid, as grid_key: false where grid_key would
// leave a direction to the original (the shortcut panel, the quick slot choice, the grid hidden
// or empty); `moved` whether the cursor changed.
bool grid_page_key(mt_cpu *cpu, std::uint32_t mgr, std::int32_t tab, int dir,
                   bool *moved = nullptr);

// A key of the item (`tab` 1) or equipment (2) grid's handler, data manager `mgr` (the hooks'
// work without the original): true if it was a direction that moved or held the cursor; false
// when the original is to run (another key, the shortcut panel or the quick slot choice open,
// the grid hidden or empty, up from the top row). `moved`: whether the cursor changed (the hook
// then runs ResetSubTimers01 0x386E1, as the original does after a move).
bool grid_key(mt_cpu *cpu, std::uint32_t mgr, std::int32_t tab, std::int32_t key,
              bool *moved = nullptr);

// The hooks on the two grids' handlers.
void install_grid_keys(bool on = true);
void uninstall_grid_keys();
struct GridStats {
    std::uint64_t keys = 0;    // directions handled
    std::uint64_t scrolls = 0; // of them, the drag moved
};
GridStats grid_stats();

// Paging from the frontend (ADR 0114), on key pad panel `panel` (mt_input_panel): with `apply`
// false, whether a list that pages owns the focus now: the shop's list (panel 6, game mode 2,
// no popup, tab Buy or Sell) or the popup menu's item or equipment grid (panel 1, game mode 1,
// that tab open (CDataMgr +0xA8), its grid shown with cells, neither the shortcut panel nor the
// quick slot choice open). With `apply` true it also pages it by `dir` (-1 up, +1 down) and,
// when the cursor moved, runs ResetSubTimers01 0x386E1 as the key handlers do. Without a game
// image (no cpu, unmapped globals): false.
bool list_page(mt_cpu *cpu, std::int32_t panel, int dir, bool apply);
struct PageStats {
    std::uint64_t pages = 0; // pages applied
    std::uint64_t moved = 0; // of them, the cursor moved
};
PageStats page_stats();

} // namespace maplethief::frontend

#endif
