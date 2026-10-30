// Device controls to game actions (#67, ADR 0048); see actions.h.
#include "actions.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

#include "maplethief/input.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

void runtime_sink(void *, std::uint32_t action, bool down) {
    if (down) {
        mt_input_press(action);
    } else {
        mt_input_release(action);
    }
}

std::int32_t runtime_panel(void *) { return mt_input_panel(); }

// A drag's moves between its began and its end, one per turn.
constexpr int kMoves = 8;
// The first virtual finger id of the synthetic touches (Input::touch: finger 0x1000 + id).
constexpr std::uint32_t kGestureFinger = 0x10;
constexpr std::uint32_t kGestureFingers = 16;

const char *action_name(std::uint32_t action) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    return info != nullptr ? info->name : "none";
}

} // namespace

Context context_of_panel(std::int32_t panel) {
    return panel == 0 ? Context::kField : Context::kMenu;
}

const char *context_name(Context context) { return context == Context::kField ? "field" : "menu"; }

const char *direction_name(Direction d) {
    switch (d) {
    case Direction::kUp:
        return "up";
    case Direction::kDown:
        return "down";
    case Direction::kLeft:
        return "left";
    case Direction::kRight:
        return "right";
    }
    return "?";
}

std::string control_name(const Control &c) {
    char buf[64];
    switch (c.kind) {
    case ControlKind::kDirection:
        return std::string("direction ") + direction_name(static_cast<Direction>(c.code));
    case ControlKind::kGamepadButton: {
        const char *n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(c.code));
        std::snprintf(buf, sizeof buf, "button %s", n != nullptr ? n : "?");
        return buf;
    }
    case ControlKind::kGamepadTrigger: {
        const char *n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(c.code));
        std::snprintf(buf, sizeof buf, "trigger %s", n != nullptr ? n : "?");
        return buf;
    }
    case ControlKind::kKey: {
        const char *n = SDL_GetKeyName(static_cast<SDL_Keycode>(c.code));
        if (n != nullptr && n[0] != '\0') {
            return std::string("key ") + n;
        }
        std::snprintf(buf, sizeof buf, "key 0x%X", static_cast<unsigned>(c.code));
        return buf;
    }
    }
    return "?";
}

std::vector<Binding> default_gamepad_bindings() {
    const auto dir = [](Direction d) {
        return Control{ControlKind::kDirection, static_cast<int>(d)};
    };
    const auto button = [](SDL_GamepadButton b) {
        return Control{ControlKind::kGamepadButton, static_cast<int>(b)};
    };
    const auto trigger = [](SDL_GamepadAxis a) {
        return Control{ControlKind::kGamepadTrigger, static_cast<int>(a)};
    };
    // docs/controls/keypad.md, "Gamepad (Xbox names, SDL3 names)". SDL3's face buttons are
    // positional: South is A on an Xbox pad and Cross on a PlayStation pad.
    return {
        {dir(Direction::kUp), MT_ACTION_UP, MT_ACTION_UP},
        {dir(Direction::kDown), MT_ACTION_DOWN, MT_ACTION_DOWN},
        {dir(Direction::kLeft), MT_ACTION_LEFT, MT_ACTION_LEFT},
        {dir(Direction::kRight), MT_ACTION_RIGHT, MT_ACTION_RIGHT},
        {button(SDL_GAMEPAD_BUTTON_SOUTH), MT_ACTION_JUMP, MT_ACTION_OK},
        {button(SDL_GAMEPAD_BUTTON_WEST), MT_ACTION_ATTACK, MT_ACTION_REVIVE},
        {button(SDL_GAMEPAD_BUTTON_EAST), MT_ACTION_SLOT2, MT_ACTION_BACK},
        {button(SDL_GAMEPAD_BUTTON_NORTH), MT_ACTION_SLOT1, MT_ACTION_GRADE_REVIVE},
        {button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), MT_ACTION_SLOT3, MT_ACTION_MINUS},
        {button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), MT_ACTION_SLOT4, MT_ACTION_PLUS},
        // RT in menus: '#', the shop's Charge (a touch button only on the device) and the
        // character screen's New, as the keyboard's N (#127).
        {trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), MT_ACTION_ATTACK, MT_ACTION_SLOT4},
        {trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), MT_ACTION_CASH_SHOP, MT_ACTION_NONE},
        {button(SDL_GAMEPAD_BUTTON_START), MT_ACTION_MENU, MT_ACTION_BACK},
        {button(SDL_GAMEPAD_BUTTON_BACK), MT_ACTION_MAP, MT_ACTION_MAP},
    };
}

