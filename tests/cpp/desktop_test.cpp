// The desktop conveniences (#145, ADR 0077) with synthetic data only: the four settings rows
// (fullscreen, vsync, pause_unfocused, language) persist and their flags work, the hotkeys and
// screenshot buttons, the cursor's auto-hide, the wheel's notches, window.ini (parse, write,
// the display check, load and save), the screenshot's name and file, which runs are
// interactive, and Desktop on a dummy-driver window: fullscreen and vsync applied live, the
// hotkeys taken (their releases and repeats too) and the mouse still mapping to the screen
// after a toggle.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "desktop.h"
#include "game.h"
#include "input.h"
#include "maplethief/uikit.h"
#include "present.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::CursorAutoHide;
using fe::Settings;
using fe::WindowState;

std::string output_dir() {
    const char *env = std::getenv("MAPLETHIEF_TEST_OUTPUT_DIR");
    return env != nullptr && *env != '\0' ? env : MT_TEST_OUTPUT_DIR;
}

// A fresh directory under the test output.
fs::path fresh_dir(const char *name) {
    const fs::path dir = fs::path(output_dir()) / "desktop" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

SDL_Event key(Uint32 type, SDL_Keycode k, SDL_Keymod mod = SDL_KMOD_NONE, bool repeat = false) {
    SDL_Event e{};
    e.type = type;
    e.key.key = k;
    e.key.mod = mod;
    e.key.repeat = repeat;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    return e;
}

SDL_Event pad_button(Uint32 type, int button, SDL_JoystickID which = 7) {
    SDL_Event e{};
    e.type = type;
    e.gbutton.which = which;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down = type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    return e;
}

SDL_Event mouse_motion(SDL_MouseID which = 1) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = which;
    return e;
}

} // namespace

TEST_CASE("desktop: the settings rows persist, take their flags and apply at once (#145)") {
    struct Row {
        const char *key, *flag, *other;
        fe::Applies applies;
    };
    for (const Row &r : {Row{"fullscreen", "--fullscreen", "desktop", fe::Applies::kAtOnce},
                         Row{"vsync", "--vsync", "off", fe::Applies::kAtOnce},
                         Row{"pause_unfocused", "--pause-unfocused", "on", fe::Applies::kAtOnce},
                         Row{"language", "--language", "ko", fe::Applies::kNextStart}}) {
        INFO(r.key);
        const fe::SettingInfo *info = fe::find_setting(r.key);
        REQUIRE(info != nullptr);
        CHECK(fe::find_setting_flag(r.flag) == info);
        CHECK(info->applies == r.applies);
        CHECK(info->game_only);
        // Changed, written, read back: the same settings (a new file and over an old one).
        Settings s;
        std::string why;
        REQUIRE(fe::apply_setting(&s, r.key, r.other, &why));
        CHECK(fe::setting_text(s, *info) == r.other);
        CHECK(s != Settings{});
        for (const std::string &old :
             {std::string(), std::string("# mine\nmaster_volume = 40\n")}) {
            Settings back;
            const std::string text = fe::serialise_settings(s, old);
            CHECK(text.find(std::string("\n") + r.key + " = " + r.other + "\n") !=
                  std::string::npos);
            CHECK(fe::parse_settings(text, &back).empty());
            CHECK(back == s); // the old text's master_volume line is rewritten with s's value
        }
        // A bad value: the file's wording, nothing changed.
        Settings t;
        CHECK_FALSE(fe::apply_setting(&t, r.key, "maybe", &why));
        CHECK(why.find(std::string(r.key) + ": 'maybe' is not ") == 0);
        CHECK(t == Settings{});
    }
    // The language's file text and the enum agree.
    Settings s;
    REQUIRE(fe::apply_setting(&s, "language", "KO", nullptr));
    CHECK(s.language == fe::Language::kKo);
    CHECK(std::string(fe::kLanguageNames[static_cast<int>(s.language)]) == "ko");
}

