// Gamepads and actions (#67, ADR 0048) with synthetic data only. First the frontend's half on
// SDL3 virtual joysticks that SDL takes for an Xbox 360, a PS4 and (through a mapping) a PS5
// controller: the stick's dead zone, 8-way sectors and 4-way mode (#138), the default bindings by
// position in the field and in menus, the d-pad and stick merged, the triggers' thresholds, a
// release that follows its press across a context change, hot-plug, two pads (the first active one
// wins), focus loss, and the help pages' swipe through Input. Then the runtime's half
// (maplethief/input.h) on a synthetic guest built like the game: an EAGLView with its
// statusOfTouchKeyPad and touchKeyPadView, a TouchKeyPadView whose BOOL setters write its
// ivars, a SkyCanvas with its two key slots, CMTimer's lastFrame and a host Clet_HandleEvent
// (platform_test.cpp's dispatcher hook): nothing is lost, one step per tick, presses together
// go in together, a busy slot waits, a release goes the way its press went.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "actions.h"
#include "gamepad.h"
#include "input.h"
#include "maplethief/cf.h"
#include "maplethief/hle/imports.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/uikit.h"
#include "uikit/uikit_internal.h"

#include "virtual_pad.h"

// platform_test.cpp's dispatcher: a host function for one guest address.
extern std::uint32_t g_platform_test_guest_addr;
extern mt_guest_fn g_platform_test_guest_fn;

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using virtual_pad::Kind;

// --- the frontend: recording sinks ------------------------------------------------------------

// "+jump", "-jump": what Actions sent to the runtime.
std::vector<std::string> g_actions;
i32 g_panel = 0;

void record_action(void *, u32 action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_actions.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
i32 fixed_panel(void *) { return g_panel; }

// "began 0x1010 (390, 160)": what Input sent to UIKit.
std::vector<std::string> g_touches;
int record_touch(void *, u32 finger, u32 phase, float x, float y) {
    const char *names[] = {"began", "moved", "ended", "cancelled"};
    char line[96];
    std::snprintf(line, sizeof line, "%s 0x%X (%g, %g)", phase < 4 ? names[phase] : "?",
                  static_cast<unsigned>(finger), static_cast<double>(x), static_cast<double>(y));
    g_touches.emplace_back(line);
    return 1;
}

// SDL's gamepad subsystem, an Input that records, Actions into the recording sink on panel
// g_panel, and Gamepads.
struct Pads {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};

    Pads() {
        g_actions.clear();
        g_touches.clear();
        g_panel = 0;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
        input.set_sink(record_touch, nullptr);
        actions.set_sink(record_action, nullptr);
        actions.set_panel_source(fixed_panel, nullptr);
        pump();
        gamepads.open_connected();
    }
    ~Pads() { SDL_Quit(); }
    Pads(const Pads &) = delete;
    Pads &operator=(const Pads &) = delete;

    // SDL's pending gamepad events into Gamepads (and Input, as the frontend does).
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input.handle(e);
            gamepads.handle(e);
        }
    }
    // What `change` sent, after a pump.
    std::vector<std::string> sent(const std::function<void()> &change) {
        g_actions.clear();
        change();
        pump();
        return g_actions;
    }
};

using Lines = std::vector<std::string>;

Sint16 stick(float v) { return static_cast<Sint16>(v * 32767.0f); }

} // namespace

TEST_CASE("gamepad: the stick's dead zone, release zone and 8-way sectors with hysteresis") {
    const fe::StickConfig c;
    const fe::Directions none{};
    const fe::Directions right{false, false, false, true};
    const fe::Directions up{true, false, false, false};
    const fe::Directions up_right{true, false, false, true};
    const fe::Directions down_left{false, true, true, false};
    // Engages past 0.30, lets go below 0.22 (radially).
    CHECK(fe::stick_directions(0.25f, 0.0f, none, c) == none);
    CHECK(fe::stick_directions(0.35f, 0.0f, none, c) == right);
    CHECK(fe::stick_directions(0.25f, 0.0f, right, c) == right);
    CHECK(fe::stick_directions(0.20f, 0.0f, right, c) == none);
    CHECK(fe::stick_directions(0.18f, -0.18f, none, c) == none); // r 0.25
    // SDL's y points down: -y is up. The diagonals hold two directions.
    CHECK(fe::stick_directions(0.0f, -0.9f, none, c) == up);
    CHECK(fe::stick_directions(0.6f, -0.6f, none, c) == up_right);
    CHECK(fe::stick_directions(-0.6f, 0.6f, none, c) == down_left);
    // 30 degrees above right: right only from rest; up held stays (75.5-degree half sector).
    const float x30 = 0.8f * std::cos(30.0f * 3.14159265f / 180.0f);
    const float y30 = -0.8f * std::sin(30.0f * 3.14159265f / 180.0f);
    CHECK(fe::stick_directions(x30, y30, none, c) == right);
    CHECK(fe::stick_directions(x30, y30, up, c) == up_right);
    // 64 degrees: up from rest, up and right while right is held, up once it is not.
    const float x64 = 0.8f * std::cos(64.0f * 3.14159265f / 180.0f);
    const float y64 = -0.8f * std::sin(64.0f * 3.14159265f / 180.0f);
    CHECK(fe::stick_directions(x64, y64, none, c) == up);
    CHECK(fe::stick_directions(x64, y64, right, c) == up_right);
    CHECK(fe::stick_directions(x64, y64, up, c) == up);
    // Nonsense is no direction.
    CHECK(fe::stick_directions(std::numeric_limits<float>::quiet_NaN(), 0.0f, none, c) == none);
}

