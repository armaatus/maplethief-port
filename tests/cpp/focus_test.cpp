// Controller navigation for the UIKit screens (#69, ADR 0051) with synthetic data only. First
// the frontend's pure parts on made-up targets: spatial navigation (focus_next), the scroll view
// to drag and the drag, the selector tables, the panels, the popup menu's tab points and the
// highlight's placement. Then Focus on a recording touch sink and a scripted target list: the
// actions' routing (on to the game while inactive, taken while active, a release where its
// press went), confirm and back as touches, the focus choice as screens and alerts come and
// go, the popup menu's OK. Last the runtime's mt_uikit_controls on a synthetic view tree (shown,
// hidden, faded, disabled, covered, clipped, scroll views) and the whole chain: Focus reading
// that tree, its touches through Input into mt_uikit_touch, the buttons' actions.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "focus.h"
#include "foundation/foundation.h"
#include "input.h"
#include "maplethief/cf.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/uikit.h"
#include "scaling.h"
#include "uikit/uikit_internal.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using fe::Direction;
using fe::FocusTarget;

FocusTarget button(u32 object, float x, float y, float w, float h, const char *action = "tap:") {
    FocusTarget t;
    t.object = object;
    t.order = object;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.px = x + w / 2;
    t.py = y + h / 2;
    t.action = action;
    t.target = "TestView";
    t.view_class = "UIButton";
    return t;
}

FocusTarget scroll(u32 object, float x, float y, float w, float h, float off_y, float max_y) {
    FocusTarget t = button(object, x, y, w, h, "");
    t.kind = FocusTarget::Kind::kScroll;
    t.view_class = "UIScrollView";
    t.offset[1] = off_y;
    t.max_offset[1] = max_y;
    return t;
}

} // namespace

TEST_CASE("focus: spatial navigation prefers the control in line, never one behind") {
    // A 2x2 grid, a small control nearer to the first one but off its row, a scroll view.
    const std::vector<FocusTarget> t = {
        button(1, 100, 100, 80, 40), button(2, 250, 100, 80, 40),
        button(3, 100, 200, 80, 40), button(4, 250, 200, 80, 40),
        button(5, 210, 190, 30, 30), scroll(6, 300, 90, 170, 60, 0, 100),
    };
    // Right of 0: 1 (in line, 150 away) beats 4 (85 along, 50 off the row: 85 + 2 * 50).
    CHECK(fe::focus_next(t, 0, Direction::kRight) == 1);
    CHECK(fe::focus_next(t, 0, Direction::kDown) == 2);
    // Up from 2: the small one between the rows (15 up, 30 aside: 15 + 2 * 30) before 0 (100).
    CHECK(fe::focus_next(t, 2, Direction::kUp) == 4);
    CHECK(fe::focus_next(t, 1, Direction::kDown) == 3); // 100 against 85 + 2 * 10
    CHECK(fe::focus_next(t, 1, Direction::kLeft) == 0);
    // Nothing that way (the scroll view right of 1 is not a control).
    CHECK(fe::focus_next(t, 0, Direction::kUp) == -1);
    CHECK(fe::focus_next(t, 1, Direction::kRight) == -1);
    CHECK(fe::focus_next(t, 9, Direction::kRight) == -1);

    // The near-edge rule: a tall control whose centre lies below but whose top lies above the
    // focused one's is not "down" from it.
    const std::vector<FocusTarget> tall = {button(1, 100, 100, 80, 40),
                                           button(2, 300, 50, 40, 200)};
    CHECK(fe::focus_next(tall, 0, Direction::kDown) == -1);
    CHECK(fe::focus_next(tall, 0, Direction::kRight) == 1);
}

