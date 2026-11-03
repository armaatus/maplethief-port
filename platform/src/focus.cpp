// Controller navigation for the UIKit screens (#69, ADR 0051); see focus.h.
#include "focus.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

// The virtual fingers of Input::touch (finger 0x1000 + id): Actions' gestures use 0x10-0x1F.
constexpr std::uint32_t kConfirmFinger = 0x30;
constexpr std::uint32_t kBackFinger = 0x31;
constexpr std::uint32_t kDragFinger = 0x32; // 0x32-0x35
constexpr std::uint32_t kDragFingers = 4;

void runtime_forward(void *, std::uint32_t action, bool down) {
    if (down) {
        mt_input_press(action);
    } else {
        mt_input_release(action);
    }
}

std::int32_t runtime_panel(void *) { return mt_input_panel(); }

// CDataMgr's instance (CDataMgr::GetInstance 0x5BAC1 keeps it here) and its popup menu fields
// (docs/controls/keypad.md, "Panel 1").
constexpr std::uint32_t kDataMgrGlobal = 0x11A984;
constexpr std::uint32_t kMenuOpenTab = 0xA8;
constexpr std::uint32_t kMenuTab = 0xB0;
// The popup menu's panel (mt_input_panel).
constexpr std::int32_t kMenuPanel = 1;
constexpr std::int32_t kChatPanel = 5;
// The engine's current state (docs/re/program-flow.md): SkyCanvas::GetInstance 0x2567D keeps
// the canvas at 0x11A950, its current state at +0xC; a CStateTitle's vtable pointer
// (docs/re/cpp-classes.md: the vtable at 0x119C90, its slots from 0x119C98).
constexpr std::uint32_t kCanvasGlobal = 0x11A950;
constexpr std::uint32_t kCanvasState = 0xC;
constexpr std::uint32_t kTitleVptr = 0x119C98;

std::uint32_t read32(const mt_cpu *cpu, std::uint32_t addr) {
    std::uint32_t v = 0;
    if (addr != 0 && addr + 4ull <= cpu->mem_size) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}

bool runtime_menu(void *, mt_cpu *cpu, std::int32_t *tab, std::int32_t *open) {
    if (cpu == nullptr) {
        return false;
    }
    const std::uint32_t mgr = read32(cpu, kDataMgrGlobal);
    if (mgr == 0) {
        return false;
    }
    *tab = static_cast<std::int32_t>(read32(cpu, mgr + kMenuTab));
    *open = static_cast<std::int32_t>(read32(cpu, mgr + kMenuOpenTab));
    return true;
}

bool runtime_title(void *, mt_cpu *cpu) { return focus_engine_at_title(cpu); }

bool runtime_list_hidden(void *, mt_cpu *cpu, std::int32_t tab) {
    return popup_menu_list_hidden(cpu, tab);
}

void runtime_targets(void *, mt_cpu *cpu, std::vector<FocusTarget> *out) {
    out->clear();
    if (cpu == nullptr) {
        return;
    }
    std::vector<mt_uikit_control> c(64);
    std::uint32_t n = mt_uikit_controls(cpu, c.data(), static_cast<std::uint32_t>(c.size()));
    if (n > c.size()) {
        c.resize(n);
        n = mt_uikit_controls(cpu, c.data(), n);
    }
    for (std::uint32_t i = 0; i < n && i < c.size(); i++) {
        const mt_uikit_control &m = c[i];
        FocusTarget t;
        t.object = m.object;
        t.kind = m.kind == MT_UIKIT_CONTROL_SCROLL   ? FocusTarget::Kind::kScroll
                 : m.kind == MT_UIKIT_CONTROL_BUTTON ? FocusTarget::Kind::kButton
                                                     : FocusTarget::Kind::kControl;
        t.order = m.order;
        t.x = m.rect[0];
        t.y = m.rect[1];
        t.w = m.rect[2];
        t.h = m.rect[3];
        t.px = m.point[0];
        t.py = m.point[1];
        t.action = m.action;
        t.target = m.target;
        t.view_class = m.view_class;
        for (int k = 0; k < 2; k++) {
            t.offset[k] = m.offset[k];
            t.max_offset[k] = m.max_offset[k];
            t.page[k] = m.page[k];
        }
        out->push_back(std::move(t));
    }
}

bool is_direction(std::uint32_t action, Direction *d) {
    switch (action) {
    case MT_ACTION_UP:
        *d = Direction::kUp;
        return true;
    case MT_ACTION_DOWN:
        *d = Direction::kDown;
        return true;
    case MT_ACTION_LEFT:
        *d = Direction::kLeft;
        return true;
    case MT_ACTION_RIGHT:
        *d = Direction::kRight;
        return true;
    default:
        return false;
    }
}

