// #198's start-up question in the window (#416, start_question.h): where it is asked, each button
// by key, by pad and by pointer, Escape and B as Start normally, and the window loop answering
// decide_start (crash_report.h) for both questions with every input method.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "canvas.h"
#include "crash_report.h"
#include "start_question.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using fe::RecoveryChoice;
using fe::SafeStartChoice;
using fe::SettingsPlatform;

std::uint64_t fnv1a(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

std::string output_dir() {
    const char *env = std::getenv("MAPLETHIEF_TEST_OUTPUT_DIR");
    return env != nullptr && *env != '\0' ? env : MT_TEST_OUTPUT_DIR;
}

SDL_Event key_down(SDL_Keycode k) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    return e;
}
SDL_Event pad_button(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = true;
    return e;
}

fe::RecoveryOffer an_offer() {
    fe::RecoveryOffer offer;
    offer.offered = true;
    offer.path = "states/recovery-1.state";
    offer.slot = 1;
    offer.minutes_before = 3;
    return offer;
}

fe::LastRun a_crash() {
    fe::LastRun last;
    last.crashed = true;
    last.crash_file = "/data/MapleThief/crashes/2026-11-26_10-00-00.txt";
    return last;
}

fe::StartQuestion two_buttons() {
    return fe::StartQuestion(fe::safe_start_question_text(a_crash().crash_file),
                             {fe::start_choice_label(SafeStartChoice::kDefaults),
                              fe::start_choice_label(SafeStartChoice::kNormal)});
}

fe::StartQuestion four_buttons() {
    std::vector<std::string> buttons;
    for (const RecoveryChoice c : {RecoveryChoice::kResume, RecoveryChoice::kResumeDefaults,
                                   RecoveryChoice::kStartDefaults, RecoveryChoice::kStartNormal}) {
        buttons.emplace_back(fe::start_choice_label(c));
    }
    return fe::StartQuestion(fe::recovery_question_text(a_crash().crash_file, an_offer()),
                             std::move(buttons));
}

// The centre of button i, in screen points.
fe::ScreenPoint button_centre(const fe::StartQuestion &q, std::size_t i) {
    return fe::ScreenPoint{
        static_cast<float>(fe::StartQuestion::kListX + fe::StartQuestion::kListW / 2),
        static_cast<float>(q.list_y() + static_cast<int>(i) * fe::StartQuestion::kRowH + 10), true};
}

// The events that choose button `i` of `n` by one input method.
enum class Input { kKeys, kNumberKey, kPad, kMouse, kTouch };
const char *input_name(Input in) {
    switch (in) {
    case Input::kKeys:
        return "keys";
    case Input::kNumberKey:
        return "a number key";
    case Input::kPad:
        return "the pad";
    case Input::kMouse:
        return "the mouse";
    case Input::kTouch:
        return "touch";
    }
    return "?";
}
constexpr Input kInputs[] = {Input::kKeys, Input::kNumberKey, Input::kPad, Input::kMouse,
                             Input::kTouch};

// The window is 960 x 640 points (integer 2x): screen point (x, y) is window point (2x, 2y).
std::vector<SDL_Event> choose_events(const fe::StartQuestion &q, std::size_t i, Input in) {
    std::vector<SDL_Event> out;
    const fe::ScreenPoint p = button_centre(q, i);
    switch (in) {
    case Input::kKeys:
        for (std::size_t k = 0; k < i; k++) {
            out.push_back(key_down(SDLK_DOWN));
        }
        out.push_back(key_down(SDLK_RETURN));
        break;
    case Input::kNumberKey:
        out.push_back(key_down(static_cast<SDL_Keycode>(SDLK_1 + i)));
        break;
    case Input::kPad:
        for (std::size_t k = 0; k < i; k++) {
            out.push_back(pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
        }
        out.push_back(pad_button(SDL_GAMEPAD_BUTTON_SOUTH));
        break;
    case Input::kMouse:
        for (const Uint32 type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event e{};
            e.type = type;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.x = p.x * 2;
            e.button.y = p.y * 2;
            e.button.which = 1;
            out.push_back(e);
        }
        break;
    case Input::kTouch:
        for (const Uint32 type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
            SDL_Event e{};
            e.type = type;
            e.tfinger.touchID = 77;
            e.tfinger.fingerID = 1;
            e.tfinger.x = p.x * 2 / 960.0f;
            e.tfinger.y = p.y * 2 / 640.0f;
            out.push_back(e);
        }
        break;
    }
    return out;
}

// Feeds `events` to the question as run_start_question does (the pointer's already in screen
// points).
void feed(fe::StartQuestion *q, const std::vector<SDL_Event> &events) {
    for (const SDL_Event &e : events) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            q->pointer(fe::ScreenPoint{e.button.x / 2, e.button.y / 2, true},
                       e.type == SDL_EVENT_MOUSE_BUTTON_DOWN, e.type == SDL_EVENT_MOUSE_BUTTON_UP);
        } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP) {
            q->pointer(fe::ScreenPoint{e.tfinger.x * 480, e.tfinger.y * 320, true},
                       e.type == SDL_EVENT_FINGER_DOWN, e.type == SDL_EVENT_FINGER_UP);
        } else {
            q->handle(e);
        }
    }
}

} // namespace

