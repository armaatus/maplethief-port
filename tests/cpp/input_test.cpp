// Touch input (#65, ADR 0047) with synthetic data only: SDL events pushed with SDL_PushEvent on
// a dummy-driver window, polled and handed to the frontend's Input, which turns them into
// mt_uikit_touch calls. First on a recording sink (the mouse as one touch, the bars, clamping,
// interruptions, the right button's fixed touch, SDL's synthetic events, fingers, the activity
// listener; a safe-area placement in both landscape orientations, turning the phone, a pen, #73);
// then into a synthetic guest built like the game's screens: a MainMenuView with its
// Play button at (46, 148, 120, 48) whose touch-up-inside sends -clickPlay:, and a multi-touch
// TouchKeyPadView (480x320) with a key button, which tracks two fingers itself.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "foundation/foundation.h"
#include "input.h"
#include "maplethief/cf.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/uikit.h"
#include "uikit/uikit_internal.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// --- SDL: a dummy window and pushed events ----------------------------------------------------

// A 1000x700 dummy-driver window; the 480x320 screen at 2x (integer mode) is 960x640 at
// (20, 30), with bars around it. Window point (20 + 2x, 30 + 2y) shows screen point (x, y).
struct Window {
    SDL_Window *window = nullptr;
    fe::Placement placement;
    Window() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("input_test", 1000, 700, SDL_WINDOW_HIDDEN);
        REQUIRE_MESSAGE(window != nullptr, SDL_GetError());
        placement = fe::place(fe::ScaleMode::kInteger, 1000, 700, 480, 320);
        REQUIRE(placement.rect.x == 20);
        REQUIRE(placement.rect.y == 30);
        REQUIRE(placement.integer_scale == 2);
        drain();
    }
    ~Window() {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    // Drops what the driver queued (the window's own events).
    void drain() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
    }
    // Polls every queued event into `input`.
    void pump(fe::Input &input) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input.handle(e);
        }
    }
    void push(SDL_Event &e) { REQUIRE_MESSAGE(SDL_PushEvent(&e), SDL_GetError()); }

    // Screen point -> window point.
    static float wx(float x) { return 20 + 2 * x; }
    static float wy(float y) { return 30 + 2 * y; }

    void button(bool down, float x, float y, Uint8 which_button = SDL_BUTTON_LEFT,
                SDL_MouseID which = 1) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = SDL_GetWindowID(window);
        e.button.which = which;
        e.button.button = which_button;
        e.button.down = down;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        push(e);
    }
    void motion(float x, float y, SDL_MouseButtonFlags state = SDL_BUTTON_LMASK,
                SDL_MouseID which = 1) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(window);
        e.motion.which = which;
        e.motion.state = state;
        e.motion.x = x;
        e.motion.y = y;
        push(e);
    }
    // A finger at window point (x, y) (SDL's are normalised to the window).
    void finger(SDL_EventType type, SDL_FingerID id, float x, float y, SDL_TouchID touch = 77) {
        SDL_Event e{};
        e.type = type;
        e.tfinger.touchID = touch;
        e.tfinger.fingerID = id;
        e.tfinger.x = x / 1000.0f;
        e.tfinger.y = y / 700.0f;
        e.tfinger.pressure = 1;
        e.tfinger.windowID = SDL_GetWindowID(window);
        push(e);
    }
    void window_event(SDL_EventType type) {
        SDL_Event e{};
        e.type = type;
        e.window.windowID = SDL_GetWindowID(window);
        push(e);
    }
};

// A recording sink: "finger phase (x, y)", and the numbers.
struct Call {
    u32 finger, phase;
    float x, y;
};
struct Sink {
    std::vector<std::string> calls;
    std::vector<Call> raw;
    std::vector<fe::InputSource> activity;
    static int touch(void *user, u32 finger, u32 phase, float x, float y) {
        static const char *const k_phase[] = {"began", "moved", "ended", "cancelled"};
        char line[96];
        std::snprintf(line, sizeof line, "0x%X %s (%g, %g)", static_cast<unsigned>(finger),
                      k_phase[phase & 3], static_cast<double>(x), static_cast<double>(y));
        static_cast<Sink *>(user)->calls.emplace_back(line);
        static_cast<Sink *>(user)->raw.push_back(Call{finger, phase, x, y});
        return 0;
    }
    static void active(void *user, fe::InputSource source) {
        static_cast<Sink *>(user)->activity.push_back(source);
    }
    // The calls so far, cleared.
    std::vector<std::string> take() {
        std::vector<std::string> out;
        out.swap(calls);
        raw.clear();
        return out;
    }
};

using Calls = std::vector<std::string>;

} // namespace