TEST_CASE("desktop: no settings file keeps today's window: windowed, vsync, no pause, English") {
    const Settings d;
    CHECK(d.fullscreen == fe::Fullscreen::kOff);
    CHECK(d.vsync);
    CHECK_FALSE(d.pause_unfocused);
    CHECK(d.language == fe::Language::kEn);
    Settings parsed;
    CHECK(fe::parse_settings("", &parsed).empty());
    CHECK(parsed == d);
}

TEST_CASE("desktop: the fullscreen hotkeys and the screenshot key") {
    CHECK(fe::is_fullscreen_hotkey(SDLK_F11, SDL_KMOD_NONE));
    CHECK(fe::is_fullscreen_hotkey(SDLK_F11, SDL_KMOD_NUM | SDL_KMOD_CAPS));
    CHECK(fe::is_fullscreen_hotkey(SDLK_RETURN, SDL_KMOD_LALT));
    CHECK(fe::is_fullscreen_hotkey(SDLK_RETURN, SDL_KMOD_RALT));
    CHECK(fe::is_fullscreen_hotkey(SDLK_KP_ENTER, SDL_KMOD_LALT | SDL_KMOD_NUM));
    CHECK(fe::is_fullscreen_hotkey(SDLK_F, SDL_KMOD_LGUI | SDL_KMOD_LCTRL));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_RETURN, SDL_KMOD_NONE)); // the game's confirm
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_RETURN, SDL_KMOD_LALT | SDL_KMOD_LCTRL));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_F, SDL_KMOD_NONE));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_F, SDL_KMOD_LGUI));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_F, SDL_KMOD_LGUI | SDL_KMOD_LCTRL | SDL_KMOD_LALT));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_F11, SDL_KMOD_LCTRL));
    CHECK_FALSE(fe::is_fullscreen_hotkey(SDLK_F12, SDL_KMOD_NONE));
    CHECK(fe::is_screenshot_key(SDLK_F12, SDL_KMOD_NONE));
    CHECK(fe::is_screenshot_key(SDLK_F12, SDL_KMOD_LSHIFT));
    CHECK_FALSE(fe::is_screenshot_key(SDLK_F12, SDL_KMOD_LALT));
    CHECK_FALSE(fe::is_screenshot_key(SDLK_F11, SDL_KMOD_NONE));
    // The pad: Share / Capture alone; Back only while the right stick is held in.
    CHECK(fe::is_screenshot_button(SDL_GAMEPAD_BUTTON_MISC1, false));
    CHECK(fe::is_screenshot_button(SDL_GAMEPAD_BUTTON_BACK, true));
    CHECK_FALSE(fe::is_screenshot_button(SDL_GAMEPAD_BUTTON_BACK, false)); // the map
    CHECK_FALSE(fe::is_screenshot_button(SDL_GAMEPAD_BUTTON_START, true));
    CHECK_FALSE(fe::is_screenshot_button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false));
}

TEST_CASE("desktop: the cursor hides on keys and pads and shows on the mouse") {
    CursorAutoHide c;
    CHECK_FALSE(c.hidden());
    CHECK(c.on_event(mouse_motion()) == CursorAutoHide::Change::kNone); // shown already
    CHECK(c.on_event(key(SDL_EVENT_KEY_DOWN, SDLK_Z)) == CursorAutoHide::Change::kHide);
    CHECK(c.hidden());
    CHECK(c.on_event(key(SDL_EVENT_KEY_DOWN, SDLK_X)) == CursorAutoHide::Change::kNone);
    // A touch's (or a pen's) mouse events are not the mouse.
    CHECK(c.on_event(mouse_motion(SDL_TOUCH_MOUSEID)) == CursorAutoHide::Change::kNone);
    CHECK(c.on_event(mouse_motion(SDL_PEN_MOUSEID)) == CursorAutoHide::Change::kNone);
    CHECK(c.hidden());
    CHECK(c.on_event(mouse_motion()) == CursorAutoHide::Change::kShow);
    // A key's repeat and its release are not new input.
    CHECK(c.on_event(key(SDL_EVENT_KEY_DOWN, SDLK_Z, SDL_KMOD_NONE, true)) ==
          CursorAutoHide::Change::kNone);
    CHECK(c.on_event(key(SDL_EVENT_KEY_UP, SDLK_Z)) == CursorAutoHide::Change::kNone);
    CHECK(c.on_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH)) ==
          CursorAutoHide::Change::kHide);
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.which = 1;
    CHECK(c.on_event(click) == CursorAutoHide::Change::kShow);
    // A stick: only past half.
    SDL_Event axis{};
    axis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    axis.gaxis.value = 9000;
    CHECK(c.on_event(axis) == CursorAutoHide::Change::kNone);
    axis.gaxis.value = -20000;
    CHECK(c.on_event(axis) == CursorAutoHide::Change::kHide);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.which = 1;
    CHECK(c.on_event(wheel) == CursorAutoHide::Change::kShow);
}