// Distance from `a` to `b` along the direction's axis and across it.
void axes(Direction d, float ax, float ay, float bx, float by, float *along, float *across) {
    switch (d) {
    case Direction::kUp:
        *along = ay - by;
        *across = bx - ax;
        break;
    case Direction::kDown:
        *along = by - ay;
        *across = bx - ax;
        break;
    case Direction::kLeft:
        *along = ax - bx;
        *across = by - ay;
        break;
    case Direction::kRight:
        *along = bx - ax;
        *across = by - ay;
        break;
    }
}

// The gap between two intervals (0 if they overlap).
float gap(float a0, float a1, float b0, float b1) {
    if (b1 < a0) {
        return a0 - b1;
    }
    if (b0 > a1) {
        return b0 - a1;
    }
    return 0;
}

bool vertical(Direction d) { return d == Direction::kUp || d == Direction::kDown; }

// Reading order: top to bottom by rows (centres within 12 points share a row), left to right.
bool reads_before(const FocusTarget &a, const FocusTarget &b) {
    if (std::fabs(a.cy() - b.cy()) > 12) {
        return a.cy() < b.cy();
    }
    if (a.cx() != b.cx()) {
        return a.cx() < b.cx();
    }
    return a.order < b.order;
}

// Whether the scroll view can still move for `d` (a drag in d's direction shows what lies that
// way: down shows more below, so the offset grows).
bool can_scroll(const FocusTarget &t, Direction d) {
    if (t.kind != FocusTarget::Kind::kScroll) {
        return false;
    }
    // The views' own axes: the game's views are upright on the landscape screen (EAGLView's
    // subviews are turned with it), so x is across and y down there too.
    switch (d) {
    case Direction::kDown:
        return t.offset[1] < t.max_offset[1] - 0.5f;
    case Direction::kUp:
        return t.offset[1] > 0.5f;
    case Direction::kRight:
        return t.offset[0] < t.max_offset[0] - 0.5f;
    case Direction::kLeft:
        return t.offset[0] > 0.5f;
    }
    return false;
}

// The game's selectors (docs/re/objc-shell.md, docs/controls/keypad.md).
struct Ranked {
    const char *action;
    int rank;
};

// Back: the screen's close, back, No and cancel buttons.
constexpr Ranked kBack[] = {
    {"closeHelp:", 0},          // MainMenuView's Help
    {"closeAbout:", 0},         // MainMenuView's About
    {"cancelAlert:", 0},        // ChooseCharacterView: the delete alert's No
    {"confirmAlert:", 1},       // ChooseCharacterView: "choose a character first", OK
    {"clickBack:", 2},          // ChooseCharacterView's Back
    {"releaseClearButton:", 2}, // TouchKeyPadView's close buttons (Clear, -16): the cash shop
    {"clickClearButton:", 2},   {"clickADClose:", 3}, // MainMenuView: the promo banner's close
};

// The focus when it appears.
constexpr Ranked kDefault[] = {
    {"releaseAgreementButton:", 0}, // TermsView: agree (the only way on)
    {"cancelAlert:", 0},            // "really delete?": No (deleting takes a move)
    {"confirmAlert:", 0},           // "choose a character first": OK
    {"closeHelp:", 1},              // Help and About: close (down scrolls About)
    {"closeAbout:", 1},
    {"chooseCharacterOne:", 2}, // the character select: the first slot
    {"newCharacterOne:", 2},
    {"releaseMeso3000Button:", 2}, // the cash shop: the first tile
    {"clickPlay:", 3},             // the main menu: Play
};

int rank_in(const Ranked *table, std::size_t n, const std::string &action) {
    for (std::size_t i = 0; i < n; i++) {
        if (action == table[i].action) {
            return table[i].rank;
        }
    }
    return -1;
}

} // namespace

int back_rank(const std::string &action) {
    const int r = rank_in(kBack, std::size(kBack), action);
    if (r >= 0) {
        return r;
    }
    // Any other close or cancel button, after the known ones.
    for (const char *word : {"close", "Close", "cancel", "Cancel"}) {
        if (action.find(word) != std::string::npos) {
            return 10;
        }
    }
    return -1;
}

int default_rank(const std::string &action) {
    return rank_in(kDefault, std::size(kDefault), action);
}