TEST_CASE("input: the left mouse button is one touch, through the window mapping") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    in.set_activity_listener(Sink::active, &sink);
    in.set_window(w.window, &w.placement);

    // Down, a drag (motion while held), up: Began, Moved, Ended in screen points.
    w.button(true, w.wx(106), w.wy(172));
    w.motion(w.wx(110), w.wy(170));
    w.motion(w.wx(110.5f), w.wy(170.25f));
    w.button(false, w.wx(110.5f), w.wy(170.25f));
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (106, 172)", "0x1 moved (110, 170)",
                               "0x1 moved (110.5, 170.25)", "0x1 ended (110.5, 170.25)"});
    CHECK(in.live() == 0);
    CHECK(sink.activity == std::vector<fe::InputSource>{fe::InputSource::kMouse});

    // Motion without a press, other buttons, an up without a down: nothing.
    w.motion(w.wx(50), w.wy(50), 0);
    w.button(true, w.wx(50), w.wy(50), SDL_BUTTON_MIDDLE);
    w.button(false, w.wx(50), w.wy(50), SDL_BUTTON_MIDDLE);
    w.button(false, w.wx(50), w.wy(50));
    w.pump(in);
    CHECK(sink.take().empty());

    // A press over the bars is dropped, and so is everything until its up.
    w.button(true, 10, 300);
    w.motion(w.wx(50), w.wy(50));
    w.button(false, w.wx(50), w.wy(50));
    w.pump(in);
    CHECK(sink.take().empty());

    // A drag out of the image is clamped to its edge, and ends there.
    w.button(true, w.wx(5), w.wy(300));
    w.motion(5, w.wy(300));  // the left bar: x 0
    w.motion(5, 690);        // the bottom left corner
    w.motion(5, 690);        // no movement on the screen: no Moved
    w.button(false, 999, 2); // released over the top right bar
    w.pump(in);
    const Calls clamped = sink.take();
    REQUIRE(clamped.size() == 4);
    CHECK(clamped[0] == "0x1 began (5, 300)");
    CHECK(clamped[1] == "0x1 moved (0, 300)");
    CHECK(clamped[2] == "0x1 moved (0, 320)"); // just below 320, printed rounded
    CHECK(clamped[3] == "0x1 ended (480, 0)");
}

TEST_CASE("input: interruptions, a lost up and a second down end the touch where it is") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    in.set_window(w.window, &w.placement);

    // Focus lost mid-press: Ended where the touch is (never Cancelled: the game's key pad
    // releases a key only on touchesEnded:); the up that follows does nothing.
    w.button(true, w.wx(100), w.wy(100));
    w.window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    w.button(false, w.wx(100), w.wy(100));
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (100, 100)", "0x1 ended (100, 100)"});

    // Hidden and minimised windows too, for every finger.
    for (const SDL_EventType type : {SDL_EVENT_WINDOW_HIDDEN, SDL_EVENT_WINDOW_MINIMIZED}) {
        w.button(true, w.wx(100), w.wy(100));
        w.finger(SDL_EVENT_FINGER_DOWN, 5, w.wx(200), w.wy(200));
        w.window_event(type);
        w.finger(SDL_EVENT_FINGER_UP, 5, w.wx(200), w.wy(200));
        w.pump(in);
        CHECK(sink.take() == Calls{"0x1 began (100, 100)", "0x100 began (200, 200)",
                                   "0x1 ended (100, 100)", "0x100 ended (200, 200)"});
        CHECK(in.live() == 0);
    }

    // The mouse leaving the window (no capture) ends the mouse's touch, not a finger's.
    w.button(true, w.wx(100), w.wy(100));
    w.finger(SDL_EVENT_FINGER_DOWN, 6, w.wx(200), w.wy(200));
    w.window_event(SDL_EVENT_WINDOW_MOUSE_LEAVE);
    w.finger(SDL_EVENT_FINGER_UP, 6, w.wx(200), w.wy(200));
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (100, 100)", "0x100 began (200, 200)",
                               "0x1 ended (100, 100)", "0x100 ended (200, 200)"});

    // Motion that shows the button no longer held: its up was lost; the touch ends there.
    w.button(true, w.wx(100), w.wy(100));
    w.motion(w.wx(120), w.wy(100), 0);
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (100, 100)", "0x1 ended (120, 100)"});

    // A second down without an up: the first touch ends, a new one begins.
    w.button(true, w.wx(100), w.wy(100));
    w.button(true, w.wx(300), w.wy(100));
    w.button(false, w.wx(300), w.wy(100));
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (100, 100)", "0x1 ended (100, 100)",
                               "0x1 began (300, 100)", "0x1 ended (300, 100)"});

    // release_all() on demand.
    w.button(true, w.wx(10), w.wy(10));
    w.pump(in);
    in.release_all();
    CHECK(sink.take() == Calls{"0x1 began (10, 10)", "0x1 ended (10, 10)"});
}

