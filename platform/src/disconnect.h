// The disconnect pause (#184, ADR 0048's addendum): when the gamepad in use goes away, the game
// pauses and says so, as console and PC games do.
//
//   opens    on SDL_EVENT_GAMEPAD_REMOVED of the active pad (Gamepads::active_id()) while no other
//            pad stays connected, if enabled (pause_on_disconnect on, and never in a scripted
//            run: game.cpp). Opening lifts every held key and pad control, as the remap overlay
//            does, so the hero stops. A pad that is not the active one, or one that goes while
//            another stays, changes nothing.
//   while    the game is paused as under the remap overlay (game.cpp's run_overlays holds the
//   open     run-loop turn) and a small box over the last composited screen says "Controller
//            disconnected. Reconnect it, or press any key.", with the lost pad's glyphs.
//   closes   when any pad connects (its ADDED still reaches Gamepads), and on a key press, a
//            click, a tap or a button on another pad. The press that closes it is taken, and so
//            is its release (the key's up, the mouse button's up, the finger's up), so nothing
//            pressed to close it reaches the game.
//
// handle() sees every event before the devices (game.cpp's handle_event); the caller hands the
// events it does not take to them. Drawn by draw() into a copy of the composited screen.
//
// Single thread: the frontend's.
#ifndef MAPLETHIEF_PLATFORM_DISCONNECT_H
#define MAPLETHIEF_PLATFORM_DISCONNECT_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "actions.h"
#include "gamepad.h"

namespace maplethief::frontend {

class DisconnectPause {
  public:
    // `actions` and `gamepads` (either may be null in tests): what opening lifts; `gamepads`
    // also says which pad is the active one.
    DisconnectPause(Actions *actions, Gamepads *gamepads)
        : actions_(actions), gamepads_(gamepads) {}

    // pause_on_disconnect, outside a scripted run. Turning it off closes an open pause.
    void set_enabled(bool on);
    bool enabled() const { return enabled_; }

    bool is_open() const { return open_; }
    // An SDL event, before the devices. Returns true if it took the event (the devices must not
    // see it): while open every key, mouse, touch and pad button or axis event; after it closed,
    // the release of the press that closed it. A pad's ADDED and REMOVED are never taken.
    bool handle(const SDL_Event &e);
    // Closes it (a quit, or the overlay cannot be shown).
    void close(const char *why);

    // Something to redraw since the last draw().
    bool dirty() const { return dirty_; }
    // Draws the box over `rgba` (width x height, top row first, opaque; or premultiplied, as GPU
    // presentation's UIKit layer), `scale` pixels per screen point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

    // The lost pad: its type (the glyphs drawn) and name.
    SDL_GamepadType lost_type() const { return lost_type_; }
    const std::string &lost_name() const { return lost_name_; }
    // How often it opened and closed (for logs and tests).
    unsigned opens() const { return opens_; }
    unsigned closes() const { return closes_; }

  private:
    void open(const SDL_Event &e);
    // The release that goes with `e`, a press that closed it.
    void swallow_release_of(const SDL_Event &e);
    bool is_swallowed_release(const SDL_Event &e);

    Actions *actions_;
    Gamepads *gamepads_;
    bool enabled_ = false;
    bool open_ = false;
    bool dirty_ = false;
    SDL_GamepadType lost_type_ = SDL_GAMEPAD_TYPE_UNKNOWN;
    std::string lost_name_;
    unsigned opens_ = 0, closes_ = 0;
    // The release to take after a close: an SDL event type (0: none) and what it names (the
    // key, the mouse button, the finger).
    Uint32 swallow_type_ = 0;
    Uint64 swallow_what_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DISCONNECT_H
