// The Nintendo Switch's part of MapleThief's start (#238, ADR 0138), called by main.cpp only in a
// Switch build with the generated code. Everything else (the data folder, the settings, the
// presenter) is the shared code with the Switch's rows in it.
//
//   NxApp           for the whole run: the network sockets and nxlink's stdio when a computer
//                   sent the NRO (scripts/switch-run): stdout and stderr, so every SDL_Log line,
//                   reach that terminal. Started from the Homebrew Menu, the connection is not
//                   tried. The player log in <data>/logs/ (player_log.h) gets the same lines in
//                   both cases.
//   prepare_sdl     the hints before SDL_Init: SDL_HINT_RENDER_DRIVER = opengles2 (ADR 0138:
//                   switch-mesa's GLES 2 path; opengl would be tried first otherwise).
//   current_screen  the screen of the console's operation mode (switch_screen.h): the window's
//                   size at the start. A dock or undock later resizes the window (SDL's backend).
#pragma once

#include "switch_screen.h"

namespace mt::switch_nx {

class NxApp {
  public:
    NxApp();
    ~NxApp();
    NxApp(const NxApp &) = delete;
    NxApp &operator=(const NxApp &) = delete;

    // Whether stdout and stderr go to nxlink's host.
    bool nxlink() const { return nxlink_ >= 0; }

  private:
    bool sockets_ = false;
    int nxlink_ = -1;
};

void prepare_sdl();
maplethief::frontend::SwitchScreen current_screen();

} // namespace mt::switch_nx