TEST_CASE("desktop: the wheel's notches, a trackpad's fractions included") {
    fe::WheelSteps w;
    CHECK(w.add(1) == 1);
    CHECK(w.add(-1) == -1);
    CHECK(w.add(3) == 3);
    CHECK(w.add(0.4f) == 0);
    CHECK(w.add(0.4f) == 0);
    CHECK(w.add(0.4f) == 1); // 1.2: one notch, 0.2 kept
    CHECK(w.add(0.8f) == 1);
    CHECK(w.add(-0.5f) == 0); // the other way: starts again
    CHECK(w.add(-0.5f) == -1);
    CHECK(w.add(0) == 0);
    CHECK(w.add(1e9f) == 64); // a burst is capped
}

TEST_CASE("desktop: window.ini: parse, write, the display check, load and save") {
    const WindowState s{-1200, 45, 1440, 960, true};
    const std::string text = fe::serialise_window_state(s);
    std::vector<std::string> problems;
    CHECK(fe::parse_window_state(text, 16, 16384, &problems) == s);
    CHECK(problems.empty());
    // Comments, blanks, CR LF; every problem reported, never fatal.
    problems.clear();
    const auto p =
        fe::parse_window_state("# x\r\n x = 10 \r\ny=20\r\nwidth = 800\r\nheight = 600\r\n"
                               "colour = red\r\nmaximized = maybe\r\nnonsense\r\n",
                               16, 16384, &problems);
    REQUIRE(p.has_value());
    CHECK(*p == WindowState{10, 20, 800, 600, false});
    CHECK(problems.size() == 3);
    // Missing keys, bad numbers and sizes out of range: no state.
    for (const char *bad :
         {"", "x = 1\ny = 2\nwidth = 300\n", "x = a\ny = 2\nwidth = 300\nheight = 200\n",
          "x = 1\ny = 2\nwidth = 8\nheight = 200\n", "x = 1\ny = 2\nwidth = 300\nheight = 99999\n",
          "x = 99999999999\ny = 2\nwidth = 300\nheight = 200\n"}) {
        INFO(bad);
        problems.clear();
        CHECK_FALSE(fe::parse_window_state(bad, 16, 16384, &problems).has_value());
        CHECK_FALSE(problems.empty());
    }
    // On a display: the middle of its top edge on one of them.
    const SDL_Rect displays[] = {{0, 25, 1920, 1055}, {1920, 0, 1280, 1024}};
    CHECK(fe::window_state_on_display({100, 100, 960, 640, false}, displays));
    CHECK(fe::window_state_on_display({1500, 100, 960, 640, false}, displays));       // the second
    CHECK_FALSE(fe::window_state_on_display({4000, 100, 960, 640, false}, displays)); // gone
    CHECK_FALSE(fe::window_state_on_display({100, -700, 960, 640, false}, displays)); // above
    CHECK_FALSE(fe::window_state_on_display({100, 100, 960, 640, false}, {}));
    // The file: none, written, read back; a bad one is ignored.
    const fs::path dir = fresh_dir("window_ini");
    CHECK_FALSE(fe::load_window_state(dir.string(), 16, 16384).has_value());
    REQUIRE(fe::save_window_state(dir.string(), s));
    CHECK(fs::exists(dir / fe::kWindowStateFileName));
    CHECK_FALSE(fs::exists(dir / "window.ini.tmp"));
    CHECK(fe::load_window_state(dir.string(), 16, 16384) == s);
    {
        std::ofstream out(dir / fe::kWindowStateFileName, std::ios::binary | std::ios::trunc);
        out << "x = 1\n";
    }
    CHECK_FALSE(fe::load_window_state(dir.string(), 16, 16384).has_value());
    CHECK_FALSE(fe::save_window_state("", s));
}