TEST_CASE("focus: the scroll view to drag, and the drag") {
    // A full-screen list that can scroll down, and an inner one at its end, holding the focus.
    const std::vector<FocusTarget> t = {
        scroll(1, 0, 0, 480, 320, 0, 400),
        scroll(2, 90, 90, 200, 200, 100, 100),
        button(3, 120, 120, 40, 40),
    };
    CHECK(fe::focus_scroll(t, 2, Direction::kDown) == 0); // the inner one cannot go down
    CHECK(fe::focus_scroll(t, 2, Direction::kUp) == 1);   // it can go up, and holds the focus
    CHECK(fe::focus_scroll(t, 2, Direction::kLeft) == -1);

    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // Down: the content moves up under the finger, 60 % of the height.
    fe::focus_scroll_drag(t[0], Direction::kDown, &x0, &y0, &x1, &y1);
    CHECK(x0 == 240);
    CHECK(x1 == 240);
    CHECK(y0 == doctest::Approx(256));
    CHECK(y1 == doctest::Approx(64));
    // A paging one: three quarters of a page.
    FocusTarget pages = scroll(4, 0, 0, 480, 320, 0, 0);
    pages.max_offset[0] = 960;
    pages.page[0] = 480;
    fe::focus_scroll_drag(pages, Direction::kRight, &x0, &y0, &x1, &y1);
    CHECK(x0 == doctest::Approx(420));
    CHECK(x1 == doctest::Approx(60));
    CHECK(y0 == y1);
}

TEST_CASE("focus: the selector tables, the panels, the popup menu's tabs") {
    CHECK(fe::back_rank("closeHelp:") == 0);
    CHECK(fe::back_rank("cancelAlert:") == 0);
    CHECK(fe::back_rank("clickBack:") == 2);
    CHECK(fe::back_rank("clickADClose:") == 3);
    CHECK(fe::back_rank("releaseCloseButton:") == 10); // any other close
    CHECK(fe::back_rank("clickPlay:") == -1);
    CHECK(fe::default_rank("releaseAgreementButton:") == 0);
    CHECK(fe::default_rank("chooseCharacterOne:") == 2);
    CHECK(fe::default_rank("clickPlay:") == 3);
    CHECK(fe::default_rank("clickHelp:") == -1);

    CHECK(fe::focus_panel(-1));
    CHECK(fe::focus_panel(9));
    for (const int p : {0, 1, 3, 5, 6, 7, 8, 10}) {
        CHECK_FALSE(fe::focus_panel(p));
    }

    float x = 0, y = 0;
    CHECK(fe::popup_menu_tab_point(1, &x, &y));
    CHECK(x == 156);
    CHECK(y == 277);
    CHECK(fe::popup_menu_tab_point(5, &x, &y));
    CHECK(x == 372);
    CHECK_FALSE(fe::popup_menu_tab_point(0, &x, &y)); // status: nothing to enable
    CHECK_FALSE(fe::popup_menu_tab_point(6, &x, &y));
}

TEST_CASE("focus: the highlight's placement in drawable pixels") {
    // Integer mode at 2x in a 1000x700 drawable: the screen at (20, 30, 960, 640).
    const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 1000, 700, 480, 320);
    REQUIRE(p.integer_scale == 2);
    // The main menu's Play (46, 148, 120, 48), grown by 3 pixels.
    fe::Rect r = fe::focus_highlight(p, 46, 148, 120, 48, 3);
    CHECK(r.x == 109);
    CHECK(r.y == 323);
    CHECK(r.w == 246);
    CHECK(r.h == 102);
    // At the screen's corner: clipped to the image, not into the bars.
    r = fe::focus_highlight(p, 470, 0, 10, 20, 3);
    CHECK(r.x == 957);
    CHECK(r.y == 30);
    CHECK(r.w == 23);
    CHECK(r.h == 43);
    // No placement, or an empty rect: nothing.
    r = fe::focus_highlight(fe::Placement{}, 46, 148, 120, 48, 3);
    CHECK(r.w == 0);
    r = fe::focus_highlight(p, 46, 148, 0, 48, 3);
    CHECK(r.w == 0);
    // Two screen points wide, at least 2 pixels.
    CHECK(fe::focus_highlight_width(p) == 4);
    CHECK(fe::focus_highlight_width(fe::place(fe::ScaleMode::kFit, 480, 320, 480, 320)) == 2);
}

namespace {

// Focus on a recording touch sink, a scripted panel, target list and popup menu state.
struct Rig {
    fe::Input input;
    fe::Focus focus{&input};
    std::vector<std::string> touches;   // "0x1030 began (106, 172)"
    std::vector<std::string> forwarded; // "ok down"
    std::vector<FocusTarget> targets;
    std::int32_t panel = -1;
    std::int32_t tab = 0, open = 0;
    double now = 100;

