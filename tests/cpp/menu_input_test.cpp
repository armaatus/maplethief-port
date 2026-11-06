// Menu repeat and the popup confirm guard (#147, ADR 0051's addendum) on synthetic data only:
// MenuInput between a scripted panel, popup id, clock and runtime queue and a recording sink,
// with real Actions (the gamepad's and the keyboard's default rows) in front of it for the
// guard's "still held from the field". Then the popup reader on synthetic guest memory and on a
// guest address space with and without a mapped segment.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "actions.h"
#include "input.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "menu_input.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;

struct Event {
    u32 action;
    bool down;
    double at;
    bool operator==(const Event &o) const {
        return action == o.action && down == o.down && at == o.at;
    }
};

// MenuInput on scripted sources, fed by Actions with the default bindings.
struct Rig {
    double now = 0;
    std::int32_t panel = 0;
    std::int32_t popup = -1;
    bool busy = false;
    bool list = false;      // a list that pages owns the focus (the page sink's answer)
    std::vector<int> pages; // the pages applied (-1 up, +1 down)
    std::vector<double> page_at;
    std::vector<Event> sent;
    fe::Input input;
    fe::Actions actions{&input};
    fe::MenuInput menu;

    Rig() {
        menu.set_forward(
            [](void *u, u32 a, bool down) {
                Rig &r = *static_cast<Rig *>(u);
                r.sent.push_back(Event{a, down, r.now});
            },
            this);
        menu.set_panel_source([](void *u) { return static_cast<Rig *>(u)->panel; }, this);
        menu.set_popup_source([](void *u, mt_cpu *) { return static_cast<Rig *>(u)->popup; }, this);
        menu.set_clock([](void *u) { return static_cast<Rig *>(u)->now; }, this);
        menu.set_busy_source([](void *u) { return static_cast<Rig *>(u)->busy; }, this);
        menu.set_page_sink(
            [](void *u, mt_cpu *, std::int32_t, int dir, bool apply) {
                Rig &r = *static_cast<Rig *>(u);
                if (r.list && apply) {
                    r.pages.push_back(dir);
                    r.page_at.push_back(r.now);
                }
                return r.list;
            },
            this);
        menu.set_actions(&actions);
        actions.set_panel_source([](void *u) { return static_cast<Rig *>(u)->panel; }, this);
        actions.set_sink(fe::MenuInput::sink, &menu);
    }
    // Runs the turns up to `t` seconds, one per millisecond (the scripted runs' fixed step), on
    // whole milliseconds.
    void until(double t) {
        while (now + 0.0005 < t) {
            now = std::round(now * 1000 + 1) / 1000;
            menu.turn(nullptr, now);
        }
    }
    void pad(SDL_GamepadButton b, bool down) {
        actions.control(fe::InputSource::kGamepad,
                        fe::Control{fe::ControlKind::kGamepadButton, static_cast<int>(b)}, down);
    }
    void dir(fe::Direction d, bool down) {
        actions.control(fe::InputSource::kGamepad,
                        fe::Control{fe::ControlKind::kDirection, static_cast<int>(d)}, down);
    }
    void lt(bool down) {
        actions.control(fe::InputSource::kGamepad,
                        fe::Control{fe::ControlKind::kGamepadTrigger,
                                    static_cast<int>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)},
                        down);
    }
    void key(SDL_Keycode k, bool down) {
        actions.control(fe::InputSource::kKeyboard,
                        fe::Control{fe::ControlKind::kKey, static_cast<int>(k)}, down);
    }
    // The presses of `action` sent, by time.
    std::vector<double> presses(u32 action) const {
        std::vector<double> out;
        for (const Event &e : sent) {
            if (e.action == action && e.down) {
                out.push_back(e.at);
            }
        }
        return out;
    }
    std::size_t count(u32 action, bool down) const {
        std::size_t n = 0;
        for (const Event &e : sent) {
            n += e.action == action && e.down == down ? 1 : 0;
        }
        return n;
    }
};

bool near(double a, double b) { return a > b - 0.0015 && a < b + 0.0015; }

} // namespace