TEST_CASE("gamepad: stick_mode 4way: the dominant axis, one direction, with hysteresis (#138)") {
    fe::StickConfig c;
    c.mode = fe::StickMode::k4Way;
    const fe::Directions none{},
        up = {true, false, false, false}, down = {false, true, false, false},
        left = {false, false, true, false}, right = {false, false, false, true};
    const auto at_degrees = [](float deg_off_up_toward_right, float r) {
        const float rad = deg_off_up_toward_right * 3.14159265f / 180.0f;
        return std::pair<float, float>(r * std::sin(rad), -r * std::cos(rad));
    };
    // 30 degrees off straight up, from rest, either side: up (a ladder climbs).
    {
        const auto [x, y] = at_degrees(30.0f, 0.9f);
        CHECK(fe::stick_directions(x, y, none, c) == up);
        CHECK(fe::stick_directions(-x, y, none, c) == up);
    }
    // Every angle engaged holds exactly one direction: never two, never none.
    for (int d = 0; d < 360; d++) {
        const auto [x, y] = at_degrees(static_cast<float>(d), 0.8f);
        int n = 0;
        for (const bool b : fe::stick_directions(x, y, none, c)) {
            n += b ? 1 : 0;
        }
        CHECK(n == 1);
    }
    // The dead zone and the cardinals as before.
    CHECK(fe::stick_directions(0.25f, 0.0f, none, c) == none);
    CHECK(fe::stick_directions(0.0f, 0.9f, none, c) == down);
    CHECK(fe::stick_directions(-0.9f, 0.0f, none, c) == left);
    // Hysteresis (8 degrees): a held direction keeps the stick up to 53 degrees off its axis,
    // and a direction not held takes it only past 45.
    {
        const auto [x, y] = at_degrees(40.0f, 0.8f); // 50 degrees off right
        CHECK(fe::stick_directions(x, y, none, c) == up);
        CHECK(fe::stick_directions(x, y, right, c) == right);
    }
    {
        const auto [x, y] = at_degrees(50.0f, 0.8f); // 50 degrees off up
        CHECK(fe::stick_directions(x, y, none, c) == right);
        CHECK(fe::stick_directions(x, y, up, c) == up);
    }
    {
        const auto [x, y] = at_degrees(54.0f, 0.8f); // past the hysteresis
        CHECK(fe::stick_directions(x, y, up, c) == right);
    }
    // 8-way's diagonal is unchanged by the same config with mode 8way.
    c.mode = fe::StickMode::k8Way;
    const auto [x, y] = at_degrees(45.0f, 0.8f);
    CHECK(fe::stick_directions(x, y, none, c) == fe::Directions{true, false, false, true});
}

TEST_CASE("gamepad: Xbox 360, PS4 and a mapped PS5 pad: SDL's types, the bindings by position") {
    struct Case {
        Kind kind;
        bool mapping;
    };
    for (const Case k :
         {Case{Kind::kXbox360, false}, Case{Kind::kPs4, false}, Case{Kind::kPs5, true}}) {
        CAPTURE(virtual_pad::kind_name(k.kind));
        CAPTURE(k.mapping);
        Pads p;
        virtual_pad::Pad pad(k.kind, k.mapping);
        REQUIRE_MESSAGE(pad.ok(), pad.error);
        p.pump();
        REQUIRE(p.gamepads.count() == 1);
        CHECK(SDL_GetGamepadTypeForID(pad.id) == virtual_pad::expected_type(k.kind));
        CHECK(g_actions.empty()); // nothing pressed by plugging in

        // Each control pressed and released, in the field (panel 0) and in a menu (panel 1).
        struct Row {
            std::function<void(bool)> set;
            const char *field;
            const char *menu; // nullptr: nothing
        };
        const auto button = [&pad](SDL_GamepadButton b) {
            return [&pad, b](bool down) { pad.button(b, down); };
        };
        const auto trigger = [&pad](SDL_GamepadAxis a) {
            return [&pad, a](bool down) { pad.trigger(a, down ? 0.8f : 0.0f); };
        };
        const auto axis = [&pad](SDL_GamepadAxis a, float v) {
            return [&pad, a, v](bool down) { pad.axis(a, down ? stick(v) : 0); };
        };
        const std::vector<Row> rows = {
            {button(SDL_GAMEPAD_BUTTON_SOUTH), "jump", "ok"},
            {button(SDL_GAMEPAD_BUTTON_WEST), "attack", "revive"},
            {button(SDL_GAMEPAD_BUTTON_EAST), "slot2", "back"},
            {button(SDL_GAMEPAD_BUTTON_NORTH), "slot1", "grade_revive"},
            {button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), "slot3", "minus"},
            {button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "slot4", "plus"},
            {trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), "attack", "slot4"},
            {trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), "cash_shop", nullptr},
            {button(SDL_GAMEPAD_BUTTON_START), "menu", "back"},
            {button(SDL_GAMEPAD_BUTTON_BACK), "map", "map"},
            {button(SDL_GAMEPAD_BUTTON_DPAD_UP), "up", "up"},
            {button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), "down", "down"},
            {button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), "left", "left"},
            {button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), "right", "right"},
            {axis(SDL_GAMEPAD_AXIS_LEFTX, 0.8f), "right", "right"},
            {axis(SDL_GAMEPAD_AXIS_LEFTX, -0.8f), "left", "left"},
            {axis(SDL_GAMEPAD_AXIS_LEFTY, -0.8f), "up", "up"},
            {axis(SDL_GAMEPAD_AXIS_LEFTY, 0.8f), "down", "down"},
        };
        for (const i32 panel : {0, 1}) {
            g_panel = panel;
            for (const Row &r : rows) {
                const char *name = panel == 0 ? r.field : r.menu;
                CAPTURE(panel);
                CAPTURE(r.field);
                const Lines down = p.sent([&r] { r.set(true); });
                const Lines up = p.sent([&r] { r.set(false); });
                if (name == nullptr) {
                    CHECK(down.empty());
                    CHECK(up.empty());
                } else {
                    CHECK(down == Lines{std::string("+") + name});
                    CHECK(up == Lines{std::string("-") + name});
                }
            }
        }
        // Buttons nobody binds: the guide button, the sticks' clicks.
        CHECK(p.sent([&pad] { pad.button(SDL_GAMEPAD_BUTTON_GUIDE, true); }).empty());
        CHECK(p.sent([&pad] { pad.button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true); }).empty());
        CHECK(p.actions.held() == 2);
        CHECK(p.gamepads.active_id() == pad.id);
        CHECK(p.gamepads.active() != nullptr);
    }
}

