// The keyboard (#68); see keyboard.h.
#include "keyboard.h"

#include "maplethief/input.h"

namespace maplethief::frontend {

Keyboard::Keyboard(Actions *actions) : actions_(actions) {}

bool Keyboard::handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        if (e.key.repeat || (e.key.mod & SDL_KMOD_GUI) != 0) {
            return false;
        }
        const Control c{ControlKind::kKey, static_cast<int>(e.key.key)};
        return actions_->control(InputSource::kKeyboard, c, true) != MT_ACTION_NONE;
    }
    case SDL_EVENT_KEY_UP: {
        const Control c{ControlKind::kKey, static_cast<int>(e.key.key)};
        return actions_->control(InputSource::kKeyboard, c, false) != MT_ACTION_NONE;
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

void Keyboard::release_all() { actions_->release_source(InputSource::kKeyboard); }

} // namespace maplethief::frontend
