// The gamepad pointer (#185, ADR 0107) with synthetic data only: the stick's dead zone, response
// curve and acceleration, the clamp to the picture (narrow and widescreen), touchpad points,
// the tap as exactly one touch began / ended pair through Input, R3 shared with fast_forward
// (gamepad.h's holds: a short press is fast-forward's, a held one taps), the touchpad's click,
// the idle and focus hides, the highlight dropped by a tap, pad_pointer = off changing nothing,
// and the `hold NAME` control in the bindings file and the glyphs. The pad is an SDL3 virtual
// Xbox 360 (virtual_pad.h); touchpad fingers are SDL events made here.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "gamepad.h"
#include "glyphs.h"
#include "input.h"
#include "maplethief/input.h"
#include "maplethief/uikit.h"
#include "pad_pointer.h"

#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using Lines = std::vector<std::string>;

// What Input sent to UIKit: "began 0x1050 (240, 160)".
Lines g_touches;
int record_touch(void *, u32 finger, u32 phase, float x, float y) {
    const char *names[] = {"began", "moved", "ended", "cancelled"};
    char line[96];
    std::snprintf(line, sizeof line, "%s 0x%X (%g, %g)", phase < 4 ? names[phase] : "?",
                  static_cast<unsigned>(finger), static_cast<double>(x), static_cast<double>(y));
    g_touches.emplace_back(line);
    return 1;
}
// What Actions sent to the runtime: "+fast_forward" never comes (a port action), so the port's
// actions are read through holding() and presses().
Lines g_actions;
void record_action(void *, u32 action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_actions.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
std::int32_t g_panel = -1; // menus (the main menu: no key pad panel)
std::int32_t fixed_panel(void *) { return g_panel; }

std::size_t count_prefix(const Lines &lines, const std::string &prefix) {
    std::size_t n = 0;
    for (const std::string &l : lines) {
        n += l.rfind(prefix, 0) == 0 ? 1 : 0;
    }
    return n;
}

// A virtual Xbox 360 pad through Input, PadPointer and Gamepads as game.cpp wires them, turned at
// 60 turns a second on a clock of its own.
struct Rig {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::PadPointer pointer{&input, &actions};
    double now = 0;
    std::uint64_t focus_uses = 0;
    std::unique_ptr<virtual_pad::Pad> pad;

    explicit Rig(fe::PadPointerMode mode = fe::PadPointerMode::kBoth) {
        g_touches.clear();
        g_actions.clear();
        g_panel = -1;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
        input.set_sink(record_touch, nullptr);
        actions.set_sink(record_action, nullptr);
        actions.set_panel_source(fixed_panel, nullptr);
        gamepads.set_hold_gate(fe::PadPointer::hold_gate, &pointer);
        pointer.set_mode(mode);
        pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
        REQUIRE_MESSAGE(pad->ok(), pad->error);
        pump();
        REQUIRE(gamepads.count() == 1);
    }
    ~Rig() {
        pad.reset();
        SDL_Quit();
    }
    Rig(const Rig &) = delete;
    Rig &operator=(const Rig &) = delete;

    void handle(const SDL_Event &e) {
        input.handle(e);
        pointer.handle(e);
        gamepads.handle(e);
    }
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            handle(e);
        }
    }
    // `n` turns of 1/60 s, as game.cpp's turn hook orders them.
    void turns(int n) {
        for (int i = 0; i < n; i++) {
            now += 1.0 / 60.0;
            pump();
            gamepads.turn(now);
            actions.turn();
            pointer.turn(now, focus_uses);
        }
    }
    void stick(float x, float y) {
        pad->axis(SDL_GAMEPAD_AXIS_RIGHTX, static_cast<Sint16>(x * 32767.0f));
        pad->axis(SDL_GAMEPAD_AXIS_RIGHTY, static_cast<Sint16>(y * 32767.0f));
        pump();
    }
    void r3(bool down) {
        pad->button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, down);
        pump();
    }
    // The touchpad's click: the virtual pad has no such button, so its event is made here.
    void touchpad_click(bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = pad->id;
        e.gbutton.button = SDL_GAMEPAD_BUTTON_TOUCHPAD;
        e.gbutton.down = down;
        handle(e);
    }
    void finger(Uint32 type, int f, float x, float y) {
        SDL_Event e{};
        e.type = type;
        e.gtouchpad.which = pad->id;
        e.gtouchpad.touchpad = 0;
        e.gtouchpad.finger = f;
        e.gtouchpad.x = x;
        e.gtouchpad.y = y;
        e.gtouchpad.pressure = 1;
        handle(e);
    }
    // Shows the pointer: a short push right, then let go.
    void show() {
        stick(1, 0);
        turns(3);
        stick(0, 0);
        turns(1);
        REQUIRE(pointer.shown());
    }
};

} // namespace

