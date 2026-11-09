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

namespace {

// The popup menu's drop spots as the game lays them out (hit rects, keypad.md): four quick slots
// on the right (values 4, 3, 2, 1 from the top), the attack key's (value 0) below them, the trash
// on the left.
std::vector<FocusTarget> drop_spots() {
    std::vector<FocusTarget> spots;
    const struct {
        float x, y, w, h;
        u32 value;
        const char *action;
    } rows[] = {{430, 80, 50, 40, 4, "slot"},  {430, 126, 50, 40, 3, "slot"},
                {430, 172, 50, 40, 2, "slot"}, {430, 218, 50, 40, 1, "slot"},
                {400, 274, 80, 40, 0, "slot"}, {20, 220, 50, 60, 0xFFFFFFFFu, "trash"}};
    u32 object = 200;
    for (const auto &row : rows) {
        FocusTarget t = button(object++, row.x, row.y, row.w, row.h, row.action);
        t.order = row.value;
        spots.push_back(t);
    }
    return spots;
}

} // namespace

TEST_CASE("focus: carry navigation: quick slots by y, left to the trash, right back") {
    const std::vector<FocusTarget> s = drop_spots();
    const int top = 0, slot2 = 2, slot1 = 3, attack = 4, trash = 5;
    CHECK(fe::carry_first_spot(s) == slot1);
    CHECK(fe::carry_first_spot({}) == -1);
    CHECK(fe::carry_next(s, slot1, Direction::kUp) == slot2);
    CHECK(fe::carry_next(s, slot1, Direction::kDown) == attack);
    CHECK(fe::carry_next(s, attack, Direction::kDown) == -1);
    CHECK(fe::carry_next(s, top, Direction::kUp) == -1);
    CHECK(fe::carry_next(s, slot1, Direction::kLeft) == trash);
    CHECK(fe::carry_next(s, attack, Direction::kLeft) == trash);
    // The trash: right to the quick slot beside it; up and down go nowhere.
    CHECK(fe::carry_next(s, trash, Direction::kRight) == slot1);
    CHECK(fe::carry_next(s, trash, Direction::kUp) == -1);
    CHECK(fe::carry_next(s, -1, Direction::kUp) == -1);
    CHECK(fe::popup_menu_carry_tab(1));
    CHECK(fe::popup_menu_carry_tab(3));
    CHECK_FALSE(fe::popup_menu_carry_tab(0));
    CHECK_FALSE(fe::popup_menu_carry_tab(4));
}

