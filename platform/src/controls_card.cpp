// The controls card (#193, ADR 0054's addendum); see controls_card.h.
#include "controls_card.h"

#include <algorithm>
#include <cmath>

#include "bindings.h"
#include "hud_labels.h"
#include "port_text.h"
#include "remap.h"

namespace maplethief::frontend {

namespace {

constexpr Rgba kBackdrop = {0x00, 0x00, 0x00, kOverlayBackdropAlpha};
constexpr Rgba kPanel = {0x14, 0x1A, 0x24, 235};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};
constexpr Rgba kUnbound = {0xFF, 0xD1, 0x66, 255};
constexpr Rgba kBody = {0x2A, 0x30, 0x3C, 255};
constexpr Rgba kBodyRim = {0x4A, 0x55, 0x68, 255};
constexpr Rgba kPlus = {0x7A, 0x84, 0x96, 255};
constexpr Rgba kKeyOff = {0x2A, 0x30, 0x3C, 255};
constexpr Rgba kKeyMenu = {0x3A, 0x4A, 0x66, 255};
constexpr Rgba kCap = {0xE4, 0xE6, 0xEA, 255};
constexpr Rgba kCapInk = {0x20, 0x24, 0x2C, 255};

// The layout, in screen points (480 x 320).
constexpr int kPanelX = 8, kPanelY = 6, kPanelW = 464, kPanelH = 308;
constexpr int kRight = kPanelX + kPanelW - 12; // the header's right edge
constexpr int kListsY = 100;                   // the lists' headers
constexpr int kRowsY = 112, kRowsEnd = kPanelY + kPanelH - 2;
constexpr int kColumnX[2] = {20, 248}, kColumnW = 216, kTitleW = 84;

const Direction kDirections[4] = {Direction::kUp, Direction::kDown, Direction::kLeft,
                                  Direction::kRight};
const std::uint32_t kDirectionActions[4] = {MT_ACTION_UP, MT_ACTION_DOWN, MT_ACTION_LEFT,
                                            MT_ACTION_RIGHT};

bool is_direction(std::uint32_t action) {
    return action >= MT_ACTION_UP && action <= MT_ACTION_RIGHT;
}

std::vector<Control> device_controls(const std::vector<Binding> &table, Context context,
                                     std::uint32_t action, PromptDevice device) {
    std::vector<Control> out;
    if (device == PromptDevice::kNone) {
        return out;
    }
    const Device want = device == PromptDevice::kKeyboard ? Device::kKeyboard : Device::kGamepad;
    for (const Control &c : controls_for(table, context, action)) {
        if (device_of(c) == want) {
            out.push_back(c);
        }
    }
    return out;
}

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

// `s` cut to `w` points, with '~' where it was cut.
std::string fit(const std::string &s, int w) {
    if (Canvas::text_width(s) <= w) {
        return s;
    }
    std::string t = s;
    while (!t.empty() && Canvas::text_width(t + "~") > w) {
        t.pop_back();
    }
    return t + "~";
}

// The action a control does in the field (MT_ACTION_NONE: nothing).
std::uint32_t field_action(const std::vector<Binding> &table, const Control &c) {
    for (const Binding &b : table) {
        if (b.control == c && b.field != MT_ACTION_NONE) {
            return b.field;
        }
    }
    return MT_ACTION_NONE;
}

bool bound_in(const std::vector<Binding> &table, const Control &c, Context context) {
    return std::any_of(table.begin(), table.end(), [&](const Binding &b) {
        return b.control == c && (context == Context::kField ? b.field : b.menu) != MT_ACTION_NONE;
    });
}

// A plus (a d-pad) in the d x d box at (x, y), arms d / 3 thick.
void plus(Canvas &c, int x, int y, int d, Rgba colour) {
    const int t = std::max(2, d / 3);
    c.fill(x + (d - t) / 2, y, t, d, colour);
    c.fill(x, y + (d - t) / 2, d, t, colour);
}

// --- the pictures --------------------------------------------------------------------------------

void draw_pad(Canvas &c, const PromptStyle &style, const std::vector<Binding> &table) {
    const SDL_GamepadType type = style.type;
    const FaceButtons face = style.face;
    const auto button = [](int b) { return Control{ControlKind::kGamepadButton, b}; };
    const auto trigger = [](int a) { return Control{ControlKind::kGamepadTrigger, a}; };
    // A glyph with its field action beside it, on the left (right-aligned) or the right.
    const auto labelled = [&](const Control &k, int y, bool left) {
        const int w = glyph_width(k, type, face);
        const int gx = left ? 146 : 334 - w;
        draw_glyph(c, gx, y, k, type, face);
        const std::uint32_t a = field_action(table, k);
        const std::string t = fit(a != MT_ACTION_NONE ? card_title(Context::kField, a) : "-", 100);
        c.text(left ? gx - 4 - Canvas::text_width(t) : gx + w + 4, y + 3, t,
               a != MT_ACTION_NONE ? kText : kDim);
    };
    labelled(trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 34, true);
    labelled(button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), 47, true);
    labelled(trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 34, false);
    labelled(button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), 47, false);
    // The body, the d-pad and its field action (Move when the four directions move).
    c.pill(140, 60, 200, 36, kBodyRim);
    c.pill(141, 61, 198, 34, kBody);
    plus(c, 154, 66, 24, kPlus);
    bool moves = true;
    for (int i = 0; i < 4; i++) {
        moves = moves && field_action(table, Control{ControlKind::kDirection,
                                                     static_cast<int>(kDirections[i])}) ==
                             kDirectionActions[i];
    }
    const std::string dpad = tr(moves ? Txt::card_move : Txt::card_dpad);
    c.text(142 - Canvas::text_width(dpad), 75, dpad, kText);
    // The face buttons as a diamond.
    draw_glyph(c, 306, 61, button(SDL_GAMEPAD_BUTTON_NORTH), type, face);
    draw_glyph(c, 294, 72, button(SDL_GAMEPAD_BUTTON_WEST), type, face);
    draw_glyph(c, 318, 72, button(SDL_GAMEPAD_BUTTON_EAST), type, face);
    draw_glyph(c, 306, 83, button(SDL_GAMEPAD_BUTTON_SOUTH), type, face);
    // View and Menu in the middle, the sticks below them.
    const Control view = button(SDL_GAMEPAD_BUTTON_BACK), menu = button(SDL_GAMEPAD_BUTTON_START);
    const int wv = glyph_width(view, type, face), wm = glyph_width(menu, type, face);
    const int x0 = 240 - (wv + 4 + wm) / 2;
    draw_glyph(c, x0, 64, view, type, face);
    draw_glyph(c, x0 + wv + 4, 64, menu, type, face);
    const Control ls = button(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    const Control rs = button(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    draw_glyph(c, 216 - glyph_width(ls, type, face) / 2, 80, ls, type, face);
    draw_glyph(c, 264 - glyph_width(rs, type, face) / 2, 80, rs, type, face);
}

// A key of the small keyboard: its keycode and its width in quarters of a key.
struct MiniKey {
    SDL_Keycode key;
    int quarters;
};

void draw_keyboard(Canvas &c, const std::vector<Binding> &table) {
    static const std::vector<MiniKey> rows[5] = {
        {{SDLK_ESCAPE, 4},
         {SDLK_1, 4},
         {SDLK_2, 4},
         {SDLK_3, 4},
         {SDLK_4, 4},
         {SDLK_5, 4},
         {SDLK_6, 4},
         {SDLK_7, 4},
         {SDLK_8, 4},
         {SDLK_9, 4},
         {SDLK_0, 4},
         {SDLK_MINUS, 4},
         {SDLK_EQUALS, 4},
         {SDLK_BACKSPACE, 8}},
        {{SDLK_TAB, 6},
         {SDLK_Q, 4},
         {SDLK_W, 4},
         {SDLK_E, 4},
         {SDLK_R, 4},
         {SDLK_T, 4},
         {SDLK_Y, 4},
         {SDLK_U, 4},
         {SDLK_I, 4},
         {SDLK_O, 4},
         {SDLK_P, 4},
         {SDLK_LEFTBRACKET, 4},
         {SDLK_RIGHTBRACKET, 4},
         {SDLK_BACKSLASH, 6}},
        {{SDLK_CAPSLOCK, 7},
         {SDLK_A, 4},
         {SDLK_S, 4},
         {SDLK_D, 4},
         {SDLK_F, 4},
         {SDLK_G, 4},
         {SDLK_H, 4},
         {SDLK_J, 4},
         {SDLK_K, 4},
         {SDLK_L, 4},
         {SDLK_SEMICOLON, 4},
         {SDLK_APOSTROPHE, 4},
         {SDLK_RETURN, 9}},
        {{SDLK_LSHIFT, 9},
         {SDLK_Z, 4},
         {SDLK_X, 4},
         {SDLK_C, 4},
         {SDLK_V, 4},
         {SDLK_B, 4},
         {SDLK_N, 4},
         {SDLK_M, 4},
         {SDLK_COMMA, 4},
         {SDLK_PERIOD, 4},
         {SDLK_SLASH, 4},
         {SDLK_RSHIFT, 11}},
        {{SDLK_LCTRL, 6},
         {SDLK_LGUI, 5},
         {SDLK_LALT, 5},
         {SDLK_SPACE, 26},
         {SDLK_RALT, 5},
         {SDLK_RGUI, 5},
         {SDLK_RCTRL, 8}},
    };
    constexpr int kUnit = 12, kX = 240 - (15 * kUnit + 6 + 3 * kUnit) / 2, kY = 35;
    const auto key = [&](SDL_Keycode k, int x, int y, int w) {
        const Control control{ControlKind::kKey, static_cast<int>(k)};
        if (bound_in(table, control, Context::kField)) {
            c.fill(x, y, w - 1, kUnit - 1, kCap);
            const std::string label = key_label(static_cast<int>(k));
            if (Canvas::text_width(label) <= w - 3) {
                c.text(x + (w - 1 - Canvas::text_width(label)) / 2, y + 2, label, kCapInk);
            }
        } else {
            c.fill(x, y, w - 1, kUnit - 1,
                   bound_in(table, control, Context::kMenu) ? kKeyMenu : kKeyOff);
        }
    };
    for (int r = 0; r < 5; r++) {
        int x = kX * 4;
        for (const MiniKey &k : rows[r]) {
            const int w = k.quarters * kUnit / 4;
            key(k.key, x / 4, kY + r * kUnit, w);
            x += w * 4;
        }
    }
    const int ax = kX + 15 * kUnit + 6;
    key(SDLK_UP, ax + kUnit, kY + 3 * kUnit, kUnit);
    key(SDLK_LEFT, ax, kY + 4 * kUnit, kUnit);
    key(SDLK_DOWN, ax + kUnit, kY + 4 * kUnit, kUnit);
    key(SDLK_RIGHT, ax + 2 * kUnit, kY + 4 * kUnit, kUnit);
}

// The two lists, field and menus.
void draw_lists(Canvas &c, const PromptStyle &style, const std::vector<Binding> &table) {
    const std::vector<CardLine> lines = controls_card_lines(table, style.device);
    std::size_t counts[2] = {0, 0};
    for (const CardLine &l : lines) {
        counts[l.context == Context::kField ? 0 : 1]++;
    }
    const int n = static_cast<int>(std::max<std::size_t>({counts[0], counts[1], 1}));
    const int row = std::clamp((kRowsEnd - kRowsY) / n, 10, 13);
    c.text(kColumnX[0], kListsY, tr(Txt::card_in_field), kHeader);
    c.text(kColumnX[1], kListsY, tr(Txt::card_in_menus), kHeader);
    int index[2] = {0, 0};
    for (const CardLine &l : lines) {
        const int col = l.context == Context::kField ? 0 : 1;
        const int y = kRowsY + index[col]++ * row;
        if (y + kButtonHeight > kRowsEnd + 1) {
            continue; // no room left (a table with very many actions)
        }
        const int x = kColumnX[col];
        c.text(x, y + 3, fit(l.title, kTitleW - 2), l.settings ? kDim : kText);
        if (l.controls.empty()) {
            c.text(x + kTitleW, y + 3, tr(Txt::card_unbound), kUnbound);
            continue;
        }
        int gx = x + kTitleW;
        for (const Control &k : l.controls) {
            const int w = glyph_width(k, style.type, style.face);
            if (gx + w > x + kColumnW) {
                c.text(gx, y + 3, "~", kDim);
                break;
            }
            draw_glyph(c, gx, y, k, style.type, style.face);
            gx += w + (l.move ? 1 : 3);
        }
    }
}

// The touch key pad: the screen (480 x 320) at 5/6 in the panel, the keys where they are drawn.
void draw_touch(Canvas &c, const KeypadLayout &layout) {
    constexpr int kX = 40, kY = 34;
    const auto sx = [](float x) { return kX + static_cast<int>(std::lround(x * 5 / 6)); };
    const auto sy = [](float y) { return kY + static_cast<int>(std::lround(y * 5 / 6)); };
    c.frame(kX, kY, 400, 267, 1, kBorder);
    struct Box {
        int x, y, w, h;
        bool meets(const Box &o, int margin) const {
            return x < o.x + o.w + margin && o.x < x + w + margin && y < o.y + o.h && o.y < y + h;
        }
    };
    // The keys first, then their names: a name never covers a key, nor runs into another name
    // (4 points apart across).
    const std::vector<TouchCardKey> keys = touch_card_keys(layout);
    std::vector<Box> drawn, names;
    for (const TouchCardKey &k : keys) {
        const int x = sx(k.rect.x), y = sy(k.rect.y);
        const Box b{x, y, std::max(4, sx(k.rect.x + k.rect.w) - x),
                    std::max(4, sy(k.rect.y + k.rect.h) - y)};
        if (k.dpad) {
            const int d = std::min(b.w, b.h);
            plus(c, x + (b.w - d) / 2, y + (b.h - d) / 2, d, kPlus);
        } else {
            c.fill(x, y, b.w, b.h, kKeyOff);
            c.frame(x, y, b.w, b.h, 1, kBorder);
        }
        drawn.push_back(b);
    }
    const auto taken = [&](const Box &b) {
        return std::any_of(drawn.begin(), drawn.end(),
                           [&b](const Box &o) { return b.meets(o, 0); }) ||
               std::any_of(names.begin(), names.end(),
                           [&b](const Box &o) { return b.meets(o, 4); });
    };
    for (std::size_t i = 0; i < keys.size(); i++) {
        const TouchCardKey &k = keys[i];
        const Box &key = drawn[i];
        const int tw = Canvas::text_width(k.title);
        if (!k.dpad && tw + 4 <= key.w && key.h >= Canvas::kGlyphHeight + 2) {
            // It fits on the key (jump, attack, Map, Bag): there, where it cannot be mistaken.
            c.text(key.x + (key.w - tw) / 2, key.y + (key.h - 7) / 2, k.title, kText);
            continue;
        }
        Box b{0, 0, tw, Canvas::kGlyphHeight};
        int step = 0;
        switch (k.side) {
        case LabelSide::kLeft:
            b.x = key.x - 3 - tw;
            b.y = key.y + (key.h - 7) / 2;
            step = 9;
            break;
        case LabelSide::kBelow:
            b.x = key.x + (key.w - tw) / 2;
            b.y = key.y + key.h + 2;
            step = 9;
            break;
        case LabelSide::kAbove:
            b.x = key.x + (key.w - tw) / 2;
            b.y = key.y - 10;
            step = -9;
            break;
        }
        b.x = std::clamp(b.x, kPanelX + 2, kPanelX + kPanelW - 2 - tw);
        for (int n = 0; n < 8 && taken(b); n++) {
            b.y += step;
        }
        names.push_back(b);
        c.text(b.x, b.y, k.title, kText);
    }
    // In the screen's empty middle, clear of the quick slots' names.
    const char *notes[] = {
        tr(Txt::card_touch_note_1),
        tr(Txt::card_touch_note_2),
        tr(Txt::card_touch_note_3),
        tr(Txt::card_touch_note_4),
    };
    int y = 112;
    for (const char *s : notes) {
        c.text(200 - Canvas::text_width(s) / 2, y, s, kDim);
        y += 12;
    }
}

} // namespace

