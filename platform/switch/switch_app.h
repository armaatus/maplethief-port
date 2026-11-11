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
//
// The applet side (#240, ADR 0141; the decisions are switch_applet.h's, tested on every host):
//   NxApp           also hooks libnx's applet messages for the log only ("applet: focus
//                   Background", "applet: exit requested", ...): nx-mod/sdl3-nx's own hook turns
//                   them into SDL's lifecycle events, and the log shows on a console what came.
//   run_mode        title takeover or applet mode (appletGetAppletType).
//   memory_sample   the guest's committed bytes (the commit map), newlib's heap (the loader's
//                   bounds and malloc's bytes in use) and the process's (svcGetInfo).
//   CpuBoost        scoped: appletSetCpuBoostMode(FastLoad) while at least one is alive, Normal
//                   again after the last; for long loads only (the start and the map loads
//                   through set_load_boost, #243's import), never in play. Thread-safe.
#pragma once

#include <optional>

#include "switch_applet.h"
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
    bool hooked_ = false;
};

void prepare_sdl();
maplethief::frontend::SwitchScreen current_screen();

maplethief::frontend::SwitchRunMode run_mode();
std::optional<maplethief::frontend::MemorySample> memory_sample();

class CpuBoost {
  public:
    CpuBoost();
    ~CpuBoost();
    CpuBoost(const CpuBoost &) = delete;
    CpuBoost &operator=(const CpuBoost &) = delete;
};
// AppletWatch's switch (switch_applet.h): holds one CpuBoost while `on`.
void set_load_boost(bool on);

} // namespace mt::switch_nx
