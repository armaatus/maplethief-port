// A process that dies on purpose for crash_report_test.cpp (#198): the crash report's signal
// path has to run in a process that really dies, and the --crash-dialog helper it starts is
// this same executable.
//
//   crash_report_child host DIR   sets up the crash report in DIR and writes through a null
//                                 pointer: exits by SIGSEGV
//   crash_report_child --crash-dialog FILE   the helper's half (MAPLETHIEF_CRASH_DIALOG_LOG)
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "crash_report.h"
#include "maplethief/recomp/calls.h"
#include "player_log.h"

// game.cpp's boot refers to it; nothing here runs a guest.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t, std::uint32_t) noexcept {
    std::abort();
}

namespace fe = maplethief::frontend;

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--crash-dialog" && argc > 2) {
        return fe::run_crash_dialog_tool(argv[2]);
    }
    if (mode != "host" || argc < 3) {
        return 2;
    }
    fe::player_log_open(std::string(argv[2]) + "/logs");
    fe::crash_marker_begin(argv[2]);
    fe::crash_report_start(argv[2]);
    fe::crash_report_set_header("version: child\n");
    SDL_Log("the child's own log line");
    volatile int *volatile null_page = nullptr;
    *null_page = 1;
    return 0;
}
