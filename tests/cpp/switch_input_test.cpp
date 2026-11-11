// Switch input (#239, ADR 0140) with synthetic gamepad reports only: SDL3 virtual joysticks built
// as the pinned Switch SDL (nx-mod/sdl3-nx 07ea26a3, src/joystick/switch/SDL_sysjoystick.c)
// reports its pads: named "Switch Controller", no vendor or product id, 28 buttons and 4 axes in
// libnx's order (A B X Y, the stick clicks, L R ZL ZR, + -, the d-pad, ...) and that driver's
// mapping (South is raw button 1, B; East raw 0, A; the triggers raw 8 and 9 as buttons). With
// synthetic libnx reports (switch_pads.h's source) for each controller mode: what SDL says of
// them (Standard, Xbox labels), the type and glyphs the port shows instead, the Switch's default
// bindings (A confirms and B goes back in menus, the field by position), + reserved for the
// settings menu, the key pad hidden at the start, rumble with the strength setting, and player 1
// as the first pad pressed among the eight that are always there.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "gamepad.h"
#include "glyphs.h"
#include "input.h"
#include "keypad_hide.h"
#include "maplethief/input.h"
#include "rumble.h"
#include "settings.h"
#include "switch_pads.h"

namespace {

namespace fe = maplethief::frontend;
using u32 = std::uint32_t;
using i32 = std::int32_t;

// The fork's raw buttons (pad_mapping_default's order).
enum Raw : int {
    kRawA = 0,
    kRawB,
    kRawX,
    kRawY,
    kRawStickL,
    kRawStickR,
    kRawL,
    kRawR,
    kRawZL,
    kRawZR,
    kRawPlus,
    kRawMinus,
    kRawLeft,
    kRawUp,
    kRawRight,
    kRawDown,
    kRawButtons = 28,
};

// A pad as the Switch's SDL reports it, through SDL's virtual driver and that driver's mapping.
struct SwitchPad {
    SDL_JoystickID id = 0;
    SDL_Joystick *joystick = nullptr;
    std::string error;
    std::vector<std::pair<Uint16, Uint16>> rumbles;

    SwitchPad() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.name = fe::kSwitchPadName;
        desc.naxes = 4;
        desc.nbuttons = kRawButtons;
        desc.button_mask = 0; // every button through the mapping below
        desc.axis_mask = 0;
        desc.userdata = this;
        desc.Rumble = [](void *user, Uint16 low, Uint16 high) {
            static_cast<SwitchPad *>(user)->rumbles.emplace_back(low, high);
            return true;
        };
        id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            error = SDL_GetError();
            return;
        }
        // SWITCH_JoystickGetGamepadMapping, as a mapping line.
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
        const std::string line =
            std::string(guid) +
            ",Switch Controller,a:b1,b:b0,x:b3,y:b2,back:b11,start:b10,leftstick:b4,"
            "rightstick:b5,leftshoulder:b6,rightshoulder:b7,dpup:b13,dpdown:b15,dpleft:b12,"
            "dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b8,righttrigger:b9,";
        if (SDL_AddGamepadMapping(line.c_str()) < 0) {
            error = SDL_GetError();
            return;
        }
        joystick = SDL_OpenJoystick(id);
        if (joystick == nullptr) {
            error = SDL_GetError();
        }
    }
    ~SwitchPad() {
        if (joystick != nullptr) {
            SDL_CloseJoystick(joystick);
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
        }
    }
    SwitchPad(const SwitchPad &) = delete;
    SwitchPad &operator=(const SwitchPad &) = delete;

    bool ok() const { return id != 0 && joystick != nullptr && error.empty(); }
    bool raw(int button, bool down) { return SDL_SetJoystickVirtualButton(joystick, button, down); }
};

