// The disconnect pause (#184); see disconnect.h.
#include "disconnect.h"

#include <string>

#include "canvas.h"
#include "glyphs.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBackdrop = {0x00, 0x00, 0x00, kOverlayBackdropAlpha};
constexpr Rgba kPanel = {0x14, 0x1A, 0x24, 235};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kTitle = {0xFF, 0xD1, 0x66, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};

// The box, in screen points (480x320).
constexpr int kBoxW = 300, kBoxH = 112;

bool is_input(Uint32 type) {
    switch (type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default:
        return false;
    }
}

} // namespace

void DisconnectPause::set_enabled(bool on) {
    if (on == enabled_) {
        return;
    }
    enabled_ = on;
    if (!on && open_) {
        close("pause_on_disconnect is off");
    }
}

void DisconnectPause::open(const SDL_Event &e) {
    const SDL_JoystickID id = e.gdevice.which;
    SDL_Gamepad *pad = gamepads_ != nullptr ? gamepads_->active() : nullptr;
    lost_type_ = pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GetGamepadTypeForID(id);
    const char *name = pad != nullptr ? SDL_GetGamepadName(pad) : nullptr;
    lost_name_ = name != nullptr ? name : "";
    // Everything held goes, as when the remap overlay opens: the hero stops.
    if (gamepads_ != nullptr) {
        gamepads_->release_all();
    }
    if (actions_ != nullptr) {
        actions_->release_source(InputSource::kKeyboard);
        actions_->release_source(InputSource::kGamepad);
    }
    open_ = true;
    dirty_ = true;
    swallow_type_ = 0;
    opens_++;
    SDL_Log("input: gamepad %u in use disconnected: the game pauses until a pad connects or a key "
            "is pressed",
            static_cast<unsigned>(id));
}

void DisconnectPause::close(const char *why) {
    if (!open_) {
        return;
    }
    open_ = false;
    dirty_ = true;
    closes_++;
    SDL_Log("input: the disconnect pause closes (%s): the game goes on", why);
}

void DisconnectPause::swallow_release_of(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        swallow_type_ = SDL_EVENT_KEY_UP;
        swallow_what_ = e.key.key;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        swallow_type_ = SDL_EVENT_MOUSE_BUTTON_UP;
        swallow_what_ = e.button.button;
        break;
    case SDL_EVENT_FINGER_DOWN:
        swallow_type_ = SDL_EVENT_FINGER_UP;
        swallow_what_ = e.tfinger.fingerID;
        break;
    default:
        // A pad's button: Gamepads never saw it go down, so its up changes nothing.
        swallow_type_ = 0;
        break;
    }
}

bool DisconnectPause::is_swallowed_release(const SDL_Event &e) {
    if (swallow_type_ == 0 || e.type != swallow_type_) {
        return false;
    }
    Uint64 what = 0;
    switch (e.type) {
    case SDL_EVENT_KEY_UP:
        what = e.key.key;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        what = e.button.button;
        break;
    case SDL_EVENT_FINGER_UP:
        what = e.tfinger.fingerID;
        break;
    default:
        return false;
    }
    if (what != swallow_what_) {
        return false;
    }
    swallow_type_ = 0;
    return true;
}

bool DisconnectPause::handle(const SDL_Event &e) {
    if (!open_) {
        if (is_swallowed_release(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && enabled_ && gamepads_ != nullptr &&
            e.gdevice.which != 0 && e.gdevice.which == gamepads_->active_id() &&
            gamepads_->count() == 1) {
            open(e); // Gamepads still closes the pad (not taken)
        }
        return false;
    }
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        close("a gamepad connected");
        return false; // Gamepads opens it
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat) {
            return true;
        }
        close(e.type == SDL_EVENT_KEY_DOWN            ? "a key"
              : e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "a click"
              : e.type == SDL_EVENT_FINGER_DOWN       ? "a tap"
                                                      : "a press on another gamepad");
        swallow_release_of(e);
        return true;
    default:
        return is_input(e.type);
    }
}

void DisconnectPause::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                           int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    c.fill(0, 0, c.width(), c.height(), kBackdrop);
    const int x = (c.width() - kBoxW) / 2;
    const int y = (c.height() - kBoxH) / 2;
    c.fill(x, y, kBoxW, kBoxH, kPanel);
    c.frame(x, y, kBoxW, kBoxH, 1, kBorder);
    const auto centred = [&](int ty, const std::string &s, Rgba colour, int size) {
        c.text(x + (kBoxW - Canvas::text_width(s, size)) / 2, ty, s, colour, size);
    };
    centred(y + 12, "CONTROLLER DISCONNECTED", kTitle, 2);
    const std::string pad = std::string(glyph_set_name(glyph_set_for(lost_type_))) + " pad" +
                            (lost_name_.empty() ? std::string() : ": " + lost_name_);
    centred(y + 36, pad.size() > 46 ? pad.substr(0, 45) + "~" : pad, kDim, 1);
    centred(y + 54, "Reconnect it, or press any key.", kText, 1);
    // The lost pad's glyphs: its face buttons and Start, which close it from another pad too.
    const std::string label = "Or a button on another pad:";
    const Control buttons[] = {
        {ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_SOUTH},
        {ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_EAST},
        {ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_START},
    };
    int w = Canvas::text_width(label) + 4;
    for (const Control &b : buttons) {
        w += glyph_width(b, lost_type_, FaceButtons::kPosition) + 3;
    }
    int gx = x + (kBoxW - w) / 2;
    const int gy = y + 78;
    gx += c.text(gx, gy + (kButtonHeight - Canvas::kGlyphHeight) / 2, label, kDim) + 4;
    for (const Control &b : buttons) {
        gx += draw_glyph(c, gx, gy, b, lost_type_, FaceButtons::kPosition) + 3;
    }
}

} // namespace maplethief::frontend