TEST_CASE("desktop: the screenshot's name and file") {
    SDL_DateTime t{};
    t.year = 2026;
    t.month = 10;
    t.day = 30;
    t.hour = 9;
    t.minute = 5;
    t.second = 7;
    t.nanosecond = 42000000;
    CHECK(fe::screenshot_file_name(t) == "MapleThief_2026-10-30_09-05-07-042.png");

    REQUIRE_MESSAGE(SDL_Init(0), SDL_GetError());
    const fs::path dir = fresh_dir("screenshots");
    std::vector<std::uint8_t> rgba(4 * 3 * 4);
    for (std::size_t i = 0; i < rgba.size(); i++) {
        rgba[i] = static_cast<std::uint8_t>(i * 13 + 1);
    }
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        rgba[i] = 255;
    }
    const std::string a = fe::write_screenshot(dir.string(), rgba.data(), 4, 3);
    REQUIRE_FALSE(a.empty());
    const fs::path pa = fs::path(a);
    CHECK(pa.parent_path() == dir / fe::kScreenshotDirName);
    CHECK(pa.filename().string().rfind("MapleThief_", 0) == 0);
    CHECK(pa.extension() == ".png");
    // Back from the PNG: the same pixels.
    SDL_Surface *loaded = SDL_LoadPNG(a.c_str());
    REQUIRE_MESSAGE(loaded != nullptr, SDL_GetError());
    SDL_Surface *abgr = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);
    REQUIRE(abgr != nullptr);
    CHECK(abgr->w == 4);
    CHECK(abgr->h == 3);
    bool same = true;
    for (int y = 0; y < 3; y++) {
        const auto *row = static_cast<const std::uint8_t *>(abgr->pixels) + y * abgr->pitch;
        for (int x = 0; x < 16; x++) {
            same = same && row[x] == rgba[static_cast<std::size_t>(y * 16 + x)];
        }
    }
    CHECK(same);
    SDL_DestroySurface(abgr);
    SDL_DestroySurface(loaded);
    // Many in the same millisecond never overwrite each other.
    std::vector<std::string> names{a};
    for (int i = 0; i < 3; i++) {
        names.push_back(fe::write_screenshot(dir.string(), rgba.data(), 4, 3));
        REQUIRE_FALSE(names.back().empty());
    }
    for (std::size_t i = 0; i < names.size(); i++) {
        for (std::size_t j = 0; j < i; j++) {
            CHECK(names[i] != names[j]);
        }
    }
    CHECK(fe::write_screenshot("", rgba.data(), 4, 3).empty());
    CHECK(fe::write_screenshot(dir.string(), nullptr, 4, 3).empty());
    SDL_Quit();
}