std::string card_title(Context context, std::uint32_t action) {
    switch (action) {
    case MT_ACTION_MENU:
        return tr(context == Context::kField ? Txt::card_popup_menu : Txt::card_menu);
    case MT_ACTION_SLOT4:
        return tr(context == Context::kField ? Txt::card_slot4 : Txt::card_new_charge);
    case MT_ACTION_MAP:
        return tr(context == Context::kField ? Txt::card_map : Txt::card_map_view);
    case MT_ACTION_MINUS:
        return tr(Txt::card_minus);
    case MT_ACTION_PLUS:
        return tr(Txt::card_plus);
    case MT_ACTION_PAGE:
        return tr(Txt::card_page);
    case MT_ACTION_OK:
        return tr(context == Context::kField ? Txt::card_ok_talk : Txt::card_ok);
    case MT_ACTION_GRADE_REVIVE:
        return tr(Txt::card_revive);
    default:
        return action_title(action);
    }
}

std::vector<CardLine> controls_card_lines(const std::vector<Binding> &table, PromptDevice device) {
    static const std::vector<Binding> defaults = default_bindings();
    std::vector<CardLine> out;
    const std::vector<ActionSlot> slots = action_slots(table);
    for (const Context context : {Context::kField, Context::kMenu}) {
        // The directions: one Move line if each has a control of the device, else one each.
        CardLine move{context,
                      MT_ACTION_UP,
                      tr(context == Context::kField ? Txt::card_move : Txt::card_move_cursor),
                      {},
                      true,
                      false};
        bool merged = device != PromptDevice::kNone;
        for (const std::uint32_t a : kDirectionActions) {
            const std::vector<Control> c = device_controls(table, context, a, device);
            if (c.empty()) {
                merged = false;
                break;
            }
            move.controls.push_back(c.front());
        }
        if (merged) {
            out.push_back(move);
        }
        for (const ActionSlot &s : slots) {
            if (s.context != context || (merged && is_direction(s.action))) {
                continue;
            }
            // Only what the device can do there: a control of it now or by default (the pad has
            // no field OK, the keyboard no menus auto-run); one it lost shows "unbound".
            if (device != PromptDevice::kNone &&
                device_controls(table, context, s.action, device).empty() &&
                device_controls(defaults, context, s.action, device).empty()) {
                continue;
            }
            CardLine line{context,
                          s.action,
                          card_title(context, s.action),
                          device_controls(table, context, s.action, device),
                          false,
                          false};
            if (line.controls.size() > kCardMaxControls) {
                line.controls.resize(kCardMaxControls);
            }
            out.push_back(std::move(line));
        }
        if (context == Context::kField) {
            // The settings menu's own controls (reserved_control: never a binding).
            CardLine settings{context, MT_ACTION_NONE, tr(Txt::card_settings), {}, false, true};
            if (device == PromptDevice::kKeyboard) {
                settings.controls.push_back(Control{ControlKind::kKey, static_cast<int>(SDLK_F1)});
            } else if (device == PromptDevice::kGamepad) {
                // + on the Switch, whose HOME button is the system's (#239).
                settings.controls.push_back(Control{ControlKind::kGamepadButton,
                                                    kSettingsPlatform == SettingsPlatform::kSwitch
                                                        ? SDL_GAMEPAD_BUTTON_START
                                                        : SDL_GAMEPAD_BUTTON_GUIDE});
            }
            out.push_back(std::move(settings));
        }
    }
    return out;
}

