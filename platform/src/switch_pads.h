// Controllers on the Nintendo Switch (#239, ADR 0140): what SDL reports for them and the type the
// port treats them as.
//
// The pinned SDL (nx-mod/sdl3-nx 07ea26a3, src/joystick/switch/SDL_sysjoystick.c, ADR 0138) adds
// eight joysticks at SDL_Init, instance ids 1 to 8 for libnx's npads No1 to No8 (the first also
// reads the handheld Joy-Cons), whether anything is connected or not, and never adds or removes
// one later. All are named "Switch Controller" with a GUID made from that name (no vendor or
// product id), so SDL_GetGamepadType says Standard and SDL's labels are the Xbox layout's. Its
// mapping is by position: South is the bottom face button (B on a Joy-Con pair), East A, West Y,
// North X; ZL and ZR are the triggers (as buttons), `-` back, `+` start, no guide (HOME is the
// system's). A Joy-Con alone, held on its side, has its own mapping: its four buttons by their
// position on the side, the stick as the d-pad, SL and SR as the triggers, the stick's click as
// start and its `-` (left) or `+` (right) as back; it has no shoulders, no stick axes.
//
// So the port does not trust SDL's type for these pads: pad_type() gives a Nintendo type by the
// controller's mode, read from libnx (the npad's style set and device type). That type picks the
// Nintendo glyphs (glyphs.h) and makes FaceButtons::kLabel (the Switch's default,
// kFaceButtonsDefault in gamepad.h) read A as A.
//
//   mode            libnx                                  type (SDL_GamepadType)
//   handheld        style Handheld (npad Handheld)         NINTENDO_SWITCH_JOYCON_PAIR
//   Joy-Con pair    style JoyDual                          NINTENDO_SWITCH_JOYCON_PAIR
//   Pro Controller  style FullKey                          NINTENDO_SWITCH_PRO
//   left Joy-Con    device JoyLeft without JoyDual         NINTENDO_SWITCH_JOYCON_LEFT
//   right Joy-Con   device JoyRight without JoyDual        NINTENDO_SWITCH_JOYCON_RIGHT
//   none            nothing connected to that npad         NINTENDO_SWITCH_PRO
//
// The single Joy-Con's choice is SDL's own (the left or right mapping when not JoyDual and the
// device type says that Joy-Con), so the mode always matches the mapping SDL uses.
//
// Off the console (the tests), set_switch_pad_source() gives synthetic libnx reports; with a
// source set, a "Switch Controller" is treated as on the console.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_SWITCH_PADS_H
#define MAPLETHIEF_PLATFORM_SWITCH_PADS_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "settings.h"

namespace maplethief::frontend {

// The name the Switch's SDL gives every controller.
inline constexpr const char *kSwitchPadName = "Switch Controller";

// libnx's bits (switch/services/hid.h: HidNpadStyleTag, HidDeviceTypeBits), mirrored so that this
// builds and is tested everywhere; switch_pads.cpp checks them against libnx on the Switch.
inline constexpr std::uint32_t kNpadStyleFullKey = 1u << 0;
inline constexpr std::uint32_t kNpadStyleHandheld = 1u << 1;
inline constexpr std::uint32_t kNpadStyleJoyDual = 1u << 2;
inline constexpr std::uint32_t kNpadStyleJoyLeft = 1u << 3;
inline constexpr std::uint32_t kNpadStyleJoyRight = 1u << 4;
inline constexpr std::uint32_t kNpadDeviceFullKey = 1u << 0;
inline constexpr std::uint32_t kNpadDeviceHandheldLeft = 1u << 2;
inline constexpr std::uint32_t kNpadDeviceHandheldRight = 1u << 3;
inline constexpr std::uint32_t kNpadDeviceJoyLeft = 1u << 4;
inline constexpr std::uint32_t kNpadDeviceJoyRight = 1u << 5;

// What libnx says of one npad: hidGetNpadStyleSet and hidGetNpadDeviceType of No<n>, and for the
// first pad also the style set of the handheld npad (SDL reads both for it).
struct SwitchPadReport {
    std::uint32_t style = 0;
    std::uint32_t device = 0;
    std::uint32_t handheld_style = 0;
};

enum class SwitchPadMode {
    kNone,
    kHandheld,
    kJoyConPair,
    kProController,
    kJoyConLeft,
    kJoyConRight
};
// "none", "handheld", "Joy-Con pair", "Pro Controller", "left Joy-Con", "right Joy-Con".
const char *switch_pad_mode_name(SwitchPadMode mode);
// The mode a report shows (the table above).
SwitchPadMode switch_pad_mode(const SwitchPadReport &report);
// The type the port treats a pad in `mode` as (the table above).
SDL_GamepadType switch_pad_type(SwitchPadMode mode);

// Whether a pad SDL names `name` and types `reported` is the Switch's own controller.
bool is_switch_console_pad(const char *name, SDL_GamepadType reported);

// Synthetic reports for tests: `source` fills `out` for pad `id` and returns true (false: no
// report, mode none). nullptr restores the default (libnx on the Switch, nothing elsewhere).
using SwitchPadSource = bool (*)(void *user, SDL_JoystickID id, SwitchPadReport *out);
void set_switch_pad_source(SwitchPadSource source, void *user);
// The report for pad `id` (SDL's instance id 1..8 is npad No1..No8). False without one.
bool switch_pad_report(SDL_JoystickID id, SwitchPadReport *out);
// Whether pads named kSwitchPadName are the console's: on the Switch, or with a source set.
bool switch_pads_live();

// The type the port treats pad `id` as: SDL's (SDL_GetGamepadTypeForID), except that a Switch
// Controller is Nintendo's by its mode (switch_pad_type) where switch_pads_live().
SDL_GamepadType pad_type(SDL_JoystickID id);
// The pure rule: `reported` unless `name` is the console's pad and `live`.
SDL_GamepadType port_pad_type(SDL_GamepadType reported, const char *name, SwitchPadMode mode,
                              bool live);

// One line for the log of what SDL reports for pad `id` and what the port makes of it:
// "pad 1: 'Switch Controller', SDL type standard, SDL labels S=A E=B W=X N=Y, rumble yes; libnx
// style 0x2 device 0x0 handheld 0x2: handheld, shown as Nintendo (switch_joycon_pair)".
std::string pad_report_line(SDL_JoystickID id);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SWITCH_PADS_H