TEST_CASE("input: the right button's fixed touch, SDL's synthetic events, virtual fingers") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    in.set_activity_listener(Sink::active, &sink);
    in.set_window(w.window, &w.placement);

    // right_touch off (the default): the right button does nothing.
    w.button(true, w.wx(50), w.wy(50), SDL_BUTTON_RIGHT);
    w.button(false, w.wx(50), w.wy(50), SDL_BUTTON_RIGHT);
    w.pump(in);
    CHECK(sink.take().empty());
    // On: a second touch at the fixed point while held, wherever the pointer is, alongside a
    // left-button drag.
    in.set_right_touch(fe::RightTouch{true, 430, 270});
    w.button(true, w.wx(60), w.wy(260));
    w.button(true, w.wx(50), w.wy(50), SDL_BUTTON_RIGHT);
    w.motion(w.wx(80), w.wy(260), SDL_BUTTON_LMASK | SDL_BUTTON_RMASK);
    w.button(false, w.wx(80), w.wy(260), SDL_BUTTON_RIGHT);
    w.button(false, w.wx(80), w.wy(260));
    w.pump(in);
    CHECK(sink.take() == Calls{"0x1 began (60, 260)", "0x2 began (430, 270)", "0x1 moved (80, 260)",
                               "0x2 ended (430, 270)", "0x1 ended (80, 260)"});

    // SDL's mouse made from a touch, and its touch made from the mouse: dropped (the finger
    // and the mouse events have them).
    w.button(true, w.wx(10), w.wy(10), SDL_BUTTON_LEFT, SDL_TOUCH_MOUSEID);
    w.motion(w.wx(20), w.wy(10), SDL_BUTTON_LMASK, SDL_TOUCH_MOUSEID);
    w.button(false, w.wx(20), w.wy(10), SDL_BUTTON_LEFT, SDL_TOUCH_MOUSEID);
    w.finger(SDL_EVENT_FINGER_DOWN, 1, w.wx(10), w.wy(10), SDL_MOUSE_TOUCHID);
    w.finger(SDL_EVENT_FINGER_UP, 1, w.wx(10), w.wy(10), SDL_MOUSE_TOUCHID);
    w.pump(in);
    CHECK(sink.take().empty());

    // The shared entry point: virtual finger 3 of the gamepad is finger 0x1003; a Began off
    // the screen is dropped; later phases are clamped.
    sink.activity.clear();
    CHECK(in.touch(fe::InputSource::kGamepad, 3, MT_UIKIT_TOUCH_BEGAN, 60, 260));
    CHECK(in.touch(fe::InputSource::kGamepad, 3, MT_UIKIT_TOUCH_MOVED, -20, 260));
    CHECK(in.touch(fe::InputSource::kGamepad, 3, MT_UIKIT_TOUCH_ENDED, -20, 260));
    CHECK_FALSE(in.touch(fe::InputSource::kGamepad, 4, MT_UIKIT_TOUCH_BEGAN, 500, 10));
    CHECK_FALSE(in.touch(fe::InputSource::kGamepad, 4, MT_UIKIT_TOUCH_ENDED, 50, 10));
    CHECK_FALSE(in.touch(fe::InputSource::kGamepad, 4, 9, 50, 10));
    CHECK(sink.take() ==
          Calls{"0x1003 began (60, 260)", "0x1003 moved (0, 260)", "0x1003 ended (0, 260)"});

    // Activity: touches (and their source) and non-touch presses.
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_A;
    w.push(key);
    key.key.repeat = true; // a key's repeats are not new input
    w.push(key);
    SDL_Event pad{};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    w.push(pad);
    w.pump(in);
    CHECK(sink.activity == std::vector<fe::InputSource>{fe::InputSource::kGamepad,
                                                        fe::InputSource::kKeyboard,
                                                        fe::InputSource::kGamepad});
    CHECK(fe::is_touch_source(fe::InputSource::kMouse));
    CHECK(fe::is_touch_source(fe::InputSource::kFinger));
    CHECK(fe::is_touch_source(fe::InputSource::kScripted));
    CHECK_FALSE(fe::is_touch_source(fe::InputSource::kGamepad));
    CHECK_FALSE(fe::is_touch_source(fe::InputSource::kKeyboard));
    CHECK(std::string(fe::input_source_name(fe::InputSource::kFinger)) == "finger");
}