std::vector<Binding> default_keyboard_bindings() {
    const auto key = [](SDL_Keycode k) { return Control{ControlKind::kKey, static_cast<int>(k)}; };
    // docs/controls/keyboard.md (#68), from docs/controls/keypad.md's proposal. W A S D are not
    // directions: A S D are quick slots (keyboard.md says why). In menus the confirm and back
    // keys send the gamepad's OK and back (South and East), and the arrows its d-pad.
    return {
        {key(SDLK_UP), MT_ACTION_UP, MT_ACTION_UP},
        {key(SDLK_DOWN), MT_ACTION_DOWN, MT_ACTION_DOWN},
        {key(SDLK_LEFT), MT_ACTION_LEFT, MT_ACTION_LEFT},
        {key(SDLK_RIGHT), MT_ACTION_RIGHT, MT_ACTION_RIGHT},
        // Jump and attack; in menus the two keys under them confirm and go back.
        {key(SDLK_X), MT_ACTION_JUMP, MT_ACTION_BACK},
        {key(SDLK_SPACE), MT_ACTION_JUMP, MT_ACTION_OK},
        {key(SDLK_LALT), MT_ACTION_JUMP, MT_ACTION_NONE},
        {key(SDLK_RALT), MT_ACTION_JUMP, MT_ACTION_NONE},
        {key(SDLK_Z), MT_ACTION_ATTACK, MT_ACTION_OK},
        {key(SDLK_LCTRL), MT_ACTION_ATTACK, MT_ACTION_NONE},
        {key(SDLK_RCTRL), MT_ACTION_ATTACK, MT_ACTION_NONE},
        // The four quick slots, and the number keys as their alternative.
        {key(SDLK_A), MT_ACTION_SLOT1, MT_ACTION_NONE},
        {key(SDLK_S), MT_ACTION_SLOT2, MT_ACTION_NONE},
        {key(SDLK_D), MT_ACTION_SLOT3, MT_ACTION_NONE},
        {key(SDLK_F), MT_ACTION_SLOT4, MT_ACTION_NONE},
        {key(SDLK_1), MT_ACTION_SLOT1, MT_ACTION_NONE},
        {key(SDLK_2), MT_ACTION_SLOT2, MT_ACTION_NONE},
        {key(SDLK_3), MT_ACTION_SLOT3, MT_ACTION_NONE},
        {key(SDLK_4), MT_ACTION_SLOT4, MT_ACTION_NONE},
        // Confirm (in the field -5 is also an attack, as the device's OK), back and the menu.
        {key(SDLK_RETURN), MT_ACTION_OK, MT_ACTION_OK},
        {key(SDLK_KP_ENTER), MT_ACTION_OK, MT_ACTION_OK},
        {key(SDLK_ESCAPE), MT_ACTION_MENU, MT_ACTION_BACK},
        {key(SDLK_BACKSPACE), MT_ACTION_NONE, MT_ACTION_BACK},
        // The map ('*': in menus the map's view switch and the character screen's Delete), the
        // cash shop.
        {key(SDLK_M), MT_ACTION_MAP, MT_ACTION_MAP},
        {key(SDLK_DELETE), MT_ACTION_NONE, MT_ACTION_MAP},
        {key(SDLK_C), MT_ACTION_CASH_SHOP, MT_ACTION_NONE},
        // Menus: minus / plus (popups' amounts, the shop's and the popup menu's tabs), the
        // death popup's two revivals, '#' (the character screen's New, the shop's Charge).
        {key(SDLK_Q), MT_ACTION_NONE, MT_ACTION_MINUS},
        {key(SDLK_E), MT_ACTION_NONE, MT_ACTION_PLUS},
        {key(SDLK_R), MT_ACTION_NONE, MT_ACTION_REVIVE},
        {key(SDLK_G), MT_ACTION_NONE, MT_ACTION_GRADE_REVIVE},
        {key(SDLK_N), MT_ACTION_NONE, MT_ACTION_SLOT4},
    };
}

std::vector<Binding> default_bindings() {
    std::vector<Binding> rows = default_gamepad_bindings();
    const std::vector<Binding> keys = default_keyboard_bindings();
    rows.insert(rows.end(), keys.begin(), keys.end());
    return rows;
}

