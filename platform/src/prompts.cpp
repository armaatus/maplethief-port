// Button prompts and the focus markers the engine does not draw (#146); see prompts.h.
#include "prompts.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "maplethief/hooks.h"
#include "maplethief/input.h"

namespace maplethief::frontend {

namespace {

// A button that gets a prompt: its target's class and selector, the action whose control the
// glyph shows, the side it goes on, and where the button's art lies inside its touch rect (points
// from the rect's left and right edges; the glyph keeps 2 points from the art). The art is
// narrower than the touch rect for most of them (docs/controls/keypad.md: the popups' buttons
// are placed 5 or 20 points left of their art). A button that answers "no" sits right of its
// "yes", so its glyph goes on its right. `inside`: in the rect's bottom-right corner (a control
// over a whole text box or the screen's width). `popup`: also while a popup is up (panel 7);
// without, a control under the popup (the character select's New) gets none.
struct Spot {
    const char *target;
    const char *action;
    std::uint32_t game_action;
    bool right = false;
    int art_left = 0, art_right = 0;
    bool inside = false;
    bool popup = true;
};

constexpr Spot kPromptSpots[] = {
    // Popups (panel 7, PopupKeyPadView) and the death popup (0x68).
    {"PopupKeyPadView", "releaseOkButton:", MT_ACTION_OK, false, 20, 30},
    {"PopupKeyPadView", "releaseOk2Button:", MT_ACTION_OK, false, 18, 8},
    {"PopupKeyPadView", "releaseYesButton:", MT_ACTION_OK, false, 18, 8},
    {"PopupKeyPadView", "releaseCancelButton:", MT_ACTION_BACK, true, 3, 22},
    {"PopupKeyPadView", "releaseNoButton:", MT_ACTION_BACK, true, 3, 22},
    {"PopupKeyPadView", "releaseEmptyButton:", MT_ACTION_OK, false, 0, 0, true},
    {"PopupKeyPadView", "releaseMinusButton:", MT_ACTION_LEFT, false, 8, 13},
    {"PopupKeyPadView", "pressMinusButton:", MT_ACTION_LEFT, false, 8, 13},
    {"PopupKeyPadView", "releasePlusButton:", MT_ACTION_RIGHT, true, 8, 13},
    {"PopupKeyPadView", "pressPlusButton:", MT_ACTION_RIGHT, true, 8, 13},
    {"PopupKeyPadView", "releaseGradeRevivalButton:", MT_ACTION_GRADE_REVIVE, false, 20, 20},
    {"PopupKeyPadView", "releaseRevivalButton:", MT_ACTION_REVIVE, false, 20, 20},
    {"PopupKeyPadView", "releaseReturnTownButton:", MT_ACTION_OK, false, 20, 20},
    {"PopupKeyPadView", "releaseGoMainMenuButton:", MT_ACTION_BACK, false, 20, 20},
    // The character select's "create and play a new game?" (popup 19 over panel -1's view).
    {"ChooseCharacterView", "okNewAlert:", MT_ACTION_OK, false, 20, 20},
    {"ChooseCharacterView", "cancelNewAlert:", MT_ACTION_BACK, true, 4, 21},
    // The NPC chat (panel 5).
    {"NpcKeyPadView", "releaseAcceptButton:", MT_ACTION_OK, false, 20, 12},
    {"NpcKeyPadView", "releaseRefuseButton:", MT_ACTION_BACK, true, 0, 30},
    {"NpcKeyPadView", "releaseOkeyButton:", MT_ACTION_OK, false, 20, 20},
    // The shop (panel 6): Buy / Sell, the tabs (minus / plus), Charge ('#': RT, the keyboard's
    // N, #127).
    {"MarketKeyPadView", "releaseBuyButton:", MT_ACTION_OK, false, 20, 20},
    {"MarketKeyPadView", "releaseSellButton:", MT_ACTION_OK, false, 19, 21},
    {"MarketKeyPadView", "releaseBuyTabButton:", MT_ACTION_MINUS, false, 0, 0},
    {"MarketKeyPadView", "releaseSellTabButton:", MT_ACTION_PLUS, true, 0, 0},
    {"MarketKeyPadView", "releaseChargeButton:", MT_ACTION_SLOT4, false, 4, 4},
    // TouchKeyPadView's buttons on the menu panels: the close buttons (panels 6, 8, 9, 10), the
    // map's World ('*') and Mini (-5), the cut scenes' strip (panel 3, -5).
    {"TouchKeyPadView", "releaseClearButton:", MT_ACTION_BACK, true, 4, 10},
    {"TouchKeyPadView", "releaseAsteriskButton:", MT_ACTION_MAP, false, 0, 0},
    {"TouchKeyPadView", "releaseOkButton:", MT_ACTION_OK, false, 0, 0},
    // The character select: New ('#', #127).
    {"ChooseCharacterView", "clickNew:", MT_ACTION_SLOT4, false, 0, 0, false, false},
};

const Spot *spot_of(const std::string &target, const std::string &action) {
    for (const Spot &s : kPromptSpots) {
        if (target == s.target && action == s.action) {
            return &s;
        }
    }
    return nullptr;
}

// CDataMgr's instance (focus.cpp: CDataMgr::GetInstance 0x5BAC1 keeps it here), its tab, its
// open tab, the open tab's page (+0xAC: 0 its list; the quest tab's quest, the option tab's
// pages), the keys' scroll (+0x10C: an object whose +4 is the list's first row) and the list
// cursors (docs/controls/keypad.md, "The list cursors"). The list widgets' globals (Screen_Init
// 0x9909's touch widgets: each points at the variable that holds the widget).
constexpr std::uint32_t kDataMgrGlobal = 0x11A984;
constexpr std::uint32_t kMenuOpenTab = 0xA8;
constexpr std::uint32_t kMenuPage = 0xAC;
constexpr std::uint32_t kMenuTab = 0xB0;
constexpr std::uint32_t kMenuScroll = 0x10C;
constexpr std::uint32_t kGameMode = 0x11AA00; // 1: the popup menu
// A tab's list cursor: CDataMgr +`cursor` is a row of list widget `widget` (plus the keys'
// scroll if `scrolled`), each row `per_row` cells from cell `first` (the frame spans them).
// `shown`: the list is drawn only once a touch showed it (the widget's +0xB1, focus.h's
// popup_menu_list_hidden). `page`: only on the tab's list (+0xAC 0). `main`: a CDataMgr byte
// that, set, puts the cursor on cell 0, drawn shown or not (the quest tab's main quest).
struct ListCursor {
    std::int32_t open; // the open tab
    std::uint32_t cursor;
    std::uint32_t widget;
    std::uint32_t per_row = 1;
    std::uint32_t first = 0;
    bool scrolled = false;
    bool shown = false;
    bool page = false;
    std::uint32_t main = 0;
};
constexpr ListCursor kLists[] = {
    // Items (CDataMgr::keyPopupItemInven 0x5285D) and equipment (PopupMenu_EquipTabOnKey
    // 0x519F9): an index into the grid.
    {1, 0xD8, 0xF14B8},
    {2, 0xDC, 0xF14C0},
    // Skills (PopupMenu_SkillTabOnKey 0x54051): the visible row; each row a skill and its +.
    {3, 0xD4, 0xF14A8, 2, 0, true, true},
    // Quests (PopupMenu_QuestTabOnKey 0x5462D): +0xB4 set, the main quest (cell 0); else the
    // visible row of the side quests (cells 1-).
    {4, 0xE4, 0xF14B4, 1, 1, true, true, true, 0xB4},
    // Options (Game_ExitToMainMenu 0x54BF9, the option tab's keys): Save, Option, Help, Main
    // menu.
    {5, 0xE0, 0xF14A4, 1, 0, false, false, true},
};
constexpr std::int32_t kMenuPanel = 1;
constexpr std::int32_t kMenuMode = 1; // the game mode while the popup menu is up
constexpr std::int32_t kPopupPanel = 7;

std::uint32_t read32(const mt_cpu *cpu, std::uint32_t addr) {
    std::uint32_t v = 0;
    if (cpu != nullptr && cpu->mem != nullptr && addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}
std::int32_t read_i32(const mt_cpu *cpu, std::uint32_t addr) {
    return static_cast<std::int32_t>(read32(cpu, addr));
}

bool runtime_menu(void *, const mt_cpu *cpu, PopupMenu *out) { return read_popup_menu(cpu, out); }

std::uint8_t read8(const mt_cpu *cpu, std::uint32_t addr) {
    return cpu != nullptr && cpu->mem != nullptr && addr != 0 && addr < cpu->mem_size
               ? cpu->mem[addr]
               : 0;
}

// The cells `first` .. `first + n - 1` of list widget `widget_var` (the address of the variable
// that holds it) as the engine draws them (0xA0370: a cell's rect at +0x20, plus the list's
// offset at +0x08 unless its +0x5C says it is placed alone; the widget's drag at +0x6C moves
// them down), their union clipped to the widget's rect; false when there is none or it is
// scrolled out. `used`: only cells below the widget's count in use (+0x08), not its capacity
// (+0x04).
bool list_cells(const mt_cpu *cpu, std::uint32_t widget_var, std::int32_t first, std::uint32_t n,
                bool used, Rect *out) {
    const std::uint32_t widget = read32(cpu, read32(cpu, widget_var));
    const std::uint32_t count = std::min(read32(cpu, widget + (used ? 8 : 4)), 100u);
    if (widget == 0 || first < 0 || n == 0 || static_cast<std::uint32_t>(first) + n > count) {
        return false;
    }
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (std::uint32_t k = 0; k < n; k++) {
        const std::uint32_t i = static_cast<std::uint32_t>(first) + k;
        const std::uint32_t cell = read32(cpu, read32(cpu, widget) + 4 * i);
        if (cell == 0) {
            return false;
        }
        const std::int32_t w = read_i32(cpu, cell + 0x28), h = read_i32(cpu, cell + 0x2C);
        if (w <= 0 || h <= 0 || w > kScreenWidth || h > kScreenHeight) {
            return false; // an empty cell
        }
        const bool alone = read8(cpu, cell + 0x5C) != 0;
        const std::int32_t x =
            read_i32(cpu, cell + 0x20) + (alone ? 0 : read_i32(cpu, cell + 0x08));
        const std::int32_t y = read_i32(cpu, cell + 0x24) +
                               (alone ? 0 : read_i32(cpu, cell + 0x0C)) +
                               read_i32(cpu, widget + 0x6C);
        x0 = k == 0 ? x : std::min(x0, x);
        y0 = k == 0 ? y : std::min(y0, y);
        x1 = k == 0 ? x + w : std::max(x1, x + w);
        y1 = k == 0 ? y + h : std::max(y1, y + h);
    }
    const std::int32_t cx0 = read_i32(cpu, widget + 0x1C), cy0 = read_i32(cpu, widget + 0x20);
    const std::int32_t cx1 = cx0 + read_i32(cpu, widget + 0x24);
    const std::int32_t cy1 = cy0 + read_i32(cpu, widget + 0x28);
    x0 = std::max(x0, cx0), y0 = std::max(y0, cy0);
    x1 = std::min(x1, cx1), y1 = std::min(y1, cy1);
    if (x1 - x0 < 4 || y1 - y0 < 4) {
        return false;
    }
    *out = Rect{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0),
                static_cast<float>(y1 - y0)};
    return true;
}

// The glyph's dark backing, one point around it.
constexpr Rgba kBacking = {0x10, 0x12, 0x16, 0xB0};

bool same_bindings(const std::vector<Binding> &a, const std::vector<Binding> &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const Binding &x, const Binding &y) {
               return x.control == y.control && x.field == y.field && x.menu == y.menu;
           });
}

} // namespace