TEST_CASE("desktop: --unfocus T,S scripts the focus lost and gained") {
    fe::GameOptions o;
    o.suspends.push_back(fe::Suspend{5, 2, false, true});
    o.suspends.push_back(fe::Suspend{1, 1, false, false});
    const auto script = fe::lifecycle_script(o);
    REQUIRE(script.size() == 4);
    CHECK(script[0] == std::pair<double, std::uint32_t>{1, SDL_EVENT_WINDOW_MINIMIZED});
    CHECK(script[1] == std::pair<double, std::uint32_t>{2, SDL_EVENT_WINDOW_RESTORED});
    CHECK(script[2] == std::pair<double, std::uint32_t>{5, SDL_EVENT_WINDOW_FOCUS_LOST});
    CHECK(script[3] == std::pair<double, std::uint32_t>{7, SDL_EVENT_WINDOW_FOCUS_GAINED});
    CHECK_FALSE(fe::is_interactive_run(o, "cocoa")); // a scripted run
}

TEST_CASE("desktop: window.ini and the picker only in a player's run") {
    fe::GameOptions o;
    CHECK(fe::is_interactive_run(o, "cocoa"));
    CHECK(fe::is_interactive_run(o, nullptr));
    CHECK_FALSE(fe::is_interactive_run(o, "dummy"));
    CHECK_FALSE(fe::is_interactive_run(o, "offscreen"));
    o.frames = 10;
    CHECK_FALSE(fe::is_interactive_run(o, "cocoa"));
    o = {};
    o.fixed_step_us = 16667;
    CHECK_FALSE(fe::is_interactive_run(o, "cocoa"));
    o = {};
    o.quit_at = 3;
    CHECK_FALSE(fe::is_interactive_run(o, "x11"));
}