const std::vector<TouchFallback> &touch_fallbacks() {
    // Panel 10, the help pages (HelpScrollKeyPadView): a paging UIScrollView that only a swipe
    // turns; its keys do nothing there (docs/controls/keypad.md, "Touch-only actions" 3). A
    // drag of 300 of the 480 points settles on the next page.
    static const std::vector<TouchFallback> k = {
        {10, MT_ACTION_RIGHT, 390, 160, 90, 160, "help: next page (swipe left)"},
        {10, MT_ACTION_LEFT, 90, 160, 390, 160, "help: previous page (swipe right)"},
    };
    return k;
}

Actions::Actions(Input *input)
    : input_(input), sink_(runtime_sink), panel_(runtime_panel), bindings_(default_bindings()) {}

void Actions::set_sink(ActionSink sink, void *user) {
    sink_ = sink != nullptr ? sink : runtime_sink;
    sink_user_ = sink != nullptr ? user : nullptr;
}

void Actions::set_panel_source(PanelSource source, void *user) {
    panel_ = source != nullptr ? source : runtime_panel;
    panel_user_ = source != nullptr ? user : nullptr;
}

Context Actions::context() const { return context_of_panel(panel_(panel_user_)); }

std::uint32_t Actions::action_for(const Control &c, Context context) const {
    for (const Binding &b : bindings_) {
        if (b.control == c) {
            return context == Context::kField ? b.field : b.menu;
        }
    }
    return MT_ACTION_NONE;
}

std::uint32_t Actions::control(InputSource source, const Control &c, bool down) {
    const auto it = std::find_if(held_.begin(), held_.end(), [&](const Held &h) {
        return h.source == source && h.control == c;
    });
    if (!down) {
        if (it == held_.end()) {
            return MT_ACTION_NONE;
        }
        const std::uint32_t action = it->action;
        held_.erase(it);
        if (action != MT_ACTION_NONE) {
            sink_(sink_user_, action, false);
        }
        return action;
    }
    if (it != held_.end()) {
        return MT_ACTION_NONE; // already down
    }
    const std::int32_t panel = panel_(panel_user_);
    const Context context = context_of_panel(panel);
    const std::uint32_t action = action_for(c, context);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "input: %s %s in the %s (panel %d): %s",
                 input_source_name(source), control_name(c).c_str(), context_name(context),
                 static_cast<int>(panel), action_name(action));
    if (action != MT_ACTION_NONE) {
        for (const TouchFallback &f : touch_fallbacks()) {
            if (f.panel == panel && f.action == action) {
                // No key code does this here: a touch does (held as nothing, so that its
                // release is quiet).
                held_.push_back(Held{source, c, MT_ACTION_NONE});
                start_gesture(source, f);
                return MT_ACTION_NONE;
            }
        }
    }
    held_.push_back(Held{source, c, action});
    if (action != MT_ACTION_NONE) {
        sink_(sink_user_, action, true);
    }
    return action;
}

void Actions::release_source(InputSource source) {
    const std::vector<Held> held = held_;
    for (const Held &h : held) {
        if (h.source == source) {
            control(source, h.control, false);
        }
    }
}

void Actions::start_gesture(InputSource source, const TouchFallback &f) {
    const std::uint32_t id = kGestureFinger + next_gesture_id_++ % kGestureFingers;
    SDL_Log("input: %s: %s (a touch: no key does this here)", input_source_name(source), f.what);
    if (input_ != nullptr && input_->touch(source, id, MT_UIKIT_TOUCH_BEGAN, f.x0, f.y0)) {
        gestures_.push_back(Gesture{source, id, &f, 0});
    }
}

void Actions::turn() {
    for (Gesture &g : gestures_) {
        const TouchFallback &f = *g.fallback;
        const bool tap = f.x0 == f.x1 && f.y0 == f.y1;
        g.step++;
        if (!tap && g.step <= kMoves) {
            const float t = static_cast<float>(g.step) / kMoves;
            input_->touch(g.source, g.id, MT_UIKIT_TOUCH_MOVED, f.x0 + (f.x1 - f.x0) * t,
                          f.y0 + (f.y1 - f.y0) * t);
        } else {
            input_->touch(g.source, g.id, MT_UIKIT_TOUCH_ENDED, f.x1, f.y1);
            g.step = -1; // done
        }
    }
    gestures_.erase(std::remove_if(gestures_.begin(), gestures_.end(),
                                   [](const Gesture &g) { return g.step < 0; }),
                    gestures_.end());
}

} // namespace maplethief::frontend
