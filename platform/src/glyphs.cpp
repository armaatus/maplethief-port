// Button glyphs (#70, ADR 0054); see glyphs.h.
#include "glyphs.h"

#include <algorithm>
#include <cstdlib>

namespace maplethief::frontend {

namespace {

constexpr Rgba kDisc = {0x2B, 0x2F, 0x38, 255};
constexpr Rgba kRim = {0xC8, 0xCC, 0xD4, 255};
constexpr Rgba kInk = {0xFF, 0xFF, 0xFF, 255};
constexpr Rgba kPill = {0x3A, 0x40, 0x4C, 255};
constexpr Rgba kPlus = {0x7A, 0x84, 0x96, 255};
constexpr Rgba kCap = {0xE4, 0xE6, 0xEA, 255};
constexpr Rgba kCapShade = {0x9A, 0x9E, 0xA8, 255};
constexpr Rgba kCapInk = {0x20, 0x24, 0x2C, 255};

bool is_face(const Control &c) {
    return c.kind == ControlKind::kGamepadButton && c.code >= SDL_GAMEPAD_BUTTON_SOUTH &&
           c.code <= SDL_GAMEPAD_BUTTON_NORTH;
}

// The four Xbox letters by position: South A, East B, West X, North Y.
const char *xbox_letter(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return "A";
    case SDL_GAMEPAD_BUTTON_EAST:
        return "B";
    case SDL_GAMEPAD_BUTTON_WEST:
        return "X";
    default:
        return "Y";
    }
}

// The label of face control `c`: under kLabel a Nintendo pad's controls are its letters by the
// Xbox positions (gamepad_button_control); otherwise the label at that position.
std::string face_label(const Control &c, SDL_GamepadType type, FaceButtons face) {
    if (face == FaceButtons::kLabel && glyph_set_for(type) == GlyphSet::kNintendo) {
        return xbox_letter(c.code);
    }
    switch (SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(c.code))) {
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
        // A pad SDL knows nothing of: the Xbox layout's letters.
        return glyph_set_for(type) == GlyphSet::kPlayStation
                   ? (c.code == SDL_GAMEPAD_BUTTON_SOUTH  ? "Cross"
                      : c.code == SDL_GAMEPAD_BUTTON_EAST ? "Circle"
                      : c.code == SDL_GAMEPAD_BUTTON_WEST ? "Square"
                                                          : "Triangle")
                   : xbox_letter(c.code);
    }
}

// The pill's text for a control that is not a face button or a direction.
std::string pill_label(const Control &c, SDL_GamepadType type) {
    const GlyphSet set = glyph_set_for(type);
    const auto pick = [set](const char *xbox, const char *ps, const char *nintendo) {
        return std::string(set == GlyphSet::kPlayStation ? ps
                           : set == GlyphSet::kNintendo  ? nintendo
                                                         : xbox);
    };
    if (c.kind == ControlKind::kGamepadTrigger) {
        return c.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? pick("LT", "L2", "ZL")
                                                       : pick("RT", "R2", "ZR");
    }
    switch (c.code) {
    case SDL_GAMEPAD_BUTTON_BACK:
        if (type == SDL_GAMEPAD_TYPE_XBOX360) {
            return "Back";
        }
        return type == SDL_GAMEPAD_TYPE_PS5 ? "Create" : pick("View", "Share", "-");
    case SDL_GAMEPAD_BUTTON_START:
        return type == SDL_GAMEPAD_TYPE_XBOX360 ? "Start" : pick("Menu", "Options", "+");
    case SDL_GAMEPAD_BUTTON_GUIDE:
        return pick("Guide", "PS", "Home");
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return pick("LS", "L3", "LS");
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return pick("RS", "R3", "RS");
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return pick("LB", "L1", "L");
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return pick("RB", "R1", "R");
    case SDL_GAMEPAD_BUTTON_MISC1:
        return pick("Share", "Mic", "Capture");
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return "Touchpad";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
        return "P1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
        return "P2";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
        return "P3";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
        return "P4";
    default: {
        const char *n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(c.code));
        return n != nullptr ? n : "?";
    }
    }
}

int pill_width(const std::string &label) { return Canvas::text_width(label) + 8; }

} // namespace

const char *glyph_set_name(GlyphSet set) {
    switch (set) {
    case GlyphSet::kPlayStation:
        return "PlayStation";
    case GlyphSet::kNintendo:
        return "Nintendo";
    default:
        return "Xbox";
    }
}

GlyphSet glyph_set_for(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return GlyphSet::kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return GlyphSet::kNintendo;
    default:
        return GlyphSet::kXbox;
    }
}

std::string pad_glyph_label(const Control &c, SDL_GamepadType type, FaceButtons face) {
    if (c.kind == ControlKind::kDirection) {
        return std::string("D-pad ") + direction_name(static_cast<Direction>(c.code));
    }
    if (c.kind == ControlKind::kKey) {
        return key_label(c.code);
    }
    return is_face(c) ? face_label(c, type, face) : pill_label(c, type);
}