TEST_CASE("pad pointer: stick deflection to motion: dead zone, curve, acceleration") {
    const fe::PointerConfig c; // dead zone 0.2, exponent 2, 240 points/s, x2 over 0.6 s
    const auto v = [&](float x, float y, double pushed = 0) {
        return fe::pointer_velocity(x, y, pushed, c);
    };
    // Inside the dead zone, radially: nothing.
    CHECK(v(0.15f, 0).x == 0);
    CHECK(v(0.14f, -0.14f).y == 0); // r 0.198
    CHECK(v(0.2f, 0).x == 0);
    // Past it: the curve. Half way through the range is a quarter of the speed.
    CHECK(v(1, 0).x == doctest::Approx(240));
    CHECK(v(0.6f, 0).x == doctest::Approx(60));
    CHECK(v(-0.6f, 0).x == doctest::Approx(-60));
    CHECK(v(0, -1).y == doctest::Approx(-240)); // SDL's y is down: up is negative
    CHECK(v(0, -1).x == doctest::Approx(0));
    // Corners past the unit circle are capped at full deflection, in the stick's direction.
    CHECK(v(1, 1).x == doctest::Approx(240 / std::sqrt(2.0)));
    CHECK(v(1, 1).y == doctest::Approx(240 / std::sqrt(2.0)));
    // Acceleration: the longer it stays pushed, the faster, up to `boost` after ramp_seconds.
    CHECK(v(1, 0, 0.3).x == doctest::Approx(360));
    CHECK(v(1, 0, 0.6).x == doctest::Approx(480));
    CHECK(v(1, 0, 5.0).x == doctest::Approx(480));
    CHECK(v(0.6f, 0, 0.6).x == doctest::Approx(120));
    // Nonsense moves nothing.
    CHECK(v(std::nanf(""), 0).x == 0);

    // In the rig: a full push moves the pointer and keeps accelerating.
    Rig r;
    r.stick(1, 0);
    r.turns(1);
    CHECK(r.pointer.shown());
    const float x0 = r.pointer.x();
    r.turns(6);
    const float early = r.pointer.x() - x0;
    const float x1 = r.pointer.x();
    r.turns(6);
    CHECK(r.pointer.x() - x1 > early); // faster now
    CHECK(r.pointer.y() == doctest::Approx(160));
    // A stick back inside the dead zone stops it.
    r.stick(0.1f, 0.1f);
    const float x2 = r.pointer.x();
    r.turns(10);
    CHECK(r.pointer.x() == x2);
}

TEST_CASE("pad pointer: clamped to the picture, narrow and widescreen") {
    const fe::PointerBounds narrow = fe::pointer_bounds(480);
    CHECK(narrow.x0 == 0);
    CHECK(narrow.x1 == 480);
    const fe::PointerBounds wide = fe::pointer_bounds(568); // 44 points each side
    CHECK(wide.x0 == -44);
    CHECK(wide.x1 == 524);
    CHECK(fe::pointer_bounds(0).x1 == 480);

    float x = 470, y = 10;
    fe::pointer_step(&x, &y, fe::PointerVelocity{480, -480}, 1.0, narrow);
    CHECK(x < 480);
    CHECK(x > 479.99f);
    CHECK(y == 0);
    x = 5;
    y = 315;
    fe::pointer_step(&x, &y, fe::PointerVelocity{-480, 480}, 1.0, narrow);
    CHECK(x == 0);
    CHECK(y < 320);
    CHECK(y > 319.99f);
    x = 5;
    fe::pointer_step(&x, &y, fe::PointerVelocity{-480, 0}, 1.0, wide);
    CHECK(x == -44);
    x = 500;
    fe::pointer_step(&x, &y, fe::PointerVelocity{480, 0}, 1.0, wide);
    CHECK(x < 524);
    CHECK(x > 523.99f);

    // In the rig: pushed far left for seconds, it stops at the picture's edge.
    Rig r;
    r.pointer.set_bounds(wide);
    r.stick(-1, -1);
    r.turns(240);
    CHECK(r.pointer.x() == -44);
    CHECK(r.pointer.y() == 0);
    // The picture narrows (widescreen leaves the field): the pointer comes back inside.
    r.stick(0, 0);
    r.pointer.set_bounds(narrow);
    r.turns(1);
    CHECK(r.pointer.x() == 0);
}

