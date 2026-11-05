// The controls card (#193, controls_card.h): the lists come from the live bindings (a rebind
// shows, an unbound action says so, a new action such as page shows on its side); the device
// and the glyph set follow the input on SDL3 virtual Xbox 360, PS5 and Switch Pro pads; the
// touch card follows the key pad's layout; opening, closing and what the card takes; the preset
// picker's second step; and the card drawn per device against synthetic goldens (port art only:
// Canvas shapes and the port's font).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "canvas.h"
#include "controls_card.h"
#include "gamepad.h"
#include "golden.h"
#include "input.h"
#include "keypad_layout.h"
#include "maplethief/input.h"
#include "preset_picker.h"
#include "prompts.h"
#include "settings.h"
#include "settings_menu.h"

#include "virtual_pad.h"

namespace {

namespace fe = maplethief::frontend;
namespace golden = maplethief::golden;
using fe::Context;
using fe::Control;
using fe::ControlKind;
using fe::PromptDevice;
using virtual_pad::Kind;

Control key(SDL_Keycode k) { return Control{ControlKind::kKey, static_cast<int>(k)}; }
Control button(SDL_GamepadButton b) {
    return Control{ControlKind::kGamepadButton, static_cast<int>(b)};
}

SDL_Event key_event(SDL_Keycode k, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

SDL_Event finger_event(bool down, SDL_FingerID id = 3) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP;
    e.tfinger.touchID = 77;
    e.tfinger.fingerID = id;
    return e;
}

const fe::CardLine *find_line(const std::vector<fe::CardLine> &lines, Context context,
                              std::uint32_t action) {
    for (const fe::CardLine &l : lines) {
        if (l.context == context && l.action == action && !l.settings) {
            return &l;
        }
    }
    return nullptr;
}

// The frontend's routing (game.cpp's handle_event): the card first, the devices only with what
// it does not take, then the tracking.
struct Front {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::ControlsCard card{&actions, &gamepads};
    std::vector<std::string> sent;

    static void record(void *user, std::uint32_t action, bool down) {
        const mt_input_action_info *info = mt_input_action_get_info(action);
        static_cast<Front *>(user)->sent.push_back(std::string(down ? "+" : "-") +
                                                   (info != nullptr ? info->name : "?"));
    }
    static std::int32_t field(void *) { return 0; }

    Front() {
        actions.set_sink(record, this);
        actions.set_panel_source(field, nullptr);
    }
    void send(const SDL_Event &e) {
        if (!card.handle(e)) {
            input.handle(e);
            gamepads.handle(e);
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
                actions.control(fe::InputSource::kKeyboard, key(e.key.key),
                                e.type == SDL_EVENT_KEY_DOWN);
            }
        }
        card.track(e);
    }
    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            send(e);
        }
    }
};

struct Sdl {
    Sdl() {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    }
    ~Sdl() { SDL_Quit(); }
};

} // namespace

TEST_CASE("controls card: the labels come from the bindings; a rebind shows at once") {
    std::vector<fe::Binding> table = fe::default_bindings();
    // The defaults: the keyboard's first two keys, the pad's first two controls.
    std::vector<fe::CardLine> keys = fe::controls_card_lines(table, PromptDevice::kKeyboard);
    const fe::CardLine *attack = find_line(keys, Context::kField, MT_ACTION_ATTACK);
    REQUIRE(attack != nullptr);
    CHECK(attack->title == "Attack");
    CHECK(attack->controls == std::vector<Control>{key(SDLK_Z), key(SDLK_LCTRL)});
    std::vector<fe::CardLine> pad = fe::controls_card_lines(table, PromptDevice::kGamepad);
    const fe::CardLine *slot1 = find_line(pad, Context::kField, MT_ACTION_SLOT1);
    REQUIRE(slot1 != nullptr);
    CHECK(slot1->controls == std::vector<Control>{button(SDL_GAMEPAD_BUTTON_NORTH)}); // Y
    const fe::CardLine *cash = find_line(pad, Context::kField, MT_ACTION_CASH_SHOP);
    REQUIRE(cash != nullptr);
    CHECK(cash->controls ==
          std::vector<Control>{{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER}});

    // Attack rebound to V: V is its only key; the pad's controls stay.
    fe::bind_control(&table, Context::kField, MT_ACTION_ATTACK, key(SDLK_V));
    keys = fe::controls_card_lines(table, PromptDevice::kKeyboard);
    attack = find_line(keys, Context::kField, MT_ACTION_ATTACK);
    REQUIRE(attack != nullptr);
    CHECK(attack->controls == std::vector<Control>{key(SDLK_V)});
    CHECK(fe::key_label(attack->controls[0].code) == "V");
    pad = fe::controls_card_lines(table, PromptDevice::kGamepad);
    CHECK(find_line(pad, Context::kField, MT_ACTION_ATTACK)->controls.size() == 2);

    // Through the overlay: the live bindings of Actions, as the remap overlay's Save leaves them.
    fe::Input input;
    fe::Actions actions(&input);
    fe::ControlsCard card(&actions, nullptr);
    fe::bind_control(&actions.bindings(), Context::kField, MT_ACTION_ATTACK, key(SDLK_V));
    const std::vector<fe::CardLine> live =
        fe::controls_card_lines(card.bindings(), PromptDevice::kKeyboard);
    CHECK(find_line(live, Context::kField, MT_ACTION_ATTACK)->controls ==
          std::vector<Control>{key(SDLK_V)});
}

