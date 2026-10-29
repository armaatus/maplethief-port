// The game (#58, ADR 0042): the frontend boots the recompiled game from the extracted bundle and
// runs it in the SDL window, on one thread.
//
//   main()  ->  run_game()
//     the bundle      --bundle <dir>, else MAPLETHIEF_BUNDLE_DIR, else the build's dev default
//                     (work/ipa/Payload/MapleThief.app, compiled in only with generated code)
//     the slice       the armv7 slice of <bundle>/MapleThief (a fat Mach-O), or --slice <file>
//     the data dir    MAPLETHIEF_DATA_DIR, else SDL_GetPrefPath("", "MapleThief")
//     mt_process_load + mt_process_run, with UIApplicationMain = mt_uikit_app_main
//
// The guest owns the main thread while it runs. The GL present hook only copies the presented
// frame (Screen::take_gl_frame); the UIKit turn hook, after every run-loop turn, pumps SDL's
// events, composites and presents when something changed, then waits for the run loop's next
// wake (`next`) or an event. Closing the window makes the turn hook return 1: UIApplicationMain
// stops the guest (MT_PROCESS_STOPPED) and mt_process_run returns.
//
// The generated program comes from main.cpp, the only unit that sees maplethief_gen (when the
// build has it), so this file builds, and is tested, without generated code.
#ifndef MAPLETHIEF_PLATFORM_GAME_H
#define MAPLETHIEF_PLATFORM_GAME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/boot.h"

struct SDL_Window;
struct SDL_Renderer;

namespace maplethief::frontend {

// Exit statuses of MapleThief (ADR 0004, 0042).
enum ExitStatus : int {
    kExitOk = 0,            // quit cleanly (and every requested check passed)
    kExitSdlInitFailed = 1, // SDL_Init or window/renderer creation failed
    kExitUsage = 2,         // invalid command line
    kExitCheckMismatch = 3, // a read-back did not show what was presented
    kExitNoGame = 4,        // no generated code, no bundle, no slice, or the load failed
    kExitGuestEnded = 5,    // the guest trapped, stopped by itself or exited non-zero
};

// The generated program and the reservation its memory model needs (MT_MEM_RESERVE_FOR_BUILD in
// main.cpp, which sees maplethief_gen's MT_MEM_CHECKED).
struct GameProgram {
    const mt_program *program = nullptr;
    std::uint32_t reservation = 0;
    const char *dev_bundle_dir = nullptr; // the build's default bundle, or nullptr
};

// A scripted tap (a test aid until mouse input, #65): down at `at` seconds after the first
// run-loop turn, up 0.1 s later, at screen point (x, y).
struct Tap {
    double at = 0;
    float x = 0, y = 0;
};

// A scripted screenshot (#60): at `at` seconds after the first run-loop turn, the composited
// screen (480x320 x render scale, what the device's screen shows) is written to
// <capture dir>/<name>.png.
struct Shot {
    double at = 0;
    std::string name;
};

struct GameOptions {
    std::string bundle_dir;         // --bundle (empty: MAPLETHIEF_BUNDLE_DIR or the dev default)
    std::string slice_path;         // --slice (empty: from the bundle's executable)
    std::uint32_t render_scale = 1; // --render-scale -> mt_gles_config.render_scale
    std::uint64_t frames = 0;       // --frames: quit after this many window presents (0: never)
    std::string capture_dir;        // --capture: read-back PNGs go here (empty: none)
    std::vector<std::uint64_t> capture_at; // --capture-at: window presents to capture
    std::vector<Tap> taps;                 // --tap
    // --fixed-step: run on the virtual clock (clock.h, ADR 0044), advanced by this many
    // microseconds per run-loop turn and never waited for (0: the host's real clock).
    std::uint64_t fixed_step_us = 0;
    std::vector<Shot> shots; // --shot (needs --capture); the run ends after the last one
};

// Whether a shot name is a plain file name: 1 to 64 of [A-Za-z0-9_-].
bool valid_shot_name(const std::string &name);

// The bundle directory: `flag` if not empty, else `env` if set and not empty, else
// `dev_default`. Returns "" (with `why`) if that is not a directory.
std::string choose_bundle_dir(const std::string &flag, const char *env, const char *dev_default,
                              std::string *why);

// The armv7 slice of a Mach-O file: a fat file's cputype 12 / cpusubtype 9 entry, or the whole
// file if it is a thin armv7 Mach-O. The file is untrusted: every count, offset and size is
// checked. Returns false (with `error`) if there is no such slice.
bool find_armv7_slice(const std::uint8_t *file, std::size_t size, std::size_t *offset,
                      std::size_t *length, std::string *error);

// Boots and runs the game in the window until the window closes, the guest ends or
// options.frames presents are done. Returns an ExitStatus.
int run_game(const GameProgram &program, SDL_Window *window, SDL_Renderer *renderer, bool vsync,
             const GameOptions &options);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GAME_H