// Synthetic libnx reports by SDL instance id.
std::map<SDL_JoystickID, fe::SwitchPadReport> g_reports;
bool report_source(void *, SDL_JoystickID id, fe::SwitchPadReport *out) {
    const auto it = g_reports.find(id);
    if (it == g_reports.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

fe::SwitchPadReport report_for(fe::SwitchPadMode mode) {
    fe::SwitchPadReport r;
    switch (mode) {
    case fe::SwitchPadMode::kHandheld:
        r.handheld_style = fe::kNpadStyleHandheld;
        break;
    case fe::SwitchPadMode::kJoyConPair:
        r.style = fe::kNpadStyleJoyDual;
        r.device = fe::kNpadDeviceJoyLeft | fe::kNpadDeviceJoyRight;
        break;
    case fe::SwitchPadMode::kProController:
        r.style = fe::kNpadStyleFullKey;
        r.device = fe::kNpadDeviceFullKey;
        break;
    case fe::SwitchPadMode::kJoyConLeft:
        r.style = fe::kNpadStyleJoyLeft;
        r.device = fe::kNpadDeviceJoyLeft;
        break;
    case fe::SwitchPadMode::kJoyConRight:
        r.style = fe::kNpadStyleJoyRight;
        r.device = fe::kNpadDeviceJoyRight;
        break;
    case fe::SwitchPadMode::kNone:
        break;
    }
    return r;
}

std::vector<std::string> g_actions;
i32 g_panel = 0;
void record_action(void *, u32 action, bool down) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    g_actions.push_back(std::string(down ? "+" : "-") + (info != nullptr ? info->name : "?"));
}
i32 fixed_panel(void *) { return g_panel; }

// SDL's gamepads, the Switch's defaults (bindings and face buttons) and the synthetic reports.
struct Console {
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};

    Console() {
        g_actions.clear();
        g_reports.clear();
        g_panel = 0;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
        fe::set_switch_pad_source(report_source, nullptr);
        actions.set_sink(record_action, nullptr);
        actions.set_panel_source(fixed_panel, nullptr);
        actions.bindings() = fe::default_bindings(fe::SettingsPlatform::kSwitch);
        gamepads.set_face_buttons(fe::face_buttons_default(fe::SettingsPlatform::kSwitch));
    }
    ~Console() {
        fe::set_switch_pad_source(nullptr, nullptr);
        SDL_Quit();
    }
    Console(const Console &) = delete;
    Console &operator=(const Console &) = delete;

    void pump() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input.handle(e);
            gamepads.handle(e);
        }
    }
    std::vector<std::string> press(SwitchPad &pad, int raw) {
        g_actions.clear();
        pad.raw(raw, true);
        pump();
        pad.raw(raw, false);
        pump();
        return g_actions;
    }
};

using Lines = std::vector<std::string>;
const fe::Control button(SDL_GamepadButton b) {
    return fe::Control{fe::ControlKind::kGamepadButton, static_cast<int>(b)};
}
const fe::Control trigger(SDL_GamepadAxis a) {
    return fe::Control{fe::ControlKind::kGamepadTrigger, static_cast<int>(a)};
}

} // namespace

TEST_CASE("switch input: libnx's controller modes, and the type each is shown as") {
    using M = fe::SwitchPadMode;
    struct Row {
        M mode;
        SDL_GamepadType type;
    };
    for (const Row r : {Row{M::kHandheld, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR},
                        Row{M::kJoyConPair, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR},
                        Row{M::kProController, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO},
                        Row{M::kJoyConLeft, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT},
                        Row{M::kJoyConRight, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT},
                        Row{M::kNone, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO}}) {
        CAPTURE(fe::switch_pad_mode_name(r.mode));
        CHECK(fe::switch_pad_mode(report_for(r.mode)) == r.mode);
        CHECK(fe::switch_pad_type(r.mode) == r.type);
        CHECK(fe::glyph_set_for(r.type) == fe::GlyphSet::kNintendo);
    }
    // SDL's own test for a lone Joy-Con: the device type without JoyDual, whatever the style
    // says; a pair is a pair even though both device bits are set.
    fe::SwitchPadReport sideways;
    sideways.style = fe::kNpadStyleFullKey; // a style SDL does not look at for this
    sideways.device = fe::kNpadDeviceJoyRight;
    CHECK(fe::switch_pad_mode(sideways) == M::kJoyConRight);
    // Pad 1 is No1 first: a Pro Controller on No1 wins over the attached Joy-Cons.
    fe::SwitchPadReport both = report_for(M::kProController);
    both.handheld_style = fe::kNpadStyleHandheld;
    CHECK(fe::switch_pad_mode(both) == M::kProController);
    // Handheld Joy-Cons report the handheld device bits, not JoyLeft / JoyRight.
    fe::SwitchPadReport handheld;
    handheld.style = fe::kNpadStyleHandheld;
    handheld.device = fe::kNpadDeviceHandheldLeft | fe::kNpadDeviceHandheldRight;
    CHECK(fe::switch_pad_mode(handheld) == M::kHandheld);

    // The rule: only the console's own pad, only where its pads are live.
    CHECK(fe::port_pad_type(SDL_GAMEPAD_TYPE_STANDARD, "Switch Controller", M::kJoyConLeft, true) ==
          SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT);
    CHECK(fe::port_pad_type(SDL_GAMEPAD_TYPE_STANDARD, "Switch Controller", M::kJoyConLeft,
                            false) == SDL_GAMEPAD_TYPE_STANDARD);
    CHECK(fe::port_pad_type(SDL_GAMEPAD_TYPE_STANDARD, "Generic Pad", M::kJoyConLeft, true) ==
          SDL_GAMEPAD_TYPE_STANDARD);
    CHECK(fe::port_pad_type(SDL_GAMEPAD_TYPE_XBOXONE, "Switch Controller", M::kHandheld, true) ==
          SDL_GAMEPAD_TYPE_XBOXONE);
}

