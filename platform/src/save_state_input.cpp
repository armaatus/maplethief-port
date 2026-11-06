// The frontend's side of a save state's restore (#203): save_state_input.h.
#include "save_state_input.h"

#include <SDL3/SDL.h>

#include "actions.h"
#include "assists.h"
#include "gamepad.h"
#include "input.h"
#include "menu_input.h"
#include "pad_pointer.h"
#include "scene_skip.h"

namespace maplethief::frontend {

void release_for_restore(const RestoreInput &layers, double t) {
    if (layers.pointer != nullptr) {
        layers.pointer->release_all();
    }
    if (layers.gamepads != nullptr) {
        layers.gamepads->release_all();
    }
    if (layers.actions != nullptr) {
        for (const InputSource s :
             {InputSource::kKeyboard, InputSource::kGamepad, InputSource::kMouse,
              InputSource::kFinger, InputSource::kScripted, InputSource::kPointer}) {
            layers.actions->release_source(s);
        }
    }
    if (layers.input != nullptr) {
        layers.input->release_all();
    }
    if (layers.assists != nullptr) {
        layers.assists->restart(t);
    }
    if (layers.scene_skip != nullptr) {
        layers.scene_skip->restart();
    }
    if (layers.menu != nullptr) {
        layers.menu->restart();
    }
    SDL_Log("save state: the input is let go for the restore at %.3f s", t);
}

} // namespace maplethief::frontend