    Rig() {
        input.set_sink(
            [](void *user, u32 finger, u32 phase, float x, float y) {
                static const char *const k_phase[] = {"began", "moved", "ended", "cancelled"};
                char line[96];
                std::snprintf(line, sizeof line, "0x%X %s (%g, %g)", static_cast<unsigned>(finger),
                              k_phase[phase & 3], static_cast<double>(x), static_cast<double>(y));
                static_cast<Rig *>(user)->touches.emplace_back(line);
                return 0;
            },
            this);
        focus.set_forward(
            [](void *user, u32 action, bool down) {
                const mt_input_action_info *info = mt_input_action_get_info(action);
                static_cast<Rig *>(user)->forwarded.push_back(
                    std::string(info != nullptr ? info->name : "?") + (down ? " down" : " up"));
            },
            this);
        focus.set_panel_source([](void *user) { return static_cast<Rig *>(user)->panel; }, this);
        focus.set_target_source(
            [](void *user, mt_cpu *, std::vector<FocusTarget> *out) {
                *out = static_cast<Rig *>(user)->targets;
            },
            this);
        focus.set_menu_source(
            [](void *user, mt_cpu *, std::int32_t *tab, std::int32_t *open) {
                *tab = static_cast<Rig *>(user)->tab;
                *open = static_cast<Rig *>(user)->open;
                return true;
            },
            this);
    }
    void turn() {
        now += 1.0 / 60;
        focus.turn(nullptr, now);
    }
    // A button press and release (one turn apart), then a turn to end the touch.
    void press(u32 action) {
        focus.action(action, true);
        turn();
        focus.action(action, false);
        turn();
        turn();
    }
    std::string focused() const {
        const FocusTarget *f = focus.focused();
        return f != nullptr ? f->action : "";
    }
    std::vector<std::string> take_touches() {
        std::vector<std::string> out;
        out.swap(touches);
        return out;
    }
};

using Lines = std::vector<std::string>;

} // namespace

TEST_CASE("focus: inactive, the actions go on to the game; active, the focus takes them") {
    Rig r;
    // A game panel with no UIKit controls: everything goes on.
    r.panel = 0;
    r.turn();
    CHECK_FALSE(r.focus.active());
    r.press(MT_ACTION_OK);
    r.press(MT_ACTION_MAP);
    CHECK(r.forwarded == Lines{"ok down", "ok up", "map down", "map up"});
    CHECK(r.touches.empty());

    // The main menu: Play, Help, the banner's close.
    r.panel = -1;
    r.targets = {button(1, 46, 148, 120, 48, "clickPlay:"),
                 button(2, 46, 198, 120, 48, "clickHelp:"),
                 button(3, 445, 210, 35, 35, "clickADClose:")};
    r.forwarded.clear();
    r.turn();
    CHECK(r.focus.active());
    CHECK(r.focused() == "clickPlay:");      // the default
    CHECK(r.focus.highlighted() == nullptr); // not used yet
    r.press(MT_ACTION_DOWN);
    CHECK(r.focused() == "clickHelp:");
    CHECK(r.focus.highlighted() != nullptr);
    // OK: a touch at the control's point, down on the press, up after the release.
    r.press(MT_ACTION_OK);
    CHECK(r.take_touches() == Lines{"0x1030 began (106, 222)", "0x1030 ended (106, 222)"});
    // Back: the banner's close.
    r.press(MT_ACTION_BACK);
    CHECK(r.take_touches() == Lines{"0x1031 began (462.5, 227.5)", "0x1031 ended (462.5, 227.5)"});
    // An action with no use here is dropped, not sent to the game.
    r.press(MT_ACTION_MAP);
    CHECK(r.forwarded.empty());
    CHECK(r.focus.stats().dropped == 1);

    // A press taken while active is released by the focus even if the game's screen came back
    // in between; a press made then goes on, and its release follows it.
    r.focus.action(MT_ACTION_OK, true);
    r.turn();
    r.panel = 0;
    r.turn();
    CHECK_FALSE(r.focus.active());
    r.focus.action(MT_ACTION_OK, false);
    r.turn();
    CHECK(r.forwarded.empty());
    r.press(MT_ACTION_JUMP);
    CHECK(r.forwarded == Lines{"jump down", "jump up"});
    CHECK(r.focus.stats().taps == 2);
    CHECK(r.focus.stats().backs == 1);
}