std::vector<TouchCardKey> touch_card_keys(const KeypadLayout &layout) {
    std::vector<TouchCardKey> out;
    // The d-pad's container view (docs/controls/keypad.md, "Panel 0"), as the layout draws it.
    const KeypadTransform t =
        keypad_view_transform(KeypadGroup::kDpad, layout[KeypadGroup::kDpad], KeypadWide{});
    const KeypadPoint a = t.apply({0, 180});
    out.push_back(
        {tr(Txt::card_move), {a.x, a.y, 168 * t.scale, 136 * t.scale}, LabelSide::kAbove, true});
    const HudKey *keys = hud_keys();
    for (std::size_t i = 0; i < kHudKeyCount; i++) {
        const HudKey &k = keys[i];
        std::string title = k.action == MT_ACTION_MENU ? std::string(tr(Txt::card_menu))
                                                       : card_title(Context::kField, k.action);
        out.push_back({std::move(title), hud_key_rect(k, layout, KeypadWide{}), k.side, false});
    }
    return out;
}

void draw_controls_card(Canvas &c, const PromptStyle &style, const std::vector<Binding> &bindings,
                        const KeypadLayout &layout) {
    c.fill(0, 0, c.width(), c.height(), kBackdrop);
    c.fill(kPanelX, kPanelY, kPanelW, kPanelH, kPanel);
    c.frame(kPanelX, kPanelY, kPanelW, kPanelH, 1, kBorder);
    c.text(20, 13, tr(Txt::card_title), kText, 2);
    std::string device, closes;
    switch (style.device) {
    case PromptDevice::kKeyboard:
        device = tr(Txt::card_keyboard);
        closes = tr(Txt::card_key_closes);
        break;
    case PromptDevice::kGamepad:
        device = tr(Txt::card_gamepad, {glyph_set_name(glyph_set_for(style.type))});
        closes = tr(Txt::card_button_closes);
        break;
    case PromptDevice::kNone:
        device = tr(Txt::card_touch);
        closes = tr(Txt::card_tap_closes);
        break;
    }
    c.text(kRight - Canvas::text_width(device), 12, device, kHeader);
    c.text(kRight - Canvas::text_width(closes), 22, closes, kDim);
    switch (style.device) {
    case PromptDevice::kKeyboard:
        draw_keyboard(c, bindings);
        draw_lists(c, style, bindings);
        break;
    case PromptDevice::kGamepad:
        draw_pad(c, style, bindings);
        draw_lists(c, style, bindings);
        break;
    case PromptDevice::kNone:
        draw_touch(c, layout);
        break;
    }
}