const char *prompt_device_name(PromptDevice device) {
    switch (device) {
    case PromptDevice::kKeyboard:
        return "keyboard";
    case PromptDevice::kGamepad:
        return "gamepad";
    default:
        return "none";
    }
}

void DeviceTracker::use(PromptDevice d) {
    if (d != device_) {
        device_ = d;
        changes_++;
        SDL_Log("input: button prompts: %s", prompt_device_name(d));
    }
}

void DeviceTracker::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat) {
            use(PromptDevice::kKeyboard);
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (std::find(pads_.begin(), pads_.end(), e.gdevice.which) == pads_.end()) {
            pads_.push_back(e.gdevice.which);
        }
        use(PromptDevice::kGamepad);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pads_.erase(std::remove(pads_.begin(), pads_.end(), e.gdevice.which), pads_.end());
        if (pads_.empty() && device_ == PromptDevice::kGamepad) {
            use(PromptDevice::kNone);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        use(PromptDevice::kGamepad);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<int>(e.gaxis.value)) > 16384) {
            use(PromptDevice::kGamepad);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_FINGER_DOWN:
        use(PromptDevice::kNone);
        break;
    default:
        break;
    }
}

bool prompt_control(const std::vector<Binding> &bindings, std::uint32_t action, PromptDevice device,
                    Control *out) {
    if (device == PromptDevice::kNone || action == MT_ACTION_NONE) {
        return false;
    }
    const bool keys = device == PromptDevice::kKeyboard;
    const SDL_Keycode preferred = action == MT_ACTION_OK     ? SDLK_RETURN
                                  : action == MT_ACTION_BACK ? SDLK_ESCAPE
                                  : action == MT_ACTION_MAP  ? SDLK_M
                                                             : SDLK_UNKNOWN;
    const Binding *first = nullptr;
    for (const Binding &b : bindings) {
        if (b.menu != action || (b.control.kind == ControlKind::kKey) != keys) {
            continue;
        }
        if (keys && preferred != SDLK_UNKNOWN && b.control.code == static_cast<int>(preferred)) {
            *out = b.control;
            return true;
        }
        if (first == nullptr) {
            first = &b;
        }
    }
    if (first == nullptr) {
        return false;
    }
    *out = first->control;
    return true;
}