TEST_CASE("focus: a screen over the focused control, an alert, and back") {
    Rig r;
    r.panel = -1;
    const FocusTarget play = button(1, 46, 148, 120, 48, "clickPlay:");
    const FocusTarget help = button(2, 46, 198, 120, 48, "clickHelp:");
    r.targets = {play, help};
    r.turn();
    r.press(MT_ACTION_DOWN);
    REQUIRE(r.focused() == "clickHelp:");
    // Help opens over the menu: its close gets the focus.
    r.targets = {button(10, 400, 20, 40, 40, "closeHelp:")};
    r.turn();
    CHECK(r.focused() == "closeHelp:");
    // Help closes: the focus goes back to Help's button, not to the default Play.
    r.targets = {play, help};
    r.turn();
    CHECK(r.focused() == "clickHelp:");
    // An alert over the screen (the screen stays reachable): its default takes the focus, and
    // gives it back when it closes.
    r.targets = {play, help, button(20, 140, 180, 90, 30, "cancelAlert:"),
                 button(21, 250, 180, 90, 30, "okDeleteAlert:")};
    r.targets[2].order = r.targets[3].order = 30;
    r.turn();
    CHECK(r.focused() == "cancelAlert:");
    r.press(MT_ACTION_RIGHT);
    CHECK(r.focused() == "okDeleteAlert:");
    r.targets = {play, help};
    r.turn();
    CHECK(r.focused() == "clickHelp:");
    // Back with no back control does nothing.
    r.press(MT_ACTION_BACK);
    CHECK(r.touches.empty());
    // Nothing that way, but a scroll view that can go: a drag over it, in eight moves.
    r.targets.push_back(scroll(30, 200, 0, 280, 320, 0, 300));
    r.turn();
    r.press(MT_ACTION_DOWN);
    for (int i = 0; i < fe::Focus::kDragMoves + 2; i++) {
        r.turn();
    }
    const Lines drag = r.take_touches();
    REQUIRE(drag.size() == static_cast<std::size_t>(fe::Focus::kDragMoves) + 2);
    CHECK(drag.front() == "0x1032 began (340, 256)");
    CHECK(drag.back() == "0x1032 ended (340, 64)");
    CHECK(r.focus.stats().scrolls == 1);
}

TEST_CASE("focus: a held direction repeats") {
    Rig r;
    r.panel = -1;
    for (u32 i = 0; i < 6; i++) {
        r.targets.push_back(button(i + 1, 100, 20 + 50 * static_cast<float>(i), 80, 40));
    }
    r.turn();
    r.focus.action(MT_ACTION_DOWN, true);
    r.turn(); // the first move
    CHECK(r.focus.stats().moves == 1);
    // 0.40 s: the first repeat; then every 0.15 s.
    for (int i = 0; i < 25; i++) { // 0.40 s
        r.turn();
    }
    CHECK(r.focus.stats().moves == 2);
    for (int i = 0; i < 10; i++) { // 0.15 s
        r.turn();
    }
    CHECK(r.focus.stats().moves == 3);
    r.focus.action(MT_ACTION_DOWN, false);
    for (int i = 0; i < 60; i++) {
        r.turn();
    }
    CHECK(r.focus.stats().moves == 3);
}

TEST_CASE("focus: OK in the popup menu opens the tab by a touch, else it is the key") {
    Rig r;
    r.panel = 1; // the popup menu: key codes, no focus
    r.turn();
    REQUIRE_FALSE(r.focus.active());
    // The item tab chosen, none open: a touch on it.
    r.tab = 1;
    r.press(MT_ACTION_OK);
    CHECK(r.take_touches() == Lines{"0x1030 began (156, 277)", "0x1030 ended (156, 277)"});
    CHECK(r.forwarded.empty());
    CHECK(r.focus.stats().menu_tabs == 1);
    // Inside the open tab OK is the key (choose, use).
    r.open = 1;
    r.press(MT_ACTION_OK);
    CHECK(r.touches.empty());
    CHECK(r.forwarded == Lines{"ok down", "ok up"});
    // The status tab has nothing to enable: the key.
    r.forwarded.clear();
    r.open = 0;
    r.tab = 0;
    r.press(MT_ACTION_OK);
    CHECK(r.forwarded == Lines{"ok down", "ok up"});
    // The other keys are the game's.
    r.forwarded.clear();
    r.press(MT_ACTION_RIGHT);
    r.press(MT_ACTION_BACK);
    CHECK(r.forwarded == Lines{"right down", "right up", "back down", "back up"});
    CHECK(r.touches.empty());
}

// --- the runtime's controls on a synthetic view tree ----------------------------------------