TEST_CASE("focus: pick up and drop (#140): a touch on the cell, the spots, a touch on the drop") {
    Rig rig;
    fe::CarryView view; // what the guest shows, read through the carry source
    rig.panel = 1;
    rig.focus.set_carry_source(
        [](void *user, mt_cpu *, fe::CarryView *out) {
            *out = *static_cast<fe::CarryView *>(user);
            return true;
        },
        &view);

    // No cursor (the tab bar): the press and release go on to the game as its key.
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, true);
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, false);
    rig.turn();
    CHECK_FALSE(rig.focus.carrying());
    CHECK(rig.forwarded == Lines{"revive down", "revive up"});
    CHECK(rig.take_touches().empty());
    CHECK(rig.focus.stats().picks == 0);

    // The item tab with a cursor: the press picks up, a touch on the cell.
    view.open = 1;
    view.cursor = true;
    view.cell = fe::Rect{83, 62, 70, 36};
    view.spots = drop_spots();
    rig.forwarded.clear();
    rig.focus.action(MT_ACTION_REVIVE, true);
    rig.turn();
    CHECK(rig.focus.carrying());
    CHECK(rig.focus.stats().picks == 1);
    REQUIRE(rig.focus.carry_target() != nullptr);
    CHECK(rig.focus.carry_target()->order == 1); // the spot on quick slot 1
    CHECK(rig.take_touches() == Lines{"0x1036 began (118, 80)"});
    CHECK(rig.forwarded.empty());
    // The touch stays down past a tick (kCarryHold), then ends.
    for (int i = 0; i < 12 && rig.touches.empty(); i++) {
        rig.turn();
    }
    CHECK(rig.take_touches() == Lines{"0x1036 ended (118, 80)"});
    rig.focus.action(MT_ACTION_REVIVE, false);
    rig.turn();
    CHECK(rig.forwarded.empty()); // its release is taken with its press

    // The directions move over the spots (and repeat is the player's own): up to slot 2, left to
    // the trash. Taken, not forwarded.
    rig.press(MT_ACTION_UP);
    CHECK(rig.focus.carry_target()->order == 2);
    rig.press(MT_ACTION_LEFT);
    CHECK(rig.focus.carry_target()->action == "trash");
    rig.press(MT_ACTION_RIGHT);
    CHECK(rig.focus.carry_target()->action == "slot");
    CHECK(rig.forwarded.empty());
    CHECK(rig.take_touches().empty());

    // OK drops: a touch on the spot's point; the mode ends.
    rig.press(MT_ACTION_OK);
    CHECK_FALSE(rig.focus.carrying());
    CHECK(rig.focus.stats().drops == 1);
    Lines drop = rig.take_touches();
    for (int i = 0; i < 12 && rig.touches.empty(); i++) {
        rig.turn();
    }
    const Lines end = rig.take_touches();
    drop.insert(drop.end(), end.begin(), end.end());
    CHECK(drop == Lines{"0x1036 began (455, 238)", "0x1036 ended (455, 238)"});
    CHECK(rig.forwarded.empty());

    // Back cancels without a touch; leaving the tab ends it too.
    rig.focus.action(MT_ACTION_REVIVE, true);
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, false);
    REQUIRE(rig.focus.carrying());
    rig.press(MT_ACTION_BACK);
    CHECK_FALSE(rig.focus.carrying());
    CHECK(rig.focus.stats().carry_cancels == 1);
    rig.take_touches();
    rig.focus.action(MT_ACTION_REVIVE, true);
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, false);
    rig.turn();
    REQUIRE(rig.focus.carrying());
    view.open = 0;
    rig.turn();
    CHECK_FALSE(rig.focus.carrying());
    CHECK(rig.focus.stats().carry_cancels == 2);

    // The same cell picked up again within the double tap: no second touch (a double tap would
    // use the item; the selection stands).
    view.open = 1;
    for (int i = 0; i < 40; i++) {
        rig.turn();
    }
    rig.take_touches();
    rig.focus.action(MT_ACTION_REVIVE, true);
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, false);
    CHECK(rig.take_touches() == Lines{"0x1036 began (118, 80)"});
    for (int i = 0; i < 12; i++) {
        rig.turn();
    }
    rig.take_touches();
    rig.press(MT_ACTION_BACK);
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE,
                     true); // again within 0.4 s of the first touch's end... a tap
    rig.turn();
    rig.focus.action(MT_ACTION_REVIVE, false);
    rig.turn();
    CHECK(rig.focus.carrying());
    CHECK(rig.take_touches().empty());
}

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
    // The other actions are still the game's keys (the keyboard's N is New), except map ('*'),
    // which would start an invisible delete on the character select.
    r.press(MT_ACTION_SLOT4);
    CHECK(r.forwarded == Lines{"slot4 down", "slot4 up"});
    r.press(MT_ACTION_MAP);
    CHECK(r.forwarded == Lines{"slot4 down", "slot4 up"});
    CHECK(r.focus.stats().dropped == 1);
    CHECK(r.touches.empty());
    r.forwarded.clear();

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

