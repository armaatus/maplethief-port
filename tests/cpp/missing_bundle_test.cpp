// The missing-bundle screen (#238, missing_bundle.h): what ends it, its drawing (the expected
// folder shown, the hint's glyphs following the pad), and its loop on a dummy-driver window.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "missing_bundle.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;

SDL_Event key(SDL_Keycode k, bool repeat = false) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.repeat = repeat;
    return e;
}

SDL_Event pad(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = static_cast<Uint8>(b);
    return e;
}

SDL_Event of_type(SDL_EventType t) {
    SDL_Event e{};
    e.type = t;
    return e;
}

std::uint64_t fnv(const std::vector<std::uint8_t> &px) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint8_t b : px) {
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

} // namespace

TEST_CASE("missing bundle: confirm, back, a tap or closing ends the screen; nothing else does") {
    using S = fe::MissingBundleScreen;
    for (const SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                      SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK}) {
        CHECK(S::ends(pad(b)));
    }
    CHECK_FALSE(S::ends(pad(SDL_GAMEPAD_BUTTON_NORTH)));
    CHECK_FALSE(S::ends(pad(SDL_GAMEPAD_BUTTON_DPAD_UP)));
    for (const SDL_Keycode k : {SDLK_RETURN, SDLK_KP_ENTER, SDLK_SPACE, SDLK_ESCAPE}) {
        CHECK(S::ends(key(k)));
    }
    CHECK_FALSE(S::ends(key(SDLK_RETURN, true)));
    CHECK_FALSE(S::ends(key(SDLK_A)));
    CHECK(S::ends(of_type(SDL_EVENT_QUIT)));
    CHECK(S::ends(of_type(SDL_EVENT_TERMINATING)));
    CHECK(S::ends(of_type(SDL_EVENT_FINGER_UP)));
    CHECK_FALSE(S::ends(of_type(SDL_EVENT_FINGER_DOWN)));
    CHECK_FALSE(S::ends(of_type(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED))); // a dock or undock
}

TEST_CASE(
    "missing bundle: the screen shows the expected folder; a different one draws differently") {
    constexpr std::uint32_t w = 960, h = 640;
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    fe::MissingBundleScreen screen("sdmc:/switch/maplethief/game/MapleThief.app");
    CHECK(screen.expected() == "sdmc:/switch/maplethief/game/MapleThief.app");
    REQUIRE(screen.dirty());
    screen.draw(px.data(), w, h, 2);
    CHECK_FALSE(screen.dirty());
    const std::uint64_t first = fnv(px);
    // The title row has text in it (the header colour on the dark background).
    bool lit = false;
    for (std::uint32_t x = 48; x < 400 && !lit; x++) {
        const std::uint8_t *p = &px[(std::size_t{40} * w + x) * 4];
        lit = p[0] > 0x80;
    }
    CHECK(lit);
    // The same text draws the same bytes; another folder other bytes.
    fe::MissingBundleScreen again("sdmc:/switch/maplethief/game/MapleThief.app");
    again.draw(px.data(), w, h, 2);
    CHECK(fnv(px) == first);
    fe::MissingBundleScreen other("/elsewhere/game/MapleThief.app");
    other.draw(px.data(), w, h, 2);
    CHECK(fnv(px) != first);
}

TEST_CASE("missing bundle: the window loop ends on a pad's B and on a close") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(
        SDL_CreateWindowAndRenderer("missing_bundle_test", 1280, 720, 0, &window, &renderer),
        SDL_GetError());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const fe::Settings settings;
    SDL_Event b = pad(SDL_GAMEPAD_BUTTON_EAST);
    REQUIRE(SDL_PushEvent(&b));
    CHECK(fe::run_missing_bundle_screen(renderer, settings, "sdmc:/switch/maplethief/game"));
    SDL_Event close = of_type(SDL_EVENT_QUIT);
    REQUIRE(SDL_PushEvent(&close));
    CHECK(fe::run_missing_bundle_screen(renderer, settings, "sdmc:/switch/maplethief/game"));
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