TEST_CASE("input: widescreen's side margin lets touches begin beside the screen (#168)") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    in.set_window(w.window, &w.placement);
    CHECK(in.side_margin() == 0.0f);
    // Without it, beside the screen image is the bars: dropped.
    w.button(true, w.wx(-10), w.wy(250));
    w.button(false, w.wx(-10), w.wy(250));
    w.pump(in);
    CHECK(sink.take().empty());
    // 10 points each side: a click there and a virtual finger there are touches, moves clamp
    // to the band and no further.
    in.set_side_margin(10);
    w.button(true, w.wx(-5), w.wy(250));
    w.motion(w.wx(-9), w.wy(260), SDL_BUTTON_LMASK);
    w.button(false, w.wx(-9), w.wy(260));
    w.pump(in);
    CHECK(sink.take() ==
          Calls{"0x1 began (-5, 250)", "0x1 moved (-9, 260)", "0x1 ended (-9, 260)"});
    CHECK(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, 485, 290));
    CHECK(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_MOVED, 520, 290));
    CHECK(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_ENDED, 520, 290));
    const Calls right = sink.take();
    REQUIRE(right.size() == 3);
    CHECK(right[0] == "0x1 began (485, 290)");
    CHECK(right[1].rfind("0x1 moved (490", 0) == 0);
    CHECK_FALSE(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, -11, 250));
    CHECK_FALSE(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, 5, 330));
    // Back to 0: the screen only.
    in.set_side_margin(0);
    CHECK_FALSE(in.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, -5, 250));
    CHECK(sink.take().empty());
}

TEST_CASE("input: SDL fingers are touches, one finger id per finger while it is down") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    in.set_window(w.window, &w.placement);

    w.finger(SDL_EVENT_FINGER_DOWN, 1001, w.wx(60), w.wy(260));
    w.finger(SDL_EVENT_FINGER_DOWN, 1002, w.wx(420), w.wy(270));
    w.finger(SDL_EVENT_FINGER_MOTION, 1001, w.wx(80), w.wy(262));
    w.finger(SDL_EVENT_FINGER_UP, 1002, w.wx(420), w.wy(270));
    // A new finger takes the free slot; the same finger id on another device is another finger.
    w.finger(SDL_EVENT_FINGER_DOWN, 1003, w.wx(300), w.wy(100));
    w.finger(SDL_EVENT_FINGER_DOWN, 1001, w.wx(10), w.wy(10), 78);
    w.finger(SDL_EVENT_FINGER_CANCELED, 1003, w.wx(300), w.wy(100));
    w.finger(SDL_EVENT_FINGER_UP, 1001, w.wx(80), w.wy(262));
    w.finger(SDL_EVENT_FINGER_UP, 1001, w.wx(10), w.wy(10), 78);
    // A finger down over the bars: nothing, also for its motion and up.
    w.finger(SDL_EVENT_FINGER_DOWN, 1004, 5, 5);
    w.finger(SDL_EVENT_FINGER_MOTION, 1004, w.wx(10), w.wy(10));
    w.finger(SDL_EVENT_FINGER_UP, 1004, w.wx(10), w.wy(10));
    w.pump(in);
    // Finger, phase, screen point (through SDL's normalised coordinates: within float rounding).
    // The system's cancel of a finger ends its touch, as every interruption does.
    const std::vector<Call> want = {
        {0x100, MT_UIKIT_TOUCH_BEGAN, 60, 260},  {0x101, MT_UIKIT_TOUCH_BEGAN, 420, 270},
        {0x100, MT_UIKIT_TOUCH_MOVED, 80, 262},  {0x101, MT_UIKIT_TOUCH_ENDED, 420, 270},
        {0x101, MT_UIKIT_TOUCH_BEGAN, 300, 100}, {0x102, MT_UIKIT_TOUCH_BEGAN, 10, 10},
        {0x101, MT_UIKIT_TOUCH_ENDED, 300, 100}, {0x100, MT_UIKIT_TOUCH_ENDED, 80, 262},
        {0x102, MT_UIKIT_TOUCH_ENDED, 10, 10},
    };
    REQUIRE(sink.raw.size() == want.size());
    for (std::size_t i = 0; i < want.size(); i++) {
        INFO("call " << i << ": " << sink.calls[i]);
        CHECK(sink.raw[i].finger == want[i].finger);
        CHECK(sink.raw[i].phase == want[i].phase);
        CHECK(sink.raw[i].x == doctest::Approx(want[i].x).epsilon(1e-4));
        CHECK(sink.raw[i].y == doctest::Approx(want[i].y).epsilon(1e-4));
    }
    CHECK(in.live() == 0);
}

