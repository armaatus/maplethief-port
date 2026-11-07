// A gamepad without hardware (#67): an SDL3 virtual joystick that SDL sees as an Xbox 360, a
// PlayStation 4, a PlayStation 5 or (#70) a Nintendo Switch Pro controller (by its USB vendor
// and product id), with all 15
// buttons and 6 axes of SDL's gamepad. Its button i and axis i are SDL_GamepadButton i and
// SDL_GamepadAxis i: SDL's virtual driver maps them so, unless `mapping` is set, in which case
// the raw buttons are numbered backwards and an SDL_AddGamepadMapping line maps them, so that
// a correct button proves the mapping is used. Setting a button or an axis takes effect at the
// next SDL_UpdateJoysticks (SDL_PumpEvents), which sends the gamepad events.
// With `rumble` (#72) the pad has rumble motors: every call SDL's virtual driver makes to them
// (SDL_RumbleGamepad, and SDL's own stop when a rumble's time is up) is recorded in `rumbles`.
// Without it SDL_RumbleGamepad fails, as on a pad without motors.
// Needs SDL_INIT_GAMEPAD.
#ifndef MAPLETHIEF_TESTS_VIRTUAL_PAD_H
#define MAPLETHIEF_TESTS_VIRTUAL_PAD_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace virtual_pad {

enum class Kind { kXbox360, kPs4, kPs5, kSwitchPro };

inline const char *kind_name(Kind k) {
    switch (k) {
    case Kind::kXbox360:
        return "Xbox 360";
    case Kind::kPs4:
        return "PS4";
    case Kind::kPs5:
        return "PS5";
    case Kind::kSwitchPro:
        return "Switch Pro";
    }
    return "?";
}

inline SDL_GamepadType expected_type(Kind k) {
    switch (k) {
    case Kind::kXbox360:
        return SDL_GAMEPAD_TYPE_XBOX360;
    case Kind::kPs4:
        return SDL_GAMEPAD_TYPE_PS4;
    case Kind::kPs5:
        return SDL_GAMEPAD_TYPE_PS5;
    case Kind::kSwitchPro:
        return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    }
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}

constexpr int kButtons = 15; // SOUTH .. DPAD_RIGHT
// With trackpads: SOUTH .. MISC2, the touchpad's click (the left trackpad's on a Steam Deck)
// and misc2 (its right trackpad's) among them.
constexpr int kTrackpadButtons = SDL_GAMEPAD_BUTTON_MISC2 + 1;
constexpr int kAxes = 6; // LEFTX .. RIGHT_TRIGGER

// One call to the motors: SDL's 0..0xFFFF amplitudes.
struct RumbleCall {
    Uint16 low = 0;
    Uint16 high = 0;
};

struct Pad {
    SDL_JoystickID id = 0;
    SDL_Joystick *joystick = nullptr;
    bool mapped = false;
    std::string error;
    std::vector<RumbleCall> rumbles; // with `rumble`: every call to the motors, in order
    // Told of every button and axis set after the constructor (axis: false for a button, the
    // SDL_GamepadButton or SDL_GamepadAxis, 1/0 down/up or the raw axis): the regression run's
    // recorder (#91, ADR 0104).
    std::function<void(bool axis, int index, int value)> on_change;

