// The Nintendo Switch's part of MapleThief's start (switch_app.h, #238).
#include "switch_app.h"

#include <SDL3/SDL.h>
#include <switch.h>
#include <unistd.h>

#include <cstdio>

namespace mt::switch_nx {

NxApp::NxApp() {
    // nxlink fills __nxlink_host from the arguments it sends; the Homebrew Menu does not. Only
    // then are the sockets worth their memory (applet mode is short of it, ADR 0138).
    if (__nxlink_host.s_addr == 0) {
        return;
    }
    sockets_ = R_SUCCEEDED(socketInitializeDefault());
    nxlink_ = sockets_ ? nxlinkStdio() : -1;
    if (nxlink_ >= 0) {
        std::fprintf(stderr, "MapleThief " MT_VERSION ": stdout and stderr over nxlink\n");
        std::fflush(stderr);
    }
}

NxApp::~NxApp() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (nxlink_ >= 0) {
        close(nxlink_);
    }
    if (sockets_) {
        socketExit();
    }
}

void prepare_sdl() { SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2"); }

maplethief::frontend::SwitchScreen current_screen() {
    return maplethief::frontend::switch_screen(appletGetOperationMode() ==
                                               AppletOperationMode_Console);
}

} // namespace mt::switch_nx