std::uint32_t prompt_action(const std::string &target, const std::string &action) {
    const Spot *s = spot_of(target, action);
    return s != nullptr ? s->game_action : static_cast<std::uint32_t>(MT_ACTION_NONE);
}

std::vector<Prompt> prompts_for(const std::vector<FocusTarget> &targets, std::int32_t panel,
                                bool focus_active) {
    std::vector<Prompt> out;
    if (panel == 0) {
        return out;
    }
    const auto at = [](float v) { return static_cast<int>(std::lround(v)); };
    bool back = false;
    for (const FocusTarget &t : targets) {
        const Spot *s = t.focusable() ? spot_of(t.target, t.action) : nullptr;
        if (s == nullptr || (panel == kPopupPanel && !s->popup)) {
            continue;
        }
        back = back || s->game_action == MT_ACTION_BACK;
        Prompt p{s->game_action, at(t.x + static_cast<float>(s->art_left) - 2),
                 at(t.x + t.w - static_cast<float>(s->art_right) + 2), at(t.cy()), s->right};
        if (s->inside || t.w >= 400) {
            // Over a whole text box or the screen's width: in its bottom-right corner.
            p = Prompt{s->game_action, at(t.x + t.w - 6), at(t.x + t.w - 6),
                       at(t.y + t.h - 6 - static_cast<float>(kButtonHeight) / 2), false};
        }
        out.push_back(p);
    }
    if (focus_active && !back) {
        const int b = focus_back_target(targets);
        if (b >= 0) {
            const FocusTarget &t = targets[static_cast<std::size_t>(b)];
            out.push_back(
                Prompt{MT_ACTION_BACK, at(t.x - 2), at(t.x + t.w + 2), at(t.cy()), false});
        }
    }
    return out;
}

