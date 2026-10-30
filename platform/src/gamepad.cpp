// Gamepads (#67, ADR 0048); see gamepad.h.
#include "gamepad.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace maplethief::frontend {

namespace {

constexpr float kPi = 3.14159265358979f;

// SDL's axis value as -1..1 (the triggers 0..1).
float axis_value(Sint16 v) {
    return v < 0 ? static_cast<float>(v) / 32768.0f : static_cast<float>(v) / 32767.0f;
}

// The smallest angle between a and b, in degrees.
float angle_between(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

Control direction(Direction d) { return Control{ControlKind::kDirection, static_cast<int>(d)}; }

bool is_dpad(int button) {
    return button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
           button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}

bool contains(const std::vector<Control> &v, const Control &c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

Directions stick_directions(float x, float y, const Directions &held, const StickConfig &config) {
    const bool engaged = held[0] || held[1] || held[2] || held[3];
    const float r = std::sqrt(x * x + y * y);
    if (!std::isfinite(r) || r < (engaged ? config.release : config.deadzone)) {
        return Directions{};
    }
    // Degrees, counter-clockwise from right, up positive (SDL's y points down).
    const float a = std::atan2(-y, x) * 180.0f / kPi;
    // Up, down, left, right: their axes.
    static constexpr float k_axis[4] = {90.0f, -90.0f, 180.0f, 0.0f};
    // Each direction covers its own 45-degree sector and the two diagonals beside it.
    constexpr float k_half = 67.5f;
    Directions out{};
    for (int i = 0; i < 4; i++) {
        const float half =
            held[i] ? k_half + config.hysteresis_degrees : k_half - config.hysteresis_degrees;
        out[i] = angle_between(a, k_axis[i]) < half;
    }
    return out;
}

const char *face_buttons_name(FaceButtons f) {
    return f == FaceButtons::kLabel ? "label" : "position";
}

bool parse_face_buttons(std::string_view text, FaceButtons *out) {
    const auto is = [&](const char *name) {
        return text.size() == std::char_traits<char>::length(name) &&
               std::equal(text.begin(), text.end(), name, [](char a, char b) {
                   return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
               });
    };
    if (is("position")) {
        *out = FaceButtons::kPosition;
    } else if (is("label")) {
        *out = FaceButtons::kLabel;
    } else {
        return false;
    }
    return true;
}

Control gamepad_button_control(SDL_GamepadType type, SDL_GamepadButton b, FaceButtons face) {
    if (face == FaceButtons::kLabel && b >= SDL_GAMEPAD_BUTTON_SOUTH &&
        b <= SDL_GAMEPAD_BUTTON_NORTH) {
        // The Xbox layout's positions of the four letters.
        switch (SDL_GetGamepadButtonLabelForType(type, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:
            return Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_SOUTH};
        case SDL_GAMEPAD_BUTTON_LABEL_B:
            return Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_EAST};
        case SDL_GAMEPAD_BUTTON_LABEL_X:
            return Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_WEST};
        case SDL_GAMEPAD_BUTTON_LABEL_Y:
            return Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_NORTH};
        default:
            break; // PlayStation's shapes, or unknown: by position
        }
    }
    return Control{ControlKind::kGamepadButton, static_cast<int>(b)};
}

Gamepads::Gamepads(Actions *actions, Input *input) : actions_(actions), input_(input) {}

void Gamepads::set_face_buttons(FaceButtons face) {
    if (face != face_) {
        release_all();
        face_ = face;
    }
}

Gamepads::~Gamepads() {
    for (Pad &p : pads_) {
        SDL_CloseGamepad(p.pad);
    }
}

SDL_Gamepad *Gamepads::active() const {
    for (const Pad &p : pads_) {
        if (p.id == active_) {
            return p.pad;
        }
    }
    return nullptr;
}

Gamepads::Pad *Gamepads::find(SDL_JoystickID id) {
    for (Pad &p : pads_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

void Gamepads::open_connected() {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids != nullptr && i < n; i++) {
        add(ids[i]);
    }
    SDL_free(ids);
}

void Gamepads::add(SDL_JoystickID id) {
    if (find(id) != nullptr) {
        return;
    }
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (pad == nullptr) {
        SDL_Log("input: gamepad %u cannot be opened: %s", static_cast<unsigned>(id),
                SDL_GetError());
        return;
    }
    Pad p;
    p.id = id;
    p.pad = pad;
    p.type = SDL_GetGamepadType(pad);
    // What it holds already (a pad plugged in with a button down): the raw state, not pressed
    // until it changes.
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        p.buttons[static_cast<std::size_t>(b)] =
            SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
    }
    p.last = held(p);
    pads_.push_back(p);
    const char *name = SDL_GetGamepadName(pad);
    SDL_Log("input: gamepad %u connected: %s (%s)", static_cast<unsigned>(id),
            name != nullptr ? name : "?", SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)));
}

