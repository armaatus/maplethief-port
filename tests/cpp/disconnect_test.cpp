// The disconnect pause (#184, disconnect.h) on SDL3 virtual joysticks and synthetic SDL events,
// routed as game.cpp's handle_event does (the pause first, the devices only with what it does
// not take), with the turn hook's rule that no game turn runs while a port overlay is open:
// the active pad unplugged with pause_on_disconnect on opens it, lifts what the pad held and
// holds the turns; a pad connecting closes it; a key, a click, a tap or another pad's button
// closes it and neither the press nor its release reaches an action; a pad that is not the
// active one, the active one while another stays, or any pad with the setting off (or in a
// scripted run, where game.cpp never enables it) changes nothing. And its drawing: the lost
// pad's glyphs, the same bytes twice.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "disconnect.h"
#include "gamepad.h"
#include "input.h"
#include "maplethief/input.h"

#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
using fe::Control;
using fe::ControlKind;
using virtual_pad::Kind;

std::vector<std::string> g_sent;
void record(void *, std::uint32_t action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_sent.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
std::int32_t field_panel(void *) { return 0; }

SDL_Event key_event(SDL_Keycode k, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

struct Sdl {
    Sdl() {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    }
    ~Sdl() { SDL_Quit(); }
};

// game.cpp's routing and turn hook, reduced: the pause sees each event first; the devices get
// what it does not take; a turn (Actions::turn, the game's run-loop turn) runs only while no
// overlay is open.
struct Front {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::DisconnectPause pause{&actions, &gamepads};
    unsigned turns = 0;

    explicit Front(bool on) {
        g_sent.clear();
        actions.set_sink(record, nullptr);
        actions.set_panel_source(field_panel, nullptr);
        pause.set_enabled(on);
    }
    void send(const SDL_Event &e) {
        if (pause.handle(e)) {
            return;
        }
        input.handle(e);
        gamepads.handle(e);
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            actions.control(fe::InputSource::kKeyboard,
                            Control{ControlKind::kKey, static_cast<int>(e.key.key)},
                            e.type == SDL_EVENT_KEY_DOWN);
        }
    }
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            send(e);
        }
    }
    // One run-loop turn: the events, then the game's turn unless the pause holds it.
    void turn() {
        pump();
        if (!pause.is_open()) {
            actions.turn();
            turns++;
        }
    }
};

std::unique_ptr<virtual_pad::Pad> attach(Front &f, Kind kind) {
    auto pad = std::make_unique<virtual_pad::Pad>(kind);
    REQUIRE_MESSAGE(pad->ok(), pad->error);
    f.pump();
    return pad;
}

// The pad walks right with its stick (and so becomes the active one).
void walk_right(Front &f, virtual_pad::Pad &pad) {
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 30000);
    f.turn();
    REQUIRE(f.gamepads.active_id() == pad.id);
    REQUIRE(g_sent == std::vector<std::string>{"+right"});
}

} // namespace

TEST_CASE("disconnect: the active pad unplugged opens it, holds the turns; a pad reconnecting "
          "closes it") {
    Sdl sdl;
    Front f(true);
    auto pad = attach(f, Kind::kXbox360);
    walk_right(f, *pad);
    const unsigned before = f.turns;

    pad.reset(); // unplugged mid-walk
    f.turn();
    REQUIRE(f.pause.is_open());
    CHECK(f.pause.opens() == 1);
    CHECK(f.pause.lost_type() == SDL_GAMEPAD_TYPE_XBOX360);
    CHECK(f.gamepads.count() == 0);
    // The hero stops: what the pad held is let go, nothing is held.
    CHECK(g_sent == std::vector<std::string>{"+right", "-right"});
    CHECK(f.actions.held() == 0);
    g_sent.clear();

    // While open: no game turn, and nothing that is not a press reaches the game.
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    SDL_Event up = key_event(SDLK_Z, false);
    for (int i = 0; i < 5; i++) {
        CHECK(f.pause.handle(motion));
        CHECK(f.pause.handle(wheel));
        CHECK(f.pause.handle(up));
        f.turn();
    }
    CHECK(f.turns == before);
    CHECK(f.pause.is_open());
    CHECK(g_sent.empty());

    // A pad connects: closed, and the pad is Gamepads' (its ADDED reached it).
    auto ps5 = attach(f, Kind::kPs5);
    CHECK_FALSE(f.pause.is_open());
    CHECK(f.pause.closes() == 1);
    CHECK(f.gamepads.count() == 1);
    f.turn();
    CHECK(f.turns == before + 1);
    // The new pad plays.
    ps5->button(SDL_GAMEPAD_BUTTON_WEST, true);
    f.turn();
    CHECK(g_sent == std::vector<std::string>{"+attack"});
    ps5->button(SDL_GAMEPAD_BUTTON_WEST, false);
    f.turn();
    // And it is the active pad now: unplugging it pauses again, with its glyphs.
    ps5.reset();
    f.turn();
    CHECK(f.pause.is_open());
    CHECK(f.pause.lost_type() == SDL_GAMEPAD_TYPE_PS5);
}