TEST_CASE("switch input: SDL reports a Standard pad with Xbox labels; the port shows Nintendo's") {
    Console c;
    SwitchPad pad;
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    c.pump();
    // What SDL itself says of the console's pad (ADR 0138's consequence).
    CHECK(std::string(SDL_GetGamepadNameForID(pad.id)) == "Switch Controller");
    CHECK(SDL_GetGamepadTypeForID(pad.id) == SDL_GAMEPAD_TYPE_STANDARD);
    CHECK(SDL_GetGamepadButtonLabelForType(SDL_GetGamepadTypeForID(pad.id),
                                           SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A);
    // The mapping is by position: libnx's B (raw 1) is South, its A (raw 0) East.
    SDL_Gamepad *g = SDL_GetGamepadFromID(pad.id);
    REQUIRE(g != nullptr);
    pad.raw(kRawB, true);
    SDL_UpdateGamepads();
    CHECK(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH));
    pad.raw(kRawB, false);
    pad.raw(kRawA, true);
    SDL_UpdateGamepads();
    CHECK(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST));
    pad.raw(kRawA, false);
    c.pump();

    // Each mode: the type the port uses, its labels, and the log line for the device runs.
    using M = fe::SwitchPadMode;
    for (const M mode :
         {M::kHandheld, M::kJoyConPair, M::kProController, M::kJoyConLeft, M::kJoyConRight}) {
        CAPTURE(fe::switch_pad_mode_name(mode));
        g_reports[pad.id] = report_for(mode);
        const SDL_GamepadType type = fe::pad_type(pad.id);
        CHECK(type == fe::switch_pad_type(mode));
        CHECK(SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_EAST) ==
              SDL_GAMEPAD_BUTTON_LABEL_A);
        CHECK(SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_SOUTH) ==
              SDL_GAMEPAD_BUTTON_LABEL_B);
        const std::string line = fe::pad_report_line(pad.id);
        CAPTURE(line);
        CHECK(line.find("'Switch Controller'") != std::string::npos);
        CHECK(line.find("SDL type standard") != std::string::npos);
        CHECK(line.find("SDL labels S=A E=B W=X N=Y") != std::string::npos);
        CHECK(line.find("rumble yes") != std::string::npos);
        CHECK(line.find(std::string(": ") + fe::switch_pad_mode_name(mode) +
                        ", shown as Nintendo") != std::string::npos);
        // The pad's type follows the mode at the next turn (Joy-Cons detached, one alone).
        c.gamepads.turn(0);
        CHECK(c.gamepads.type_of(pad.id) == type);
    }
    // Off the console nothing changes: the source gone, SDL's Standard type stays.
    fe::set_switch_pad_source(nullptr, nullptr);
    if (fe::kSettingsPlatform != fe::SettingsPlatform::kSwitch) {
        CHECK(fe::pad_type(pad.id) == SDL_GAMEPAD_TYPE_STANDARD);
    }
}

