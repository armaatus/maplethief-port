// Gamepads (#67, ADR 0048): SDL3 gamepads as controls of the action layer (actions.h).
//
//   buttons    every SDL_GamepadButton but the d-pad: ControlKind::kGamepadButton
//   triggers   SDL_GAMEPAD_AXIS_LEFT/RIGHT_TRIGGER pressed past StickConfig::trigger_press,
//              released below trigger_release: ControlKind::kGamepadTrigger
//   directions the d-pad buttons and the left stick merged: a direction is held while its
//              d-pad button is down or the stick points that way. The stick has a radial dead
//              zone (engages at `deadzone`, lets go below `release`) and 8-way d-pad
//              emulation: each direction covers 135 degrees around its axis (its own sector
//              and the two diagonals beside it), widened by `hysteresis_degrees` while it is
//              held and narrowed by as much while it is not, so that a stick on a sector edge
//              does not flicker. A diagonal holds two directions (the key pad's tick then
//              chooses: left and right before up and down).
//
// Hot-plug: SDL_EVENT_GAMEPAD_ADDED opens a pad, _REMOVED closes it and lifts whatever it held.
// Several pads: the first active one wins. A pad becomes the active one with a press while no
// other pad holds anything; until then its input is ignored. The active pad (active()) is the
// one #72 rumbles.
//
// Activity: Input::handle already reports gamepad button presses to the activity listener
// (ADR 0047); presses from the stick and the triggers are reported here (Input::note).
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_GAMEPAD_H
#define MAPLETHIEF_PLATFORM_GAMEPAD_H

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <vector>

#include "actions.h"
#include "input.h"

namespace maplethief::frontend {

struct StickConfig {
    float deadzone = 0.30f;          // the stick engages past this fraction of its range
    float release = 0.22f;           // and lets go below it (radial)
    float hysteresis_degrees = 8.0f; // 8-way sector edges move this far (see above)
    float trigger_press = 0.5f;      // a trigger presses past this
    float trigger_release = 0.4f;    // and releases below it
};

// up, down, left, right (Direction order).
using Directions = std::array<bool, 4>;

// The directions a stick at (x, y) holds (each -1..1, y down as SDL's), given the ones it held.
Directions stick_directions(float x, float y, const Directions &held, const StickConfig &config);

class Gamepads {
  public:
    Gamepads(Actions *actions, Input *input);
    ~Gamepads();
    Gamepads(const Gamepads &) = delete;
    Gamepads &operator=(const Gamepads &) = delete;

    void set_stick(const StickConfig &config) { stick_ = config; }
    const StickConfig &stick() const { return stick_; }

    // Opens the gamepads that are connected now (SDL also sends their ADDED events; a pad
    // already open is not opened twice). Needs SDL_INIT_GAMEPAD.
    void open_connected();
    // An SDL event: gamepads added, removed, buttons, axes; focus lost, hidden, minimised lift
    // everything held. Returns true if it was a gamepad's.
    bool handle(const SDL_Event &e);
    // Lifts everything the active pad holds (until it is pressed again).
    void release_all();

    // The active pad (nullptr: none yet, or it went away): #72 rumbles it.
    SDL_Gamepad *active() const;
    SDL_JoystickID active_id() const { return active_; }
    std::size_t count() const { return pads_.size(); }

  private:
    struct Pad {
        SDL_JoystickID id = 0;
        SDL_Gamepad *pad = nullptr;
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
        std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
        Directions stick{};
        std::array<bool, 2> triggers{}; // left, right
        std::vector<Control> emitted;   // the controls it pressed (only the active pad)
        std::vector<Control> last;      // the controls it held at the last event
    };

    Pad *find(SDL_JoystickID id);
    void add(SDL_JoystickID id);
    void remove(SDL_JoystickID id);
    // The controls `p` holds now.
    std::vector<Control> held(const Pad &p) const;
    // Brings what `p` pressed in line with what it holds; returns the presses.
    std::size_t sync(Pad &p);
    void lift(Pad &p);

    Actions *actions_;
    Input *input_;
    StickConfig stick_;
    std::vector<Pad> pads_;
    SDL_JoystickID active_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GAMEPAD_H