TEST_CASE("menu input: the panels, the guarded actions, the defaults") {
    for (const std::int32_t p : {1, 5, 6, 8}) {
        CHECK(fe::menu_repeat_panel(p));
    }
    for (const std::int32_t p : {-1, 0, 3, 7, 9, 10}) {
        CHECK_FALSE(fe::menu_repeat_panel(p));
    }
    for (const u32 a : {MT_ACTION_OK, MT_ACTION_BACK, MT_ACTION_REVIVE, MT_ACTION_GRADE_REVIVE}) {
        CHECK(fe::confirm_guarded(a));
    }
    for (const u32 a : {MT_ACTION_JUMP, MT_ACTION_ATTACK, MT_ACTION_UP, MT_ACTION_MENU,
                        MT_ACTION_MAP, MT_ACTION_PLUS, MT_ACTION_SLOT4}) {
        CHECK_FALSE(fe::confirm_guarded(a));
    }
    fe::MenuInput m;
    CHECK_FALSE(m.repeat()); // the original: no repeat, no guard
    CHECK_FALSE(m.guard());
    CHECK(fe::MenuInput::kRepeatDelay == 0.40);
    CHECK(fe::MenuInput::kRepeatEvery == 0.15);
    CHECK(fe::MenuInput::kGuardTime == 0.30);
}

TEST_CASE("menu input: off, everything goes through as it came") {
    Rig r;
    r.panel = 6;
    r.dir(fe::Direction::kDown, true);
    r.until(2.0);
    r.dir(fe::Direction::kDown, false);
    r.popup = 3;
    r.until(2.01);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true); // OK, 10 ms after the popup
    r.until(2.05);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    const std::vector<Event> want = {{MT_ACTION_DOWN, true, 0},
                                     {MT_ACTION_DOWN, false, 2.0},
                                     {MT_ACTION_OK, true, 2.01},
                                     {MT_ACTION_OK, false, 2.05}};
    REQUIRE(r.sent.size() == want.size());
    for (std::size_t i = 0; i < want.size(); i++) {
        CHECK(r.sent[i].action == want[i].action);
        CHECK(r.sent[i].down == want[i].down);
        CHECK(near(r.sent[i].at, want[i].at));
    }
    CHECK(r.menu.stats().repeats == 0);
    CHECK(r.menu.stats().guarded == 0);
}

TEST_CASE("menu input: a held direction repeats after 0.40 s, then every 0.15 s") {
    for (const std::int32_t panel : {1, 5, 6, 8}) {
        CAPTURE(panel);
        Rig r;
        r.menu.set_repeat(true);
        r.panel = panel;
        r.now = 1.0;
        r.dir(fe::Direction::kDown, true);
        r.until(2.05);
        // The press at 1.0, repeats at 1.40, 1.55, 1.70, 1.85, 2.00.
        const std::vector<double> got = r.presses(MT_ACTION_DOWN);
        const std::vector<double> want = {1.0, 1.40, 1.55, 1.70, 1.85, 2.00};
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); i++) {
            CHECK(near(got[i], want[i]));
        }
        // Each repeat is a release, then a press, at the same turn.
        CHECK(r.count(MT_ACTION_DOWN, false) == 5);
        for (std::size_t i = 1; i < r.sent.size(); i += 2) {
            CHECK_FALSE(r.sent[i].down);
            CHECK(r.sent[i + 1].down);
        }
        CHECK(r.menu.stats().repeats == 5);
        // Let go: no more, and one release.
        r.dir(fe::Direction::kDown, false);
        r.until(3.0);
        CHECK(r.presses(MT_ACTION_DOWN).size() == 6);
        CHECK(r.count(MT_ACTION_DOWN, false) == 6);
        CHECK(r.sent.back().action == MT_ACTION_DOWN);
        CHECK_FALSE(r.sent.back().down);
    }
}

TEST_CASE("menu input: no repeat in the field, a cut scene, a popup or the help pages") {
    for (const std::int32_t panel : {0, 3, 7, 10}) {
        CAPTURE(panel);
        Rig r;
        r.menu.set_repeat(true);
        r.panel = panel;
        r.dir(fe::Direction::kDown, true);
        r.until(2.0);
        CHECK(r.presses(MT_ACTION_DOWN).size() == 1); // a held direction stays one press
        CHECK(r.count(MT_ACTION_DOWN, false) == 0);
    }
}