TEST_CASE("gamepad: d-pad and stick merged, the triggers' thresholds, a release across contexts") {
    Pads p;
    virtual_pad::Pad pad(Kind::kXbox360);
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    p.pump();

    // The d-pad's left and the stick's left are one direction: one press, one release.
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true); }) == Lines{"+left"});
    CHECK(p.sent([&] { pad.axis(SDL_GAMEPAD_AXIS_LEFTX, stick(-0.9f)); }).empty());
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, false); }).empty());
    // The stick to the up-left diagonal adds up; inside the release zone lets both go.
    CHECK(p.sent([&] { pad.axis(SDL_GAMEPAD_AXIS_LEFTY, stick(-0.9f)); }) == Lines{"+up"});
    CHECK(p.sent([&] {
        pad.axis(SDL_GAMEPAD_AXIS_LEFTX, stick(-0.1f));
        pad.axis(SDL_GAMEPAD_AXIS_LEFTY, stick(-0.1f));
    }) == Lines{"-left", "-up"}); // x first: the angle leaves left
    // A stick inside the dead zone presses nothing.
    CHECK(p.sent([&] { pad.axis(SDL_GAMEPAD_AXIS_LEFTX, stick(0.25f)); }).empty());
    CHECK(p.sent([&] { pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0); }).empty());

    // The right trigger presses past 0.5 and releases below 0.4.
    CHECK(p.sent([&] { pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.45f); }).empty());
    CHECK(p.sent([&] { pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.6f); }) == Lines{"+attack"});
    CHECK(p.sent([&] { pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.45f); }).empty());
    CHECK(p.sent([&] { pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.3f); }) == Lines{"-attack"});

    // X and the right trigger are both attack: the runtime counts the holds (each sends).
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_WEST, true); }) == Lines{"+attack"});
    CHECK(p.sent([&] { pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0f); }) == Lines{"+attack"});
    CHECK(p.sent([&] {
        pad.button(SDL_GAMEPAD_BUTTON_WEST, false);
        pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f);
    }) == Lines{"-attack", "-attack"});

    // A press resolves in its context, its release releases the same action: A pressed in the
    // field jumps, a menu opens, A's release is the jump's (not OK's).
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true); }) == Lines{"+jump"});
    g_panel = 1;
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false); }) == Lines{"-jump"});
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true); }) == Lines{"+ok"});
    g_panel = 0;
    CHECK(p.sent([&] { pad.button(SDL_GAMEPAD_BUTTON_SOUTH, false); }) == Lines{"-ok"});
    CHECK(p.actions.held() == 0);
}

TEST_CASE("gamepad: hot-plug, the first active pad wins, unplugging and focus loss lift") {
    Pads p;
    virtual_pad::Pad a(Kind::kXbox360);
    REQUIRE_MESSAGE(a.ok(), a.error);
    p.pump();
    CHECK(p.gamepads.count() == 1);
    CHECK(p.gamepads.active() == nullptr); // none until one is pressed

    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_SOUTH, true); }) == Lines{"+jump"});
    CHECK(p.gamepads.active_id() == a.id);

    // A second pad, plugged in while the first one is used: ignored while the first holds
    // something, and its control held then does not press later.
    auto b = std::make_unique<virtual_pad::Pad>(Kind::kPs4);
    REQUIRE_MESSAGE(b->ok(), b->error);
    p.pump();
    CHECK(p.gamepads.count() == 2);
    CHECK(p.sent([&] { b->button(SDL_GAMEPAD_BUTTON_WEST, true); }).empty());
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_SOUTH, false); }) == Lines{"-jump"});
    CHECK(p.sent([&] { b->axis(SDL_GAMEPAD_AXIS_LEFTX, stick(0.1f)); }).empty()); // no press
    CHECK(p.gamepads.active_id() == a.id);
    // Its next press, with the first pad idle, makes it the active one.
    CHECK(p.sent([&] { b->button(SDL_GAMEPAD_BUTTON_EAST, true); }) ==
          Lines{"+slot2", "+attack"}); // West was held: it counts now (button order)
    CHECK(p.gamepads.active_id() == b->id);
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, true); }).empty());
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, false); }).empty());

    // Unplugging the active pad releases what it held; then no pad is active.
    CHECK(p.sent([&] { b.reset(); }) == Lines{"-slot2", "-attack"});
    CHECK(p.gamepads.count() == 1);
    CHECK(p.gamepads.active() == nullptr);
    CHECK(p.actions.held() == 0);

    // Focus lost (or the window hidden, minimised): everything held goes, and stays gone until
    // pressed again.
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, true); }) == Lines{"+slot1"});
    CHECK(p.sent([&] { a.axis(SDL_GAMEPAD_AXIS_LEFTX, stick(0.9f)); }) == Lines{"+right"});
    for (const SDL_EventType type :
         {SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_HIDDEN, SDL_EVENT_WINDOW_MINIMIZED}) {
        CAPTURE(static_cast<int>(type));
        g_actions.clear();
        SDL_Event e{};
        e.type = type;
        CHECK_FALSE(p.gamepads.handle(e));
        if (type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CHECK(g_actions == Lines{"-slot1", "-right"});
        } else {
            CHECK(g_actions.empty());
        }
    }
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, false); }).empty());
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, true); }) == Lines{"+slot1"});
    CHECK(p.sent([&] { a.button(SDL_GAMEPAD_BUTTON_NORTH, false); }) == Lines{"-slot1"});
    // The stick still pushed: its next movement presses again.
    CHECK(p.sent([&] { a.axis(SDL_GAMEPAD_AXIS_LEFTX, stick(0.95f)); }) == Lines{"+right"});
    CHECK(p.sent([&] { a.axis(SDL_GAMEPAD_AXIS_LEFTX, 0); }) == Lines{"-right"});
}