TEST_CASE("switch input: the defaults: A confirms and B goes back in menus, the field by "
          "position, + reserved") {
    Console c;
    SwitchPad pad;
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    g_reports[pad.id] = report_for(fe::SwitchPadMode::kHandheld);
    c.pump();
    REQUIRE(c.gamepads.count() == 1);
    CHECK(c.gamepads.face_buttons() == fe::FaceButtons::kLabel);
    struct Row {
        int raw;
        const char *name;
        const char *field; // nullptr: nothing
        const char *menu;
    };
    // `page` is the port's own (held for the list's paging): it never reaches the runtime.
    const std::vector<Row> rows = {
        {kRawA, "A", "slot2", "ok"},
        {kRawB, "B", "jump", "back"},
        {kRawX, "X", "slot1", "grade_revive"},
        {kRawY, "Y", "attack", "revive"},
        {kRawL, "L", "map", "minus"},
        {kRawR, "R", "cash_shop", "plus"},
        {kRawZL, "ZL", "slot3", nullptr},
        {kRawZR, "ZR", "slot4", "slot4"},
        {kRawMinus, "-", "menu", "map"},
        {kRawPlus, "+", nullptr, nullptr},
        {kRawUp, "up", "up", "up"},
        {kRawDown, "down", "down", "down"},
        {kRawLeft, "left", "left", "left"},
        {kRawRight, "right", "right", "right"},
    };
    for (const i32 panel : {0, 1}) {
        g_panel = panel;
        for (const Row &r : rows) {
            CAPTURE(panel);
            CAPTURE(r.name);
            const char *want = panel == 0 ? r.field : r.menu;
            const Lines sent = c.press(pad, r.raw);
            if (want == nullptr) {
                CHECK(sent.empty());
            } else {
                CHECK(sent == Lines{std::string("+") + want, std::string("-") + want});
            }
        }
    }
    // + opens the settings menu on the Switch: reserved, never bound; Guide everywhere.
    CHECK(fe::settings_button(SDL_GAMEPAD_BUTTON_START, fe::SettingsPlatform::kSwitch));
    CHECK_FALSE(fe::settings_button(SDL_GAMEPAD_BUTTON_START, fe::SettingsPlatform::kDesktop));
    CHECK(fe::settings_button(SDL_GAMEPAD_BUTTON_GUIDE, fe::SettingsPlatform::kSwitch));
    CHECK(fe::reserved_control(button(SDL_GAMEPAD_BUTTON_START), fe::SettingsPlatform::kSwitch));
    CHECK_FALSE(
        fe::reserved_control(button(SDL_GAMEPAD_BUTTON_START), fe::SettingsPlatform::kDesktop));
    for (const fe::Binding &b : fe::default_bindings(fe::SettingsPlatform::kSwitch)) {
        CHECK_FALSE(fe::reserved_control(b.control, fe::SettingsPlatform::kSwitch));
    }
    // The desktop keeps its own: by position, South confirms.
    CHECK(fe::face_buttons_default(fe::SettingsPlatform::kDesktop) == fe::FaceButtons::kPosition);
    CHECK(fe::controls_for(fe::default_bindings(fe::SettingsPlatform::kDesktop), fe::Context::kMenu,
                           MT_ACTION_OK)
              .front() == button(SDL_GAMEPAD_BUTTON_SOUTH));
}