    // `trackpads`: that many touchpads (a Steam Deck's or Steam Controller's two, as SDL
    // reports them), with buttons up to misc2; not with `mapping`.
    explicit Pad(Kind kind, bool mapping = false, bool rumble = false, int trackpads = 0)
        : mapped(mapping) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        if (rumble) {
            desc.userdata = this;
            desc.Rumble = [](void *user, Uint16 low, Uint16 high) {
                static_cast<Pad *>(user)->rumbles.push_back(RumbleCall{low, high});
                return true;
            };
        }
        switch (kind) {
        case Kind::kXbox360:
            desc.vendor_id = 0x045E; // Microsoft
            desc.product_id = 0x028E;
            break;
        case Kind::kPs4:
            desc.vendor_id = 0x054C; // Sony
            desc.product_id = 0x09CC;
            break;
        case Kind::kPs5:
            desc.vendor_id = 0x054C;
            desc.product_id = 0x0CE6;
            break;
        case Kind::kSwitchPro:
            desc.vendor_id = 0x057E; // Nintendo
            desc.product_id = 0x2009;
            break;
        }
        desc.naxes = kAxes;
        desc.nbuttons = trackpads > 0 ? kTrackpadButtons : kButtons;
        desc.button_mask = (1u << desc.nbuttons) - 1u;
        SDL_VirtualJoystickTouchpadDesc pads[2] = {{2, {}}, {2, {}}};
        if (trackpads > 0) {
            desc.ntouchpads = static_cast<Uint16>(trackpads < 2 ? trackpads : 2);
            desc.touchpads = pads;
        }
        desc.axis_mask = (1u << kAxes) - 1u;
        desc.name = kind_name(kind);
        id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            error = SDL_GetError();
            return;
        }
        if (mapping) {
            // Button i of SDL's gamepad is raw button (14 - i).
            static const char *const k_names[kButtons] = {"a",
                                                          "b",
                                                          "x",
                                                          "y",
                                                          "back",
                                                          "guide",
                                                          "start",
                                                          "leftstick",
                                                          "rightstick",
                                                          "leftshoulder",
                                                          "rightshoulder",
                                                          "dpup",
                                                          "dpdown",
                                                          "dpleft",
                                                          "dpright"};
            char guid[64];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
            std::string line = std::string(guid) + "," + kind_name(kind) + " (mapped)";
            for (int i = 0; i < kButtons; i++) {
                line += std::string(",") + k_names[i] + ":b" + std::to_string(kButtons - 1 - i);
            }
            line += ",leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5";
            if (SDL_AddGamepadMapping(line.c_str()) < 0) {
                error = SDL_GetError();
            }
        }
        joystick = SDL_OpenJoystick(id);
        if (joystick == nullptr) {
            error = SDL_GetError();
            return;
        }
        // A virtual axis starts at raw 0, which is a half-pulled trigger: released, as a real
        // pad's triggers rest.
        trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f);
        trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f);
    }
    ~Pad() { detach(); }
    Pad(const Pad &) = delete;
    Pad &operator=(const Pad &) = delete;

    bool ok() const { return id != 0 && joystick != nullptr && error.empty(); }

    void detach() {
        if (joystick != nullptr) {
            SDL_CloseJoystick(joystick);
            joystick = nullptr;
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            id = 0;
        }
    }

    // SDL_GamepadButton `b` down or up.
    bool button(SDL_GamepadButton b, bool down) {
        const int raw = mapped ? kButtons - 1 - static_cast<int>(b) : static_cast<int>(b);
        if (on_change) {
            on_change(false, static_cast<int>(b), down ? 1 : 0);
        }
        return SDL_SetJoystickVirtualButton(joystick, raw, down);
    }
    // Finger `finger` of touchpad `pad` down at (x, y) (0..1 from its top-left), or up.
    bool touch(int pad, int finger, bool down, float x, float y) {
        return SDL_SetJoystickVirtualTouchpad(joystick, pad, finger, down, x, y,
                                              down ? 1.0f : 0.0f);
    }
    // A stick's SDL_GamepadAxis `a` to `value` (-32768..32767).
    bool axis(SDL_GamepadAxis a, Sint16 value) {
        if (on_change) {
            on_change(true, static_cast<int>(a), value);
        }
        return SDL_SetJoystickVirtualAxis(joystick, static_cast<int>(a), value);
    }
    // A trigger to `amount` (0 released .. 1 fully pressed): its raw axis runs over the whole
    // range, which SDL's gamepad turns into 0..32767.
    bool trigger(SDL_GamepadAxis a, float amount) {
        const float raw = -32768.0f + amount * 65535.0f;
        return axis(a, static_cast<Sint16>(raw < -32768.0f  ? -32768.0f
                                           : raw > 32767.0f ? 32767.0f
                                                            : raw));
    }
};

} // namespace virtual_pad

#endif // MAPLETHIEF_TESTS_VIRTUAL_PAD_H