TEST_CASE("menu input: the repeat stops when the panel changes and waits for a busy queue") {
    Rig r;
    r.menu.set_repeat(true);
    r.panel = 6;
    r.dir(fe::Direction::kDown, true);
    r.until(0.5); // 0.0, 0.40
    CHECK(r.presses(MT_ACTION_DOWN).size() == 2);
    // A popup opens over the shop (panel 7): the direction stays held, without repeats.
    r.panel = 7;
    r.until(1.5);
    CHECK(r.presses(MT_ACTION_DOWN).size() == 2);
    // Back on the shop: still no repeat until it is pressed again.
    r.panel = 6;
    r.until(2.0);
    CHECK(r.presses(MT_ACTION_DOWN).size() == 2);
    r.dir(fe::Direction::kDown, false);
    r.dir(fe::Direction::kDown, true); // 2.0
    // The runtime still delivering: a due repeat waits for an empty queue.
    r.until(2.39);
    r.busy = true;
    r.until(2.6);
    CHECK(r.presses(MT_ACTION_DOWN).size() == 3);
    CHECK(r.menu.stats().waits > 0);
    r.busy = false;
    r.until(2.601); // at once when the queue is empty, then 0.15 s on
    r.until(2.76);
    const std::vector<double> got = r.presses(MT_ACTION_DOWN);
    REQUIRE(got.size() == 5);
    CHECK(near(got[3], 2.601));
    CHECK(near(got[4], 2.751));
    // Turning the setting off stops it at once.
    r.menu.set_repeat(false);
    r.until(3.5);
    CHECK(r.presses(MT_ACTION_DOWN).size() == 5);
}

TEST_CASE("menu input: the last direction repeats; two devices on one direction") {
    Rig r;
    r.menu.set_repeat(true);
    r.panel = 8;
    r.dir(fe::Direction::kDown, true);
    r.until(0.2);
    r.dir(fe::Direction::kRight, true); // 0.2: right takes over
    r.until(0.7);                       // right at 0.2, 0.60
    CHECK(r.presses(MT_ACTION_DOWN).size() == 1);
    CHECK(r.presses(MT_ACTION_RIGHT).size() == 2);
    r.dir(fe::Direction::kRight, false);
    r.until(1.5); // down is not taken up again
    CHECK(r.presses(MT_ACTION_DOWN).size() == 1);
    r.dir(fe::Direction::kDown, false);

    // The d-pad's up and the keyboard's Up arrow held together: each repeat lets both go and
    // presses both again, so the runtime's count goes to 0 and the guest sees a new press.
    Rig k;
    k.menu.set_repeat(true);
    k.panel = 1;
    k.dir(fe::Direction::kUp, true);
    k.key(SDLK_UP, true);
    k.until(0.45);
    CHECK(k.presses(MT_ACTION_UP).size() == 4); // 2 presses, then a repeat of both
    CHECK(k.count(MT_ACTION_UP, false) == 2);
    k.key(SDLK_UP, false); // one still holds it: it goes on repeating
    k.until(0.6);
    CHECK(k.presses(MT_ACTION_UP).size() == 5);
    k.dir(fe::Direction::kUp, false);
    k.until(1.5);
    CHECK(k.presses(MT_ACTION_UP).size() == 5);
    CHECK(k.count(MT_ACTION_UP, false) == 5);
}