TEST_CASE("pad pointer: touchpad coordinates to picture points, one touch per finger") {
    float x = -1, y = -1;
    fe::touchpad_point(0, 0, fe::pointer_bounds(480), &x, &y);
    CHECK(x == 0);
    CHECK(y == 0);
    fe::touchpad_point(0.5f, 0.5f, fe::pointer_bounds(480), &x, &y);
    CHECK(x == 240);
    CHECK(y == 160);
    fe::touchpad_point(1, 1, fe::pointer_bounds(480), &x, &y);
    CHECK(x < 480);
    CHECK(y < 320);
    fe::touchpad_point(0, 0.25f, fe::pointer_bounds(568), &x, &y);
    CHECK(x == -44);
    CHECK(y == 80);
    fe::touchpad_point(0.5f, 0.5f, fe::pointer_bounds(568), &x, &y);
    CHECK(x == 240);
    fe::touchpad_point(2, -1, fe::pointer_bounds(480), &x, &y); // out of range: clamped
    CHECK(x < 480);
    CHECK(y == 0);

    Rig r;
    // Two fingers: each its own touch, absolute, while it is down. No click needed.
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 0, 0.25f, 0.5f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 1, 0.75f, 0.5f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION, 0, 0.5f, 0.25f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_UP, 1, 0.75f, 0.5f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_UP, 0, 0.5f, 0.25f);
    CHECK(g_touches == Lines{"began 0x1060 (120, 160)", "began 0x1061 (360, 160)",
                             "moved 0x1060 (240, 80)", "ended 0x1061 (360, 160)",
                             "ended 0x1060 (240, 80)"});
    CHECK(r.pointer.stats().pad_touches == 2);
    CHECK(r.input.live() == 0);
    // A finger down on the touchpad hides the stick's pointer.
    r.show();
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 0, 0.5f, 0.5f);
    CHECK_FALSE(r.pointer.shown());
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_UP, 0, 0.5f, 0.5f);
    // pad_pointer = stick: the touchpad is not the screen.
    g_touches.clear();
    r.pointer.set_mode(fe::PadPointerMode::kStick);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 0, 0.5f, 0.5f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_UP, 0, 0.5f, 0.5f);
    CHECK(g_touches.empty());
    // A pad unplugged lifts its fingers.
    r.pointer.set_mode(fe::PadPointerMode::kTouchpad);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 0, 0.5f, 0.5f);
    CHECK(r.input.live() == 1);
    r.pad->detach();
    r.pump();
    CHECK(r.input.live() == 0);
}