TEST_CASE("switch input: the defaults survive the bindings file, and a remap of attack") {
    fe::BindingSet set{fe::default_bindings(fe::SettingsPlatform::kSwitch),
                       fe::face_buttons_default(fe::SettingsPlatform::kSwitch)};
    const std::string text = fe::serialise_bindings(set);
    CHECK(text.find("nintendo_buttons = label") != std::string::npos);
    CHECK(text.find("attack = button y") != std::string::npos);
    CHECK(text.find("jump = button b") != std::string::npos);
    // Read back over the desktop's defaults, as a fresh start reads the file.
    fe::BindingSet back;
    back.face = fe::FaceButtons::kPosition;
    const std::vector<std::string> problems = fe::parse_bindings(text, "bindings.ini", &back);
    CHECK(problems.empty());
    CHECK(back.face == fe::FaceButtons::kLabel);
    CHECK(fe::same_bindings(back.table, set.table));
    // The remap overlay's edit: attack on ZR instead of Y, kept by the file.
    fe::bind_control(&set.table, fe::Context::kField, MT_ACTION_ATTACK,
                     trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    fe::BindingSet again{fe::default_bindings(fe::SettingsPlatform::kSwitch),
                         fe::FaceButtons::kLabel};
    const std::vector<std::string> warnings =
        fe::parse_bindings(fe::serialise_bindings(set), "bindings.ini", &again);
    // ZR left quick slot 4, which says so (its keys stay).
    CHECK(warnings ==
          std::vector<std::string>{"bindings.ini: slot4 has no gamepad control in the field"});
    const std::vector<fe::Control> attack =
        fe::controls_for(again.table, fe::Context::kField, MT_ACTION_ATTACK);
    REQUIRE(!attack.empty());
    CHECK(std::find(attack.begin(), attack.end(), trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) !=
          attack.end());
    CHECK(std::find(attack.begin(), attack.end(), button(SDL_GAMEPAD_BUTTON_NORTH)) ==
          attack.end());
}

TEST_CASE("switch input: Nintendo's glyphs for every mode, by label") {
    const fe::FaceButtons label = fe::FaceButtons::kLabel;
    struct Row {
        SDL_GamepadType type;
        const char *zl, *zr, *l, *r, *plus, *minus;
    };
    for (const Row &r :
         {Row{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, "ZL", "ZR", "L", "R", "+", "-"},
          Row{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, "ZL", "ZR", "L", "R", "+", "-"},
          Row{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, "SL", "SR", "SL", "SR", "LS", "-"},
          Row{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT, "SL", "SR", "SL", "SR", "RS", "+"}}) {
        CAPTURE(SDL_GetGamepadStringForType(r.type));
        // Under label the controls are the letters by the Xbox layout's places: `a` is A.
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_SOUTH), r.type, label) == "A");
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_EAST), r.type, label) == "B");
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_WEST), r.type, label) == "X");
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_NORTH), r.type, label) == "Y");
        // By position (a player's choice) the bottom button is B.
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_SOUTH), r.type,
                                  fe::FaceButtons::kPosition) == "B");
        CHECK(fe::pad_glyph_label(trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), r.type, label) == r.zl);
        CHECK(fe::pad_glyph_label(trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), r.type, label) == r.zr);
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), r.type, label) == r.l);
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), r.type, label) == r.r);
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_START), r.type, label) == r.plus);
        CHECK(fe::pad_glyph_label(button(SDL_GAMEPAD_BUTTON_BACK), r.type, label) == r.minus);
    }
    // The menus' OK and back in the Switch's table show A and B (the prompts and the overlay).
    const std::vector<fe::Binding> table = fe::default_bindings(fe::SettingsPlatform::kSwitch);
    const auto first_pad = [&table](fe::Context context, u32 action) {
        for (const fe::Control &c : fe::controls_for(table, context, action)) {
            if (fe::device_of(c) == fe::Device::kGamepad) {
                return c;
            }
        }
        return fe::Control{};
    };
    const SDL_GamepadType pair = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    CHECK(fe::pad_glyph_label(first_pad(fe::Context::kMenu, MT_ACTION_OK), pair, label) == "A");
    CHECK(fe::pad_glyph_label(first_pad(fe::Context::kMenu, MT_ACTION_BACK), pair, label) == "B");
    CHECK(fe::pad_glyph_label(first_pad(fe::Context::kField, MT_ACTION_JUMP), pair, label) == "B");
    CHECK(fe::pad_glyph_label(first_pad(fe::Context::kField, MT_ACTION_ATTACK), pair, label) ==
          "Y");

    // The tracker shows the Nintendo set for the console's pad.
    Console c;
    SwitchPad pad;
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    g_reports[pad.id] = report_for(fe::SwitchPadMode::kProController);
    fe::GlyphTracker tracker;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        c.gamepads.handle(e);
        tracker.handle(e, c.gamepads.active_id());
    }
    CHECK(tracker.pad() == pad.id);
    CHECK(tracker.set() == fe::GlyphSet::kNintendo);
    CHECK(tracker.type() == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
}