TEST_CASE("menu input: the guard drops confirm and back for 0.30 s after a popup opens") {
    Rig r;
    r.menu.set_guard(true);
    r.until(1.0);
    // A was jump in the field; the level-up's or the fall's popup opens at 1.0.
    r.popup = 0x68;
    r.panel = 7;
    r.until(1.001);
    CHECK(r.menu.popup() == 0x68);
    CHECK(r.menu.stats().popups == 1);
    // Mashed: 1.05, 1.15, 1.25 dropped (press and release); 1.35 confirms.
    for (const double t : {1.05, 1.15, 1.25, 1.35}) {
        r.until(t);
        r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true);
        r.until(t + 0.05);
        r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    }
    const std::vector<double> ok = r.presses(MT_ACTION_OK);
    REQUIRE(ok.size() == 1);
    CHECK(near(ok[0], 1.35));
    CHECK(r.count(MT_ACTION_OK, false) == 1);
    CHECK(r.menu.stats().guarded == 3);
    CHECK(r.menu.stats().held == 0);
    // Back (B), revive (X) and grade revive (Y) the same; minus and plus go through.
    r.popup = -1;
    r.until(2.0);
    r.popup = 0x68;
    r.until(2.01);
    for (const SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                      SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}) {
        r.pad(b, true);
        r.until(r.now + 0.02);
        r.pad(b, false);
    }
    CHECK(r.count(MT_ACTION_BACK, true) == 0);
    CHECK(r.count(MT_ACTION_REVIVE, true) == 0);
    CHECK(r.count(MT_ACTION_GRADE_REVIVE, true) == 0);
    CHECK(r.count(MT_ACTION_MINUS, true) == 1);
    CHECK(r.count(MT_ACTION_MINUS, false) == 1);
    CHECK(r.count(MT_ACTION_BACK, false) == 0); // a dropped press's release is dropped too
    // Another popup over this one (a sub-popup): its own 0.30 s.
    r.until(2.5);
    r.popup = 0x69;
    r.until(2.51);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, true);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(r.count(MT_ACTION_BACK, true) == 0);
    r.until(2.82);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, true);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(r.count(MT_ACTION_BACK, true) == 1);
    CHECK(r.count(MT_ACTION_BACK, false) == 1);
}

TEST_CASE("menu input: a popup seen at the press itself, before the turn") {
    Rig r;
    r.menu.set_guard(true);
    r.until(1.0);
    r.popup = 5;
    r.panel = 6;
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true); // no turn yet since it opened
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(r.count(MT_ACTION_OK, true) == 0);
    CHECK(r.menu.stats().guarded == 1);
}

TEST_CASE("menu input: the guard holds while a button pressed in the field is still held") {
    Rig r;
    r.menu.set_guard(true);
    r.panel = 0;
    // X held in the field (jump; back in menus), then the popup opens.
    r.key(SDLK_X, true);
    CHECK(r.count(MT_ACTION_JUMP, true) == 1);
    CHECK(r.actions.held_from_field(MT_ACTION_BACK));
    CHECK_FALSE(r.actions.held_from_field(MT_ACTION_OK));
    r.until(0.5);
    r.popup = 13;
    r.panel = 7;
    r.until(0.501);
    // 0.40 s later Enter (OK) and Esc (back): dropped, X is still down.
    r.until(0.9);
    r.key(SDLK_RETURN, true);
    r.key(SDLK_RETURN, false);
    r.key(SDLK_ESCAPE, true);
    r.key(SDLK_ESCAPE, false);
    CHECK(r.count(MT_ACTION_OK, true) == 0);
    CHECK(r.count(MT_ACTION_BACK, true) == 0);
    // X let go: its release is jump's (the field's action), and the guard ends.
    r.key(SDLK_X, false);
    CHECK(r.count(MT_ACTION_JUMP, false) == 1);
    CHECK_FALSE(r.actions.held_from_field(MT_ACTION_BACK));
    r.key(SDLK_RETURN, true);
    r.key(SDLK_RETURN, false);
    CHECK(r.count(MT_ACTION_OK, true) == 1);
    CHECK(r.count(MT_ACTION_OK, false) == 1);
    CHECK(r.menu.stats().guarded == 2);
    CHECK(r.menu.stats().held == 2);

    // A walk held from the field doesn't count: the arrows don't confirm in menus.
    Rig w;
    w.menu.set_guard(true);
    w.key(SDLK_RIGHT, true);
    w.until(0.1);
    w.popup = 13;
    w.panel = 7;
    w.until(0.5);
    w.key(SDLK_RETURN, true);
    CHECK(w.count(MT_ACTION_OK, true) == 1);
    w.key(SDLK_RETURN, false);
    w.key(SDLK_RIGHT, false);
}

TEST_CASE("menu input: a confirm pressed before the popup is released to the game") {
    Rig r;
    r.menu.set_guard(true);
    r.panel = 6; // the shop: A buys, and its popup opens while A is still down
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(r.count(MT_ACTION_OK, true) == 1);
    r.until(0.05);
    r.popup = 3;
    r.panel = 7;
    r.until(0.1);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(r.count(MT_ACTION_OK, false) == 1); // the press went to the game: so does its release
    // Pressed in a menu (the shop), not in the field: after 0.30 s it confirms.
    r.until(0.40);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(r.count(MT_ACTION_OK, true) == 2);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    // No popup: nothing is guarded.
    r.popup = -1;
    r.until(0.5);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, true);
    r.pad(SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(r.count(MT_ACTION_BACK, true) == 1);
    // The guard turned off while a press is dropped: its release is still dropped.
    r.popup = 4;
    r.until(0.51);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, true);
    r.menu.set_guard(false);
    r.pad(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(r.count(MT_ACTION_OK, true) == 2);
    CHECK(r.count(MT_ACTION_OK, false) == 2);
}