TEST_CASE("controls card: every action on the card has a binding or shows unbound") {
    for (const PromptDevice device : {PromptDevice::kKeyboard, PromptDevice::kGamepad}) {
        CAPTURE(fe::prompt_device_name(device));
        std::vector<fe::Binding> table = fe::default_bindings();
        std::vector<fe::CardLine> lines = fe::controls_card_lines(table, device);
        const auto of_device = [device](const Control &c) {
            return (c.kind == ControlKind::kKey) == (device == PromptDevice::kKeyboard);
        };
        // Every (context, action) the bindings list and the device has a control for is on the
        // card once: Move holds the directions when each has a control of the device.
        for (const fe::ActionSlot &s : fe::action_slots(table)) {
            CAPTURE(s.action);
            const std::vector<Control> all = fe::controls_for(table, s.context, s.action);
            if (std::none_of(all.begin(), all.end(), of_device)) {
                CHECK(find_line(lines, s.context, s.action) == nullptr);
                continue;
            }
            const bool direction = s.action >= MT_ACTION_UP && s.action <= MT_ACTION_RIGHT;
            const auto n = std::count_if(lines.begin(), lines.end(), [&](const fe::CardLine &l) {
                return l.context == s.context && !l.settings &&
                       (direction ? l.move : l.action == s.action && !l.move);
            });
            CHECK(n == 1);
        }
        // A line has the device's controls: with the defaults, every one has some.
        for (const fe::CardLine &l : lines) {
            CAPTURE(l.title);
            CHECK(l.controls.size() <= (l.move ? 4u : fe::kCardMaxControls));
            CHECK(std::all_of(l.controls.begin(), l.controls.end(), of_device));
            CHECK_FALSE(l.controls.empty());
        }
        // The settings menu's own control closes the field's list.
        const auto settings = std::find_if(lines.begin(), lines.end(),
                                           [](const fe::CardLine &l) { return l.settings; });
        REQUIRE(settings != lines.end());
        CHECK(settings->context == Context::kField);
        CHECK(settings->controls.size() == 1);
        CHECK(fe::reserved_control(settings->controls[0]));
        // Page (ADR 0114) is on the menus side only.
        CHECK(find_line(lines, Context::kMenu, MT_ACTION_PAGE) != nullptr);
        CHECK(find_line(lines, Context::kField, MT_ACTION_PAGE) == nullptr);
        CHECK(find_line(lines, Context::kMenu, MT_ACTION_PAGE)->title == "Page (hold)");

        // Jump unbound: its line stays and has no control (drawn "unbound").
        fe::set_controls(&table, Context::kField, MT_ACTION_JUMP, {});
        lines = fe::controls_card_lines(table, device);
        const fe::CardLine *jump = find_line(lines, Context::kField, MT_ACTION_JUMP);
        REQUIRE(jump != nullptr);
        CHECK(jump->controls.empty());
        // Left unbound: no Move line in the field, each direction its own line, Left unbound.
        fe::set_controls(&table, Context::kField, MT_ACTION_LEFT, {});
        lines = fe::controls_card_lines(table, device);
        CHECK(std::none_of(lines.begin(), lines.end(), [](const fe::CardLine &l) {
            return l.move && l.context == Context::kField;
        }));
        REQUIRE(find_line(lines, Context::kField, MT_ACTION_LEFT) != nullptr);
        CHECK(find_line(lines, Context::kField, MT_ACTION_LEFT)->controls.empty());
        CHECK(find_line(lines, Context::kField, MT_ACTION_RIGHT)->controls.size() >= 1);
        // Menus keep their Move line.
        CHECK(std::any_of(lines.begin(), lines.end(), [](const fe::CardLine &l) {
            return l.move && l.context == Context::kMenu;
        }));
    }
}

