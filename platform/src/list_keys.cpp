// The game's lists by the keys (ADR 0102); see list_keys.h.
#include "list_keys.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/objc/host.h"

namespace maplethief::frontend {

namespace {

// The shop object (Shop_OnKey's and Shop_Paint's first argument, docs/controls/keypad.md).
constexpr std::uint32_t kShopTab = 0x5C;      // 0 Buy, 1 Sell
constexpr std::uint32_t kShopCursor = 0x60;   // + 4 x tab: the tab's chosen row (-1: none)
constexpr std::uint32_t kShopKind = 0x90;     // 0 or 1: which of the hero's lists Sell shows
constexpr std::uint32_t kShopRows = 0x98;     // the Buy tab's rows
constexpr std::uint32_t kShopScrolls = 0x114; // -> the tabs' key scrolls (KeyScroll_Init 0x1B7D1)
constexpr std::uint32_t kScrollFirst = 0x4;   // the first row in view
constexpr std::uint32_t kScrollPixels = 0x14; // the drag in points (Shop_Paint: -drag)
// The game (CStateGame): +0x30 the hero; the hero's two item lists (48 pointers each).
constexpr std::uint32_t kGameHero = 0x30;
constexpr std::uint32_t kHeroList0 = 0xD4, kHeroList1 = 0xD8;
constexpr std::int32_t kMaxItems = 48;
// The open popup's id (-1: none), Popup_Open 0x3EAA1 / Popup_Close 0x3E945.
constexpr std::uint32_t kPopupId = 0x108C64;
// ResetSubTimers01 (shop): what the original does after each list key.
constexpr std::uint32_t kResetSubTimers = 0x386E1;
// MarketKeyPadView's ivars (objc.json): the drag Shop_Paint reads, CGPoints of floats.
constexpr std::uint32_t kScrollPoint = 56, kMovedPoint = 64;
constexpr std::uint32_t kForceScroll = 84;
// TouchKeyPadView's marketKeyPadView ivar.
constexpr std::uint32_t kPadMarket = 60;
// The game's key codes (docs/controls/keypad.md).
constexpr std::int32_t kKeyUp = -1, kKeyDown = -2, kKeyLeft = -3, kKeyRight = -4;

bool mapped(const mt_cpu *cpu, std::uint32_t addr, std::uint32_t n) {
    return cpu != nullptr && cpu->mem != nullptr && addr != 0 &&
           addr + std::uint64_t{n} <= cpu->mem_size;
}
std::uint32_t rd32(const mt_cpu *cpu, std::uint32_t addr) {
    std::uint32_t v = 0;
    if (mapped(cpu, addr, 4)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
std::int32_t rdi(const mt_cpu *cpu, std::uint32_t addr) {
    return static_cast<std::int32_t>(rd32(cpu, addr));
}
float rdf(const mt_cpu *cpu, std::uint32_t addr) {
    float f = 0;
    if (mapped(cpu, addr, 4)) {
        std::memcpy(&f, cpu->mem + addr, 4);
    }
    return std::isfinite(f) ? f : 0.0f;
}
void wr32(mt_cpu *cpu, std::uint32_t addr, std::uint32_t v) {
    if (mapped(cpu, addr, 4)) {
        std::memcpy(cpu->mem + addr, &v, 4);
    }
}
void wrf(mt_cpu *cpu, std::uint32_t addr, float f) {
    if (mapped(cpu, addr, 4)) {
        std::memcpy(cpu->mem + addr, &f, 4);
    }
}

// The drawn list at `drag`: the drag, the scroll and any fling Shop_Paint would add.
void set_drag(mt_cpu *cpu, std::uint32_t market, std::int32_t drag) {
    if (market == 0) {
        return;
    }
    wrf(cpu, market + kScrollPoint, 0.0f);
    wrf(cpu, market + kScrollPoint + 4, 0.0f);
    wrf(cpu, market + kMovedPoint, 0.0f);
    wrf(cpu, market + kMovedPoint + 4, static_cast<float>(drag));
    wrf(cpu, market + kForceScroll, 0.0f);
}

std::atomic<std::uint64_t> g_keys{0}, g_scrolls{0}, g_tabs{0};

// The key pad's MarketKeyPadView, or 0.
std::uint32_t market_view(mt_cpu *cpu) {
    const std::uint32_t pad = mt_input_pad_view();
    if (pad == 0 || mt_objc_current() == nullptr) {
        return 0;
    }
    return rd32(cpu, pad + kPadMarket);
}

int on_shop_key(mt_cpu *cpu, const mt_hook_call *call) {
    const std::uint32_t shop = cpu->r[0], game = cpu->r[2];
    const auto key = static_cast<std::int32_t>(cpu->r[1]);
    const std::uint32_t market = market_view(cpu);
    if (shop_list_key(cpu, shop, key, game, market)) {
        const std::uint32_t args[1] = {shop};
        mt_call_guest(cpu, kResetSubTimers, args, 1, MT_HOOK_FN_SHOP_ON_KEY);
        return MT_HOOK_HANDLED;
    }
    // Every other key: the original, then the drag back to the top if it changed the tab (left
    // and right, the tabs' '4' and '6').
    const std::int32_t tab = rdi(cpu, shop + kShopTab);
    call->original(cpu);
    shop_list_tab_changed(cpu, shop, tab, market);
    return MT_HOOK_HANDLED;
}

} // namespace

std::int32_t shop_list_clamp(std::int32_t rows, std::int32_t drag) {
    const std::int32_t lo = std::min(0, kShopRowHeight * (kShopRowsShown - rows));
    return std::clamp(drag, lo, 0);
}

ShopListView shop_list_step(std::int32_t rows, ShopListView now, int dir) {
    ShopListView v = now;
    if (rows <= 0) {
        v.cursor = -1;
        return v;
    }
    v.drag = shop_list_clamp(rows, v.drag);
    if (v.cursor < 0 || v.cursor >= rows) {
        // No cursor yet: the first row wholly in view.
        const std::int32_t top = (-v.drag + kShopRowHeight - 1) / kShopRowHeight;
        v.cursor = std::min(top, rows - 1);
    } else {
        v.cursor = std::clamp(v.cursor + (dir < 0 ? -1 : 1), 0, rows - 1);
    }
    const std::int32_t y = v.cursor * kShopRowHeight + v.drag; // the row's top in the view
    if (y < 0) {
        v.drag = -v.cursor * kShopRowHeight;
    } else if (y + kShopRowHeight > kShopRowsShown * kShopRowHeight) {
        v.drag = -(v.cursor - kShopRowsShown + 1) * kShopRowHeight;
    }
    v.drag = shop_list_clamp(rows, v.drag);
    return v;
}

ShopListView shop_list_page(std::int32_t rows, ShopListView now, int dir) {
    ShopListView v = now;
    if (rows <= 0) {
        v.cursor = -1;
        return v;
    }
    v.drag = shop_list_clamp(rows, v.drag);
    if (v.cursor < 0 || v.cursor >= rows) {
        const std::int32_t top = (-v.drag + kShopRowHeight - 1) / kShopRowHeight;
        v.cursor = std::min(top, rows - 1);
    }
    const std::int32_t by = dir < 0 ? -kShopRowsShown : kShopRowsShown;
    v.cursor = std::clamp(v.cursor + by, 0, rows - 1);
    v.drag = shop_list_clamp(rows, v.drag - by * kShopRowHeight);
    const std::int32_t y = v.cursor * kShopRowHeight + v.drag; // the row's top in the view
    if (y < 0) {
        v.drag = -v.cursor * kShopRowHeight;
    } else if (y + kShopRowHeight > kShopRowsShown * kShopRowHeight) {
        v.drag = -(v.cursor - kShopRowsShown + 1) * kShopRowHeight;
    }
    v.drag = shop_list_clamp(rows, v.drag);
    return v;
}

std::int32_t shop_list_rows(const mt_cpu *cpu, std::uint32_t shop, std::uint32_t game) {
    if (rdi(cpu, shop + kShopTab) != 1) {
        return std::clamp(rdi(cpu, shop + kShopRows), 0, 1000);
    }
    const std::uint32_t hero = rd32(cpu, game + kGameHero);
    const std::uint32_t list =
        rd32(cpu, hero + (rdi(cpu, shop + kShopKind) == 0 ? kHeroList0 : kHeroList1));
    if (hero == 0 || list == 0) {
        return 0;
    }
    std::int32_t n = 0;
    while (n < kMaxItems && rd32(cpu, list + 4 * static_cast<std::uint32_t>(n)) != 0) {
        n++;
    }
    return n;
}

std::int32_t shop_list_drag(const mt_cpu *cpu, std::uint32_t market) {
    if (market == 0) {
        return 0;
    }
    return static_cast<std::int32_t>(rdf(cpu, market + kMovedPoint + 4) +
                                     rdf(cpu, market + kScrollPoint + 4));
}

namespace {

// The shop's list moved by `move` (a step or a page): the cursor, the drag and the tab's key
// scroll written. False over a popup or on a bad shop or tab.
template <typename Move>
bool shop_list_move(mt_cpu *cpu, std::uint32_t shop, std::uint32_t game, std::uint32_t market,
                    Move move) {
    if (!mapped(cpu, shop, kShopScrolls + 4) || rdi(cpu, kPopupId) != -1) {
        return false;
    }
    const std::int32_t tab = rdi(cpu, shop + kShopTab);
    if (tab != 0 && tab != 1) {
        return false;
    }
    const std::uint32_t cursor_at = shop + kShopCursor + 4 * static_cast<std::uint32_t>(tab);
    const std::int32_t rows = shop_list_rows(cpu, shop, game);
    const std::int32_t drag = shop_list_drag(cpu, market);
    const ShopListView v = move(rows, ShopListView{rdi(cpu, cursor_at), drag});
    wr32(cpu, cursor_at, static_cast<std::uint32_t>(v.cursor));
    if (v.drag != drag) {
        set_drag(cpu, market, v.drag);
        g_scrolls++;
    }
    // The tab's key scroll as the next paint writes it.
    const std::uint32_t scroll =
        rd32(cpu, rd32(cpu, shop + kShopScrolls) + 4 * static_cast<std::uint32_t>(tab));
    if (mapped(cpu, scroll, kScrollPixels + 4)) {
        wr32(cpu, scroll + kScrollFirst, static_cast<std::uint32_t>(-v.drag / kShopRowHeight));
        wr32(cpu, scroll + kScrollPixels, static_cast<std::uint32_t>(-v.drag));
    }
    return true;
}

} // namespace

bool shop_list_key(mt_cpu *cpu, std::uint32_t shop, std::int32_t key, std::uint32_t game,
                   std::uint32_t market) {
    if (key != kKeyUp && key != kKeyDown) {
        return false;
    }
    const int dir = key == kKeyUp ? -1 : 1;
    if (!shop_list_move(cpu, shop, game, market, [dir](std::int32_t rows, ShopListView now) {
            return shop_list_step(rows, now, dir);
        })) {
        return false;
    }
    g_keys++;
    return true;
}

bool shop_list_page_key(mt_cpu *cpu, std::uint32_t shop, int dir, std::uint32_t game,
                        std::uint32_t market) {
    return shop_list_move(cpu, shop, game, market, [dir](std::int32_t rows, ShopListView now) {
        return shop_list_page(rows, now, dir);
    });
}

bool shop_list_tab_changed(mt_cpu *cpu, std::uint32_t shop, std::int32_t tab_before,
                           std::uint32_t market) {
    if (rdi(cpu, shop + kShopTab) == tab_before) {
        return false;
    }
    set_drag(cpu, market, 0);
    g_tabs++;
    return true;
}

void install_shop_list_keys(bool on) {
    mt_hook_register(MT_HOOK_FN_SHOP_ON_KEY, on_shop_key, nullptr, on ? 1 : 0);
}

void uninstall_shop_list_keys() { mt_hook_unregister(MT_HOOK_FN_SHOP_ON_KEY); }

bool shop_list_keys_installed() { return mt_hook_enabled(MT_HOOK_FN_SHOP_ON_KEY) != 0; }

ShopListStats shop_list_stats() {
    return ShopListStats{g_keys.load(), g_scrolls.load(), g_tabs.load()};
}

// --- the popup menu's item and equipment grids ------------------------------------------------

namespace {

// CDataMgr's fields (docs/controls/keypad.md, "GL touch widgets").
constexpr std::uint32_t kMgrShortcuts = 0xCC; // the shortcut panel open
constexpr std::uint32_t kMgrAssign = 0xBC;    // choosing a quick slot for an item
constexpr std::uint32_t kMgrKeyScroll = 0x10C;
constexpr std::uint32_t kItemCursor = 0xD8, kEquipCursor = 0xDC;
constexpr std::uint32_t kItemGrid = 0xF14B8, kEquipGrid = 0xF14C0; // -> the widget
// A list widget: its cells, their capacity and use, its rect, the content's height and the
// touch area's (the drag's clamp: [area - content, 0]), the touch drag, the fling, shown.
constexpr std::uint32_t kWCells = 0x0, kWCapacity = 0x4, kWUsed = 0x8, kWRectY = 0x20,
                        kWRectH = 0x28, kWContent = 0x38, kWArea = 0x48, kWDrag = 0x6C,
                        kWFling = 0x74, kWShown = 0xB1;
// A cell: the list's offset (the cell's +0x08, +0x0C), its rect (+0x20), placed alone (+0x5C).
constexpr std::uint32_t kCOffX = 0x08, kCOffY = 0x0C, kCRect = 0x20, kCAlone = 0x5C;

std::atomic<std::uint64_t> g_grid_keys{0}, g_grid_scrolls{0};

std::uint8_t rd8(const mt_cpu *cpu, std::uint32_t addr) {
    return mapped(cpu, addr, 1) ? cpu->mem[addr] : 0;
}

std::vector<GridCell> grid_cells(const mt_cpu *cpu, std::uint32_t widget) {
    std::vector<GridCell> out;
    const std::uint32_t used =
        std::min({rd32(cpu, widget + kWUsed), rd32(cpu, widget + kWCapacity), 100u});
    const std::uint32_t cells = rd32(cpu, widget + kWCells);
    for (std::uint32_t i = 0; i < used; i++) {
        const std::uint32_t c = rd32(cpu, cells + 4 * i);
        if (!mapped(cpu, c, kCAlone + 1)) {
            break;
        }
        const bool alone = rd8(cpu, c + kCAlone) != 0;
        GridCell g;
        g.x = rdi(cpu, c + kCRect) + (alone ? 0 : rdi(cpu, c + kCOffX));
        g.y = rdi(cpu, c + kCRect + 4) + (alone ? 0 : rdi(cpu, c + kCOffY));
        g.w = rdi(cpu, c + kCRect + 8);
        g.h = rdi(cpu, c + kCRect + 12);
        if (g.w <= 0 || g.h <= 0 || g.w > 480 || g.h > 320) {
            break;
        }
        out.push_back(g);
    }
    return out;
}

int on_grid_key(mt_cpu *cpu, const mt_hook_call *call) {
    const std::uint32_t mgr = cpu->r[0];
    const auto key = static_cast<std::int32_t>(cpu->r[1]);
    const std::int32_t tab = call->fn == MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY ? 1 : 2;
    bool moved = false;
    if (!grid_key(cpu, mgr, tab, key, &moved)) {
        return MT_HOOK_PASS;
    }
    if (moved) { // as the original after a move
        const std::uint32_t args[1] = {mgr};
        mt_call_guest(cpu, kResetSubTimers, args, 1, call->fn);
    }
    return MT_HOOK_HANDLED;
}

} // namespace

int grid_step(const std::vector<GridCell> &cells, int from, std::int32_t key) {
    const int n = static_cast<int>(cells.size());
    if (n == 0) {
        return -1;
    }
    if (from < 0 || from >= n) {
        return std::clamp(from, 0, n - 1); // off the cells in use: the nearest end
    }
    if (key == kKeyLeft) {
        return from > 0 ? from - 1 : -1;
    }
    if (key == kKeyRight) {
        return from + 1 < n ? from + 1 : -1;
    }
    if (key != kKeyUp && key != kKeyDown) {
        return -1;
    }
    const GridCell &f = cells[static_cast<std::size_t>(from)];
    const int sign = key == kKeyDown ? 1 : -1;
    int best = -1;
    std::int32_t best_dy = 0, best_dx = 0;
    for (int i = 0; i < n; i++) {
        const GridCell &c = cells[static_cast<std::size_t>(i)];
        const std::int32_t dy = (c.y - f.y) * sign;
        if (i == from || dy < std::max(1, f.h / 2)) {
            continue; // not in a row that way
        }
        const std::int32_t dx = std::abs((c.x + c.w / 2) - (f.x + f.w / 2));
        if (best < 0 || dy < best_dy || (dy == best_dy && dx < best_dx)) {
            best = i;
            best_dy = dy;
            best_dx = dx;
        }
    }
    return best;
}

std::int32_t grid_follow(std::int32_t y, std::int32_t h, std::int32_t top, std::int32_t bottom,
                         std::int32_t lo, std::int32_t drag) {
    std::int32_t want = drag;
    if (y + want + h > bottom) {
        want = bottom - h - y;
    }
    if (y + want < top) {
        want = top - y;
    }
    return std::clamp(want, std::min(lo, 0), 0);
}

namespace {

// The grid of tab `tab` (1 items, 2 equipment) as the keys see it, if they may move on it: not
// with the shortcut panel or the quick slot choice open, the widget shown with cells in use.
// The cursor is folded into a cell index (the handlers' cursor plus 6 x the key scroll's first
// row, which is then kept at 0) and written back.
struct GridNow {
    std::uint32_t widget = 0, cursor_at = 0;
    std::vector<GridCell> cells;
    int from = 0;
};
bool grid_now(mt_cpu *cpu, std::uint32_t mgr, std::int32_t tab, GridNow *g) {
    if ((tab != 1 && tab != 2) || !mapped(cpu, mgr, kMgrKeyScroll + 4) ||
        rd8(cpu, mgr + kMgrShortcuts) != 0 || rd8(cpu, mgr + kMgrAssign) != 0) {
        return false;
    }
    g->widget = rd32(cpu, rd32(cpu, tab == 1 ? kItemGrid : kEquipGrid));
    if (!mapped(cpu, g->widget, kWShown + 1) || rd8(cpu, g->widget + kWShown) == 0) {
        return false;
    }
    g->cells = grid_cells(cpu, g->widget);
    if (g->cells.empty()) {
        return false;
    }
    g->cursor_at = mgr + (tab == 1 ? kItemCursor : kEquipCursor);
    const std::uint32_t scroll = rd32(cpu, mgr + kMgrKeyScroll);
    const std::int32_t first = mapped(cpu, scroll, 8) ? rdi(cpu, scroll + kScrollFirst) : 0;
    g->from = rdi(cpu, g->cursor_at) + 6 * first;
    if (mapped(cpu, scroll, 8)) {
        wr32(cpu, scroll + kScrollFirst, 0);
    }
    wr32(cpu, g->cursor_at, static_cast<std::uint32_t>(g->from));
    return true;
}

// The cursor on cell `to` (from g.from), the drag at `want`.
void grid_set(mt_cpu *cpu, const GridNow &g, int to, std::int32_t want, bool *moved) {
    if (to != g.from) {
        wr32(cpu, g.cursor_at, static_cast<std::uint32_t>(to));
        if (moved != nullptr) {
            *moved = true;
        }
    }
    if (want != rdi(cpu, g.widget + kWDrag)) {
        wr32(cpu, g.widget + kWDrag, static_cast<std::uint32_t>(want));
        wr32(cpu, g.widget + kWFling, 0);
        g_grid_scrolls++;
    }
}

} // namespace

bool grid_key(mt_cpu *cpu, std::uint32_t mgr, std::int32_t tab, std::int32_t key, bool *moved) {
    if (moved != nullptr) {
        *moved = false;
    }
    GridNow g;
    if ((key != kKeyUp && key != kKeyDown && key != kKeyLeft && key != kKeyRight) ||
        !grid_now(cpu, mgr, tab, &g)) {
        return false;
    }
    int to = grid_step(g.cells, g.from, key);
    if (to < 0 && key == kKeyUp) {
        return false; // the top row: the original goes to the tab bar
    }
    if (to < 0) {
        to = std::clamp(g.from, 0, static_cast<int>(g.cells.size()) - 1); // an end: stay
    }
    const GridCell &c = g.cells[static_cast<std::size_t>(to)];
    const std::int32_t top = rdi(cpu, g.widget + kWRectY);
    const std::int32_t bottom = top + rdi(cpu, g.widget + kWRectH);
    const std::int32_t lo = rdi(cpu, g.widget + kWArea) - rdi(cpu, g.widget + kWContent);
    const std::int32_t drag = rdi(cpu, g.widget + kWDrag);
    grid_set(cpu, g, to, grid_follow(c.y, c.h, top, bottom, lo, drag), moved);
    g_grid_keys++;
    return true;
}

int grid_rows_in_view(const std::vector<GridCell> &cells, std::int32_t view_h) {
    if (cells.empty()) {
        return 1;
    }
    std::vector<std::int32_t> tops;
    std::int32_t h = 0;
    for (const GridCell &c : cells) {
        tops.push_back(c.y);
        h = std::max(h, c.h);
    }
    std::sort(tops.begin(), tops.end());
    std::int32_t pitch = 0;
    for (std::size_t i = 1; i < tops.size(); i++) {
        const std::int32_t gap = tops[i] - tops[i - 1];
        if (gap > 0 && (pitch == 0 || gap < pitch)) {
            pitch = gap;
        }
    }
    if (pitch <= 0) {
        pitch = std::max(h, 1);
    }
    return std::max(1, static_cast<int>((view_h - h) / pitch) + 1);
}

int grid_page(const std::vector<GridCell> &cells, int from, int dir, int rows) {
    const int n = static_cast<int>(cells.size());
    if (n == 0) {
        return -1;
    }
    if (from < 0 || from >= n) {
        return std::clamp(from, 0, n - 1);
    }
    int at = from;
    for (int i = 0; i < std::max(rows, 1); i++) {
        const int next = grid_step(cells, at, dir < 0 ? kKeyUp : kKeyDown);
        if (next < 0) {
            break;
        }
        at = next;
    }
    return at;
}

std::int32_t grid_page_drag(const GridCell &from, const GridCell &to, std::int32_t top,
                            std::int32_t bottom, std::int32_t lo, std::int32_t drag) {
    const std::int32_t paged = std::clamp(drag - (to.y - from.y), std::min(lo, 0), 0);
    return grid_follow(to.y, to.h, top, bottom, lo, paged);
}

bool grid_page_key(mt_cpu *cpu, std::uint32_t mgr, std::int32_t tab, int dir, bool *moved) {
    if (moved != nullptr) {
        *moved = false;
    }
    GridNow g;
    if (!grid_now(cpu, mgr, tab, &g)) {
        return false;
    }
    const std::int32_t top = rdi(cpu, g.widget + kWRectY);
    const std::int32_t bottom = top + rdi(cpu, g.widget + kWRectH);
    const std::int32_t lo = rdi(cpu, g.widget + kWArea) - rdi(cpu, g.widget + kWContent);
    const std::int32_t drag = rdi(cpu, g.widget + kWDrag);
    const int from = std::clamp(g.from, 0, static_cast<int>(g.cells.size()) - 1);
    const int to = grid_page(g.cells, from, dir, grid_rows_in_view(g.cells, bottom - top));
    const GridCell &a = g.cells[static_cast<std::size_t>(from)];
    const GridCell &b = g.cells[static_cast<std::size_t>(to)];
    grid_set(cpu, g, to, grid_page_drag(a, b, top, bottom, lo, drag), moved);
    return true;
}

void install_grid_keys(bool on) {
    mt_hook_register(MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY, on_grid_key, nullptr, on ? 1 : 0);
    mt_hook_register(MT_HOOK_FN_POPUP_MENU_EQUIP_TAB_ON_KEY, on_grid_key, nullptr, on ? 1 : 0);
}

void uninstall_grid_keys() {
    mt_hook_unregister(MT_HOOK_FN_POPUP_MENU_ITEM_TAB_ON_KEY);
    mt_hook_unregister(MT_HOOK_FN_POPUP_MENU_EQUIP_TAB_ON_KEY);
}

GridStats grid_stats() { return GridStats{g_grid_keys.load(), g_grid_scrolls.load()}; }

// --- paging from the frontend (ADR 0114) ------------------------------------------------------

namespace {

constexpr std::uint32_t kGameMode = 0x11AA00;      // 1 the popup menu, 2 the shop
constexpr std::uint32_t kShopGlobal = 0xF1454;     // -> -> the shop object
constexpr std::uint32_t kCanvasGlobal = 0x11A950;  // SkyCanvas
constexpr std::uint32_t kCanvasState = 0x0C;       // its current state (CStateGame)
constexpr std::uint32_t kDataMgrGlobal = 0x11A984; // CDataMgr::GetInstance 0x5BAC1
constexpr std::uint32_t kMgrOpenTab = 0xA8;        // the open tab (1 items, 2 equipment)

std::atomic<std::uint64_t> g_pages{0}, g_pages_moved{0};

// A guest pointer slot that the image maps, or 0.
std::uint32_t global(const mt_cpu *cpu, std::uint32_t addr) {
    return mapped(cpu, addr, 4) ? rd32(cpu, addr) : 0;
}

} // namespace

bool list_page(mt_cpu *cpu, std::int32_t panel, int dir, bool apply) {
    if (!mapped(cpu, kGameMode, 4) || !mapped(cpu, kPopupId, 4)) {
        return false;
    }
    const std::int32_t mode = rdi(cpu, kGameMode);
    if (panel == 6 && mode == 2 && rdi(cpu, kPopupId) == -1) {
        const std::uint32_t shop = rd32(cpu, global(cpu, kShopGlobal));
        const std::int32_t tab = rdi(cpu, shop + kShopTab);
        if (!mapped(cpu, shop, kShopScrolls + 4) || (tab != 0 && tab != 1)) {
            return false;
        }
        if (!apply) {
            return true;
        }
        const std::uint32_t game = rd32(cpu, global(cpu, kCanvasGlobal) + kCanvasState);
        const std::int32_t before =
            rdi(cpu, shop + kShopCursor + 4 * static_cast<std::uint32_t>(tab));
        if (!shop_list_page_key(cpu, shop, dir, game, market_view(cpu))) {
            return false;
        }
        g_pages++;
        if (rdi(cpu, shop + kShopCursor + 4 * static_cast<std::uint32_t>(tab)) != before) {
            g_pages_moved++;
            const std::uint32_t args[1] = {shop};
            mt_call_guest(cpu, kResetSubTimers, args, 1, kResetSubTimers);
        }
        return true;
    }
    if (panel == 1 && mode == 1) {
        const std::uint32_t mgr = global(cpu, kDataMgrGlobal);
        if (!mapped(cpu, mgr, kMgrKeyScroll + 4)) {
            return false;
        }
        const std::int32_t tab = rdi(cpu, mgr + kMgrOpenTab);
        if (!apply) {
            GridNow g; // the checks only (folding the cursor changes nothing the handlers read)
            return grid_now(cpu, mgr, tab, &g);
        }
        bool moved = false;
        if (!grid_page_key(cpu, mgr, tab, dir, &moved)) {
            return false;
        }
        g_pages++;
        if (moved) {
            g_pages_moved++;
            const std::uint32_t args[1] = {mgr};
            mt_call_guest(cpu, kResetSubTimers, args, 1, kResetSubTimers);
        }
        return true;
    }
    return false;
}

PageStats page_stats() { return PageStats{g_pages.load(), g_pages_moved.load()}; }

} // namespace maplethief::frontend