TEST_CASE("input: iOS: fingers through a safe-area placement in both orientations (#73)") {
    Window w;
    fe::Input in;
    Sink sink;
    in.set_sink(Sink::touch, &sink);
    // A 30-point strip on the left (landscape left: the image moves from x 20 to 30), then on
    // the right (landscape right: to 10). Window point (x0 + 2x, 30 + 2y) is screen (x, y).
    struct Case {
        fe::Insets insets;
        float x0;
    };
    for (const Case &c : {Case{fe::Insets{0, 30, 0, 0}, 30}, Case{fe::Insets{0, 0, 0, 30}, 10}}) {
        INFO("insets left " << c.insets.left << ", right " << c.insets.right);
        w.placement = fe::place(fe::ScaleMode::kInteger, 1000, 700, 480, 320, c.insets, 1, 1);
        REQUIRE(w.placement.rect.x == c.x0);
        in.set_window(w.window, &w.placement);
        const auto wx = [&](float x) { return c.x0 + 2 * x; };
        // Two fingers (the d-pad and attack corners of the key pad), a drag, the ups.
        w.finger(SDL_EVENT_FINGER_DOWN, 1, wx(40), w.wy(280));
        w.finger(SDL_EVENT_FINGER_DOWN, 2, wx(440), w.wy(280));
        w.finger(SDL_EVENT_FINGER_MOTION, 1, wx(60), w.wy(280));
        w.finger(SDL_EVENT_FINGER_UP, 2, wx(440), w.wy(280));
        w.finger(SDL_EVENT_FINGER_UP, 1, wx(60), w.wy(280));
        // A finger in the strip is over the bars: dropped.
        const float strip = c.insets.left > 0 ? 15.0f : 985.0f;
        w.finger(SDL_EVENT_FINGER_DOWN, 3, strip, w.wy(160));
        w.finger(SDL_EVENT_FINGER_UP, 3, strip, w.wy(160));
        w.pump(in);
        const std::vector<Call> want = {
            {0x100, MT_UIKIT_TOUCH_BEGAN, 40, 280}, {0x101, MT_UIKIT_TOUCH_BEGAN, 440, 280},
            {0x100, MT_UIKIT_TOUCH_MOVED, 60, 280}, {0x101, MT_UIKIT_TOUCH_ENDED, 440, 280},
            {0x100, MT_UIKIT_TOUCH_ENDED, 60, 280},
        };
        REQUIRE(sink.raw.size() == want.size());
        for (std::size_t i = 0; i < want.size(); i++) {
            INFO("call " << i << ": " << sink.calls[i]);
            CHECK(sink.raw[i].finger == want[i].finger);
            CHECK(sink.raw[i].phase == want[i].phase);
            CHECK(sink.raw[i].x == doctest::Approx(want[i].x).epsilon(1e-4));
            CHECK(sink.raw[i].y == doctest::Approx(want[i].y).epsilon(1e-4));
        }
        sink.take();
        CHECK(in.live() == 0);
    }

    // Turning the phone over (the display's orientation, the safe area changing) lifts every
    // live touch where it is: a held finger must not jump to the mirrored point.
    in.set_window(w.window, &w.placement); // the second case's: x0 10
    for (const SDL_EventType type :
         {SDL_EVENT_DISPLAY_ORIENTATION, SDL_EVENT_WINDOW_SAFE_AREA_CHANGED}) {
        w.finger(SDL_EVENT_FINGER_DOWN, 4, 10 + 2 * 100.0f, w.wy(200));
        w.button(true, 10 + 2 * 300.0f, w.wy(100));
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_DISPLAY_ORIENTATION) {
            e.display.displayID = SDL_GetPrimaryDisplay();
            e.display.data1 = SDL_ORIENTATION_LANDSCAPE_FLIPPED;
        } else {
            e.window.windowID = SDL_GetWindowID(w.window);
        }
        w.push(e);
        w.finger(SDL_EVENT_FINGER_MOTION, 4, 10 + 2 * 120.0f, w.wy(200));
        w.finger(SDL_EVENT_FINGER_UP, 4, 10 + 2 * 120.0f, w.wy(200));
        w.button(false, 10 + 2 * 300.0f, w.wy(100));
        w.pump(in);
        const Calls got = sink.take();
        REQUIRE(got.size() == 4);
        CHECK(got[0] == "0x100 began (100, 200)");
        CHECK(got[1] == "0x1 began (300, 100)");
        // Both end at their last point, in the order they went down; nothing after.
        CHECK(got[2] == "0x100 ended (100, 200)");
        CHECK(got[3] == "0x1 ended (300, 100)");
        CHECK(in.live() == 0);
    }

    // A pen (an Apple Pencil, a drawing tablet): SDL sends a mouse press (SDL_PEN_MOUSEID) and a
    // touch (SDL_PEN_TOUCHID) for it. The touch counts, the mouse press is dropped: one touch.
    w.button(true, 10 + 2 * 50.0f, w.wy(50), SDL_BUTTON_LEFT, SDL_PEN_MOUSEID);
    w.finger(SDL_EVENT_FINGER_DOWN, 1, 10 + 2 * 50.0f, w.wy(50), SDL_PEN_TOUCHID);
    w.motion(10 + 2 * 60.0f, w.wy(50), SDL_BUTTON_LMASK, SDL_PEN_MOUSEID);
    w.finger(SDL_EVENT_FINGER_MOTION, 1, 10 + 2 * 60.0f, w.wy(50), SDL_PEN_TOUCHID);
    w.button(false, 10 + 2 * 60.0f, w.wy(50), SDL_BUTTON_LEFT, SDL_PEN_MOUSEID);
    w.finger(SDL_EVENT_FINGER_UP, 1, 10 + 2 * 60.0f, w.wy(50), SDL_PEN_TOUCHID);
    w.pump(in);
    REQUIRE(sink.raw.size() == 3);
    CHECK(sink.raw[0].finger == 0x100);
    CHECK(sink.raw[0].phase == MT_UIKIT_TOUCH_BEGAN);
    CHECK(sink.raw[0].x == doctest::Approx(50).epsilon(1e-4));
    CHECK(sink.raw[1].phase == MT_UIKIT_TOUCH_MOVED);
    CHECK(sink.raw[1].x == doctest::Approx(60).epsilon(1e-4));
    CHECK(sink.raw[2].phase == MT_UIKIT_TOUCH_ENDED);
    CHECK(in.live() == 0);
}