TEST_CASE("switch input: player 1 is the first of the eight pads pressed; the others wait") {
    Console c;
    std::vector<std::unique_ptr<SwitchPad>> pads;
    for (int i = 0; i < 8; i++) { // the fork adds all eight at SDL_Init, connected or not
        pads.push_back(std::make_unique<SwitchPad>());
        REQUIRE_MESSAGE(pads.back()->ok(), pads.back()->error);
    }
    g_reports[pads[3]->id] = report_for(fe::SwitchPadMode::kProController);
    g_reports[pads[5]->id] = report_for(fe::SwitchPadMode::kJoyConLeft);
    c.pump();
    CHECK(c.gamepads.count() == 8);
    CHECK(c.gamepads.active_id() == 0);
    // The Pro Controller on No4 presses first: it plays, and its B jumps.
    pads[3]->raw(kRawB, true);
    c.pump();
    CHECK(c.gamepads.active_id() == pads[3]->id);
    CHECK(g_actions == Lines{"+jump"});
    // The lone Joy-Con presses while player 1 holds B: ignored.
    g_actions.clear();
    pads[5]->raw(kRawY, true);
    c.pump();
    CHECK(g_actions.empty());
    CHECK(c.gamepads.active_id() == pads[3]->id);
    pads[5]->raw(kRawY, false);
    pads[3]->raw(kRawB, false);
    c.pump();
    // With player 1's hands off, the Joy-Con takes over with a press.
    g_actions.clear();
    pads[5]->raw(kRawY, true);
    c.pump();
    CHECK(c.gamepads.active_id() == pads[5]->id);
    CHECK(g_actions == Lines{"+attack"});
}

TEST_CASE("switch input: the key pad starts hidden with a controller, a touch shows it") {
    CHECK(std::string(fe::platform_value("hide_keypad", fe::SettingsPlatform::kSwitch)) == "auto");
    CHECK(fe::platform_choice("hide_keypad", fe::kKeypadModeNames, fe::KeypadMode::kNever,
                              fe::SettingsPlatform::kSwitch) == fe::KeypadMode::kAuto);
    CHECK(fe::kKeypadAutoStartsHidden == (fe::kSettingsPlatform == fe::SettingsPlatform::kSwitch));
    fe::KeypadHider hider(fe::KeypadMode::kAuto, true);
    CHECK(hider.hidden());
    CHECK(hider.opacity(0) == 0.0f);
    hider.on_activity(fe::InputSource::kFinger);
    hider.turn(1.0, 1);
    CHECK_FALSE(hider.hidden());
    CHECK(hider.opacity(1.0 + fe::kKeypadFadeSeconds) == 1.0f);
    hider.on_activity(fe::InputSource::kGamepad);
    hider.turn(2.0, 0);
    CHECK(hider.hidden());
    // never stays shown, and the desktop's auto starts shown.
    CHECK_FALSE(fe::KeypadHider(fe::KeypadMode::kNever, true).hidden());
    CHECK_FALSE(fe::KeypadHider(fe::KeypadMode::kAuto, false).hidden());
}

TEST_CASE("switch input: rumble on the console's pad with the strength setting") {
    Console c;
    SwitchPad pad;
    REQUIRE_MESSAGE(pad.ok(), pad.error);
    g_reports[pad.id] = report_for(fe::SwitchPadMode::kHandheld);
    c.pump();
    // Rumble goes to player 1: none until a press.
    fe::Rumble rumble(&c.gamepads);
    rumble.configure(true, 50);
    rumble.request(fe::kRumbleDamage);
    rumble.turn();
    CHECK(rumble.stats().no_pad == 1);
    CHECK(pad.rumbles.empty());
    pad.raw(kRawA, true);
    c.pump();
    rumble.request(fe::kRumbleDamage);
    rumble.turn();
    SDL_UpdateJoysticks();
    CHECK(rumble.stats().sent == 1);
    REQUIRE(!pad.rumbles.empty());
    // Half the hit's amplitudes on both motors (the fork sends them to both sides).
    const auto expect = [](float amp) { return static_cast<Uint16>(amp * 0.5f * 65535.0f + 0.5f); };
    CHECK(std::abs(static_cast<int>(pad.rumbles.front().first) - expect(fe::kRumbleDamage.low)) <=
          1);
    CHECK(std::abs(static_cast<int>(pad.rumbles.front().second) - expect(fe::kRumbleDamage.high)) <=
          1);
    rumble.stop();
}