TEST_CASE("controls card: a new action shows without code of its own") {
    // An action the card has no title for (as #185's pointer tap will be) shows through the
    // table's title; here the port's auto-run, bound in menus where it has no default.
    std::vector<fe::Binding> table = fe::default_bindings();
    fe::set_controls(&table, Context::kMenu, MT_ACTION_AUTO_RUN, {key(SDLK_K)});
    const std::vector<fe::CardLine> lines = fe::controls_card_lines(table, PromptDevice::kKeyboard);
    const fe::CardLine *l = find_line(lines, Context::kMenu, MT_ACTION_AUTO_RUN);
    REQUIRE(l != nullptr);
    CHECK(l->title == "Auto-run");
    CHECK(l->controls == std::vector<Control>{key(SDLK_K)});
}

TEST_CASE("controls card: the device and the pad's glyph set follow the input") {
    Sdl sdl;
    Front f;
    f.pump();
    CHECK(f.card.style().device == PromptDevice::kNone); // nothing used yet: touch
    // The preset picker's device until the card sees input of its own.
    f.card.seed_device(PromptDevice::kKeyboard);
    CHECK(f.card.style().device == PromptDevice::kKeyboard);
    f.send(key_event(SDLK_LEFT, true));
    f.send(key_event(SDLK_LEFT, false));
    CHECK(f.card.style().device == PromptDevice::kKeyboard);

    auto xbox = std::make_unique<virtual_pad::Pad>(Kind::kXbox360);
    REQUIRE_MESSAGE(xbox->ok(), xbox->error);
    f.pump();
    CHECK(f.card.style().device == PromptDevice::kGamepad);
    CHECK(f.card.glyphs().set() == fe::GlyphSet::kXbox);
    auto ps5 = std::make_unique<virtual_pad::Pad>(Kind::kPs5, true);
    REQUIRE_MESSAGE(ps5->ok(), ps5->error);
    f.pump();
    CHECK(f.card.glyphs().set() == fe::GlyphSet::kPlayStation);
    CHECK(f.card.style().type == SDL_GAMEPAD_TYPE_PS5);
    auto sw = std::make_unique<virtual_pad::Pad>(Kind::kSwitchPro);
    REQUIRE_MESSAGE(sw->ok(), sw->error);
    f.pump();
    CHECK(f.card.glyphs().set() == fe::GlyphSet::kNintendo);
    // The card open: a pad that connects redraws it with its glyphs.
    sw.reset();
    f.pump();
    f.card.open();
    std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0);
    f.card.draw(px.data(), 480, 320, 1);
    CHECK_FALSE(f.card.dirty());
    auto sw2 = std::make_unique<virtual_pad::Pad>(Kind::kSwitchPro);
    REQUIRE_MESSAGE(sw2->ok(), sw2->error);
    f.pump();
    CHECK(f.card.is_open()); // a connection is no press
    CHECK(f.card.glyphs().set() == fe::GlyphSet::kNintendo);
    CHECK(f.card.dirty());
    f.card.close("test");
    // A tap: touch (tracked; the devices need a window for a finger).
    f.card.track(finger_event(true));
    f.card.track(finger_event(false));
    CHECK(f.card.style().device == PromptDevice::kNone);
}