void Gamepads::remove(SDL_JoystickID id) {
    Pad *p = find(id);
    if (p == nullptr) {
        return;
    }
    lift(*p);
    if (active_ == id) {
        active_ = 0;
    }
    SDL_CloseGamepad(p->pad);
    pads_.erase(pads_.begin() + (p - pads_.data()));
    SDL_Log("input: gamepad %u disconnected", static_cast<unsigned>(id));
}

std::vector<Control> Gamepads::held(const Pad &p) const {
    std::vector<Control> out;
    const Directions dpad = {
        p.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP], p.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN],
        p.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT], p.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]};
    for (int d = 0; d < 4; d++) {
        if (dpad[static_cast<std::size_t>(d)] || p.stick[static_cast<std::size_t>(d)]) {
            out.push_back(direction(static_cast<Direction>(d)));
        }
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (!is_dpad(b) && p.buttons[static_cast<std::size_t>(b)]) {
            out.push_back(gamepad_button_control(p.type, static_cast<SDL_GamepadButton>(b), face_));
        }
    }
    if (p.triggers[0]) {
        out.push_back(Control{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER});
    }
    if (p.triggers[1]) {
        out.push_back(Control{ControlKind::kGamepadTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER});
    }
    return out;
}

void Gamepads::lift(Pad &p) {
    for (const Control &c : p.emitted) {
        actions_->control(InputSource::kGamepad, c, false);
    }
    p.emitted.clear();
}

std::size_t Gamepads::sync(Pad &p) {
    const std::vector<Control> now = held(p);
    if (active_ != p.id) {
        // First active wins: another pad takes over only with a press while the active one
        // holds nothing.
        const Pad *a = find(active_);
        const bool busy = a != nullptr && !a->emitted.empty();
        bool pressed = false;
        for (const Control &c : now) {
            pressed = pressed || !contains(p.last, c);
        }
        p.last = now;
        if (busy || !pressed) {
            return 0;
        }
        if (active_ != 0) {
            SDL_Log("input: gamepad %u is the active one now", static_cast<unsigned>(p.id));
        }
        active_ = p.id;
    }
    // Releases first, then presses, each in table order.
    std::vector<Control> kept;
    for (const Control &c : p.emitted) {
        if (contains(now, c)) {
            kept.push_back(c);
        } else {
            actions_->control(InputSource::kGamepad, c, false);
        }
    }
    std::size_t presses = 0;
    for (const Control &c : now) {
        if (!contains(kept, c)) {
            kept.push_back(c);
            actions_->control(InputSource::kGamepad, c, true);
            presses++;
        }
    }
    p.emitted = kept;
    p.last = now;
    return presses;
}

void Gamepads::release_all() {
    for (Pad &p : pads_) {
        lift(p);
        // Held until let go and pressed again.
        p.buttons.fill(false);
        p.stick = Directions{};
        p.triggers = {false, false};
        p.last.clear();
    }
}

bool Gamepads::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        add(e.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove(e.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        Pad *p = find(e.gbutton.which);
        if (p == nullptr || e.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) {
            return false;
        }
        if (!SDL_GamepadConnected(p->pad)) {
            return true; // SDL recentres a pad that is going away: its REMOVED lifts it
        }
        p->buttons[e.gbutton.button] = e.gbutton.down;
        sync(*p); // Input::handle reports the press to the activity listener
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        Pad *p = find(e.gaxis.which);
        if (p == nullptr || e.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT) {
            return false;
        }
        if (!SDL_GamepadConnected(p->pad)) {
            return true;
        }
        p->axes[e.gaxis.axis] = axis_value(e.gaxis.value);
        p->stick = stick_directions(p->axes[SDL_GAMEPAD_AXIS_LEFTX],
                                    p->axes[SDL_GAMEPAD_AXIS_LEFTY], p->stick, stick_);
        for (int t = 0; t < 2; t++) {
            const float v =
                p->axes[t == 0 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
            bool &on = p->triggers[static_cast<std::size_t>(t)];
            on = on ? v >= stick_.trigger_release : v >= stick_.trigger_press;
        }
        if (sync(*p) != 0 && input_ != nullptr) {
            input_->note(InputSource::kGamepad);
        }
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
        release_all();
        return false;
    default:
        return false;
    }
}

} // namespace maplethief::frontend