TEST_CASE("menu input: the open popup, read from guest memory") {
    std::vector<std::uint8_t> mem(0x110000);
    mt_cpu cpu{};
    cpu.mem = mem.data();
    cpu.mem_size = mem.size();
    const auto put = [&mem](u32 addr, std::int32_t v) { std::memcpy(mem.data() + addr, &v, 4); };
    CHECK(fe::menu_popup_id(nullptr) == -1);
    put(0x108C64, -1);
    CHECK(fe::menu_popup_id(&cpu) == -1);
    put(0x108C64, 0x68);
    CHECK(fe::menu_popup_id(&cpu) == 0x68);
    mt_cpu small{};
    small.mem = mem.data();
    small.mem_size = 0x108C66; // the id would cross the end
    CHECK(fe::menu_popup_id(&small) == -1);

    // A guest address space: nothing is read until a segment holds the id.
    mt_mem *space = mt_mem_create(nullptr);
    REQUIRE(space != nullptr);
    mt_cpu guest{};
    mt_mem_attach(space, &guest);
    CHECK(fe::menu_popup_id(&guest) == -1);
    const mt_segment data{"__DATA", 0x108000u, 0x1000u, 0u, 0u, MT_PROT_READ | MT_PROT_WRITE};
    REQUIRE(mt_mem_map_segment(space, &data, nullptr, 0u) == 0);
    CHECK(fe::menu_popup_id(&guest) == 0); // mapped, zero-filled
    const std::int32_t id = 111;
    std::memcpy(guest.mem + 0x108C64, &id, 4);
    CHECK(fe::menu_popup_id(&guest) == 111);
    mt_mem_destroy(space);
}

TEST_CASE("menu input: the quantity popup repeats a held direction with the menu's timing") {
    CHECK(fe::menu_repeat_at(7, fe::kQuantityPopup));
    CHECK_FALSE(fe::menu_repeat_at(7, -1));
    CHECK_FALSE(fe::menu_repeat_at(7, 111));
    CHECK_FALSE(fe::menu_repeat_at(0, fe::kQuantityPopup));
    CHECK(fe::menu_repeat_at(6, -1));
    for (const fe::Direction d :
         {fe::Direction::kRight, fe::Direction::kLeft, fe::Direction::kUp, fe::Direction::kDown}) {
        Rig r;
        r.menu.set_repeat(true);
        r.panel = 7;
        r.popup = fe::kQuantityPopup;
        r.now = 1.0;
        r.menu.turn(nullptr, r.now);
        r.dir(d, true);
        r.until(2.05);
        const u32 a = d == fe::Direction::kRight  ? MT_ACTION_RIGHT
                      : d == fe::Direction::kLeft ? MT_ACTION_LEFT
                      : d == fe::Direction::kUp   ? MT_ACTION_UP
                                                  : MT_ACTION_DOWN;
        // The press at 1.0, repeats after kRepeatDelay, then every kRepeatEvery.
        const std::vector<double> got = r.presses(a);
        const std::vector<double> want = {1.0, 1.40, 1.55, 1.70, 1.85, 2.00};
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); i++) {
            CHECK(near(got[i], want[i]));
        }
        CHECK(near(fe::MenuInput::kRepeatDelay, 0.40));
        CHECK(near(fe::MenuInput::kRepeatEvery, 0.15));
        CHECK(r.count(a, false) == 5);
        // The amount accepted (popup 111 "bought" over the same panel): the repeat stops.
        r.popup = 111;
        r.until(3.0);
        CHECK(r.presses(a).size() == 6);
        r.dir(d, false);
    }
    // Another popup on panel 7 (a notice, the death popup): one press, no repeat.
    Rig r;
    r.menu.set_repeat(true);
    r.panel = 7;
    r.popup = 104;
    r.menu.turn(nullptr, 0);
    r.dir(fe::Direction::kRight, true);
    r.until(2.0);
    CHECK(r.presses(MT_ACTION_RIGHT).size() == 1);
    // menu_repeat off: the quantity popup's direction is one press too (the original).
    Rig off;
    off.panel = 7;
    off.popup = fe::kQuantityPopup;
    off.menu.turn(nullptr, 0);
    off.dir(fe::Direction::kRight, true);
    off.until(2.0);
    CHECK(off.presses(MT_ACTION_RIGHT).size() == 1);
}