TEST_CASE("controls card: open pauses input; any press closes it, and its release is taken") {
    Front f;
    // Walking when it opens: the walk is let go.
    f.send(key_event(SDLK_LEFT, true));
    CHECK(f.sent == std::vector<std::string>{"+left"});
    f.card.open();
    CHECK(f.card.is_open());
    CHECK(f.card.opens() == 1);
    CHECK(f.sent == std::vector<std::string>{"+left", "-left"});
    // While open nothing reaches the game: key ups, motion, a repeat (which does not close).
    f.send(key_event(SDLK_LEFT, false));
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    CHECK(f.card.handle(motion));
    f.send(key_event(SDLK_Z, true, true));
    CHECK(f.card.is_open());
    SDL_Event added{};
    added.type = SDL_EVENT_GAMEPAD_ADDED;
    CHECK_FALSE(f.card.handle(added)); // Gamepads opens pads whatever is shown
    // A press closes it; neither it nor its release reaches the game.
    f.send(key_event(SDLK_Z, true));
    CHECK_FALSE(f.card.is_open());
    CHECK(f.card.closes() == 1);
    f.send(key_event(SDLK_Z, false));
    CHECK(f.sent == std::vector<std::string>{"+left", "-left"});
    // Closed: the next press is the game's again.
    f.send(key_event(SDLK_Z, true));
    f.send(key_event(SDLK_Z, false));
    CHECK(f.sent == std::vector<std::string>{"+left", "-left", "+attack", "-attack"});
    // A tap closes it too, its finger's up taken; a pad button as well.
    f.card.open();
    CHECK(f.card.handle(finger_event(true, 9)));
    CHECK_FALSE(f.card.is_open());
    CHECK(f.card.handle(finger_event(false, 9)));
    CHECK_FALSE(f.card.handle(finger_event(false, 9))); // only once
    f.card.open();
    SDL_Event b{};
    b.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    b.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(f.card.handle(b));
    CHECK_FALSE(f.card.is_open());
}

TEST_CASE("controls card: the touch card follows the key pad's layout") {
    const fe::KeypadLayout original;
    std::vector<fe::TouchCardKey> keys = fe::touch_card_keys(original);
    REQUIRE(keys.size() == 1 + fe::kHudKeyCount);
    CHECK(keys[0].title == "Move");
    CHECK(keys[0].dpad);
    CHECK(keys[0].rect.x == 0);
    CHECK(keys[0].rect.y == 180);
    CHECK(keys[0].rect.w == 168);
    const auto titled = [&keys](const char *t) {
        return std::find_if(keys.begin(), keys.end(),
                            [t](const fe::TouchCardKey &k) { return k.title == t; });
    };
    for (const char *t :
         {"Jump", "Attack", "Quick slot 1", "Quick slot 4", "Map", "Menu", "Cash shop"}) {
        CAPTURE(t);
        CHECK(titled(t) != keys.end());
    }
    // The d-pad at 80 % moved up, the actions moved left: the card draws them there.
    fe::KeypadLayout custom;
    REQUIRE(fe::parse_keypad_layout("dpad=80,12,-20,70;actions=100,-40,0,100", &custom));
    keys = fe::touch_card_keys(custom);
    const fe::KeypadRect dpad =
        fe::keypad_view_rect(fe::KeypadGroup::kDpad, custom[fe::KeypadGroup::kDpad], {});
    CHECK(keys[0].rect.w == doctest::Approx(168 * 0.8));
    CHECK(keys[0].rect.x == doctest::Approx(dpad.x).epsilon(0.5));
    CHECK(titled("Jump")->rect.x == doctest::Approx(335 - 40));
    CHECK(titled("Map")->rect.x == 0); // Map, Bag and Cash stay
}

TEST_CASE("controls card: drawn per device, against synthetic goldens") {
    // The card is the port's own drawing: Canvas shapes and the port's font, integer arithmetic
    // only, over a plain synthetic backdrop. No game art.
    const std::vector<fe::Binding> defaults = fe::default_bindings();
    fe::KeypadLayout custom;
    REQUIRE(fe::parse_keypad_layout("dpad=80,12,-20,70;slots=120,-30,0,100", &custom));
    struct Case {
        const char *name;
        fe::PromptStyle style;
        const fe::KeypadLayout *layout;
    };
    const fe::KeypadLayout original;
    const Case cases[] = {
        {"keyboard", {PromptDevice::kKeyboard, SDL_GAMEPAD_TYPE_UNKNOWN}, &original},
        {"xbox", {PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_XBOXONE}, &original},
        {"playstation", {PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_PS5}, &original},
        {"nintendo", {PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO}, &original},
        {"touch", {PromptDevice::kNone, SDL_GAMEPAD_TYPE_UNKNOWN}, &original},
        {"touch_custom", {PromptDevice::kNone, SDL_GAMEPAD_TYPE_UNKNOWN}, &custom},
    };
    for (const Case &k : cases) {
        CAPTURE(k.name);
        golden::Image img(480, 320, {0x40, 0x70, 0x50, 255});
        std::vector<std::uint8_t> px = img.bytes();
        fe::Canvas c(px.data(), 480, 320, 1);
        fe::draw_controls_card(c, k.style, defaults, *k.layout);
        golden::Image out(480, 320);
        for (int y = 0; y < 320; y++) {
            for (int x = 0; x < 480; x++) {
                const std::uint8_t *p = &px[(std::size_t(y) * 480 + x) * 4];
                out.set(x, y, {p[0], p[1], p[2], p[3]});
            }
        }
        const auto r = golden::check_golden(out,
                                            std::filesystem::path(MT_GOLDEN_DATA_DIR) /
                                                (std::string("controls_card_") + k.name + ".png"),
                                            std::string("controls_card_") + k.name,
                                            {.channel = 0, .max_differing_ratio = 0});
        CHECK_MESSAGE(r.passed, r.message);
    }
    // The rebind shows in the picture: attack on V lights V in the keyboard and lists it.
    std::vector<fe::Binding> rebound = defaults;
    fe::bind_control(&rebound, Context::kField, MT_ACTION_ATTACK, key(SDLK_V));
    std::vector<std::uint8_t> a(std::size_t{480} * 320 * 4, 0), b = a;
    fe::Canvas ca(a.data(), 480, 320, 1), cb(b.data(), 480, 320, 1);
    const fe::PromptStyle keyboard{PromptDevice::kKeyboard, SDL_GAMEPAD_TYPE_UNKNOWN};
    fe::draw_controls_card(ca, keyboard, defaults, original);
    fe::draw_controls_card(cb, keyboard, rebound, original);
    CHECK(a != b);
    // At render scale 2 the same layout, finer: it draws without leaving the image.
    std::vector<std::uint8_t> px2(std::size_t{960} * 640 * 4, 0);
    fe::Canvas c2(px2.data(), 960, 640, 2);
    fe::draw_controls_card(c2, keyboard, defaults, original);
    CHECK(std::any_of(px2.begin(), px2.end(), [](std::uint8_t v) { return v != 0; }));
}