TEST_CASE("pad pointer: R3 held taps once at the pointer; a short R3 is fast-forward's") {
    Rig r;
    r.show();
    const float x = r.pointer.x(), y = r.pointer.y();
    char began[64], ended[64];
    std::snprintf(began, sizeof began, "began 0x1050 (%g, %g)", static_cast<double>(x),
                  static_cast<double>(y));
    std::snprintf(ended, sizeof ended, "ended 0x1050 (%g, %g)", static_cast<double>(x),
                  static_cast<double>(y));
    // Held 0.3 s: one began, then one ended at the release; fast_forward never held.
    g_touches.clear();
    const std::uint64_t ff = r.actions.presses(MT_ACTION_FAST_FORWARD, fe::Context::kMenu);
    r.r3(true);
    r.turns(10);
    CHECK(g_touches.empty()); // held back, not yet a hold
    CHECK(r.gamepads.held_back() == 1);
    CHECK_FALSE(r.actions.holding(MT_ACTION_FAST_FORWARD));
    r.turns(10); // past 0.3 s
    CHECK(r.actions.holding(MT_ACTION_POINTER_TAP));
    CHECK(r.pointer.pressing());
    r.turns(30); // a long press: still one touch
    r.r3(false);
    r.turns(1);
    CHECK(g_touches == Lines{began, ended});
    CHECK(r.pointer.stats().taps == 1);
    CHECK(r.actions.presses(MT_ACTION_FAST_FORWARD, fe::Context::kMenu) == ff);
    CHECK(r.input.live() == 0);

    // A short press (under 0.3 s) with the pointer shown: fast_forward pressed and released at
    // once, no touch.
    g_touches.clear();
    r.r3(true);
    r.turns(5);
    r.r3(false);
    r.turns(1);
    CHECK(g_touches.empty());
    CHECK(r.actions.presses(MT_ACTION_FAST_FORWARD, fe::Context::kMenu) == ff + 1);
    CHECK_FALSE(r.actions.holding(MT_ACTION_FAST_FORWARD));
    CHECK(r.pointer.stats().taps == 1);

    // Held to drag: the stick moves the touch.
    g_touches.clear();
    r.r3(true);
    r.turns(20);
    r.stick(-1, 0);
    r.turns(10);
    r.stick(0, 0);
    r.r3(false);
    r.turns(1);
    CHECK(count_prefix(g_touches, "began 0x1050") == 1);
    CHECK(count_prefix(g_touches, "moved 0x1050") >= 5);
    CHECK(count_prefix(g_touches, "ended 0x1050") == 1);

    // The pointer hidden (idle 3 s): R3 is fast-forward's at once, held as long as it is.
    r.turns(200);
    CHECK_FALSE(r.pointer.shown());
    g_touches.clear();
    r.r3(true);
    CHECK(r.actions.holding(MT_ACTION_FAST_FORWARD));
    r.turns(40);
    CHECK(r.actions.holding(MT_ACTION_FAST_FORWARD));
    CHECK_FALSE(r.actions.holding(MT_ACTION_POINTER_TAP));
    r.r3(false);
    r.turns(1);
    CHECK_FALSE(r.actions.holding(MT_ACTION_FAST_FORWARD));
    CHECK(g_touches.empty());
}

TEST_CASE("pad pointer: the touchpad's click taps at the pointer, one began / ended pair") {
    Rig r(fe::PadPointerMode::kStick);
    // Hidden: the click does nothing.
    r.touchpad_click(true);
    r.turns(2);
    r.touchpad_click(false);
    r.turns(1);
    CHECK(g_touches.empty());
    r.show();
    r.touchpad_click(true);
    r.turns(2);
    r.touchpad_click(false);
    r.turns(1);
    CHECK(count_prefix(g_touches, "began 0x1050") == 1);
    CHECK(count_prefix(g_touches, "ended 0x1050") == 1);
    CHECK(g_touches.size() == 2);
}

TEST_CASE("pad pointer: hides after 3 s idle and on a focus use; a tap drops the highlight") {
    Rig r;
    r.show();
    r.turns(170); // about 2.9 s after the last push
    CHECK(r.pointer.shown());
    r.turns(10);
    CHECK_FALSE(r.pointer.shown());
    // A tap drops the focus highlight; the focus's next use brings it back and hides the pointer.
    r.show();
    r.touchpad_click(true);
    r.turns(1);
    r.touchpad_click(false);
    r.turns(1);
    CHECK(r.pointer.highlight_dropped());
    CHECK(r.pointer.shown());
    r.focus_uses++; // a d-pad press on the focus's screens
    r.turns(1);
    CHECK_FALSE(r.pointer.highlight_dropped());
    CHECK_FALSE(r.pointer.shown());
    // release_all (a port overlay): the touch lifts, the pointer hides.
    r.show();
    r.touchpad_click(true);
    r.turns(1);
    CHECK(r.input.live() == 1);
    r.pointer.release_all();
    CHECK(r.input.live() == 0);
    CHECK_FALSE(r.pointer.shown());
}