TEST_CASE("focus: before the engine's main menu a confirm is dropped, touches are not (#129)") {
    Rig r;
    bool title = true; // the engine still in CStateTitle
    r.focus.set_title_source([](void *user, mt_cpu *) { return *static_cast<bool *>(user); },
                             &title);
    r.panel = -1;
    r.targets = {button(1, 46, 148, 120, 48, "clickPlay:"),
                 button(2, 46, 198, 120, 48, "clickHelp:"),
                 button(3, 445, 210, 35, 35, "clickADClose:")};
    r.turn();
    REQUIRE(r.focus.active());
    REQUIRE(r.focused() == "clickPlay:");
    // OK on Play: no touch, nothing to the game (not even its release), no highlight.
    r.press(MT_ACTION_OK);
    CHECK(r.touches.empty());
    CHECK(r.forwarded.empty());
    CHECK(r.focus.stats().early == 1);
    CHECK(r.focus.stats().taps == 0);
    CHECK(r.focus.highlighted() == nullptr);
    // A press held across the change to the main menu stays dropped: its release does nothing.
    r.focus.action(MT_ACTION_OK, true);
    r.turn();
    title = false;
    r.turn();
    r.focus.action(MT_ACTION_OK, false);
    r.turn();
    r.turn();
    CHECK(r.touches.empty());
    CHECK(r.forwarded.empty());
    CHECK(r.focus.stats().early == 2);
    // Directions and back work all along.
    title = true;
    r.turn();
    r.press(MT_ACTION_DOWN);
    CHECK(r.focused() == "clickHelp:");
    CHECK(r.focus.highlighted() != nullptr);
    r.press(MT_ACTION_BACK);
    CHECK(r.take_touches() == Lines{"0x1031 began (462.5, 227.5)", "0x1031 ended (462.5, 227.5)"});
    r.press(MT_ACTION_UP);
    CHECK(r.focused() == "clickPlay:");
    // The main menu's state: OK is a touch on Play again.
    title = false;
    r.turn();
    r.press(MT_ACTION_OK);
    CHECK(r.take_touches() == Lines{"0x1030 began (106, 172)", "0x1030 ended (106, 172)"});
    CHECK(r.focus.stats().taps == 1);
    CHECK(r.focus.stats().early == 2);
    // A finger's touch never goes through the focus: Input passes it on whatever the state
    // (the device's behaviour, #122).
    title = true;
    r.turn();
    CHECK(r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, 106, 165));
    const Lines finger = r.take_touches();
    REQUIRE(finger.size() == 1);
    CHECK(finger[0].find("began (106, 165)") != std::string::npos);
}