bool read_popup_menu(const mt_cpu *cpu, PopupMenu *out) {
    const std::uint32_t mgr = read32(cpu, kDataMgrGlobal);
    if (mgr == 0) {
        return false;
    }
    PopupMenu m;
    m.tab = read_i32(cpu, mgr + kMenuTab);
    m.open = read_i32(cpu, mgr + kMenuOpenTab);
    m.mode = read_i32(cpu, kGameMode);
    for (const ListCursor &l : kLists) {
        if (m.open != l.open || (l.page && read_i32(cpu, mgr + kMenuPage) != 0)) {
            continue;
        }
        // The grids (items, equipment) count their capacity, the other lists their cells in use.
        const bool used = l.open > 2;
        if (l.main != 0 && read8(cpu, mgr + l.main) != 0) {
            m.cursor = list_cells(cpu, l.widget, 0, 1, used, &m.cell);
            continue;
        }
        const std::uint32_t widget = read32(cpu, read32(cpu, l.widget));
        if (l.shown && (widget == 0 || read8(cpu, widget + 0xB1) == 0)) {
            continue; // not drawn
        }
        std::int32_t row = read_i32(cpu, mgr + l.cursor);
        if (const std::uint32_t scroll = read32(cpu, mgr + kMenuScroll);
            l.scrolled && scroll != 0) {
            row += read_i32(cpu, scroll + 4);
        }
        m.cursor = row >= 0 && list_cells(cpu, l.widget,
                                          static_cast<std::int32_t>(l.first) +
                                              row * static_cast<std::int32_t>(l.per_row),
                                          l.per_row, used, &m.cell);
    }
    *out = m;
    return true;
}