TEST_CASE("start question: asked in the window where no message box answers every input") {
    CHECK_FALSE(fe::start_question_in_window(false, SettingsPlatform::kDesktop));
    CHECK_FALSE(fe::start_question_in_window(false, SettingsPlatform::kIos));
    CHECK(fe::start_question_in_window(false, SettingsPlatform::kSwitch));
    // A Steam Deck's Game Mode: a desktop build, no keyboard or mouse at hand.
    CHECK(fe::start_question_in_window(true, SettingsPlatform::kDesktop));
    CHECK(fe::platform_has(fe::kCapMessageBox, SettingsPlatform::kDesktop));
    CHECK_FALSE(fe::platform_has(fe::kCapMessageBox, SettingsPlatform::kSwitch));
    // Asked at all: a crash, an interactive run, no --safe-start.
    CHECK(fe::start_asks(false, a_crash(), true));
    CHECK_FALSE(fe::start_asks(true, a_crash(), true));
    CHECK_FALSE(fe::start_asks(false, a_crash(), false));
    CHECK_FALSE(fe::start_asks(false, fe::LastRun{}, true));
}

TEST_CASE("start question: each button by keys, a number key, the pad, the mouse and touch") {
    for (const bool four : {false, true}) {
        const std::size_t n = four ? 4 : 2;
        for (std::size_t i = 0; i < n; i++) {
            for (const Input in : kInputs) {
                CAPTURE(four);
                CAPTURE(i);
                CAPTURE(input_name(in));
                fe::StartQuestion q = four ? four_buttons() : two_buttons();
                REQUIRE(q.buttons().size() == n);
                CHECK_FALSE(q.done());
                feed(&q, choose_events(q, i, in));
                CHECK(q.chosen() == i);
            }
        }
    }
}

TEST_CASE("start question: Escape and B choose Start normally; moving wraps; no early choice") {
    for (const bool four : {false, true}) {
        CAPTURE(four);
        {
            fe::StartQuestion q = four ? four_buttons() : two_buttons();
            q.handle(key_down(SDLK_ESCAPE));
            REQUIRE(q.chosen().has_value());
            CHECK(q.buttons()[*q.chosen()] == fe::start_choice_label(RecoveryChoice::kStartNormal));
        }
        {
            fe::StartQuestion q = four ? four_buttons() : two_buttons();
            q.handle(pad_button(SDL_GAMEPAD_BUTTON_EAST));
            REQUIRE(q.chosen().has_value());
            CHECK(q.buttons()[*q.chosen()] == "Start normally");
        }
        {
            fe::StartQuestion q = four ? four_buttons() : two_buttons();
            q.handle(key_down(SDLK_UP));
            CHECK(q.selected() == q.buttons().size() - 1);
            q.handle(key_down(SDLK_DOWN));
            CHECK(q.selected() == 0);
            // A number past the last button, a press off the buttons: nothing chosen.
            q.handle(key_down(SDLK_9));
            q.pointer(fe::ScreenPoint{240, 20, true}, true, false);
            q.pointer(fe::ScreenPoint{240, 20, true}, false, true);
            CHECK_FALSE(q.done());
            // A press on one button released on another chooses neither.
            q.pointer(button_centre(q, 0), true, false);
            q.pointer(button_centre(q, 1), false, true);
            CHECK_FALSE(q.done());
            CHECK(q.button_at(button_centre(q, 0).x,
                              static_cast<float>(q.list_y() + fe::StartQuestion::kRowH - 2)) == -1);
        }
    }
}