TEST_CASE("focus: the engine's title state, read from guest memory") {
    std::vector<std::uint8_t> mem(0x120000);
    mt_cpu cpu{};
    cpu.mem = mem.data();
    cpu.mem_size = mem.size();
    const auto put = [&mem](u32 addr, u32 v) { std::memcpy(mem.data() + addr, &v, 4); };
    CHECK_FALSE(fe::focus_engine_at_title(nullptr));
    CHECK_FALSE(fe::focus_engine_at_title(&cpu)); // no SkyCanvas yet
    put(0x11A950, 0x100000);                      // SkyCanvas::GetInstance's canvas
    CHECK_FALSE(fe::focus_engine_at_title(&cpu)); // no state
    put(0x100000 + 0xC, 0x100100);                // its current state
    put(0x100100, 0x119C98);                      // a CStateTitle
    CHECK(fe::focus_engine_at_title(&cpu));
    put(0x100100, 0x119C08); // a CStateMainMenu
    CHECK_FALSE(fe::focus_engine_at_title(&cpu));
    // A canvas pointer out of the guest's memory: not the title.
    put(0x11A950, 0xFFFFFF00u);
    CHECK_FALSE(fe::focus_engine_at_title(&cpu));

    // A guest address space: nothing is read until a segment holds the global.
    mt_mem *space = mt_mem_create(nullptr);
    REQUIRE(space != nullptr);
    mt_cpu guest{};
    mt_mem_attach(space, &guest);
    CHECK_FALSE(fe::focus_engine_at_title(&guest)); // no image: no committed page there
    const mt_segment data{"__DATA", 0x118000u, 0x4000u, 0u, 0u, MT_PROT_READ | MT_PROT_WRITE};
    REQUIRE(mt_mem_map_segment(space, &data, nullptr, 0u) == 0);
    const u32 canvas = mt_heap_calloc(space, 1, 0xFC);
    const u32 title = mt_heap_calloc(space, 1, 0x2C);
    REQUIRE(canvas != 0);
    REQUIRE(title != 0);
    const auto gput = [&guest](u32 addr, u32 v) { std::memcpy(guest.mem + addr, &v, 4); };
    CHECK_FALSE(fe::focus_engine_at_title(&guest)); // mapped, no SkyCanvas yet
    gput(0x11A950, canvas);
    gput(canvas + 0xC, title);
    gput(title, 0x119C98);
    CHECK(fe::focus_engine_at_title(&guest));
    gput(title, 0x119C08);
    CHECK_FALSE(fe::focus_engine_at_title(&guest));
    mt_mem_destroy(space);
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

TEST_CASE("focus: a control is known by its object and its action") {
    Rig r;
    r.panel = -1;
    r.targets = {button(1, 46, 148, 120, 48, "clickPlay:"),
                 button(2, 46, 198, 120, 48, "clickHelp:")};
    r.turn();
    r.press(MT_ACTION_DOWN);
    REQUIRE(r.focused() == "clickHelp:");
    // A screen over it; then another screen whose Start button has the freed Help button's
    // address: the focus does not "return" to it, it takes the new screen's default.
    r.targets = {button(10, 400, 20, 40, 40, "closeHelp:")};
    r.turn();
    r.targets = {button(2, 400, 60, 40, 40, "clickStart:"),
                 button(11, 80, 200, 40, 40, "chooseCharacterOne:")};
    r.turn();
    CHECK(r.focused() == "chooseCharacterOne:");
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

TEST_CASE("focus: a skill or quest tab the d-pad opened gets the touch that shows its list") {
    Rig r;
    bool hidden = true;
    std::int32_t asked = -1;
    struct List {
        bool *hidden;
        std::int32_t *asked;
    } list{&hidden, &asked};
    r.focus.set_menu_list_source(
        [](void *user, mt_cpu *, std::int32_t tab) {
            List &l = *static_cast<List *>(user);
            *l.asked = tab;
            return *l.hidden;
        },
        &list);
    r.panel = 1;
    // The tab bar, the item tab chosen: nothing.
    r.tab = 1;
    r.turn();
    CHECK(r.touches.empty());
    // Right reached the skill tab, which Menu_OnKey opened: a tap on it, once.
    r.tab = 3;
    r.open = 3;
    r.turn();
    CHECK(asked == 3);
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (264, 277)", "0x1030 ended (264, 277)"});
    CHECK(r.focus.stats().menu_tabs == 1);
    // Still hidden (the touch not taken yet): not again while the tab stays open.
    r.turn();
    r.turn();
    CHECK(r.touches.empty());
    // On to the quest tab: its tap.
    r.tab = 4;
    r.open = 4;
    r.turn();
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (318, 277)", "0x1030 ended (318, 277)"});
    // Back to the skill tab with its list shown: nothing. The option tab has no such list.
    hidden = false;
    r.tab = 3;
    r.open = 3;
    r.turn();
    r.tab = 5;
    r.open = 5;
    r.turn();
    r.turn();
    CHECK(r.touches.empty());
    // A tab chosen but not open (the tab bar), or another panel: nothing.
    hidden = true;
    r.tab = 3;
    r.open = 0;
    r.turn();
    r.panel = 0;
    r.open = 3;
    r.turn();
    r.turn();
    CHECK(r.touches.empty());
    CHECK(r.focus.stats().menu_tabs == 2);
    CHECK(r.forwarded.empty());

    // The runtime's reader: the widget's +0xB1 behind the skill (0xF14A8) and quest (0xF14B4)
    // tabs' globals.
    std::vector<std::uint8_t> mem(0x200000, 0);
    mt_cpu cpu{};
    cpu.mem = mem.data();
    cpu.mem_size = static_cast<u32>(mem.size());
    const auto put = [&mem](u32 a, u32 v) { std::memcpy(&mem[a], &v, 4); };
    put(0xF14A8, 0x180000);
    put(0x180000, 0x181000);
    CHECK(fe::popup_menu_list_hidden(&cpu, 3));
    mem[0x181000 + 0xB1] = 1;
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 3));
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 4)); // no widget
    CHECK_FALSE(fe::popup_menu_list_hidden(nullptr, 3));
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 0)); // status, option: no such list
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 5));
}