TEST_CASE("pad pointer: off: no pointer, no touches, R3 as before") {
    Rig r(fe::PadPointerMode::kOff);
    r.stick(1, 1);
    r.turns(30);
    CHECK_FALSE(r.pointer.shown());
    r.stick(0, 0);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, 0, 0.5f, 0.5f);
    r.finger(SDL_EVENT_GAMEPAD_TOUCHPAD_UP, 0, 0.5f, 0.5f);
    r.touchpad_click(true);
    r.turns(2);
    r.touchpad_click(false);
    r.r3(true);
    CHECK(r.actions.holding(MT_ACTION_FAST_FORWARD)); // at once: no hold gate
    CHECK(r.gamepads.held_back() == 0);
    r.turns(40);
    r.r3(false);
    r.turns(1);
    CHECK(g_touches.empty());
    CHECK(r.pointer.stats().taps == 0);
    CHECK(r.pointer.stats().shows == 0);
}

TEST_CASE("pad pointer: pointer_tap's defaults, `hold NAME` in the bindings file and glyphs") {
    const fe::Control touchpad{fe::ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_TOUCHPAD};
    const fe::Control hold_r3{fe::ControlKind::kGamepadHold, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    const fe::Control r3{fe::ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (const fe::Context c : {fe::Context::kField, fe::Context::kMenu}) {
        CHECK(fe::controls_for(fe::default_bindings(), c, MT_ACTION_POINTER_TAP) ==
              std::vector<fe::Control>{touchpad, hold_r3});
        CHECK(fe::controls_for(fe::default_bindings(), c, MT_ACTION_FAST_FORWARD).front() == r3);
    }
    fe::Control parsed;
    std::string why;
    REQUIRE(fe::parse_control("hold rightstick", &parsed, &why));
    CHECK(parsed == hold_r3);
    REQUIRE(fe::parse_control("HOLD a", &parsed, &why));
    CHECK(parsed == fe::Control{fe::ControlKind::kGamepadHold, SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(fe::control_text(hold_r3) == "hold rightstick");
    CHECK(fe::control_name(hold_r3) == "hold rightstick");
    CHECK_FALSE(fe::parse_control("hold dpup", &parsed, &why));
    CHECK_FALSE(fe::parse_control("hold", &parsed, &why));
    CHECK(
        fe::reserved_control(fe::Control{fe::ControlKind::kGamepadHold, SDL_GAMEPAD_BUTTON_GUIDE}));
    // The file's round trip keeps it.
    fe::BindingSet set;
    const Lines problems =
        fe::parse_bindings("[menu]\npointer_tap = hold leftshoulder\n", "bindings.ini", &set);
    CHECK(problems.empty());
    CHECK(fe::controls_for(set.table, fe::Context::kMenu, MT_ACTION_POINTER_TAP) ==
          std::vector<fe::Control>{
              fe::Control{fe::ControlKind::kGamepadHold, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}});
    const std::string text = fe::serialise_bindings(set);
    CHECK(text.find("pointer_tap = button touchpad, hold rightstick") != std::string::npos);
    CHECK(text.find("pointer_tap = hold leftshoulder") != std::string::npos);
    fe::BindingSet back;
    CHECK(fe::parse_bindings(text, "bindings.ini", &back).empty());
    CHECK(fe::controls_for(back.table, fe::Context::kMenu, MT_ACTION_POINTER_TAP) ==
          fe::controls_for(set.table, fe::Context::kMenu, MT_ACTION_POINTER_TAP));
    // Glyphs: "Hold" and the button's own label, per set.
    CHECK(fe::pad_glyph_label(hold_r3, SDL_GAMEPAD_TYPE_XBOX360, fe::FaceButtons::kPosition) ==
          "Hold RS");
    CHECK(fe::pad_glyph_label(hold_r3, SDL_GAMEPAD_TYPE_PS5, fe::FaceButtons::kPosition) ==
          "Hold R3");
    CHECK(fe::pad_glyph_label(fe::Control{fe::ControlKind::kGamepadHold, SDL_GAMEPAD_BUTTON_SOUTH},
                              SDL_GAMEPAD_TYPE_PS5, fe::FaceButtons::kPosition) == "Hold Cross");
    CHECK(fe::glyph_width(hold_r3, SDL_GAMEPAD_TYPE_XBOX360, fe::FaceButtons::kPosition) >
          fe::glyph_width(r3, SDL_GAMEPAD_TYPE_XBOX360, fe::FaceButtons::kPosition));
}