namespace {

void write32(mt_cpu *cpu, std::uint32_t addr, std::uint32_t v) {
    if (cpu != nullptr && cpu->mem != nullptr && addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(cpu->mem + addr, &v, 4);
    }
}

// Cell `i` of list widget `widget` (the widget itself) as 0xA0370 places it, before the drag:
// its y and height. False past the cells in use or for an empty cell.
bool cell_row(const mt_cpu *cpu, std::uint32_t widget, std::int32_t i, std::int32_t *y,
              std::int32_t *h) {
    if (i < 0 || static_cast<std::uint32_t>(i) >= std::min(read32(cpu, widget + 8), 100u)) {
        return false;
    }
    const std::uint32_t cell = read32(cpu, read32(cpu, widget) + 4 * static_cast<std::uint32_t>(i));
    *h = read_i32(cpu, cell + 0x2C);
    if (cell == 0 || *h <= 0 || *h > kScreenHeight) {
        return false;
    }
    *y = read_i32(cpu, cell + 0x24) +
         (read8(cpu, cell + 0x5C) != 0 ? 0 : read_i32(cpu, cell + 0x0C));
    return true;
}

std::atomic<std::uint64_t> g_scroll_keys{0}, g_scroll_moved{0};

// PopupMenu_SkillTabOnKey / PopupMenu_QuestTabOnKey (data manager, key, game): the original,
// then the drawn list follows the keys.
int on_tab_key(mt_cpu *cpu, const mt_hook_call *call) {
    const std::uint32_t mgr = cpu->r[0];
    const std::uint32_t scroll = read32(cpu, mgr + kMenuScroll);
    const std::int32_t before = scroll != 0 ? read_i32(cpu, scroll + 4) : 0;
    call->original(cpu);
    g_scroll_keys++;
    if (popup_menu_follow_keys(cpu, mgr, before)) {
        g_scroll_moved++;
    }
    return MT_HOOK_HANDLED;
}

} // namespace