TEST_CASE("actions: the help pages turn by a swipe through Input, not by a key") {
    Pads p;
    g_panel = 10; // HelpScrollKeyPadView
    const fe::Control right{fe::ControlKind::kDirection, static_cast<int>(fe::Direction::kRight)};
    const fe::Control left{fe::ControlKind::kDirection, static_cast<int>(fe::Direction::kLeft)};
    CHECK(p.actions.control(fe::InputSource::kGamepad, right, true) == MT_ACTION_NONE);
    CHECK(g_actions.empty());
    CHECK(p.actions.gestures() == 1);
    for (int i = 0; i < 10; i++) {
        p.actions.turn();
    }
    CHECK(p.actions.gestures() == 0);
    CHECK(p.actions.control(fe::InputSource::kGamepad, right, false) == MT_ACTION_NONE);
    CHECK(g_actions.empty()); // its release is quiet
    CHECK(g_touches == Lines{"began 0x1010 (390, 160)", "moved 0x1010 (352.5, 160)",
                             "moved 0x1010 (315, 160)", "moved 0x1010 (277.5, 160)",
                             "moved 0x1010 (240, 160)", "moved 0x1010 (202.5, 160)",
                             "moved 0x1010 (165, 160)", "moved 0x1010 (127.5, 160)",
                             "moved 0x1010 (90, 160)", "ended 0x1010 (90, 160)"});
    g_touches.clear();
    p.actions.control(fe::InputSource::kGamepad, left, true);
    for (int i = 0; i < 10; i++) {
        p.actions.turn();
    }
    p.actions.control(fe::InputSource::kGamepad, left, false);
    REQUIRE(g_touches.size() == 10);
    CHECK(g_touches.front() == "began 0x1011 (90, 160)");
    CHECK(g_touches.back() == "ended 0x1011 (390, 160)");
    // Elsewhere the same directions are keys.
    g_panel = 0;
    CHECK(p.actions.control(fe::InputSource::kGamepad, right, true) == MT_ACTION_RIGHT);
    CHECK(p.actions.control(fe::InputSource::kGamepad, right, false) == MT_ACTION_RIGHT);
    CHECK(g_actions == Lines{"+right", "-right"});
}

// Direction resolution (#138, ADR 0128). Key rows on a recording sink: g_actions is what the
// runtime would get, and every press has exactly one release.
TEST_CASE("actions: direction_priority game passes every direction on, as before") {
    Pads p;
    const auto key = [](SDL_Keycode k) {
        return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)};
    };
    p.actions.control(fe::InputSource::kKeyboard, key(SDLK_LEFT), true);
    p.actions.control(fe::InputSource::kKeyboard, key(SDLK_UP), true);
    p.actions.turn();
    p.actions.control(fe::InputSource::kKeyboard, key(SDLK_UP), false);
    p.actions.control(fe::InputSource::kKeyboard, key(SDLK_LEFT), false);
    p.actions.turn();
    CHECK(g_actions == Lines{"+left", "+up", "-up", "-left"});
}

