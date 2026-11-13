// Controllers on the Nintendo Switch (#239, ADR 0140); see switch_pads.h.
#include "switch_pads.h"

#include <cstdio>
#include <cstring>

#include "glyphs.h"

#if defined(__SWITCH__)
#include <switch.h>

static_assert(maplethief::frontend::kNpadStyleFullKey == HidNpadStyleTag_NpadFullKey);
static_assert(maplethief::frontend::kNpadStyleHandheld == HidNpadStyleTag_NpadHandheld);
static_assert(maplethief::frontend::kNpadStyleJoyDual == HidNpadStyleTag_NpadJoyDual);
static_assert(maplethief::frontend::kNpadStyleJoyLeft == HidNpadStyleTag_NpadJoyLeft);
static_assert(maplethief::frontend::kNpadStyleJoyRight == HidNpadStyleTag_NpadJoyRight);
static_assert(maplethief::frontend::kNpadDeviceFullKey == HidDeviceTypeBits_FullKey);
static_assert(maplethief::frontend::kNpadDeviceHandheldLeft == HidDeviceTypeBits_HandheldLeft);
static_assert(maplethief::frontend::kNpadDeviceHandheldRight == HidDeviceTypeBits_HandheldRight);
static_assert(maplethief::frontend::kNpadDeviceJoyLeft == HidDeviceTypeBits_JoyLeft);
static_assert(maplethief::frontend::kNpadDeviceJoyRight == HidDeviceTypeBits_JoyRight);
#endif

namespace maplethief::frontend {

namespace {

SwitchPadSource g_source = nullptr;
void *g_source_user = nullptr;

// The SDL label of face button `b` as one letter or shape name.
const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:
        return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B:
        return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X:
        return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
        return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
        return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        return "Triangle";
    default:
        return "?";
    }
}

} // namespace

const char *switch_pad_mode_name(SwitchPadMode mode) {
    switch (mode) {
    case SwitchPadMode::kHandheld:
        return "handheld";
    case SwitchPadMode::kJoyConPair:
        return "Joy-Con pair";
    case SwitchPadMode::kProController:
        return "Pro Controller";
    case SwitchPadMode::kJoyConLeft:
        return "left Joy-Con";
    case SwitchPadMode::kJoyConRight:
        return "right Joy-Con";
    default:
        return "none";
    }
}

SwitchPadMode switch_pad_mode(const SwitchPadReport &r) {
    if (r.style == 0) {
        // Nothing on No1: the first pad is the handheld Joy-Cons if they are attached.
        return (r.handheld_style & kNpadStyleHandheld) != 0 ? SwitchPadMode::kHandheld
                                                            : SwitchPadMode::kNone;
    }
    if ((r.style & kNpadStyleJoyDual) != 0) {
        return SwitchPadMode::kJoyConPair;
    }
    // SDL's own test for its single Joy-Con mappings: the device type, without JoyDual.
    if ((r.device & kNpadDeviceJoyLeft) != 0) {
        return SwitchPadMode::kJoyConLeft;
    }
    if ((r.device & kNpadDeviceJoyRight) != 0) {
        return SwitchPadMode::kJoyConRight;
    }
    if ((r.style & kNpadStyleFullKey) != 0) {
        return SwitchPadMode::kProController;
    }
    if ((r.style & kNpadStyleHandheld) != 0) {
        return SwitchPadMode::kHandheld;
    }
    if ((r.style & kNpadStyleJoyLeft) != 0) {
        return SwitchPadMode::kJoyConLeft;
    }
    if ((r.style & kNpadStyleJoyRight) != 0) {
        return SwitchPadMode::kJoyConRight;
    }
    return SwitchPadMode::kNone;
}

SDL_GamepadType switch_pad_type(SwitchPadMode mode) {
    switch (mode) {
    case SwitchPadMode::kHandheld:
    case SwitchPadMode::kJoyConPair:
        return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    case SwitchPadMode::kJoyConLeft:
        return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
    case SwitchPadMode::kJoyConRight:
        return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
    default:
        return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    }
}