TEST_CASE("desktop: on a window: fullscreen and vsync live, the hotkeys taken, the mouse still "
          "maps after a toggle") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("desktop_test", 960, 640, SDL_WINDOW_RESIZABLE,
                                                &window, &renderer),
                    SDL_GetError());
    fe::Desktop d;
    // Without a window: nothing.
    CHECK_FALSE(d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F11)).consumed);
    CHECK_FALSE(d.state().has_value());
    d.attach(window, renderer);
    REQUIRE(d.state().has_value());
    CHECK(d.state()->width == 960);
    CHECK(d.state()->height == 640);
    CHECK_FALSE(d.state()->maximized);

    // The defaults: windowed, vsync asked for (the renderer's answer is what counts).
    Settings s;
    d.apply(s);
    CHECK_FALSE(d.fullscreen());
    int interval = -1;
    REQUIRE(SDL_GetRenderVSync(renderer, &interval));
    CHECK(d.vsync() == (interval == 1));
    s.vsync = false;
    d.apply(s);
    CHECK_FALSE(d.vsync());
    REQUIRE(SDL_GetRenderVSync(renderer, &interval));
    CHECK(interval == 0);
    s.vsync = true;
    d.apply(s);

    // Fullscreen at once, and back; the windowed size is kept.
    s.fullscreen = fe::Fullscreen::kDesktop;
    d.apply(s);
    SDL_SyncWindow(window);
    CHECK(d.fullscreen());
    SDL_Event e;
    bool entered = false;
    while (SDL_PollEvent(&e)) {
        const fe::Desktop::Request r = d.handle(e);
        if (r.fullscreen_changed) {
            entered = *r.fullscreen_changed;
        }
    }
    CHECK(entered);
    REQUIRE(d.state().has_value());
    CHECK(d.state()->width == 960); // not the display's size
    s.fullscreen = fe::Fullscreen::kOff;
    d.apply(s);
    SDL_SyncWindow(window);
    CHECK_FALSE(d.fullscreen());
    while (SDL_PollEvent(&e)) {
        d.handle(e);
    }

    // The hotkeys: taken with their repeats and releases; Enter alone is the game's.
    fe::Desktop::Request r = d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F11));
    CHECK(r.consumed);
    CHECK(r.toggle_fullscreen);
    CHECK_FALSE(r.screenshot);
    CHECK(d.toggles() == 1);
    r = d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F11, SDL_KMOD_NONE, true));
    CHECK(r.consumed);
    CHECK_FALSE(r.toggle_fullscreen);
    CHECK(d.handle(key(SDL_EVENT_KEY_UP, SDLK_F11)).consumed);
    CHECK_FALSE(d.handle(key(SDL_EVENT_KEY_UP, SDLK_F11)).consumed); // not held any more
    CHECK_FALSE(d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN)).consumed);
    CHECK_FALSE(d.handle(key(SDL_EVENT_KEY_UP, SDLK_RETURN)).consumed);
    r = d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_LALT));
    CHECK(r.consumed);
    CHECK(r.toggle_fullscreen);
    // Alt let go first: Enter's release is still the port's.
    CHECK(d.handle(key(SDL_EVENT_KEY_UP, SDLK_RETURN)).consumed);
    r = d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F12));
    CHECK(r.consumed);
    CHECK(r.screenshot);
    CHECK_FALSE(r.toggle_fullscreen);
    CHECK(d.handle(key(SDL_EVENT_KEY_UP, SDLK_F12)).consumed);
    // The pad's Share button and its release (no such pad open: R3 reads as up).
    r = d.handle(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_MISC1));
    CHECK(r.consumed);
    CHECK(r.screenshot);
    CHECK(d.handle(pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_MISC1)).consumed);
    CHECK_FALSE(
        d.handle(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_BACK)).consumed);
    CHECK_FALSE(
        d.handle(pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_BACK)).consumed);
    // The cursor: hidden by the keys above, shown by the mouse.
    CHECK(d.cursor_hides() >= 1);
    d.handle(mouse_motion());
    CHECK(SDL_CursorVisible());
    d.handle(key(SDL_EVENT_KEY_DOWN, SDLK_Z));
    CHECK_FALSE(SDL_CursorVisible());
    d.handle(mouse_motion());
    CHECK(SDL_CursorVisible());

    // The mouse maps through the placement of the window's current size, before and after a
    // toggle (and a resize, as a real fullscreen window gets): a click at the window point that
    // shows screen point (120, 80) touches (120, 80).
    fe::Presenter presenter;
    REQUIRE(presenter.start(renderer, 480, 320));
    std::vector<std::uint8_t> img(480 * 320 * 4, 0x80);
    REQUIRE(presenter.upload(img.data(), 480 * 4));
    struct Touch {
        float x, y;
    };
    std::vector<Touch> touches;
    fe::Input input;
    input.set_sink(
        [](void *user, std::uint32_t, std::uint32_t phase, float x, float y) {
            if (phase == MT_UIKIT_TOUCH_BEGAN) {
                static_cast<std::vector<Touch> *>(user)->push_back({x, y});
            }
            return 1;
        },
        &touches);
    input.set_window(window, &presenter.placement());
    auto click_at = [&](float sx, float sy) {
        REQUIRE(presenter.draw());
        float wx = 0, wy = 0;
        fe::screen_to_window_point(window, presenter.placement(), sx, sy, &wx, &wy);
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.which = 1;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.down = true;
        down.button.x = wx;
        down.button.y = wy;
        input.handle(down);
        SDL_Event up = down;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.down = false;
        input.handle(up);
    };
    click_at(120, 80);
    const fe::Rect windowed = presenter.placement().rect;
    s.fullscreen = fe::Fullscreen::kDesktop;
    d.apply(s);
    SDL_SyncWindow(window);
    SDL_SetWindowSize(window, 1920, 1080); // a display's size, as a real fullscreen window gets
    SDL_SyncWindow(window);
    click_at(120, 80);
    const fe::Rect full = presenter.placement().rect;
    s.fullscreen = fe::Fullscreen::kOff;
    d.apply(s);
    SDL_SetWindowSize(window, 960, 640);
    SDL_SyncWindow(window);
    click_at(120, 80);
    REQUIRE(touches.size() == 3);
    for (const Touch &t : touches) {
        CHECK(t.x == doctest::Approx(120).epsilon(0.01));
        CHECK(t.y == doctest::Approx(80).epsilon(0.01));
    }
    CHECK(full.w > windowed.w); // the image grew with the window
    while (SDL_PollEvent(&e)) {
        d.handle(e);
    }
    presenter.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