TEST_CASE("actions: direction_priority last: the newest held direction replaces the older one") {
    Pads p;
    p.actions.set_direction_priority(fe::DirectionPriority::kLast);
    const auto key = [](SDL_Keycode k) {
        return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)};
    };
    const fe::Control up_key = key(SDLK_UP), left_key = key(SDLK_LEFT), down_key = key(SDLK_DOWN);
    fe::Actions &a = p.actions;
    // Left, then up: left goes, up comes. The newest released: the older one returns.
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    CHECK(g_actions == Lines{"+left", "-left", "+up"});
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, up_key, false);
    CHECK(g_actions == Lines{"-up", "+left"});
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, left_key, false);
    CHECK(g_actions == Lines{"-left"});
    g_actions.clear();
    // An older one released while replaced: quiet, and the newest stands.
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, left_key, false);
    CHECK(g_actions.empty());
    a.control(fe::InputSource::kKeyboard, up_key, false);
    CHECK(g_actions == Lines{"-up"});
    g_actions.clear();
    // Three: the middle one released, then the newest: the first returns.
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    a.control(fe::InputSource::kKeyboard, down_key, true);
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, up_key, false);
    CHECK(g_actions.empty());
    a.control(fe::InputSource::kKeyboard, down_key, false);
    CHECK(g_actions == Lines{"-down", "+left"});
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, left_key, false);
    CHECK(g_actions == Lines{"-left"});
    g_actions.clear();
    // A press of a held direction from another device: it is held twice, each with a release.
    const fe::Control dpad_up{fe::ControlKind::kDirection, static_cast<int>(fe::Direction::kUp)};
    a.control(fe::InputSource::kKeyboard, up_key, true);
    a.control(fe::InputSource::kGamepad, dpad_up, true);
    a.control(fe::InputSource::kKeyboard, up_key, false);
    a.control(fe::InputSource::kGamepad, dpad_up, false);
    CHECK(g_actions == Lines{"+up", "+up", "-up", "-up"});
    g_actions.clear();
    // Focus lost (release_source): nothing comes back, nothing is left held.
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    g_actions.clear();
    a.release_source(fe::InputSource::kKeyboard);
    CHECK(g_actions == Lines{"-up"});
    CHECK(a.held() == 0);
    g_actions.clear();
    // Other actions are not directions: unaffected.
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, key(SDLK_SPACE), true);
    CHECK(g_actions == Lines{"+left", "+jump"});
    a.control(fe::InputSource::kKeyboard, key(SDLK_SPACE), false);
    a.control(fe::InputSource::kKeyboard, left_key, false);
    g_actions.clear();
    // Set while directions are held: applied at the next turn, and back.
    a.set_direction_priority(fe::DirectionPriority::kGame);
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    CHECK(g_actions == Lines{"+left", "+up"});
    g_actions.clear();
    a.set_direction_priority(fe::DirectionPriority::kLast);
    a.turn();
    CHECK(g_actions == Lines{"-left"});
    g_actions.clear();
    a.set_direction_priority(fe::DirectionPriority::kGame);
    a.turn();
    CHECK(g_actions == Lines{"+left"});
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, up_key, false);
    a.control(fe::InputSource::kKeyboard, left_key, false);
    CHECK(g_actions == Lines{"-up", "-left"});
}

namespace {
double g_clock = 0;
double menu_clock(void *) { return g_clock; }
} // namespace

TEST_CASE("actions: a diagonal in a menu is one move, by either priority") {
    Pads p;
    const auto key = [](SDL_Keycode k) {
        return fe::Control{fe::ControlKind::kKey, static_cast<int>(k)};
    };
    const fe::Control up_key = key(SDLK_UP), left_key = key(SDLK_LEFT);
    fe::Actions &a = p.actions;
    g_panel = 1;
    g_clock = 10;
    a.set_clock(menu_clock, nullptr);
    for (const fe::DirectionPriority pri :
         {fe::DirectionPriority::kGame, fe::DirectionPriority::kLast}) {
        g_actions.clear();
        a.set_direction_priority(pri);
        // Two directions together: the first moves, the other waits (one move), both releases
        // quiet for the one that waited.
        a.control(fe::InputSource::kKeyboard, left_key, true);
        g_clock += 0.02;
        a.control(fe::InputSource::kKeyboard, up_key, true);
        a.turn();
        a.control(fe::InputSource::kKeyboard, up_key, false);
        a.control(fe::InputSource::kKeyboard, left_key, false);
        a.turn();
        CHECK(g_actions == Lines{"+left", "-left"});
        // Past the window, a second direction is a deliberate press (#147's "right takes over").
        g_actions.clear();
        a.control(fe::InputSource::kKeyboard, left_key, true);
        g_clock += fe::Actions::kDiagonalSeconds + 0.01;
        a.control(fe::InputSource::kKeyboard, up_key, true);
        CHECK(g_actions == Lines{"+left", "+up"});
        a.control(fe::InputSource::kKeyboard, up_key, false);
        a.control(fe::InputSource::kKeyboard, left_key, false);
        g_clock += 1;
    }
    // A direction held from the field does not make a diagonal; the same direction from two
    // devices is not one.
    g_actions.clear();
    g_panel = 0;
    a.set_direction_priority(fe::DirectionPriority::kGame);
    a.control(fe::InputSource::kKeyboard, left_key, true);
    g_panel = 1;
    a.control(fe::InputSource::kKeyboard, up_key, true);
    CHECK(g_actions == Lines{"+left", "+up"});
    a.control(fe::InputSource::kKeyboard, left_key, false);
    a.control(fe::InputSource::kKeyboard, up_key, false);
    g_actions.clear();
    const fe::Control dpad_up{fe::ControlKind::kDirection, static_cast<int>(fe::Direction::kUp)};
    a.control(fe::InputSource::kKeyboard, up_key, true);
    a.control(fe::InputSource::kGamepad, dpad_up, true);
    CHECK(g_actions == Lines{"+up", "+up"});
    a.control(fe::InputSource::kKeyboard, up_key, false);
    a.control(fe::InputSource::kGamepad, dpad_up, false);
    g_clock += 1;
    // A press the menu held back catches up in the field when the menu closes.
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    g_panel = 0;
    a.turn();
    CHECK(g_actions == Lines{"+left", "+up"});
    a.control(fe::InputSource::kKeyboard, left_key, false);
    a.control(fe::InputSource::kKeyboard, up_key, false);
    // No clock: nothing is a diagonal (the bare Actions of the older tests).
    g_panel = 1;
    a.set_clock(nullptr, nullptr);
    g_actions.clear();
    a.control(fe::InputSource::kKeyboard, left_key, true);
    a.control(fe::InputSource::kKeyboard, up_key, true);
    CHECK(g_actions == Lines{"+left", "+up"});
}