TEST_CASE("start question: the drawing, by hash; the text fits above the buttons") {
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    const fs::path out = fs::path(output_dir()) / "start_question";
    std::error_code ec;
    fs::create_directories(out, ec);
    struct Case {
        bool four;
        const char *name;
        std::uint64_t hash;
    };
    // The port's own drawing (integer arithmetic only): the same bytes everywhere.
    const Case cases[] = {
        {false, "safe_start", 0x56218687e8ee0576ull},
        {true, "recovery", 0xce08fb2a1aa26e19ull},
    };
    for (const Case &k : cases) {
        CAPTURE(k.name);
        fe::StartQuestion q = k.four ? four_buttons() : two_buttons();
        // Every line fits the width, and all of them above the buttons.
        int bottom = fe::StartQuestion::kTextY;
        for (const std::string &line : q.lines()) {
            CHECK(fe::Canvas::text_width(line) <= 480 - 2 * fe::StartQuestion::kTextX);
            bottom += line.empty() ? fe::StartQuestion::kParagraphGap : fe::StartQuestion::kLineH;
        }
        CHECK(bottom <= q.list_y() - 4);
        for (const std::string &b : q.buttons()) {
            CHECK(fe::Canvas::text_width(b, 2) <= fe::StartQuestion::kListW - 38);
        }
        q.handle(key_down(SDLK_DOWN));
        CHECK(q.dirty());
        std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0x55);
        q.draw(px.data(), 480, 320, 1);
        CHECK_FALSE(q.dirty());
        const std::uint64_t h = fnv1a(px);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        std::vector<std::uint8_t> px2(std::size_t{960} * 640 * 4, 0x55);
        q.draw(px2.data(), 960, 640, 2);
        SDL_Surface *s =
            SDL_CreateSurfaceFrom(960, 640, SDL_PIXELFORMAT_ABGR8888, px2.data(), 960 * 4);
        const fs::path png = out / (std::string(k.name) + ".png");
        CHECK((s != nullptr && SDL_SavePNG(s, png.string().c_str())));
        SDL_DestroySurface(s);
        MESSAGE(k.name << ": " << png.string() << " hash " << hex);
        CHECK(h == k.hash);
    }
    SDL_Quit();
}

TEST_CASE("start question: the window answers decide_start with every input method") {
    // A dummy-driver window at 960 x 640 points. Events pushed before the loop runs are what the
    // player does.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(
        SDL_CreateWindowAndRenderer("start_question_test", 960, 640, 0, &window, &renderer),
        SDL_GetError());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const fe::Settings settings;
    bool quit = true;
    const auto push = [](const std::vector<SDL_Event> &events) {
        for (SDL_Event e : events) {
            REQUIRE(SDL_PushEvent(&e));
        }
    };

    // Without a recovery point: the two buttons. What the box's answer gives, the window's does.
    const bool safe[] = {true, false}; // Start with default settings, Start normally
    for (std::size_t i = 0; i < 2; i++) {
        for (const Input in : kInputs) {
            CAPTURE(i);
            CAPTURE(input_name(in));
            push(choose_events(two_buttons(), i, in));
            const fe::StartDecision d =
                fe::decide_start(false, a_crash(), true, fe::RecoveryOffer{},
                                 fe::window_start_prompts(window, renderer, settings, &quit));
            CHECK_FALSE(quit);
            CHECK(d.safe_start == safe[i]);
            CHECK_FALSE(d.resume);
        }
    }
    // With one: ADR 0151's four.
    struct Want {
        bool safe_start, resume;
    };
    const Want want[] = {{false, true}, {true, true}, {true, false}, {false, false}};
    for (std::size_t i = 0; i < 4; i++) {
        for (const Input in : kInputs) {
            CAPTURE(i);
            CAPTURE(input_name(in));
            push(choose_events(four_buttons(), i, in));
            const fe::StartDecision d =
                fe::decide_start(false, a_crash(), true, an_offer(),
                                 fe::window_start_prompts(window, renderer, settings, &quit));
            CHECK_FALSE(quit);
            CHECK(d.safe_start == want[i].safe_start);
            CHECK(d.resume == want[i].resume);
        }
    }
    // Escape and B: Start normally, in both questions.
    for (const SDL_Event &e : {key_down(SDLK_ESCAPE), pad_button(SDL_GAMEPAD_BUTTON_EAST)}) {
        for (const bool four : {false, true}) {
            CAPTURE(four);
            push({e});
            const fe::StartDecision d =
                fe::decide_start(false, a_crash(), true, four ? an_offer() : fe::RecoveryOffer{},
                                 fe::window_start_prompts(window, renderer, settings, &quit));
            CHECK_FALSE(quit);
            CHECK_FALSE(d.safe_start);
            CHECK_FALSE(d.resume);
        }
    }
    // Closing the window: Start normally, and quit.
    {
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        push({e});
        const fe::StartDecision d =
            fe::decide_start(false, a_crash(), true, an_offer(),
                             fe::window_start_prompts(window, renderer, settings, &quit));
        CHECK(quit);
        CHECK_FALSE(d.safe_start);
        CHECK_FALSE(d.resume);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