int focus_back_target(const std::vector<FocusTarget> &targets) {
    int best = -1, best_rank = -1;
    for (int i = 0; i < static_cast<int>(targets.size()); i++) {
        const FocusTarget &t = targets[static_cast<std::size_t>(i)];
        const int r = t.focusable() ? back_rank(t.action) : -1;
        // The best rank; of equal ones the frontmost (an alert over its screen).
        if (r >= 0 &&
            (best < 0 || r < best_rank ||
             (r == best_rank && t.order > targets[static_cast<std::size_t>(best)].order))) {
            best = i;
            best_rank = r;
        }
    }
    return best;
}

bool focus_panel(std::int32_t panel) { return panel == -1 || panel == 9; }

int chat_row_of(const std::string &action) {
    static const std::string head = "releaseQuest", tail = "Button:";
    if (action.size() != head.size() + 1 + tail.size() ||
        action.compare(0, head.size(), head) != 0 ||
        action.compare(head.size() + 1, tail.size(), tail) != 0) {
        return -1;
    }
    const char n = action[head.size()];
    return n >= '1' && n <= '6' ? n - '1' : -1;
}

std::vector<int> chat_rows(const std::vector<FocusTarget> &targets) {
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(targets.size()); i++) {
        const FocusTarget &t = targets[static_cast<std::size_t>(i)];
        if (t.focusable() && t.target == "NpcKeyPadView" && chat_row_of(t.action) >= 0) {
            rows.push_back(i);
        }
    }
    std::sort(rows.begin(), rows.end(), [&targets](int a, int b) {
        return chat_row_of(targets[static_cast<std::size_t>(a)].action) <
               chat_row_of(targets[static_cast<std::size_t>(b)].action);
    });
    return rows;
}

bool focus_engine_at_title(const mt_cpu *cpu) {
    if (cpu == nullptr || cpu->mem == nullptr) {
        return false;
    }
    // In a guest address space only the mapped image holds the global: a guest without the
    // game's image (a test's) has no committed page there.
    if (const mt_mem *mem = mt_mem_from_cpu(cpu)) {
        std::uint32_t n = 0;
        const mt_segment *segs = mt_mem_segments(mem, &n);
        bool mapped = false;
        for (std::uint32_t i = 0; i < n && !mapped; i++) {
            mapped = kCanvasGlobal >= segs[i].vmaddr &&
                     kCanvasGlobal + 4ull <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize;
        }
        if (!mapped) {
            return false;
        }
    }
    const std::uint32_t canvas = read32(cpu, kCanvasGlobal);
    const std::uint32_t state = canvas != 0 ? read32(cpu, canvas + kCanvasState) : 0;
    return state != 0 && read32(cpu, state) == kTitleVptr;
}

bool popup_menu_list_hidden(const mt_cpu *cpu, std::int32_t tab) {
    // The tab's list widget: its global points at the variable that holds it (Screen_Init
    // 0x9909); its shown flag at +0xB1. Items 0xF14B8, equipment 0xF14C0, skills 0xF14A8,
    // quests 0xF14B4.
    const std::uint32_t var = tab == 1   ? 0xF14B8u
                              : tab == 2 ? 0xF14C0u
                              : tab == 3 ? 0xF14A8u
                              : tab == 4 ? 0xF14B4u
                                         : 0u;
    if (cpu == nullptr || cpu->mem == nullptr || var == 0) {
        return false;
    }
    const std::uint32_t widget = read32(cpu, read32(cpu, var));
    return widget != 0 && widget + 0xB2ull <= cpu->mem_size && cpu->mem[widget + 0xB1] == 0;
}

bool popup_menu_tab_point(std::int32_t tab, float *x, float *y) {
    if (tab < 1 || tab > 5) {
        return false;
    }
    *x = 75.0f + 54.0f * static_cast<float>(tab) + 27.0f;
    *y = 277.0f;
    return true;
}