namespace {

std::vector<std::string> g_actions;
void record_action(mt_cpu *cpu) {
    g_actions.emplace_back(mt_objc_sel_name(mt_objc_current(), cpu->r[1]));
}
void quiet(void *, const char *) {}

const mt_objc_host_method k_menu_methods[] = {
    {"clickPlay:", record_action}, {"clickHelp:", record_action}, {"closeHelp:", record_action}};
const mt_objc_host_class k_main_menu_view = {"MainMenuView", "UIView", 46, k_menu_methods, 3,
                                             nullptr,        0};

double g_now = 1000;
double test_clock(void *) { return g_now; }

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// As input_test.cpp's: a 320x480 portrait window holding a view turned to landscape like the
// EAGLView (`root`: its local space is the 480x320 screen).
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    u32 root = 0;

    Guest() {
        g_actions.clear();
        g_now = 1000;
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = quiet;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        REQUIRE(mt_objc_add_host_class(rt, &k_main_menu_view, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        mt_ui::State &st = mt_ui::state(&cpu);
        st.config.clock = test_clock;
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        const u32 window = view("UIWindow", 0, 0, 320, 480);
        st.windows.push_back(window);
        root = view("UIView", 0, 0, 320, 480);
        send(root, "setTransform:", {fb(0), fb(-1), fb(1), fb(0), fb(0), fb(0)});
        send(root, "setBounds:", {fb(0), fb(0), fb(480), fb(320)});
        send(window, "addSubview:", {root});
        mt_objc_release(&cpu, root);
    }
    ~Guest() {
        mt_objc_pool_pop(rt, pool);
        mt_ui::state(&cpu).config.clock = nullptr;
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    u32 view(const char *cls, float x, float y, float w, float h) {
        return send(send(klass(cls), "alloc"), "initWithFrame:", {fb(x), fb(y), fb(w), fb(h)});
    }
    u32 add(u32 parent, const char *cls, float x, float y, float w, float h) {
        const u32 v = view(cls, x, y, w, h);
        send(parent, "addSubview:", {v});
        mt_objc_release(&cpu, v);
        return v;
    }
    u32 button(u32 parent, float x, float y, float w, float h, u32 target, const char *action) {
        const u32 b = send(klass("UIButton"), "buttonWithType:", {0});
        send(b, "setFrame:", {fb(x), fb(y), fb(w), fb(h)});
        send(parent, "addSubview:", {b});
        send(b, "addTarget:action:forControlEvents:",
             {target, mt_objc_sel(rt, action), mt_ui::k_event_touch_up_inside});
        return b;
    }
    std::vector<mt_uikit_control> controls() {
        std::vector<mt_uikit_control> c(32);
        const u32 n = mt_uikit_controls(&cpu, c.data(), static_cast<u32>(c.size()));
        REQUIRE(n <= c.size());
        c.resize(n);
        return c;
    }
    void turn(double dt = 1.0 / 60) {
        g_now += dt;
        mt_uikit_turn(&cpu, g_now);
    }
};

const mt_uikit_control *find(const std::vector<mt_uikit_control> &c, u32 object) {
    for (const mt_uikit_control &m : c) {
        if (m.object == object) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("focus: mt_uikit_controls lists what a tap reaches, as the hit test does") {
    Guest g;
    const u32 menu = g.add(g.root, "MainMenuView", 0, 0, 480, 320);
    const u32 play = g.button(menu, 46, 148, 120, 48, menu, "clickPlay:");
    const u32 help = g.button(menu, 46, 198, 120, 48, menu, "clickHelp:");
    // Half off the menu's right edge: clipped to what a touch can hit.
    const u32 edge = g.button(menu, 440, 10, 80, 40, menu, "closeHelp:");

    std::vector<mt_uikit_control> c = g.controls();
    REQUIRE(c.size() == 3);
    const mt_uikit_control *p = find(c, play);
    REQUIRE(p != nullptr);
    CHECK(p->kind == MT_UIKIT_CONTROL_BUTTON);
    CHECK(std::string(p->action) == "clickPlay:");
    CHECK(std::string(p->target) == "MainMenuView");
    CHECK(std::string(p->view_class) == "UIButton");
    CHECK(p->rect[0] == 46);
    CHECK(p->rect[1] == 148);
    CHECK(p->rect[2] == 120);
    CHECK(p->rect[3] == 48);
    CHECK(p->point[0] == 106);
    CHECK(p->point[1] == 172);
    const mt_uikit_control *e = find(c, edge);
    REQUIRE(e != nullptr);
    CHECK(e->rect[0] == 440);
    CHECK(e->rect[2] == 40);
    CHECK(find(c, help)->order > p->order); // tree order

    // Hidden, faded out, disabled, or in a subtree that takes no touches: not reachable.
    g.send(play, "setHidden:", {1});
    g.send(help, "setAlpha:", {fb(0)});
    g.send(edge, "setEnabled:", {0});
    CHECK(g.controls().empty());
    g.send(play, "setHidden:", {0});
    g.send(help, "setAlpha:", {fb(1)});
    g.send(edge, "setEnabled:", {1});
    g.send(menu, "setUserInteractionEnabled:", {0});
    CHECK(g.controls().empty());
    g.send(menu, "setUserInteractionEnabled:", {1});
    CHECK(g.controls().size() == 3);

    // A view over Play's left two thirds: Play is reached on its right, the point moves there.
    const u32 cover = g.add(g.root, "UIView", 0, 140, 126, 60);
    c = g.controls();
    p = find(c, play);
    REQUIRE(p != nullptr);
    CHECK(p->point[0] > 126);
    CHECK(p->point[1] >= 148);
    // Over all of it: gone. A cover that takes no touches hides nothing.
    g.send(cover, "setFrame:", {fb(0), fb(140), fb(480), fb(60)});
    CHECK(find(g.controls(), play) == nullptr);
    g.send(cover, "setUserInteractionEnabled:", {0});
    CHECK(find(g.controls(), play) != nullptr);

    // A scroll view: its offsets, the page when paging.
    const u32 list = g.add(g.root, "UIScrollView", 200, 220, 200, 100);
    g.send(list, "setContentSize:", {fb(600), fb(100)});
    g.send(list, "setPagingEnabled:", {1});
    c = g.controls();
    const mt_uikit_control *s = find(c, list);
    REQUIRE(s != nullptr);
    CHECK(s->kind == MT_UIKIT_CONTROL_SCROLL);
    CHECK(s->max_offset[0] == 400);
    CHECK(s->max_offset[1] == 0);
    CHECK(s->page[0] == 200);
    CHECK(std::string(s->action).empty());
    // More than `max`: the count, nothing written past it.
    mt_uikit_control one[1];
    CHECK(mt_uikit_controls(&g.cpu, one, 1) == static_cast<u32>(c.size()));
    CHECK(mt_uikit_controls(&g.cpu, nullptr, 0) == static_cast<u32>(c.size()));
}

TEST_CASE("focus: the whole chain, from actions to the buttons' actions") {
    Guest g;
    const u32 menu = g.add(g.root, "MainMenuView", 0, 0, 480, 320);
    g.button(menu, 46, 148, 120, 48, menu, "clickPlay:");
    g.button(menu, 46, 198, 120, 48, menu, "clickHelp:");
    fe::Input input; // into mt_uikit_touch
    fe::Focus focus{&input};
    std::vector<std::string> forwarded;
    focus.set_forward(
        [](void *user, u32, bool) {
            static_cast<std::vector<std::string> *>(user)->push_back("forwarded");
        },
        &forwarded);
    focus.set_panel_source([](void *) { return std::int32_t{-1}; }, nullptr);
    const auto turn = [&] {
        g.turn();
        focus.turn(&g.cpu, g_now);
    };
    const auto press = [&](u32 action) {
        focus.action(action, true);
        turn();
        focus.action(action, false);
        turn();
        turn();
        turn();
    };
    turn();
    REQUIRE(focus.active());
    REQUIRE(focus.focused() != nullptr);
    CHECK(focus.focused()->action == "clickPlay:");
    press(MT_ACTION_DOWN);
    press(MT_ACTION_OK);
    CHECK(g_actions == std::vector<std::string>{"clickHelp:"});
    // Help's screen over the menu, with its close: back taps it.
    const u32 help = g.add(g.root, "MainMenuView", 0, 0, 480, 320);
    g.button(help, 400, 20, 40, 40, help, "closeHelp:");
    turn();
    CHECK(focus.focused()->action == "closeHelp:");
    press(MT_ACTION_BACK);
    CHECK(g_actions == std::vector<std::string>{"clickHelp:", "closeHelp:"});
    g.send(help, "removeFromSuperview");
    turn();
    CHECK(focus.focused()->action == "clickHelp:");
    CHECK(input.live() == 0);
    CHECK(forwarded.empty());
}