// --- into the guest -----------------------------------------------------------------------------

namespace {

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

// What the guest's views saw.
struct Seen {
    std::vector<std::string> actions; // "clickPlay:", "clickCashButton:", ...
    // "TouchKeyPadView began #1 (60, 260) prev (60, 260)": the class, the phase, which touch
    // (#1, #2: the UITouch objects in the order they first came), its location and previous
    // location in the receiving view.
    std::vector<std::string> touches;
    std::vector<u32> objects; // the UITouch objects seen, in first-seen order
};
Seen g_seen;
u32 g_scratch = 0;

void record_action(mt_cpu *cpu) {
    g_seen.actions.emplace_back(mt_objc_sel_name(mt_objc_current(), cpu->r[1]));
}

void record_touches(mt_cpu *cpu, const char *phase) {
    const u32 self = cpu->r[0];
    mt_objc *rt = mt_objc_current();
    const std::vector<u32> *items = mt_ns::set_items(cpu, cpu->r[2]);
    if (items == nullptr) {
        return;
    }
    for (const u32 touch : *items) {
        std::size_t n = 0;
        while (n < g_seen.objects.size() && g_seen.objects[n] != touch) {
            n++;
        }
        if (n == g_seen.objects.size()) {
            g_seen.objects.push_back(touch);
        }
        float p[2] = {}, q[2] = {};
        const u32 args[1] = {self};
        mt_objc_msg_stret(cpu, g_scratch, touch, mt_objc_sel(rt, "locationInView:"), args, 1);
        std::memcpy(p, cpu->mem + g_scratch, 8);
        mt_objc_msg_stret(cpu, g_scratch, touch, mt_objc_sel(rt, "previousLocationInView:"), args,
                          1);
        std::memcpy(q, cpu->mem + g_scratch, 8);
        char line[128];
        std::snprintf(line, sizeof line, "%s %s #%zu (%g, %g) prev (%g, %g)",
                      mt_objc_class_name(rt, mt_objc_class_of(rt, self)), phase, n + 1,
                      static_cast<double>(p[0]), static_cast<double>(p[1]),
                      static_cast<double>(q[0]), static_cast<double>(q[1]));
        g_seen.touches.emplace_back(line);
    }
}
void touches_began(mt_cpu *cpu) { record_touches(cpu, "began"); }
void touches_moved(mt_cpu *cpu) { record_touches(cpu, "moved"); }
void touches_ended(mt_cpu *cpu) { record_touches(cpu, "ended"); }
void touches_cancelled(mt_cpu *cpu) { record_touches(cpu, "cancelled"); }

// MainMenuView: its Play button's action.
const mt_objc_host_method k_menu_methods[] = {{"clickPlay:", record_action}};
const mt_objc_host_class k_main_menu_view = {"MainMenuView", "UIView", 46, k_menu_methods, 1,
                                             nullptr,        0};
// TouchKeyPadView: tracks its fingers in touches*; its key buttons' actions.
const mt_objc_host_method k_keypad_methods[] = {
    {"touchesBegan:withEvent:", touches_began}, {"touchesMoved:withEvent:", touches_moved},
    {"touchesEnded:withEvent:", touches_ended}, {"touchesCancelled:withEvent:", touches_cancelled},
    {"clickCashButton:", record_action},        {"releaseCashButton:", record_action},
};
const mt_objc_host_class k_touch_keypad_view = {
    "TouchKeyPadView", "UIView", 46, k_keypad_methods, 6, nullptr, 0};
// PadArea: a separate view per pad area (a d-pad view and an attack view), single-touch.
const mt_objc_host_class k_pad_area = {"PadArea", "UIView", 46, k_keypad_methods, 4, nullptr, 0};

double g_now = 1000;
double test_clock(void *) { return g_now; }

// A guest address space with the runtime's host classes, the test classes, UIKit's clock on
// g_now, and an application with a 320x480 portrait window holding a view turned to landscape
// like the EAGLView (`root`: its local space is the 480x320 screen).
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    u32 root = 0;