TEST_CASE("menu input: LT with up or down pages a list that owns the focus, never the game") {
    Rig r;
    r.panel = 6;
    r.list = true;
    // Without LT: a plain direction.
    r.dir(fe::Direction::kDown, true);
    r.dir(fe::Direction::kDown, false);
    CHECK(r.count(MT_ACTION_DOWN, true) == 1);
    // LT held: down is a page, applied at the next turn; its press and release never sent.
    r.lt(true);
    CHECK(r.sent.size() == 2); // LT itself is the port's: nothing reaches the game
    r.dir(fe::Direction::kDown, true);
    r.dir(fe::Direction::kDown, false);
    r.dir(fe::Direction::kUp, true);
    r.dir(fe::Direction::kUp, false);
    CHECK(r.pages.empty());
    r.until(0.001);
    CHECK(r.pages == std::vector<int>{1, -1});
    CHECK(r.sent.size() == 2);
    CHECK(r.menu.stats().pages == 2);
    // Left and right with LT: plain directions.
    r.dir(fe::Direction::kRight, true);
    r.dir(fe::Direction::kRight, false);
    CHECK(r.count(MT_ACTION_RIGHT, true) == 1);
    // The runtime still delivering earlier keys: the page waits for an empty queue.
    r.busy = true;
    r.dir(fe::Direction::kDown, true);
    r.until(0.1);
    CHECK(r.pages.size() == 2);
    r.busy = false;
    r.until(0.101);
    CHECK(r.pages.size() == 3);
    r.dir(fe::Direction::kDown, false);
    // No list owns the focus (a popup over the shop, another screen): LT changes nothing.
    r.list = false;
    r.dir(fe::Direction::kDown, true);
    r.dir(fe::Direction::kDown, false);
    CHECK(r.count(MT_ACTION_DOWN, true) == 2);
    CHECK(r.count(MT_ACTION_DOWN, false) == 2);
    r.lt(false);
    // A page asked on the shop, which closed before it applied: dropped.
    r.list = true;
    r.lt(true);
    r.busy = true;
    r.dir(fe::Direction::kDown, true);
    r.panel = 0;
    r.busy = false;
    r.until(0.3);
    CHECK(r.pages.size() == 3);
    r.dir(fe::Direction::kDown, false);
    CHECK(r.count(MT_ACTION_DOWN, false) == 2); // the page's release is not sent either
}

TEST_CASE("menu input: a held page repeats with menu_repeat, and stops with LT") {
    Rig r;
    r.menu.set_repeat(true);
    r.panel = 6;
    r.list = true;
    r.now = 1.0;
    r.lt(true);
    r.dir(fe::Direction::kDown, true);
    r.until(2.05);
    // The page at the first turn, then at 1.40, 1.55, 1.70, 1.85, 2.00.
    const std::vector<double> want = {1.001, 1.40, 1.55, 1.70, 1.85, 2.00};
    REQUIRE(r.page_at.size() == want.size());
    for (std::size_t i = 0; i < want.size(); i++) {
        CHECK(near(r.page_at[i], want[i]));
    }
    CHECK(r.menu.stats().page_repeats == 5);
    CHECK(r.count(MT_ACTION_DOWN, true) == 0);
    // LT let go with down still held: the pages stop (down's press never went to the game).
    r.lt(false);
    r.until(3.0);
    CHECK(r.page_at.size() == 6);
    CHECK(r.count(MT_ACTION_DOWN, true) == 0);
    r.dir(fe::Direction::kDown, false);
    CHECK(r.count(MT_ACTION_DOWN, false) == 0);
    // Without menu_repeat a held page is one page.
    Rig once;
    once.panel = 1;
    once.list = true;
    once.lt(true);
    once.dir(fe::Direction::kUp, true);
    once.until(2.0);
    CHECK(once.pages == std::vector<int>{-1});
}