TEST_CASE("focus: an item or equipment tab that down opened gets the touch that shows its list") {
    // Down (-2) on the tab bar opens the tab (Menu_OnKey sets +0xA8) without the touch that
    // shows its list: the item list stayed empty and the equipment tab showed the doll alone.
    Rig r;
    bool hidden = true;
    r.focus.set_menu_list_source(
        [](void *user, mt_cpu *, std::int32_t) { return *static_cast<bool *>(user); }, &hidden);
    r.panel = 1;
    r.tab = 1;
    r.open = 0;
    r.press(MT_ACTION_DOWN); // the game's key
    CHECK(r.forwarded == Lines{"down down", "down up"});
    r.open = 1; // as Menu_OnKey leaves it
    r.turn();
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (156, 277)", "0x1030 ended (156, 277)"});
    hidden = false; // the touch showed it: nothing more
    r.turn();
    CHECK(r.touches.empty());
    // The equipment tab the same way.
    r.forwarded.clear();
    r.open = 0;
    r.turn();
    r.tab = 2;
    r.press(MT_ACTION_DOWN);
    r.open = 2;
    hidden = true;
    r.turn();
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (210, 277)", "0x1030 ended (210, 277)"});
    CHECK(r.focus.stats().menu_tabs == 2);

    // The runtime's reader: the item (0xF14B8) and equipment (0xF14C0) lists' +0xB1.
    std::vector<std::uint8_t> mem(0x200000, 0);
    mt_cpu cpu{};
    cpu.mem = mem.data();
    cpu.mem_size = static_cast<u32>(mem.size());
    const auto put = [&mem](u32 a, u32 v) { std::memcpy(&mem[a], &v, 4); };
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 1)); // no widget yet
    put(0xF14B8, 0x180000);
    put(0x180000, 0x181000);
    put(0xF14C0, 0x182000);
    put(0x182000, 0x183000);
    CHECK(fe::popup_menu_list_hidden(&cpu, 1));
    CHECK(fe::popup_menu_list_hidden(&cpu, 2));
    mem[0x181000 + 0xB1] = 1;
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 1));
    CHECK(fe::popup_menu_list_hidden(&cpu, 2));
    mem[0x183000 + 0xB1] = 1;
    CHECK_FALSE(fe::popup_menu_list_hidden(&cpu, 2));
}

TEST_CASE("focus: OK's touch on the item or equipment tab gets no second touch") {
    // OK on the tab bar is a touch on the tab (menu_ok). The tab opens, but its list reads hidden
    // until the game takes the touch's release, a turn after the focus ends it: menu_list must not
    // touch the tab again (that second touch left the item and equipment lists empty).
    Rig r;
    bool hidden = true;
    r.focus.set_menu_list_source(
        [](void *user, mt_cpu *, std::int32_t) { return *static_cast<bool *>(user); }, &hidden);
    r.panel = 1;
    for (const std::int32_t tab : {1, 2}) {
        r.tab = tab;
        r.open = 0;
        hidden = true;
        r.turn();
        r.focus.action(MT_ACTION_OK, true);
        r.turn();
        r.open = tab; // the touch's began opened it
        r.focus.action(MT_ACTION_OK, false);
        r.turn();
        r.turn(); // the release ended; the list still reads hidden
        r.turn();
        hidden = false;
        r.turn();
        const Lines touches = r.take_touches();
        REQUIRE(touches.size() == 2);
        CHECK(touches[0].find("began") != std::string::npos);
        CHECK(touches[1].find("ended") != std::string::npos);
    }
    CHECK(r.focus.stats().menu_tabs == 2);
    // Closed, then down opens the item tab with its list hidden: the focus's touch, as before.
    r.open = 0;
    r.turn();
    r.tab = 1;
    r.press(MT_ACTION_DOWN);
    r.open = 1;
    hidden = true;
    r.turn();
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (156, 277)", "0x1030 ended (156, 277)"});
}