    Guest() {
        g_log.clear();
        g_seen = Seen();
        g_now = 1000;
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        for (const mt_objc_host_class *c : {&k_main_menu_view, &k_touch_keypad_view, &k_pad_area}) {
            REQUIRE(mt_objc_add_host_class(rt, c, 0, 0) == 0);
        }
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        g_scratch = mt_heap_calloc(mem, 1, 64);
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
    // A view of class `cls` added to `parent` (the parent keeps it).
    u32 add(u32 parent, const char *cls, float x, float y, float w, float h) {
        const u32 v = view(cls, x, y, w, h);
        send(parent, "addSubview:", {v});
        mt_objc_release(&cpu, v);
        return v;
    }
    // A UIButton at (x, y, w, h) in `parent` sending `target` `action` for `events`.
    u32 button(u32 parent, float x, float y, float w, float h, u32 target,
               std::initializer_list<std::pair<const char *, u32>> actions) {
        const u32 b = send(klass("UIButton"), "buttonWithType:", {0});
        send(b, "setFrame:", {fb(x), fb(y), fb(w), fb(h)});
        send(parent, "addSubview:", {b});
        for (const auto &[action, events] : actions) {
            send(b,
                 "addTarget:action:forControlEvents:", {target, mt_objc_sel(rt, action), events});
        }
        return b;
    }
    // One UIKit turn, dt seconds after the last.
    void turn(double dt = 1.0 / 60) {
        g_now += dt;
        mt_uikit_turn(&cpu, g_now);
    }
};

} // namespace

TEST_CASE("input: mouse clicks reach -[MainMenuView clickPlay:] and a key pad button") {
    Guest g;
    // As -[EAGLView startApp] builds it: the full-screen multi-touch key pad, and the main
    // menu over it with its Play button at (46, 148, 120, 48).
    const u32 keypad = g.add(g.root, "TouchKeyPadView", 0, 0, 480, 320);
    g.send(keypad, "setMultipleTouchEnabled:", {1});
    // Panel 0's Cash button (docs/controls/keypad.md): (433, 0, 47, 52).
    const u32 cash = g.button(keypad, 433, 0, 47, 52, keypad,
                              {{"clickCashButton:", mt_ui::k_event_touch_down},
                               {"releaseCashButton:", mt_ui::k_event_touch_up_inside}});
    (void)cash;
    const u32 menu = g.add(g.root, "MainMenuView", 0, 0, 480, 320);
    g.button(menu, 46, 148, 120, 48, menu, {{"clickPlay:", mt_ui::k_event_touch_up_inside}});

    Window w;
    fe::Input in; // into mt_uikit_touch
    in.set_window(w.window, &w.placement);

    // A click on Play: down, up 0.1 s later (the frontend pumps SDL's events every turn).
    w.button(true, w.wx(106), w.wy(172));
    w.pump(in);
    g.turn();
    CHECK(g_seen.actions.empty()); // touch up inside, not down
    g.turn(0.1);
    w.button(false, w.wx(106), w.wy(172));
    w.pump(in);
    g.turn();
    CHECK(g_seen.actions == std::vector<std::string>{"clickPlay:"});
    // A click beside the button (on the menu, outside the button's 70-point margin) is not one.
    w.button(true, w.wx(300), w.wy(40));
    w.button(false, w.wx(300), w.wy(40));
    w.pump(in);
    g.turn();
    CHECK(g_seen.actions.size() == 1);

    // The menu goes (clickPlay: shows the next screen); the key pad's turn.
    g.send(menu, "removeFromSuperview");
    g_seen = Seen();
    // Its cash button: touch down, then up inside.
    w.button(true, w.wx(455), w.wy(24));
    w.pump(in);
    g.turn();
    CHECK(g_seen.actions == std::vector<std::string>{"clickCashButton:"});
    w.button(false, w.wx(455), w.wy(24));
    w.pump(in);
    g.turn();
    CHECK(g_seen.actions == std::vector<std::string>{"clickCashButton:", "releaseCashButton:"});
    // Its d-pad (the mask's left arm: x 0-70, y 215-299): the key pad's own touches, in its
    // coordinates, with the drag.
    w.button(true, w.wx(60), w.wy(260));
    w.pump(in);
    g.turn();
    w.motion(w.wx(75), w.wy(258));
    w.pump(in);
    g.turn();
    w.button(false, w.wx(75), w.wy(258));
    w.pump(in);
    g.turn();
    CHECK(g_seen.touches ==
          std::vector<std::string>{"TouchKeyPadView began #1 (60, 260) prev (60, 260)",
                                   "TouchKeyPadView moved #1 (75, 258) prev (60, 260)",
                                   "TouchKeyPadView ended #1 (75, 258) prev (60, 260)"});
}