bool popup_menu_follow_keys(mt_cpu *cpu, std::uint32_t mgr, std::int32_t first_before) {
    if (mgr == 0 || read_i32(cpu, mgr + kMenuPage) != 0) {
        return false; // a quest's page: no list
    }
    const std::int32_t open = read_i32(cpu, mgr + kMenuOpenTab);
    for (const ListCursor &l : kLists) {
        if (l.open != open || !l.scrolled || (l.main != 0 && read8(cpu, mgr + l.main) != 0)) {
            continue; // not this tab's, or on the main quest (placed alone, not scrolled)
        }
        const std::uint32_t widget = read32(cpu, read32(cpu, l.widget));
        const std::uint32_t scroll = read32(cpu, mgr + kMenuScroll);
        if (widget == 0 || read8(cpu, widget + 0xB1) == 0 || scroll == 0) {
            return false; // not drawn
        }
        const auto per = static_cast<std::int32_t>(l.per_row);
        const auto base = static_cast<std::int32_t>(l.first);
        const std::int32_t first = read_i32(cpu, scroll + 4);
        std::int32_t y0 = 0, yf = 0, h = 0;
        if (!cell_row(cpu, widget, base, &y0, &h) ||
            !cell_row(cpu, widget, base + first * per, &yf, &h)) {
            return false;
        }
        // The cursor's row, and the list's rect (+0x20 y, +0x28 height).
        std::int32_t yc = 0, hc = 0;
        if (!cell_row(cpu, widget, base + (read_i32(cpu, mgr + l.cursor) + first) * per, &yc,
                      &hc)) {
            return false;
        }
        const std::int32_t top = read_i32(cpu, widget + 0x20);
        const std::int32_t bottom = top + read_i32(cpu, widget + 0x28);
        const std::int32_t drag = read_i32(cpu, widget + 0x6C);
        const bool seen = yc + drag >= top && yc + drag + hc <= bottom;
        if (first == first_before && seen) {
            return false;
        }
        // The keys' view, moved on as far as the cursor's row needs to be wholly inside (the
        // last of the 5 rows the keys show reaches past the rect), within the engine's clamp.
        std::int32_t want = y0 - yf;
        if (yc + want + hc > bottom) {
            want = bottom - hc - yc;
        }
        if (yc + want < top) {
            want = top - yc;
        }
        const std::int32_t lo = std::min(0, read_i32(cpu, widget + 0x48) -
                                                read_i32(cpu, widget + 0x38)); // the engine's clamp
        want = std::clamp(want, lo, 0);
        if (drag == want) {
            return false;
        }
        write32(cpu, widget + 0x6C, static_cast<std::uint32_t>(want));
        write32(cpu, widget + 0x74, 0); // no fling goes on
        return true;
    }
    return false;
}

void install_popup_menu_scroll() {
    mt_hook_register(MT_HOOK_FN_SKILL_TAB_ON_KEY, on_tab_key, nullptr, 1);
    mt_hook_register(MT_HOOK_FN_QUEST_TAB_ON_KEY, on_tab_key, nullptr, 1);
}

void uninstall_popup_menu_scroll() {
    mt_hook_unregister(MT_HOOK_FN_SKILL_TAB_ON_KEY);
    mt_hook_unregister(MT_HOOK_FN_QUEST_TAB_ON_KEY);
}

PopupMenuScrollStats popup_menu_scroll_stats() {
    return PopupMenuScrollStats{g_scroll_keys.load(), g_scroll_moved.load()};
}

std::vector<Prompt> popup_menu_prompts(const PopupMenu &menu) {
    // Its close (a GL widget, the X at (398, 28)): back, right of it over the field. With no
    // tab open, the tab keys cycle the six tabs (Menu_OnKey 0x53B5D): minus left of the tab bar
    // (75-399, y 260-295), plus right of it.
    std::vector<Prompt> out = {Prompt{MT_ACTION_BACK, 384, 412, 28, true}};
    if (menu.open == 0) {
        out.push_back(Prompt{MT_ACTION_MINUS, 73, 73, 271, false});
        out.push_back(Prompt{MT_ACTION_PLUS, 403, 403, 271, true});
    }
    return out;
}

const FocusTarget *focus_highlight_target(const Focus &focus, PromptDevice device) {
    if (const FocusTarget *f = focus.highlighted()) {
        return f;
    }
    return focus.active() && device != PromptDevice::kNone ? focus.focused() : nullptr;
}

void HintTracker::set_menu_source(PopupMenuSource source, void *user) {
    menu_ = source;
    menu_user_ = user;
}

void HintTracker::turn(const mt_cpu *cpu, const Focus &focus, std::int32_t panel, bool prompts,
                       bool markers) {
    Hints h;
    if (device_.device() != PromptDevice::kNone) {
        if (prompts) {
            h.prompts = prompts_for(focus.targets(), panel, focus.active());
        }
        PopupMenu m;
        if ((panel == kMenuPanel || (panel == -1 && !focus.active())) &&
            (menu_ != nullptr ? menu_ : runtime_menu)(menu_user_, cpu, &m) &&
            (panel == kMenuPanel || m.mode == kMenuMode)) {
            if (prompts) {
                const std::vector<Prompt> own = popup_menu_prompts(m);
                h.prompts.insert(h.prompts.end(), own.begin(), own.end());
            }
            if (markers && m.cursor) {
                h.markers.push_back(m.cell);
            }
        }
    }
    hints_ = std::move(h);
}

