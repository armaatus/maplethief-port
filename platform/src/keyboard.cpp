// The keyboard (#68); see keyboard.h.
#include "keyboard.h"

#include <algorithm>
#include <cstdint>

#include "maplethief/input.h"

namespace maplethief::frontend {

namespace {

// The modifier SDL sets while `key` is held (0: not a modifier key a chord uses).
SDL_Keymod chord_modifier(SDL_Keycode key) {
    switch (key) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return SDL_KMOD_CTRL;
    case SDLK_LALT:
    case SDLK_RALT:
        return SDL_KMOD_ALT;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return SDL_KMOD_SHIFT;
    default:
        return 0;
    }
}

} // namespace

Keyboard::Keyboard(Actions *actions) : actions_(actions) {}

bool Keyboard::press(SDL_Keycode key) {
    return actions_->control(InputSource::kKeyboard,
                             Control{ControlKind::kKey, static_cast<int>(key)},
                             true) != MT_ACTION_NONE;
}

bool Keyboard::flush_held_back() {
    bool any = false;
    for (const HeldBack &h : held_back_) {
        any = press(h.key) || any;
    }
    held_back_.clear();
    return any;
}

void Keyboard::drop_lifted() {
    const std::uint64_t lifts = actions_->lifts(InputSource::kKeyboard);
    std::erase_if(held_back_, [&](const HeldBack &h) { return h.lifts != lifts; });
}

bool Keyboard::handle(const SDL_Event &e) {
    drop_lifted();
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (e.key.repeat || (e.key.mod & SDL_KMOD_GUI) != 0) {
            return false;
        }
        if (chord_modifier(e.key.key) != 0) {
            // Held back until it is let go, another key goes down or kChordSeconds pass.
            held_back_.push_back(
                HeldBack{e.key.key, now_, actions_->lifts(InputSource::kKeyboard)});
            return false;
        }
        const bool flushed = flush_held_back();
        return press(e.key.key) || flushed;
    }
    case SDL_EVENT_KEY_UP: {
        const Control c{ControlKind::kKey, static_cast<int>(e.key.key)};
        if (std::erase_if(swallowed_, [&](SDL_Keycode k) { return k == e.key.key; }) != 0) {
            return false; // a chord's modifier: its press never went out
        }
        const auto it = std::find_if(held_back_.begin(), held_back_.end(),
                                     [&](const HeldBack &h) { return h.key == e.key.key; });
        bool pressed = false;
        if (it != held_back_.end()) {
            held_back_.erase(it); // a tap: pressed and released at once
            pressed = press(e.key.key);
        }
        return actions_->control(InputSource::kKeyboard, c, false) != MT_ACTION_NONE || pressed;
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

void Keyboard::chord_taken(SDL_Keymod mod) {
    std::erase_if(held_back_, [&](const HeldBack &h) {
        if ((chord_modifier(h.key) & mod) == 0) {
            return false;
        }
        swallowed_.push_back(h.key);
        return true;
    });
}

void Keyboard::turn(double now) {
    now_ = now;
    drop_lifted();
    std::erase_if(held_back_, [&](const HeldBack &h) {
        if (now - h.since < kChordSeconds) {
            return false;
        }
        press(h.key);
        return true;
    });
}

void Keyboard::release_all() {
    held_back_.clear();
    swallowed_.clear();
    actions_->release_source(InputSource::kKeyboard);
}

} // namespace maplethief::frontend