// --- the overlay ---------------------------------------------------------------------------------

void ControlsCard::set_keypad_layout(const KeypadLayout &layout) {
    if (!(layout == layout_)) {
        layout_ = layout;
        dirty_ = true;
    }
}

const std::vector<Binding> &ControlsCard::bindings() const {
    static const std::vector<Binding> defaults = default_bindings();
    return actions_ != nullptr ? actions_->bindings() : defaults;
}

PromptStyle ControlsCard::style() const {
    const PromptDevice device = devices_.changes() == 0 ? seed_ : devices_.device();
    return PromptStyle{device, glyphs_.type(),
                       gamepads_ != nullptr ? gamepads_->face_buttons() : kFaceButtonsDefault};
}

void ControlsCard::open() {
    if (open_) {
        return;
    }
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
    SDL_Log("input: the controls card opens for the %s (the game is paused)",
            prompt_device_name(style().device));
}

void ControlsCard::close(const char *why) {
    if (!open_) {
        return;
    }
    open_ = false;
    dirty_ = true;
    closes_++;
    SDL_Log("input: the controls card closes (%s)", why);
}

bool ControlsCard::handle(const SDL_Event &e) {
    if (!open_) {
        if (swallow_type_ != 0 && e.type == swallow_type_) {
            const Uint64 what = e.type == SDL_EVENT_KEY_UP            ? Uint64{e.key.key}
                                : e.type == SDL_EVENT_MOUSE_BUTTON_UP ? Uint64{e.button.button}
                                                                      : Uint64{e.tfinger.fingerID};
            if (what == swallow_what_) {
                swallow_type_ = 0;
                return true;
            }
        }
        return false;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) {
            return true;
        }
        close("a key");
        swallow_type_ = SDL_EVENT_KEY_UP;
        swallow_what_ = e.key.key;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        close("a click");
        swallow_type_ = SDL_EVENT_MOUSE_BUTTON_UP;
        swallow_what_ = e.button.button;
        return true;
    case SDL_EVENT_FINGER_DOWN:
        close("a tap");
        swallow_type_ = SDL_EVENT_FINGER_UP;
        swallow_what_ = e.tfinger.fingerID;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Gamepads never saw it go down (taken), so its up changes nothing there.
        close("a gamepad button");
        swallow_type_ = 0;
        return true;
    default:
        return is_input(e.type);
    }
}

void ControlsCard::track(const SDL_Event &e) {
    glyphs_.handle(e, gamepads_ != nullptr ? gamepads_->active_id() : 0);
    devices_.handle(e);
    if (open_ && !(style() == drawn_)) {
        dirty_ = true;
    }
}

void ControlsCard::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    drawn_ = style();
    Canvas c(rgba, width, height, scale);
    draw_controls_card(c, drawn_, bindings(), layout_);
}

} // namespace maplethief::frontend