int focus_next(const std::vector<FocusTarget> &targets, int from, Direction d) {
    if (from < 0 || from >= static_cast<int>(targets.size())) {
        return -1;
    }
    const FocusTarget &f = targets[static_cast<std::size_t>(from)];
    int best = -1;
    float best_score = 0;
    for (int i = 0; i < static_cast<int>(targets.size()); i++) {
        const FocusTarget &t = targets[static_cast<std::size_t>(i)];
        if (i == from || !t.focusable()) {
            continue;
        }
        float along = 0, across = 0;
        axes(d, f.cx(), f.cy(), t.cx(), t.cy(), &along, &across);
        if (along < 1) {
            continue;
        }
        // Its near edge must not lie behind the focused one's near edge.
        float near_t = 0, near_f = 0;
        switch (d) {
        case Direction::kUp:
            near_t = -(t.y + t.h), near_f = -(f.y + f.h);
            break;
        case Direction::kDown:
            near_t = t.y, near_f = f.y;
            break;
        case Direction::kLeft:
            near_t = -(t.x + t.w), near_f = -(f.x + f.w);
            break;
        case Direction::kRight:
            near_t = t.x, near_f = f.x;
            break;
        }
        if (near_t < near_f) {
            continue;
        }
        const float off =
            vertical(d) ? gap(f.x, f.x + f.w, t.x, t.x + t.w) : gap(f.y, f.y + f.h, t.y, t.y + t.h);
        (void)across;
        const float score = along + 2 * off;
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

int focus_scroll(const std::vector<FocusTarget> &targets, int from, Direction d) {
    const FocusTarget *f = from >= 0 && from < static_cast<int>(targets.size())
                               ? &targets[static_cast<std::size_t>(from)]
                               : nullptr;
    int inner = -1, largest = -1;
    for (int i = 0; i < static_cast<int>(targets.size()); i++) {
        const FocusTarget &t = targets[static_cast<std::size_t>(i)];
        if (!can_scroll(t, d)) {
            continue;
        }
        const float area = t.w * t.h;
        if (f != nullptr && f->px >= t.x && f->px < t.x + t.w && f->py >= t.y &&
            f->py < t.y + t.h) {
            const FocusTarget *b = inner >= 0 ? &targets[static_cast<std::size_t>(inner)] : nullptr;
            if (b == nullptr || area < b->w * b->h) {
                inner = i;
            }
        }
        const FocusTarget *l = largest >= 0 ? &targets[static_cast<std::size_t>(largest)] : nullptr;
        if (l == nullptr || area > l->w * l->h) {
            largest = i;
        }
    }
    return inner >= 0 ? inner : largest;
}

void focus_scroll_drag(const FocusTarget &t, Direction d, float *x0, float *y0, float *x1,
                       float *y1) {
    const float cx = t.x + t.w / 2, cy = t.y + t.h / 2;
    // A page of a paging view (UIKit pages past half way); 60 % of a plain one.
    const float dist_x = t.page[0] > 0 ? std::min(t.page[0] * 0.75f, t.w * 0.9f) : t.w * 0.6f;
    const float dist_y = t.page[1] > 0 ? std::min(t.page[1] * 0.75f, t.h * 0.9f) : t.h * 0.6f;
    float dx = 0, dy = 0;
    switch (d) {
    case Direction::kDown: // the content moves up under the finger
        dy = -dist_y;
        break;
    case Direction::kUp:
        dy = dist_y;
        break;
    case Direction::kRight:
        dx = -dist_x;
        break;
    case Direction::kLeft:
        dx = dist_x;
        break;
    }
    *x0 = cx - dx / 2;
    *y0 = cy - dy / 2;
    *x1 = cx + dx / 2;
    *y1 = cy + dy / 2;
}

Rect focus_highlight(const Placement &placement, float x, float y, float w, float h, float outset) {
    const Rect &img = placement.rect;
    if (!(img.w > 0 && img.h > 0) || !(w > 0 && h > 0)) {
        return {};
    }
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    screen_to_drawable(placement, x, y, &x0, &y0);
    screen_to_drawable(placement, x + w, y + h, &x1, &y1);
    x0 = std::max(img.x, std::floor(x0 - outset));
    y0 = std::max(img.y, std::floor(y0 - outset));
    x1 = std::min(img.x + img.w, std::ceil(x1 + outset));
    y1 = std::min(img.y + img.h, std::ceil(y1 + outset));
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

float focus_highlight_width(const Placement &placement) {
    const float per_point = placement.rect.w / static_cast<float>(kScreenWidth);
    return std::max(2.0f, std::round(2 * per_point));
}

Focus::Focus(Input *input)
    : input_(input), forward_(runtime_forward), panel_(runtime_panel), source_(runtime_targets),
      menu_(runtime_menu), list_(runtime_list_hidden), title_(runtime_title) {}

void Focus::attach(Actions &actions) {
    actions.set_sink([](void *user, std::uint32_t action,
                        bool down) { static_cast<Focus *>(user)->action(action, down); },
                     this);
}

void Focus::set_forward(ActionSink sink, void *user) {
    forward_ = sink != nullptr ? sink : runtime_forward;
    forward_user_ = sink != nullptr ? user : nullptr;
}

void Focus::set_panel_source(PanelSource source, void *user) {
    panel_ = source != nullptr ? source : runtime_panel;
    panel_user_ = source != nullptr ? user : nullptr;
}

void Focus::set_target_source(TargetSource source, void *user) {
    source_ = source != nullptr ? source : runtime_targets;
    source_user_ = source != nullptr ? user : nullptr;
}

void Focus::set_menu_source(MenuSource source, void *user) {
    menu_ = source != nullptr ? source : runtime_menu;
    menu_user_ = source != nullptr ? user : nullptr;
}

void Focus::set_menu_list_source(MenuListSource source, void *user) {
    list_ = source != nullptr ? source : runtime_list_hidden;
    list_user_ = source != nullptr ? user : nullptr;
}

void Focus::set_title_source(TitleSource source, void *user) {
    title_ = source != nullptr ? source : runtime_title;
    title_user_ = source != nullptr ? user : nullptr;
}

int Focus::index_of(const Known &k) const {
    if (k.object == 0) {
        return -1;
    }
    for (std::size_t i = 0; i < targets_.size(); i++) {
        if (targets_[i].object == k.object && targets_[i].action == k.action &&
            targets_[i].focusable()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const FocusTarget *Focus::focused() const {
    const int i = index_of(focus_);
    return i >= 0 ? &targets_[static_cast<std::size_t>(i)] : nullptr;
}

const FocusTarget *Focus::highlighted() const { return active_ && used_ ? focused() : nullptr; }

void Focus::action(std::uint32_t action, bool down) {
    const bool known = action < std::size(taken_);
    Direction d = Direction::kUp;
    const bool mine = is_direction(action, &d) || action == MT_ACTION_OK ||
                      action == MT_ACTION_BACK || action == MT_ACTION_MAP;
    if (down) {
        if (active_ && known && mine) {
            taken_[action]++;
            queue_.push_back(Event{action, true});
            return;
        }
        if ((action == MT_ACTION_UP || action == MT_ACTION_DOWN) && !active_ &&
            panel_(panel_user_) == kChatPanel && !chat_rows(targets_).empty()) {
            // The NPC chat's quest rows: touch-only in the game (#77).
            chat_taken_[action == MT_ACTION_DOWN ? 1 : 0]++;
            chat_queue_.push_back(Event{action, true});
            return;
        }
        if (action == MT_ACTION_OK && panel_(panel_user_) == kMenuPanel) {
            // The popup menu's OK: a touch on the tab or the key (turn decides).
            menu_taken_++;
            menu_queue_.push_back(Event{action, true});
            return;
        }
    } else if ((action == MT_ACTION_UP || action == MT_ACTION_DOWN) &&
               chat_taken_[action == MT_ACTION_DOWN ? 1 : 0] > 0) {
        chat_taken_[action == MT_ACTION_DOWN ? 1 : 0]--;
        return;
    } else if (action == MT_ACTION_OK && menu_taken_ > 0) {
        menu_taken_--;
        menu_queue_.push_back(Event{action, false});
        return;
    } else if (known && taken_[action] > 0) {
        taken_[action]--;
        queue_.push_back(Event{action, false});
        return;
    }
    stats_.forwarded++;
    forward_(forward_user_, action, down);
}

void Focus::refresh(mt_cpu *cpu) {
    std::vector<FocusTarget> old;
    old.swap(targets_);
    source_(source_user_, cpu, &targets_);
    at_title_ = title_(title_user_, cpu);
    const std::int32_t panel = panel_(panel_user_);
    const bool any = std::any_of(targets_.begin(), targets_.end(),
                                 [](const FocusTarget &t) { return t.focusable(); });
    const bool was = active_;
    active_ = focus_panel(panel) && any;
    const bool changed = old.size() != targets_.size() ||
                         !std::equal(old.begin(), old.end(), targets_.begin(),
                                     [](const FocusTarget &a, const FocusTarget &b) {
                                         return a.object == b.object;
                                     });
    if (changed && mt_trace_enabled("focus")) {
        mt_tracef("focus", "panel %d, %zu targets", static_cast<int>(panel), targets_.size());
        for (const FocusTarget &t : targets_) {
            mt_tracef("focus",
                      "  %s 0x%08X #%u (%g, %g, %g, %g) at (%g, %g) -[%s %s] offset (%g, "
                      "%g) of (%g, %g)",
                      t.view_class.c_str(), static_cast<unsigned>(t.object),
                      static_cast<unsigned>(t.order), static_cast<double>(t.x),
                      static_cast<double>(t.y), static_cast<double>(t.w), static_cast<double>(t.h),
                      static_cast<double>(t.px), static_cast<double>(t.py), t.target.c_str(),
                      t.action.c_str(), static_cast<double>(t.offset[0]),
                      static_cast<double>(t.offset[1]), static_cast<double>(t.max_offset[0]),
                      static_cast<double>(t.max_offset[1]));
        }
    }
    if (active_ != was) {
        SDL_Log("focus: %s (panel %d, %zu controls)", active_ ? "on" : "off",
                static_cast<int>(panel), targets_.size());
        used_ = false;
        if (!active_) {
            focus_ = Known{};
            remembered_.clear(); // a screen of the game in between: start afresh
        }
    }
    if (!active_) {
        return;
    }
    // Controls that were not reachable before.
    int best_new = -1, best_rank = -1, first_new = -1;
    for (int i = 0; i < static_cast<int>(targets_.size()); i++) {
        const FocusTarget &t = targets_[static_cast<std::size_t>(i)];
        if (!t.focusable()) {
            continue;
        }
        const bool seen = std::any_of(old.begin(), old.end(), [&](const FocusTarget &o) {
            return o.object == t.object && o.focusable();
        });
        if (seen && was) {
            continue;
        }
        const int r = default_rank(t.action);
        if (r >= 0 && (best_rank < 0 || r < best_rank)) {
            best_rank = r;
            best_new = i;
        }
        if (first_new < 0 || reads_before(t, targets_[static_cast<std::size_t>(first_new)])) {
            first_new = i;
        }
    }
    int now_at = index_of(focus_);
    // The latest control that had the focus before (most recent first) that is reachable now:
    // back again after the screen over it closed, or still there when the alert that took the
    // focus from it closes.
    int back_again = -1;
    bool reappeared = false;
    for (auto it = remembered_.rbegin(); it != remembered_.rend() && back_again < 0; ++it) {
        const int i = index_of(*it);
        if (i >= 0 && i != now_at) {
            back_again = i;
            reappeared = !was || !std::any_of(old.begin(), old.end(), [&](const FocusTarget &o) {
                return o.object == it->object && o.action == it->action && o.focusable();
            });
        }
    }
    const auto forget = [this](std::uint32_t object) {
        remembered_.erase(std::remove_if(remembered_.begin(), remembered_.end(),
                                         [object](const Known &k) { return k.object == object; }),
                          remembered_.end());
    };
    const auto remember = [this, &forget](const Known &k) {
        forget(k.object);
        remembered_.push_back(k);
        if (remembered_.size() > kRemembered) {
            remembered_.erase(remembered_.begin());
        }
    };
    if (now_at < 0 && focus_.object != 0) {
        remember(focus_); // a screen opened over it: it gets the focus back when it returns
    }
    if (reappeared && (best_new < 0 || targets_[static_cast<std::size_t>(back_again)].order >=
                                           targets_[static_cast<std::size_t>(best_new)].order)) {
        now_at = back_again;
    } else if (best_new >= 0) {
        if (now_at >= 0) {
            remember(focus_); // an alert took it: it comes back when the alert closes
        }
        now_at = best_new;
    } else if (now_at < 0 && back_again >= 0) {
        now_at = back_again;
    } else if (now_at < 0 && first_new >= 0) {
        now_at = first_new;
    } else if (now_at < 0) {
        // Its control went: the nearest one to where it was.
        float best = 0;
        for (int i = 0; i < static_cast<int>(targets_.size()); i++) {
            const FocusTarget &t = targets_[static_cast<std::size_t>(i)];
            if (!t.focusable()) {
                continue;
            }
            const float dist = std::hypot(t.cx() - focus_x_, t.cy() - focus_y_);
            if (now_at < 0 || dist < best) {
                now_at = i;
                best = dist;
            }
        }
    }
    if (now_at >= 0) {
        forget(targets_[static_cast<std::size_t>(now_at)].object);
    }
    Known now;
    if (now_at >= 0) {
        now = Known{targets_[static_cast<std::size_t>(now_at)].object,
                    targets_[static_cast<std::size_t>(now_at)].action};
    }
    if (now_at >= 0 && (now.object != focus_.object || now.action != focus_.action)) {
        const FocusTarget &t = targets_[static_cast<std::size_t>(now_at)];
        SDL_Log("focus: -[%s %s] at (%g, %g)", t.target.c_str(), t.action.c_str(),
                static_cast<double>(t.cx()), static_cast<double>(t.cy()));
    }
    focus_ = now;
    if (const FocusTarget *f = focused()) {
        focus_x_ = f->cx();
        focus_y_ = f->cy();
    }
}

bool Focus::move(Direction d) {
    const int from = index_of(focus_);
    const int to = focus_next(targets_, from, d);
    if (to >= 0) {
        const FocusTarget &t = targets_[static_cast<std::size_t>(to)];
        focus_ = Known{t.object, t.action};
        focus_x_ = t.cx();
        focus_y_ = t.cy();
        stats_.moves++;
        SDL_Log("focus: %s to -[%s %s] at (%g, %g)", direction_name(d), t.target.c_str(),
                t.action.c_str(), static_cast<double>(t.cx()), static_cast<double>(t.cy()));
        return true;
    }
    const int s = focus_scroll(targets_, from, d);
    if (s < 0 || drag_.live) {
        return false;
    }
    const FocusTarget &t = targets_[static_cast<std::size_t>(s)];
    drag_ = Drag{};
    focus_scroll_drag(t, d, &drag_.x0, &drag_.y0, &drag_.x1, &drag_.y1);
    drag_.id = kDragFinger + next_drag_++ % kDragFingers;
    if (input_ != nullptr &&
        input_->touch(InputSource::kGamepad, drag_.id, MT_UIKIT_TOUCH_BEGAN, drag_.x0, drag_.y0)) {
        drag_.live = true;
        stats_.scrolls++;
        SDL_Log("focus: %s scrolls %s 0x%08X: a drag (%g, %g) -> (%g, %g)", direction_name(d),
                t.view_class.c_str(), static_cast<unsigned>(t.object),
                static_cast<double>(drag_.x0), static_cast<double>(drag_.y0),
                static_cast<double>(drag_.x1), static_cast<double>(drag_.y1));
    }
    return true;
}

void Focus::begin_touch(Touch &t, std::uint32_t id, float x, float y, const char *what) {
    if (t.live || input_ == nullptr) {
        return;
    }
    if (input_->touch(InputSource::kGamepad, id, MT_UIKIT_TOUCH_BEGAN, x, y)) {
        t = Touch{true, id, x, y, false, turns_};
        SDL_Log("focus: %s: a touch at (%g, %g)", what, static_cast<double>(x),
                static_cast<double>(y));
    }
}

void Focus::end_touch(Touch &t) {
    if (t.live && input_ != nullptr) {
        input_->touch(InputSource::kGamepad, t.id, MT_UIKIT_TOUCH_ENDED, t.x, t.y);
    }
    t = Touch{};
}

void Focus::apply(const Event &e, double now) {
    Direction d = Direction::kUp;
    if (is_direction(e.action, &d)) {
        if (e.down) {
            used_ = true;
            move(d);
            held_dir_ = static_cast<int>(d);
            repeat_at_ = now + kRepeatDelay;
        } else if (held_dir_ == static_cast<int>(d)) {
            held_dir_ = -1;
        }
        return;
    }
    if (e.action == MT_ACTION_OK) {
        if (e.down && at_title_) {
            // Before the engine's main menu: Play would end the game (#129, see focus.h).
            stats_.early++;
            SDL_Log("focus: confirm dropped: the engine is not at its main menu yet");
            return;
        }
        if (e.down) {
            used_ = true;
            if (const FocusTarget *f = focused()) {
                stats_.taps++;
                std::string what = "confirm -[" + f->target + " " + f->action + "]";
                begin_touch(confirm_, kConfirmFinger, f->px, f->py, what.c_str());
            }
        } else if (confirm_.live) {
            confirm_.release = true;
        }
        return;
    }
    if (e.action == MT_ACTION_BACK) {
        if (e.down) {
            used_ = true;
            const int best = focus_back_target(targets_);
            if (best >= 0) {
                const FocusTarget &t = targets_[static_cast<std::size_t>(best)];
                stats_.backs++;
                std::string what = "back -[" + t.target + " " + t.action + "]";
                begin_touch(back_, kBackFinger, t.px, t.py, what.c_str());
            } else {
                SDL_Log("focus: back: no close, back or No button on this screen");
            }
        } else if (back_.live) {
            back_.release = true;
        }
        return;
    }
    if (e.down) { // map ('*'): see focus.h
        stats_.dropped++;
        SDL_Log("focus: map ('*') dropped: the Delete button is the visible way");
    }
}

void Focus::menu_ok(mt_cpu *cpu, const Event &e) {
    if (!e.down) {
        if (menu_forwarded_ > 0) {
            menu_forwarded_--;
            stats_.forwarded++;
            forward_(forward_user_, MT_ACTION_OK, false);
        } else if (confirm_.live) {
            confirm_.release = true;
        }
        return;
    }
    std::int32_t tab = -1, open = -1;
    float x = 0, y = 0;
    if (!confirm_.live && menu_(menu_user_, cpu, &tab, &open) && open == 0 &&
        popup_menu_tab_point(tab, &x, &y)) {
        stats_.menu_tabs++;
        const std::string what = "popup menu: open tab " + std::to_string(tab);
        begin_touch(confirm_, kConfirmFinger, x, y, what.c_str());
        ok_tab_ = tab; // this touch shows the tab's list itself: menu_list leaves it be
        return;
    }
    menu_forwarded_++;
    stats_.forwarded++;
    forward_(forward_user_, MT_ACTION_OK, true);
}

void Focus::menu_list(mt_cpu *cpu) {
    // A tab that a key opened with its list hidden (#164: left / right; down on any tab): the
    // touch on its tab that shows it, once while the tab stays open (the touch keeps the tab
    // open and the cursor where it is).
    std::int32_t tab = -1, open = -1;
    if (panel_(panel_user_) != kMenuPanel || !menu_(menu_user_, cpu, &tab, &open)) {
        listed_ = -1;
        ok_tab_ = -1;
        return;
    }
    if (open != tab) {
        listed_ = -1;
        if (open != 0) {
            ok_tab_ = -1;
        }
        return;
    }
    if (open != listed_) {
        listed_ = -1;
    }
    if (ok_tab_ == open) {
        // The tab that OK's touch opened (menu_ok): that touch shows its list. Its release
        // reaches the game a turn after the focus ends it, so the list still reads hidden here;
        // a second touch on the tab would undo the first (the item and equipment lists stayed
        // empty).
        listed_ = open;
        ok_tab_ = -1;
        return;
    }
    float x = 0, y = 0;
    if (listed_ >= 0 || confirm_.live || !list_(list_user_, cpu, open) ||
        !popup_menu_tab_point(open, &x, &y)) {
        return;
    }
    listed_ = open;
    stats_.menu_tabs++;
    const std::string what = "popup menu: show tab " + std::to_string(open) + "'s list";
    begin_touch(confirm_, kConfirmFinger, x, y, what.c_str());
    confirm_.release = true; // a tap: up at the next turn
}

void Focus::chat_move(const Event &e) {
    const std::vector<int> rows = chat_rows(targets_);
    if (!e.down || rows.empty() || confirm_.live) {
        return;
    }
    const int last = static_cast<int>(rows.size()) - 1;
    const int to = std::clamp(chat_row_ + (e.action == MT_ACTION_DOWN ? 1 : -1), 0, last);
    const FocusTarget &t = targets_[static_cast<std::size_t>(rows[static_cast<std::size_t>(to)])];
    chat_row_ = to;
    stats_.chat_rows++;
    const std::string what =
        "NPC chat: quest row " + std::to_string(to + 1) + " of " + std::to_string(rows.size());
    begin_touch(confirm_, kConfirmFinger, t.px, t.py, what.c_str());
    confirm_.release = true; // a tap: up at the next turn
}

void Focus::turn(mt_cpu *cpu, double now) {
    turns_++;
    // The touches it makes: a release that came ends its touch, a turn after its began at
    // the earliest (so that the began reaches UIKit first); the drag moves on.
    for (Touch *t : {&confirm_, &back_}) {
        if (t->live && t->release && t->turn < turns_) {
            end_touch(*t);
        }
    }
    if (drag_.live) {
        drag_.step++;
        if (drag_.step <= kDragMoves) {
            const float f = static_cast<float>(drag_.step) / kDragMoves;
            input_->touch(InputSource::kGamepad, drag_.id, MT_UIKIT_TOUCH_MOVED,
                          drag_.x0 + (drag_.x1 - drag_.x0) * f,
                          drag_.y0 + (drag_.y1 - drag_.y0) * f);
        } else {
            input_->touch(InputSource::kGamepad, drag_.id, MT_UIKIT_TOUCH_ENDED, drag_.x1,
                          drag_.y1);
            drag_.live = false;
        }
    }
    refresh(cpu);
    const bool shown = panel_(panel_user_) == kChatPanel && !chat_rows(targets_).empty();
    if (shown && !chat_shown_) {
        chat_row_ = 0; // a new list: the game starts on its first row
    }
    chat_shown_ = shown;
    const std::vector<Event> chat = std::move(chat_queue_);
    chat_queue_.clear();
    for (const Event &e : chat) {
        chat_move(e);
    }
    const std::vector<Event> menu = std::move(menu_queue_);
    menu_queue_.clear();
    for (const Event &e : menu) {
        menu_ok(cpu, e);
    }
    menu_list(cpu);
    const std::vector<Event> queue = std::move(queue_);
    queue_.clear();
    for (const Event &e : queue) {
        apply(e, now);
    }
    if (!active_) {
        held_dir_ = -1;
        return;
    }
    if (held_dir_ >= 0 && now >= repeat_at_ && !drag_.live) {
        move(static_cast<Direction>(held_dir_));
        repeat_at_ = now + kRepeatEvery;
    }
}

} // namespace maplethief::frontend