// --- the runtime: a synthetic guest ------------------------------------------------------------

namespace {

// What the guest saw: "setOnLeftKey:1", "checkCrossKeyPad", "event 2 53 0".
Lines g_guest;
u32 g_canvas = 0; // the synthetic SkyCanvas
mt_objc *g_rt = nullptr;

// TouchKeyPadView's ivars (objc.json, docs/controls/keypad.md).
struct PadKey {
    const char *setter;
    u32 offset;
};
const PadKey k_pad_keys[] = {
    {"setOnLeftKey:", 104},   {"setOnRightKey:", 105},  {"setOnUpKey:", 106},
    {"setOnDownKey:", 107},   {"setOnJumpKey:", 108},   {"setOnAttackKey:", 109},
    {"setOnShort1Key:", 110}, {"setOnShort2Key:", 111}, {"setOnShort3Key:", 112},
    {"setOnShort4Key:", 113},
};

void set_pad_bool(mt_cpu *cpu) {
    const std::string sel = mt_objc_sel_name(g_rt, cpu->r[1]);
    for (const PadKey &k : k_pad_keys) {
        if (sel == k.setter) {
            cpu->mem[cpu->r[0] + k.offset] = static_cast<std::uint8_t>(cpu->r[2] != 0);
        }
    }
    g_guest.push_back(sel + std::to_string(cpu->r[2]));
}
void check_cross_key_pad(mt_cpu *) { g_guest.emplace_back("checkCrossKeyPad"); }

// Clet_HandleEvent(type, code, arg), as far as SkyCanvas's slots go: a press waits in +0xD8, a
// release in +0xE8, until the next tick takes them.
void handle_event(mt_cpu *cpu) {
    g_guest.push_back("event " + std::to_string(cpu->r[0]) + " " +
                      std::to_string(static_cast<i32>(cpu->r[1])) + " " +
                      std::to_string(cpu->r[2]));
    const u32 slot = cpu->r[0] == 2 ? 0xD8u : 0xE8u;
    std::memcpy(cpu->mem + g_canvas + slot, &cpu->r[1], 4);
}

const mt_objc_host_method k_pad_methods[] = {
    {"setOnLeftKey:", set_pad_bool},
    {"setOnRightKey:", set_pad_bool},
    {"setOnUpKey:", set_pad_bool},
    {"setOnDownKey:", set_pad_bool},
    {"setOnJumpKey:", set_pad_bool},
    {"setOnAttackKey:", set_pad_bool},
    {"setOnShort1Key:", set_pad_bool},
    {"setOnShort2Key:", set_pad_bool},
    {"setOnShort3Key:", set_pad_bool},
    {"setOnShort4Key:", set_pad_bool},
    {"checkCrossKeyPad", check_cross_key_pad},
};
// UIView's SDK size is 46: the subclasses' ivars follow (EAGLView's up to +92, the pad's to
// touchKeyPadEnable at +228).
const mt_objc_host_class k_eagl_view = {"EAGLView", "UIView", 96, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_touch_keypad_view = {"TouchKeyPadView", "UIView", 232, k_pad_methods, 11,
                                                nullptr,           0};

constexpr u32 k_handle_event = 0x5A75;
constexpr i32 k_no_key = -1000;

double g_now = 1000;
double test_clock(void *) { return g_now; }

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// A window with an EAGLView holding a TouchKeyPadView, as -[EAGLView startApp] builds them; a
// SkyCanvas whose pointer is in a global; CMTimer's lastFrame; the runtime's input module
// pointed at them.
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    u32 eagl = 0;
    u32 pad = 0;
    u32 canvas_global = 0;
    u32 last_frame = 0;

    Guest() {
        g_guest.clear();
        g_now = 1000;
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        g_rt = rt;
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        REQUIRE(mt_objc_add_host_class(rt, &k_eagl_view, 0, 0) == 0);
        REQUIRE(mt_objc_add_host_class(rt, &k_touch_keypad_view, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        mt_ui::State &st = mt_ui::state(&cpu);
        st.config.clock = test_clock;
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        const u32 window = view("UIWindow", 320, 480);
        st.windows.push_back(window);
        eagl = view("EAGLView", 320, 480);
        send(window, "addSubview:", {eagl});
        pad = view("TouchKeyPadView", 480, 320);
        send(eagl, "addSubview:", {pad});
        std::memcpy(cpu.mem + eagl + 84, &pad, 4);
        set_panel(0);
        set_live(true);

        g_canvas = mt_heap_calloc(mem, 1, 0x100);
        canvas_global = mt_heap_calloc(mem, 1, 4);
        last_frame = mt_heap_calloc(mem, 1, 8);
        const i32 none = k_no_key;
        std::memcpy(cpu.mem + g_canvas + 0xD8, &none, 4);
        std::memcpy(cpu.mem + g_canvas + 0xE8, &none, 4);
        std::memcpy(cpu.mem + canvas_global, &g_canvas, 4);
        mt_input_config c = mt_input_config_default();
        c.handle_event = k_handle_event;
        c.canvas_global = canvas_global;
        c.last_frame = last_frame;
        mt_input_configure(&c);
        g_platform_test_guest_addr = k_handle_event;
        g_platform_test_guest_fn = handle_event;
    }
    ~Guest() {
        mt_input_configure(nullptr);
        g_platform_test_guest_fn = nullptr;
        mt_objc_pool_pop(rt, pool);
        mt_ui::state(&cpu).config.clock = nullptr;
        mt_objc_set_current(nullptr);
        g_rt = nullptr;
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
    u32 view(const char *cls, float w, float h) {
        return send(send(klass(cls), "alloc"), "initWithFrame:", {fb(0), fb(0), fb(w), fb(h)});
    }
    void set_panel(i32 panel) { std::memcpy(cpu.mem + eagl + 92, &panel, 4); }
    void set_live(bool live) { cpu.mem[pad + 228] = live ? 1 : 0; }
    std::uint8_t key(u32 offset) const { return cpu.mem[pad + offset]; }
    i32 slot(u32 offset) const {
        i32 v;
        std::memcpy(&v, cpu.mem + g_canvas + offset, 4);
        return v;
    }

    // One UIKit turn (mt_input_turn runs first in it), 1 ms later, in the same game tick.
    Lines turn() {
        g_guest.clear();
        g_now += 0.001;
        mt_uikit_turn(&cpu, g_now);
        return g_guest;
    }
    // A game tick, as CMTimer and SkyCanvas::Tick make it: lastFrame moves on, the waiting
    // key events are taken. Then a turn.
    Lines tick() {
        std::int64_t t;
        std::memcpy(&t, cpu.mem + last_frame, 8);
        t += 33;
        std::memcpy(cpu.mem + last_frame, &t, 8);
        const i32 none = k_no_key;
        std::memcpy(cpu.mem + g_canvas + 0xD8, &none, 4);
        std::memcpy(cpu.mem + g_canvas + 0xE8, &none, 4);
        return turn();
    }
};

} // namespace

TEST_CASE("input runtime: the action table") {
    CHECK(mt_input_action_get_info(MT_ACTION_NONE) == nullptr);
    CHECK(mt_input_action_get_info(MT_ACTION_COUNT) == nullptr);
    for (u32 a = 1; a < MT_ACTION_COUNT; a++) {
        const mt_input_action_info *info = mt_input_action_get_info(a);
        REQUIRE(info != nullptr);
        CHECK(mt_input_action_named(info->name) == a);
    }
    CHECK(mt_input_action_named("teleport") == MT_ACTION_NONE);
    CHECK(mt_input_action_named(nullptr) == MT_ACTION_NONE);
    CHECK(std::string(mt_input_action_get_info(MT_ACTION_LEFT)->pad_key) == "onLeftKey");
    CHECK(mt_input_action_get_info(MT_ACTION_ATTACK)->code == '5');
    CHECK(mt_input_action_get_info(MT_ACTION_SLOT4)->code == '#');
    CHECK(mt_input_action_get_info(MT_ACTION_OK)->pad_key == nullptr);
    CHECK(mt_input_action_get_info(MT_ACTION_OK)->code == -5);
}

TEST_CASE("input runtime: pad keys as the pad's BOOLs, one step per tick, presses together") {
    Guest g;
    g.turn(); // the first turn: the EAGLView, the panel, the pad, the tick
    CHECK(mt_input_panel() == 0);
    CHECK(mt_input_pad_live() == 1);

    // Left and jump pressed together (one pump): one step, both BOOLs, the d-pad's images.
    mt_input_press(MT_ACTION_LEFT);
    mt_input_press(MT_ACTION_JUMP);
    CHECK(g.turn() == Lines{"setOnLeftKey:1", "setOnJumpKey:1", "checkCrossKeyPad"});
    CHECK(g.key(104) == 1);
    CHECK(g.key(108) == 1);
    // Released in the same tick: they wait for the next one (the tick has to see them held).
    mt_input_release(MT_ACTION_JUMP);
    mt_input_release(MT_ACTION_LEFT);
    CHECK(g.turn().empty());
    CHECK(g.turn().empty());
    CHECK(mt_input_get_stats().queued == 2);
    CHECK(g.tick() == Lines{"setOnJumpKey:0", "setOnLeftKey:0", "checkCrossKeyPad"});
    CHECK(g.key(104) == 0);
    CHECK(g.key(108) == 0);

    // A press, release and press of one key in one turn: nothing lost, one change per tick.
    mt_input_press(MT_ACTION_ATTACK);
    mt_input_release(MT_ACTION_ATTACK);
    mt_input_press(MT_ACTION_ATTACK);
    CHECK(g.tick() == Lines{"setOnAttackKey:1"});
    CHECK(g.tick() == Lines{"setOnAttackKey:0"});
    CHECK(g.tick() == Lines{"setOnAttackKey:1"});
    CHECK(g.tick().empty());
    // Two devices hold it: the guest sees the first press and the last release.
    mt_input_press(MT_ACTION_ATTACK);
    CHECK(mt_input_held(MT_ACTION_ATTACK) == 2);
    mt_input_release(MT_ACTION_ATTACK);
    CHECK(g.tick().empty());
    CHECK(g.key(109) == 1);
    mt_input_release(MT_ACTION_ATTACK);
    mt_input_release(MT_ACTION_ATTACK); // not held: ignored
    CHECK(g.tick() == Lines{"setOnAttackKey:0"});
    CHECK(g.tick().empty());

    // The quick slots have BOOLs too; no d-pad images for them.
    mt_input_press(MT_ACTION_SLOT1);
    mt_input_press(MT_ACTION_SLOT4);
    CHECK(g.tick() == Lines{"setOnShort1Key:1", "setOnShort4Key:1"});
    mt_input_release_all();
    CHECK(g.tick() == Lines{"setOnShort1Key:0", "setOnShort4Key:0"});

    const mt_input_stats s = mt_input_get_stats();
    CHECK(s.queued == 0);
    CHECK(s.presses == 6);
    CHECK(s.releases == 6);
    CHECK(s.pad_edges == 12);
    CHECK(s.key_events == 0);
}

TEST_CASE("input runtime: key events through Clet_HandleEvent wait for SkyCanvas's slots") {
    Guest g;
    g.turn();
    // OK has no key on the pad: a key event even while the pad is live.
    mt_input_press(MT_ACTION_OK);
    mt_input_release(MT_ACTION_OK);
    CHECK(g.turn() == Lines{"event 2 -5 0"});
    CHECK(g.slot(0xD8) == -5);
    // The next tick has not come: the release waits, then goes.
    CHECK(g.turn().empty());
    CHECK(g.tick() == Lines{"event 3 -5 0"});

    // With the pad off (a menu's panel), the pad's actions are key events too.
    g.set_live(false);
    g.set_panel(1);
    g.turn();
    CHECK(mt_input_panel() == 1);
    CHECK(mt_input_pad_live() == 0);
    mt_input_press(MT_ACTION_ATTACK);
    mt_input_press(MT_ACTION_DOWN);
    CHECK(g.tick() == Lines{"event 2 53 0"});
    CHECK(g.tick() == Lines{"event 2 -2 0"});
    // A slot still busy at the next tick (the game has not taken it): wait.
    const u32 before = static_cast<u32>(mt_input_get_stats().waits_busy);
    mt_input_release(MT_ACTION_DOWN);
    std::int64_t t;
    std::memcpy(&t, g.cpu.mem + g.last_frame, 8);
    t += 33;
    std::memcpy(g.cpu.mem + g.last_frame, &t, 8);
    CHECK(g.turn().empty()); // the press of -2 is still in +0xD8
    CHECK(mt_input_get_stats().waits_busy == before + 1);
    CHECK(g.tick() == Lines{"event 3 -2 0"});
    mt_input_release(MT_ACTION_ATTACK);
    CHECK(g.tick() == Lines{"event 3 53 0"});
    // Before the engine is up (no SkyCanvas), nothing is delivered, nothing lost.
    const u32 none = 0;
    std::memcpy(g.cpu.mem + g.canvas_global, &none, 4);
    mt_input_press(MT_ACTION_MAP);
    CHECK(g.tick().empty());
    std::memcpy(g.cpu.mem + g.canvas_global, &g_canvas, 4);
    CHECK(g.tick() == Lines{"event 2 42 0"});
    const mt_input_stats s = mt_input_get_stats();
    CHECK(s.key_events == 7);
    CHECK(s.pad_edges == 0);
}

TEST_CASE("input runtime: a release goes the way its press went") {
    Guest g;
    g.turn();
    // Left pressed on the live pad, the pad goes off (a popup) before the release: the BOOL is
    // cleared, not a key release sent.
    mt_input_press(MT_ACTION_LEFT);
    CHECK(g.tick() == Lines{"setOnLeftKey:1", "checkCrossKeyPad"});
    g.set_live(false);
    g.set_panel(7);
    mt_input_release(MT_ACTION_LEFT);
    CHECK(g.tick() == Lines{"setOnLeftKey:0", "checkCrossKeyPad"});
    CHECK(g.key(104) == 0);
    // Jump pressed as a key while the pad is off, the pad comes back: a key release.
    mt_input_press(MT_ACTION_JUMP);
    CHECK(g.tick() == Lines{"event 2 50 0"});
    g.set_live(true);
    g.set_panel(0);
    mt_input_release(MT_ACTION_JUMP);
    CHECK(g.tick() == Lines{"event 3 50 0"});
    CHECK(g.key(108) == 0);
    // A step is one route: a BOOL change and a key event queued together go in two ticks.
    mt_input_press(MT_ACTION_RIGHT);
    mt_input_press(MT_ACTION_MENU);
    mt_input_press(MT_ACTION_UP);
    CHECK(g.tick() == Lines{"setOnRightKey:1", "checkCrossKeyPad"});
    CHECK(g.tick() == Lines{"event 2 -16 0"});
    CHECK(g.tick() == Lines{"setOnUpKey:1", "checkCrossKeyPad"});
    mt_input_release_all(); // in table order: up, right, menu
    CHECK(g.tick() == Lines{"setOnUpKey:0", "setOnRightKey:0", "checkCrossKeyPad"});
    CHECK(g.tick() == Lines{"event 3 -16 0"});
    CHECK(g.tick().empty());
}

TEST_CASE("input runtime: without an EAGLView there is no panel and no pad") {
    Guest g;
    g.send(g.eagl, "removeFromSuperview");
    g.turn();
    CHECK(mt_input_panel() == -1);
    CHECK(mt_input_pad_live() == 0);
    // The pad's actions go in as keys then.
    mt_input_press(MT_ACTION_RIGHT);
    CHECK(g.tick() == Lines{"event 2 -4 0"});
    mt_input_release(MT_ACTION_RIGHT);
    CHECK(g.tick() == Lines{"event 3 -4 0"});
}