TEST_CASE("input: two simultaneous SDL fingers on the key pad: d-pad and attack") {
    Guest g;
    Window w;
    fe::Input in;
    in.set_window(w.window, &w.placement);

    SUBCASE("one multi-touch TouchKeyPadView tracks both fingers (the game's key pad)") {
        const u32 keypad = g.add(g.root, "TouchKeyPadView", 0, 0, 480, 320);
        g.send(keypad, "setMultipleTouchEnabled:", {1});
        // Both fingers go down at once (one poll, one turn): the d-pad's left arm (mask x 0-70,
        // y 215-299), then attack (x 405-479, y 263-319; docs/controls/keypad.md).
        w.finger(SDL_EVENT_FINGER_DOWN, 11, w.wx(40), w.wy(262));
        w.finger(SDL_EVENT_FINGER_DOWN, 12, w.wx(424), w.wy(276));
        w.pump(in);
        g.turn();
        REQUIRE(g_seen.touches.size() == 2);
        CHECK(g_seen.touches[0] == "TouchKeyPadView began #1 (40, 262) prev (40, 262)");
        CHECK(g_seen.touches[1] == "TouchKeyPadView began #2 (424, 276) prev (424, 276)");
        CHECK(g_seen.objects[0] != g_seen.objects[1]);
        // The d-pad finger slides right: only its touch moves; attack holds still.
        w.finger(SDL_EVENT_FINGER_MOTION, 11, w.wx(84), w.wy(262));
        w.pump(in);
        g.turn();
        REQUIRE(g_seen.touches.size() == 3);
        CHECK(g_seen.touches[2] == "TouchKeyPadView moved #1 (84, 262) prev (40, 262)");
        // Both lift: each ends with its own touch.
        w.finger(SDL_EVENT_FINGER_UP, 12, w.wx(424), w.wy(276));
        w.finger(SDL_EVENT_FINGER_UP, 11, w.wx(84), w.wy(262));
        w.pump(in);
        g.turn();
        REQUIRE(g_seen.touches.size() == 5);
        CHECK(g_seen.touches[3] == "TouchKeyPadView ended #2 (424, 276) prev (424, 276)");
        CHECK(g_seen.touches[4] == "TouchKeyPadView ended #1 (84, 262) prev (40, 262)");
        CHECK(g_seen.objects.size() == 2);
    }
    SUBCASE("a d-pad view and an attack view each get their own finger") {
        const u32 pad = g.add(g.root, "UIView", 0, 0, 480, 320);
        g.add(pad, "PadArea", 0, 200, 120, 120);  // the d-pad
        g.add(pad, "PadArea", 380, 230, 100, 90); // attack
        w.finger(SDL_EVENT_FINGER_DOWN, 21, w.wx(40), w.wy(262));
        w.finger(SDL_EVENT_FINGER_DOWN, 22, w.wx(424), w.wy(276));
        w.pump(in);
        g.turn();
        // The move leaves the d-pad's bounds: the touch stays with the view it began on.
        w.finger(SDL_EVENT_FINGER_MOTION, 21, w.wx(150), w.wy(262));
        w.pump(in);
        g.turn();
        w.finger(SDL_EVENT_FINGER_UP, 21, w.wx(150), w.wy(262));
        w.finger(SDL_EVENT_FINGER_UP, 22, w.wx(424), w.wy(276));
        w.pump(in);
        g.turn();
        CHECK(g_seen.touches ==
              std::vector<std::string>{"PadArea began #1 (40, 62) prev (40, 62)",
                                       "PadArea began #2 (44, 46) prev (44, 46)",
                                       "PadArea moved #1 (150, 62) prev (40, 62)",
                                       "PadArea ended #1 (150, 62) prev (40, 62)",
                                       "PadArea ended #2 (44, 46) prev (44, 46)"});
    }
}
