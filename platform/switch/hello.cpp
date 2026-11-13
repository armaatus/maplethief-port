// The Nintendo Switch's hello path (hello.h, #235).

#include "hello.h"

#include <switch.h>
#include <unistd.h>

#include <cstdio>

namespace mt::switch_nx {

const char *hello_line() { return "MapleThief " MT_VERSION; }

int run_hello() {
    // nxlink: stderr goes to the host that sent the NRO (scripts/switch-run), stdout stays on the
    // screen's console. Started from the Homebrew Menu there is no host, and the connection fails
    // quietly.
    const bool sockets = R_SUCCEEDED(socketInitializeDefault());
    const int nxlink = sockets ? nxlinkConnectToHost(false, true) : -1;
    std::fprintf(stderr, "%s\n", hello_line());
    std::fflush(stderr);

    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    std::printf("%s\n\n", hello_line());
    std::printf("The Nintendo Switch build's toolchain check (#235).\n");
    std::printf("The game does not run here yet.\n\n");
    std::printf("Press + to return to the Homebrew Menu.\n");
    consoleUpdate(nullptr);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);

    if (nxlink >= 0) {
        std::fprintf(stderr, "%s: bye\n", hello_line());
        std::fflush(stderr);
        close(nxlink);
    }
    if (sockets) {
        socketExit();
    }
    return 0;
}

} // namespace mt::switch_nx