TEST_CASE("focus: a chosen character slot hands the focus to Start (ADR 0102)") {
    CHECK(std::string(fe::focus_after_confirm("chooseCharacterOne:")) == "clickStart:");
    CHECK(std::string(fe::focus_after_confirm("chooseCharacterTwo:")) == "clickStart:");
    CHECK(std::string(fe::focus_after_confirm("chooseCharacterThree:")) == "clickStart:");
    CHECK(fe::focus_after_confirm("clickStart:") == nullptr);
    CHECK(fe::focus_after_confirm("clickPlay:") == nullptr);

    // ChooseCharacterView: the three slots, Back, and the column Start, New, Delete.
    Rig r;
    r.panel = -1;
    r.targets = {button(1, 73, 193, 53, 78, "chooseCharacterOne:"),
                 button(2, 163, 193, 53, 78, "chooseCharacterTwo:"),
                 button(3, 253, 193, 53, 78, "chooseCharacterThree:"),
                 button(4, 398, 5, 82, 29, "clickBack:"),
                 button(5, 360, 55, 120, 48, "clickStart:"),
                 button(6, 360, 105, 120, 48, "clickNew:"),
                 button(7, 360, 155, 120, 48, "clickDelete:")};
    r.turn();
    REQUIRE(r.focus.active());
    REQUIRE(r.focused() == "chooseCharacterOne:");
    r.press(MT_ACTION_RIGHT);
    REQUIRE(r.focused() == "chooseCharacterTwo:");
    r.take_touches();
    // OK on the slot: its tap, then the focus on Start, so a second OK starts the game.
    r.press(MT_ACTION_OK);
    CHECK(r.take_touches() == Lines{"0x1030 began (189.5, 232)", "0x1030 ended (189.5, 232)"});
    CHECK(r.focused() == "clickStart:");
    CHECK(r.focus.stats().follows == 1);
    r.press(MT_ACTION_OK);
    CHECK(r.take_touches() == Lines{"0x1030 began (420, 79)", "0x1030 ended (420, 79)"});
    CHECK(r.focused() == "clickStart:");
    CHECK(r.focus.stats().follows == 1);

    // A slot with no Start on screen: the focus stays on the slot.
    r.targets = {button(1, 73, 193, 53, 78, "chooseCharacterOne:"),
                 button(4, 398, 5, 82, 29, "clickBack:")};
    r.turn();
    r.press(MT_ACTION_LEFT);
    REQUIRE(r.focused() == "chooseCharacterOne:");
    r.press(MT_ACTION_OK);
    CHECK(r.focused() == "chooseCharacterOne:");
    CHECK(r.focus.stats().follows == 1);
}

TEST_CASE("focus: LB / RB open the item and equipment tabs, and leave them") {
    CHECK(fe::popup_menu_shoulder_tab(1, true) == 2);
    CHECK(fe::popup_menu_shoulder_tab(1, false) == 0);
    CHECK(fe::popup_menu_shoulder_tab(2, true) == 3);
    CHECK(fe::popup_menu_shoulder_tab(2, false) == 1);
    CHECK(fe::popup_menu_shoulder_tab(0, true) == -1); // the game's keys work there
    CHECK(fe::popup_menu_shoulder_tab(3, false) == -1);
    CHECK(fe::popup_menu_bar_opens(1));
    CHECK(fe::popup_menu_bar_opens(2));
    CHECK_FALSE(fe::popup_menu_bar_opens(0));
    CHECK_FALSE(fe::popup_menu_bar_opens(3));

    Rig r;
    r.panel = 1;
    r.turn();
    // RB on the status tab: the key goes to the game, which selects the item tab with nothing
    // open (a blank panel); the focus touches the tab, which opens it.
    r.press(MT_ACTION_PLUS);
    CHECK(r.forwarded == Lines{"plus down", "plus up"});
    r.tab = 1;
    r.turn();
    r.turn();
    CHECK(r.take_touches() == Lines{"0x1030 began (156, 277)", "0x1030 ended (156, 277)"});
    r.open = 1;
    r.turn();
    CHECK(r.take_touches().empty()); // once: its list is shown by that touch
    // RB in the open item tab, which ignores it: a touch on the equipment tab; the key never goes.
    r.forwarded.clear();
    r.press(MT_ACTION_PLUS);
    CHECK(r.forwarded.empty());
    CHECK(r.take_touches() == Lines{"0x1030 began (210, 277)", "0x1030 ended (210, 277)"});
    r.tab = 2;
    r.open = 2;
    r.turn();
    // LB twice: the item tab, then the status tab, each by a touch.
    r.press(MT_ACTION_MINUS);
    CHECK(r.take_touches() == Lines{"0x1030 began (156, 277)", "0x1030 ended (156, 277)"});
    r.tab = 1;
    r.open = 1;
    r.turn();
    r.press(MT_ACTION_MINUS);
    CHECK(r.take_touches() == Lines{"0x1030 began (102, 277)", "0x1030 ended (102, 277)"});
    r.tab = 0;
    r.open = 0;
    r.turn();
    r.turn();
    CHECK(r.forwarded.empty());
    CHECK(r.take_touches().empty());
    // Back from an open item tab leaves the bar on it: no touch reopens it.
    r.tab = 1;
    r.open = 1;
    r.turn();
    r.press(MT_ACTION_BACK);
    r.open = 0;
    r.turn();
    r.turn();
    CHECK(r.take_touches().empty());
    // The d-pad's right selecting the equipment tab is the game's key alone: no touch (inside
    // an open grid, right moves its cursor).
    r.press(MT_ACTION_RIGHT);
    r.tab = 2;
    r.turn();
    r.turn();
    CHECK(r.take_touches().empty());
    // RB from the bar's last tab (no move), then right: the d-pad's move opens nothing.
    r.tab = 5;
    r.turn();
    r.press(MT_ACTION_PLUS);
    r.turn();
    r.press(MT_ACTION_LEFT);
    r.tab = 1;
    r.turn();
    r.turn();
    CHECK(r.take_touches().empty());
    CHECK(r.focus.stats().menu_tabs == 4);
}