int prompt_left(const Prompt &p, int w, int screen_w) {
    // One point of backing around the glyph stays on the screen.
    const bool right_fits = p.after + w + 1 <= screen_w;
    const bool left_fits = p.before - w - 1 >= 0;
    const bool right = p.right ? right_fits || !left_fits : !left_fits && right_fits;
    return std::clamp(right ? p.after : p.before - w, 1, std::max(1, screen_w - w - 1));
}

int draw_prompts(Canvas &canvas, const std::vector<Prompt> &prompts, const PromptStyle &style,
                 const std::vector<Binding> &bindings) {
    int drawn = 0;
    for (const Prompt &p : prompts) {
        Control c;
        if (!prompt_control(bindings, p.action, style.device, &c)) {
            continue;
        }
        const int w = glyph_width(c, style.type, style.face);
        const int x = prompt_left(p, w, canvas.width());
        const int y = std::clamp(p.y - kButtonHeight / 2, 1, canvas.height() - kButtonHeight - 1);
        canvas.fill(x - 1, y - 1, w + 2, kButtonHeight + 2, kBacking);
        draw_glyph(canvas, x, y, c, style.type, style.face);
        drawn++;
    }
    return drawn;
}

void draw_marker(Canvas &canvas, const Rect &r) {
    // Presenter::draw_highlight at one point per pixel: a dark line outside a yellow one, two
    // points out from the rect.
    const int x = static_cast<int>(std::floor(r.x)) - 2, y = static_cast<int>(std::floor(r.y)) - 2;
    const int w = static_cast<int>(std::ceil(r.x + r.w)) + 2 - x;
    const int h = static_cast<int>(std::ceil(r.y + r.h)) + 2 - y;
    canvas.frame(x, y, w, h, 1, Rgba{0x20, 0x18, 0x00, 255});
    canvas.frame(x + 1, y + 1, w - 2, h - 2, 1, Rgba{0xFF, 0xD4, 0x00, 255});
}

void HintLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    scale_ = 0;
    drawn_.clear();
}

bool HintLayer::draw(SDL_Renderer *renderer, const Placement &placement,
                     const std::vector<Prompt> &prompts, const PromptStyle &style,
                     const std::vector<Binding> &bindings) {
    const Rect &img = placement.rect;
    if (prompts.empty() || renderer == nullptr || !(img.w > 0 && img.h > 0)) {
        return true;
    }
    // The layer's pixels per point: the placement's, rounded, so the glyphs are drawn at the
    // size they are shown (crisp at an integer scale).
    const int scale =
        std::clamp(static_cast<int>(std::lround(img.w / static_cast<float>(kScreenWidth))), 1, 8);
    if (scale != scale_ || texture_ == nullptr) {
        stop();
        texture_ =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              kScreenWidth * scale, kScreenHeight * scale);
        if (texture_ == nullptr) {
            if (!told_) {
                SDL_Log("prompts: cannot make the prompts' layer: %s", SDL_GetError());
                told_ = true;
            }
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
        scale_ = scale;
        pixels_.assign(static_cast<std::size_t>(kScreenWidth) * kScreenHeight * 4 * scale * scale,
                       0);
    }
    if (prompts != drawn_ || !(style == drawn_style_) ||
        !same_bindings(bindings, drawn_bindings_)) {
        std::fill(pixels_.begin(), pixels_.end(), 0);
        Canvas canvas(pixels_.data(), kScreenWidth * scale_, kScreenHeight * scale_, scale_);
        draw_prompts(canvas, prompts, style, bindings);
        if (!SDL_UpdateTexture(texture_, nullptr, pixels_.data(), kScreenWidth * scale_ * 4)) {
            return false;
        }
        drawn_ = prompts;
        drawn_style_ = style;
        drawn_bindings_ = bindings;
        redraws_++;
    }
    const SDL_FRect dst{img.x, img.y, img.w, img.h};
    return SDL_RenderTexture(renderer, texture_, nullptr, &dst);
}

} // namespace maplethief::frontend