bool is_switch_console_pad(const char *name, SDL_GamepadType reported) {
    return name != nullptr && std::strcmp(name, kSwitchPadName) == 0 &&
           (reported == SDL_GAMEPAD_TYPE_STANDARD || reported == SDL_GAMEPAD_TYPE_UNKNOWN);
}

void set_switch_pad_source(SwitchPadSource source, void *user) {
    g_source = source;
    g_source_user = source != nullptr ? user : nullptr;
}

bool switch_pads_live() {
    return g_source != nullptr || kSettingsPlatform == SettingsPlatform::kSwitch;
}

bool switch_pad_report(SDL_JoystickID id, SwitchPadReport *out) {
    *out = SwitchPadReport{};
    if (g_source != nullptr) {
        return g_source(g_source_user, id, out);
    }
#if defined(__SWITCH__)
    if (id < 1 || id > 8) {
        return false;
    }
    const auto npad = static_cast<HidNpadIdType>(HidNpadIdType_No1 + (id - 1));
    out->style = hidGetNpadStyleSet(npad);
    out->device = hidGetNpadDeviceType(npad);
    if (id == 1) {
        out->handheld_style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    }
    return true;
#else
    return false;
#endif
}

SDL_GamepadType port_pad_type(SDL_GamepadType reported, const char *name, SwitchPadMode mode,
                              bool live) {
    return live && is_switch_console_pad(name, reported) ? switch_pad_type(mode) : reported;
}

SDL_GamepadType pad_type(SDL_JoystickID id) {
    const SDL_GamepadType reported = SDL_GetGamepadTypeForID(id);
    if (!switch_pads_live() || !is_switch_console_pad(SDL_GetGamepadNameForID(id), reported)) {
        return reported;
    }
    SwitchPadReport r;
    const SwitchPadMode mode =
        switch_pad_report(id, &r) ? switch_pad_mode(r) : SwitchPadMode::kNone;
    return switch_pad_type(mode);
}

std::string pad_report_line(SDL_JoystickID id) {
    const char *name = SDL_GetGamepadNameForID(id);
    const SDL_GamepadType reported = SDL_GetGamepadTypeForID(id);
    const SDL_GamepadType real = SDL_GetRealGamepadTypeForID(id);
    const auto label = [reported](SDL_GamepadButton b) {
        return label_name(SDL_GetGamepadButtonLabelForType(reported, b));
    };
    SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
    const bool rumble =
        pad != nullptr && SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                                 SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    char line[320];
    int n = std::snprintf(
        line, sizeof line,
        "pad %u: '%s', SDL type %s (real %s), SDL labels S=%s E=%s W=%s N=%s, rumble %s",
        static_cast<unsigned>(id), name != nullptr ? name : "?",
        SDL_GetGamepadStringForType(reported) != nullptr ? SDL_GetGamepadStringForType(reported)
                                                         : "unknown",
        SDL_GetGamepadStringForType(real) != nullptr ? SDL_GetGamepadStringForType(real)
                                                     : "unknown",
        label(SDL_GAMEPAD_BUTTON_SOUTH), label(SDL_GAMEPAD_BUTTON_EAST),
        label(SDL_GAMEPAD_BUTTON_WEST), label(SDL_GAMEPAD_BUTTON_NORTH), rumble ? "yes" : "no");
    if (switch_pads_live() && is_switch_console_pad(name, reported) && n > 0 &&
        static_cast<std::size_t>(n) < sizeof line) {
        SwitchPadReport r;
        const bool have = switch_pad_report(id, &r);
        const SwitchPadMode mode = have ? switch_pad_mode(r) : SwitchPadMode::kNone;
        std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n),
                      "; libnx style 0x%X device 0x%X handheld 0x%X: %s",
                      static_cast<unsigned>(r.style), static_cast<unsigned>(r.device),
                      static_cast<unsigned>(r.handheld_style), switch_pad_mode_name(mode));
    }
    const SDL_GamepadType shown = pad_type(id);
    const char *shown_name = SDL_GetGamepadStringForType(shown);
    return std::string(line) + ", shown as " + glyph_set_name(glyph_set_for(shown)) + " (" +
           (shown_name != nullptr ? shown_name : "unknown") + ")";
}

} // namespace maplethief::frontend