TEST_CASE("focus: up and down choose the NPC chat's quest rows by touches (#77)") {
    CHECK(fe::chat_row_of("releaseQuest1Button:") == 0);
    CHECK(fe::chat_row_of("releaseQuest6Button:") == 5);
    CHECK(fe::chat_row_of("releaseQuest7Button:") == -1);
    CHECK(fe::chat_row_of("pressQuest1Button:") == -1);
    CHECK(fe::chat_row_of("releaseQuestButton:") == -1);
    Rig r;
    r.panel = 5; // NPC chat: key codes, no focus
    // A page of text: up and down are the game's keys.
    r.targets = {button(1, 192, 213, 80, 30, "releaseOkeyButton:")};
    r.targets[0].target = "NpcKeyPadView";
    r.turn();
    REQUIRE_FALSE(r.focus.active());
    r.press(MT_ACTION_DOWN);
    CHECK(r.forwarded == Lines{"down down", "down up"});
    r.forwarded.clear();
    // The quest list: three rows (listed out of order), each a button of the NPC key pad.
    r.targets.clear();
    for (const auto &[object, row] : {std::pair<u32, int>{12, 2}, {10, 0}, {11, 1}}) {
        const std::string action = "releaseQuest" + std::to_string(row + 1) + "Button:";
        FocusTarget t = button(object, 192, 122 + 20.0f * static_cast<float>(row), 161, 20, "");
        t.action = action;
        t.target = "NpcKeyPadView";
        r.targets.push_back(t);
    }
    r.turn();
    CHECK(fe::chat_rows(r.targets) == std::vector<int>{1, 2, 0});
    CHECK_FALSE(r.focus.active());
    // Down: a tap on the second row (the game starts on the first); again: the third; past the
    // last nothing moves on; up: the second. The game sees no key.
    r.press(MT_ACTION_DOWN);
    CHECK(r.take_touches() == Lines{"0x1030 began (272.5, 152)", "0x1030 ended (272.5, 152)"});
    r.press(MT_ACTION_DOWN);
    CHECK(r.take_touches() == Lines{"0x1030 began (272.5, 172)", "0x1030 ended (272.5, 172)"});
    r.press(MT_ACTION_DOWN);
    CHECK(r.take_touches() == Lines{"0x1030 began (272.5, 172)", "0x1030 ended (272.5, 172)"});
    r.press(MT_ACTION_UP);
    CHECK(r.take_touches() == Lines{"0x1030 began (272.5, 152)", "0x1030 ended (272.5, 152)"});
    CHECK(r.forwarded.empty());
    CHECK(r.focus.stats().chat_rows == 4);
    // OK chooses the row: the game's key.
    r.press(MT_ACTION_OK);
    CHECK(r.forwarded == Lines{"ok down", "ok up"});
    CHECK(r.touches.empty());
    // The rows go and come back: a new list starts on its first row again.
    const std::vector<FocusTarget> rows = r.targets;
    r.targets.clear();
    r.turn();
    r.targets = rows;
    r.turn();
    r.press(MT_ACTION_DOWN);
    CHECK(r.take_touches() == Lines{"0x1030 began (272.5, 152)", "0x1030 ended (272.5, 152)"});
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