std::string key_label(int keycode) {
    switch (static_cast<SDL_Keycode>(keycode)) {
    case SDLK_RETURN:
        return "Enter";
    case SDLK_KP_ENTER:
        return "KpEnter";
    case SDLK_ESCAPE:
        return "Esc";
    case SDLK_BACKSPACE:
        return "Bksp";
    case SDLK_DELETE:
        return "Del";
    case SDLK_SPACE:
        return "Space";
    case SDLK_LCTRL:
        return "LCtrl";
    case SDLK_RCTRL:
        return "RCtrl";
    case SDLK_LALT:
        return "LAlt";
    case SDLK_RALT:
        return "RAlt";
    case SDLK_LSHIFT:
        return "LShift";
    case SDLK_RSHIFT:
        return "RShift";
    case SDLK_LGUI:
        return "LCmd";
    case SDLK_RGUI:
        return "RCmd";
    case SDLK_PAGEUP:
        return "PgUp";
    case SDLK_PAGEDOWN:
        return "PgDn";
    default:
        break;
    }
    const char *n = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
    std::string s = n != nullptr && n[0] != '\0' ? n : "?";
    if (s.size() > 8) {
        s.resize(8);
    }
    return s;
}

int glyph_width(const Control &c, SDL_GamepadType type, FaceButtons face) {
    if (c.kind == ControlKind::kKey) {
        return Canvas::text_width(key_label(c.code)) + 6;
    }
    if (c.kind == ControlKind::kDirection || is_face(c)) {
        return kButtonHeight;
    }
    return pill_width(pad_glyph_label(c, type, face));
}

int draw_glyph(Canvas &canvas, int x, int y, const Control &c, SDL_GamepadType type,
               FaceButtons face) {
    const int h = kButtonHeight;
    if (c.kind == ControlKind::kKey) {
        const std::string label = key_label(c.code);
        const int w = Canvas::text_width(label) + 6;
        canvas.fill(x, y, w, h, kCap);
        canvas.fill(x, y + h - 2, w, 2, kCapShade);
        canvas.text(x + 3, y + 2, label, kCapInk);
        return w;
    }
    if (c.kind == ControlKind::kDirection) {
        // A plus with the pressed arm lit.
        canvas.fill(x + 4, y, 4, h, kPlus);
        canvas.fill(x, y + 4, h, 4, kPlus);
        switch (static_cast<Direction>(c.code)) {
        case Direction::kUp:
            canvas.fill(x + 4, y, 4, 5, kInk);
            break;
        case Direction::kDown:
            canvas.fill(x + 4, y + 7, 4, 5, kInk);
            break;
        case Direction::kLeft:
            canvas.fill(x, y + 4, 5, 4, kInk);
            break;
        case Direction::kRight:
            canvas.fill(x + 7, y + 4, 5, 4, kInk);
            break;
        }
        return h;
    }
    if (is_face(c)) {
        canvas.disc(x, y, h, kDisc);
        canvas.ring(x, y, h, 1, kRim);
        const std::string label = face_label(c, type, face);
        if (label == "Cross") {
            canvas.cross(x + 3, y + 3, 6, 1, kInk);
        } else if (label == "Circle") {
            canvas.ring(x + 3, y + 3, 6, 1, kInk);
        } else if (label == "Square") {
            canvas.frame(x + 3, y + 3, 6, 6, 1, kInk);
        } else if (label == "Triangle") {
            canvas.triangle_outline(x + 2, y + 3, 8, 6, 1, kInk);
        } else {
            canvas.text(x + 4, y + 3, label, kInk);
        }
        return h;
    }
    const std::string label = pill_label(c, type);
    const int w = pill_width(label);
    canvas.pill(x, y, w, h, kPill);
    canvas.text(x + 4, y + 3, label, kInk);
    return w;
}

void GlyphTracker::show(SDL_JoystickID id) {
    const SDL_GamepadType type = id != 0 ? SDL_GetGamepadTypeForID(id) : SDL_GAMEPAD_TYPE_UNKNOWN;
    if (id == pad_ && type == type_) {
        return;
    }
    if (type != type_) {
        changes_++;
        SDL_Log("input: button glyphs: %s (%s)", glyph_set_name(glyph_set_for(type)),
                id != 0 ? SDL_GetGamepadStringForType(type) : "no gamepad");
    }
    pad_ = id;
    type_ = type;
}

void GlyphTracker::handle(const SDL_Event &e, SDL_JoystickID active) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (std::find(connected_.begin(), connected_.end(), e.gdevice.which) == connected_.end()) {
            connected_.push_back(e.gdevice.which);
        }
        show(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const SDL_JoystickID gone = e.gdevice.which;
        connected_.erase(std::remove(connected_.begin(), connected_.end(), gone), connected_.end());
        if (pad_ == gone) {
            const bool active_left =
                active != 0 && active != gone &&
                std::find(connected_.begin(), connected_.end(), active) != connected_.end();
            show(active_left ? active : connected_.empty() ? 0 : connected_.back());
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (active == 0 || active == e.gbutton.which) {
            show(e.gbutton.which);
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<int>(e.gaxis.value)) > 16384 &&
            (active == 0 || active == e.gaxis.which)) {
            show(e.gaxis.which);
        }
        break;
    default:
        break;
    }
}

} // namespace maplethief::frontend