TEST_CASE("controls card: an action row in the settings menu's Controls tab") {
    const fe::MenuActionInfo *row = fe::find_menu_action("controls_card");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kControls);
    CHECK(std::string(row->label) == "Controls card");
}

TEST_CASE("preset picker: the second step offers the controls card") {
    const auto key_down = [](SDL_Keycode k) { return key_event(k, true); };
    {
        fe::PresetPicker picker(false, true);
        CHECK(picker.count() == 3);
        picker.handle(key_down(SDLK_DOWN));
        picker.handle(key_down(SDLK_RETURN));
        CHECK(picker.chosen() == fe::Preset::kOriginalController);
        CHECK(picker.offering());
        CHECK_FALSE(picker.done());
        CHECK(picker.count() == 2);
        CHECK(picker.selected() == 0); // "Show controls" first
        picker.handle(key_down(SDLK_DOWN));
        picker.handle(key_down(SDLK_RETURN));
        CHECK(picker.done());
        CHECK(picker.show_controls() == false);
        CHECK(picker.chosen() == fe::Preset::kOriginalController); // unchanged
    }
    {
        fe::PresetPicker picker(false, true);
        picker.handle(key_down(SDLK_3));
        picker.handle(key_down(SDLK_1));
        CHECK(picker.chosen() == fe::Preset::kEnhanced);
        CHECK(picker.show_controls() == true);
        // Its drawing: the second step's title and lines.
        std::vector<std::uint8_t> px(std::size_t{480} * 320 * 4, 0);
        picker.draw(px.data(), 480, 320, 1);
        CHECK_FALSE(picker.dirty());
    }
    {
        // Without the step (the default): done at the preset, as before.
        fe::PresetPicker picker(false);
        picker.handle(key_down(SDLK_RETURN));
        CHECK(picker.done());
        CHECK_FALSE(picker.show_controls().has_value());
    }
}

TEST_CASE("preset picker: the window loop asks the second step when asked to") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(
        SDL_CreateWindowAndRenderer("controls_card_test", 960, 640, 0, &window, &renderer),
        SDL_GetError());
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    const fe::Settings settings;
    for (const bool show : {true, false}) {
        CAPTURE(show);
        for (const SDL_Keycode k : {SDLK_RETURN, show ? SDLK_RETURN : SDLK_2}) {
            SDL_Event e = key_event(k, true);
            REQUIRE(SDL_PushEvent(&e));
        }
        bool quit = true;
        fe::ControlsOffer offer{!show, PromptDevice::kGamepad};
        CHECK(fe::run_preset_picker(window, renderer, settings, false, &quit, &offer) ==
              fe::Preset::kOriginal);
        CHECK_FALSE(quit);
        CHECK(offer.show == show);
        CHECK(offer.device == PromptDevice::kKeyboard); // answered with keys
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