TEST_CASE("disconnect: a key, a click, a tap or another pad's button closes it; neither the "
          "press nor its release reaches the game") {
    Sdl sdl;
    Front f(true);
    const auto reopen = [&f] {
        auto pad = attach(f, Kind::kXbox360);
        pad->button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        f.turn();
        pad->button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        f.turn();
        REQUIRE(f.gamepads.active_id() == pad->id);
        pad.reset();
        f.turn();
        REQUIRE(f.pause.is_open());
        g_sent.clear();
    };

    // Z is attack: its press closes the pause, a repeat before does not, its release is taken.
    reopen();
    f.send(key_event(SDLK_Z, true, true));
    CHECK(f.pause.is_open());
    f.send(key_event(SDLK_Z, true));
    CHECK_FALSE(f.pause.is_open());
    f.turn();
    f.send(key_event(SDLK_Z, false));
    f.turn();
    CHECK(g_sent.empty());
    CHECK(f.actions.held() == 0);
    // Only that one release: the next press and release of Z attack.
    f.send(key_event(SDLK_Z, true));
    f.send(key_event(SDLK_Z, false));
    CHECK(g_sent == std::vector<std::string>{"+attack", "-attack"});

    // A click: taken, and its button's up.
    reopen();
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.button = SDL_BUTTON_LEFT;
    click.button.down = true;
    CHECK(f.pause.handle(click));
    CHECK_FALSE(f.pause.is_open());
    SDL_Event release = click;
    release.type = SDL_EVENT_MOUSE_BUTTON_UP;
    release.button.down = false;
    CHECK(f.pause.handle(release));
    CHECK_FALSE(f.pause.handle(release)); // only once

    // A tap: taken, and that finger's up (not another finger's).
    reopen();
    SDL_Event tap{};
    tap.type = SDL_EVENT_FINGER_DOWN;
    tap.tfinger.fingerID = 3;
    CHECK(f.pause.handle(tap));
    CHECK_FALSE(f.pause.is_open());
    SDL_Event lift = tap;
    lift.type = SDL_EVENT_FINGER_UP;
    lift.tfinger.fingerID = 4;
    CHECK_FALSE(f.pause.handle(lift));
    lift.tfinger.fingerID = 3;
    CHECK(f.pause.handle(lift));

    // A button on another pad (one Gamepads does not hold: it never opened it).
    reopen();
    SDL_Event button{};
    button.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    button.gbutton.which = 99;
    button.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
    button.gbutton.down = true;
    CHECK(f.pause.handle(button));
    CHECK_FALSE(f.pause.is_open());
    CHECK(g_sent.empty());
    CHECK(f.pause.opens() == 4);
    CHECK(f.pause.closes() == 4);
}

TEST_CASE("disconnect: a pad not in use, the pad in use while another stays, or the setting off "
          "changes nothing") {
    Sdl sdl;
    SUBCASE("a pad that is not the active one") {
        Front f(true);
        auto xbox = attach(f, Kind::kXbox360);
        auto ps4 = attach(f, Kind::kPs4);
        walk_right(f, *xbox);
        ps4.reset();
        f.turn();
        CHECK_FALSE(f.pause.is_open());
        CHECK(g_sent == std::vector<std::string>{"+right"}); // still walking
        // The active one, now alone, does open it.
        xbox.reset();
        f.turn();
        CHECK(f.pause.is_open());
    }
    SUBCASE("the active pad while another stays connected") {
        Front f(true);
        auto xbox = attach(f, Kind::kXbox360);
        auto ps4 = attach(f, Kind::kPs4);
        walk_right(f, *xbox);
        const unsigned before = f.turns;
        xbox.reset();
        f.turn();
        CHECK_FALSE(f.pause.is_open());
        CHECK(f.turns == before + 1);
        CHECK(g_sent == std::vector<std::string>{"+right", "-right"}); // lifted, as before #184
    }
    SUBCASE("the setting off (or a scripted run: never enabled)") {
        Front f(false);
        auto xbox = attach(f, Kind::kXbox360);
        walk_right(f, *xbox);
        const unsigned before = f.turns;
        xbox.reset();
        f.turn();
        CHECK_FALSE(f.pause.is_open());
        CHECK(f.pause.opens() == 0);
        CHECK(f.turns == before + 1);
        // Keys reach the game as ever.
        f.send(key_event(SDLK_Z, true));
        CHECK(g_sent == std::vector<std::string>{"+right", "-right", "+attack"});
    }
    SUBCASE("turned off while open: it closes") {
        Front f(true);
        auto xbox = attach(f, Kind::kXbox360);
        walk_right(f, *xbox);
        xbox.reset();
        f.turn();
        REQUIRE(f.pause.is_open());
        f.pause.set_enabled(false);
        CHECK_FALSE(f.pause.is_open());
    }
}

TEST_CASE("disconnect: drawn with the lost pad's glyphs, the same bytes each time") {
    Sdl sdl;
    constexpr std::uint32_t w = 480, h = 320;
    const auto capture = [&](Kind kind) {
        Front f(true);
        auto pad = attach(f, kind);
        walk_right(f, *pad);
        pad.reset();
        f.turn();
        REQUIRE(f.pause.is_open());
        CHECK(f.pause.dirty());
        std::vector<std::uint8_t> px(std::size_t{w} * h * 4, 0x80);
        f.pause.draw(px.data(), w, h, 1);
        CHECK_FALSE(f.pause.dirty());
        return px;
    };
    const std::vector<std::uint8_t> xbox = capture(Kind::kXbox360);
    CHECK(xbox == capture(Kind::kXbox360));
    const std::vector<std::uint8_t> ps5 = capture(Kind::kPs5);
    CHECK(xbox != ps5); // Cross and Circle, not A and B
    CHECK(xbox != std::vector<std::uint8_t>(xbox.size(), 0x80));
}
