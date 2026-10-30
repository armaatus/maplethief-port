// The game (#58, ADR 0042): the frontend boots the recompiled game from the extracted bundle and
// runs it in the SDL window, on one thread.
//
//   main()  ->  run_game()
//     the bundle      --bundle <dir>, else MAPLETHIEF_BUNDLE_DIR, else the build's dev default
//                     (work/ipa/Payload/MapleThief.app, compiled in only with generated code)
//     the slice       the armv7 slice of <bundle>/MapleThief (a fat Mach-O), or --slice <file>
//     the data dir    MAPLETHIEF_DATA_DIR, else SDL_GetPrefPath("", "MapleThief"); its
//                     settings.ini (settings.h, ADR 0043) is read by main.cpp before this
//     mt_process_load + mt_process_run, with UIApplicationMain = mt_uikit_app_main
//
// The guest owns the main thread while it runs. The GL present hook only copies the presented
// frame (Screen::take_gl_frame); the UIKit turn hook, after every run-loop turn, pumps SDL's
// events, composites and presents when something changed, then waits for the run loop's next
// wake (`next`) or an event. Closing the window makes the turn hook return 1: UIApplicationMain
// stops the guest (MT_PROCESS_STOPPED) and mt_process_run returns.
//
// The app lifecycle (lifecycle.h, ADR 0063): a minimise (on iOS the app events) reaches the
// delegate as applicationWillResignActive: / applicationDidBecomeActive: from the turn hook;
// while the window is minimised (in the background) nothing is presented and the turn hook
// waits, as iOS suspends an app. A quit (the window's close button, SDL_EVENT_QUIT or
// _TERMINATING) sends applicationWillTerminate: from the turn hook, then ends the run.
//
// Frame telemetry (frame_stats.h, ADR 0071): the hooks time the guest and every frontend phase;
// the exit log has the tick intervals (p50, p99, max, GL frames a second, overruns), the split of
// a window frame and the vblanks frames were held for; --perf-csv writes a row per window frame,
// and the frame-time overlay (perf_overlay.h, F3) shows the last second's numbers.
//
// Audio (audio_out.h): the device stream pulls the mixer from SDL's audio thread, resumed at
// the first turn; or, with --audio-capture, the turn hook renders the mix up to the run's time
// (the virtual clock's with --fixed-step) into a WAV after every turn.
//
// The generated program comes from main.cpp, the only unit that sees maplethief_gen (when the
// build has it), so this file builds, and is tested, without generated code.
#ifndef MAPLETHIEF_PLATFORM_GAME_H
#define MAPLETHIEF_PLATFORM_GAME_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bindings.h"
#include "maplethief/boot.h"
#include "scaling.h"
#include "settings.h"

struct SDL_Window;
struct SDL_Renderer;

namespace maplethief::frontend {

class Desktop;

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

// A scripted tap (a test aid): down at `at` seconds after the first run-loop turn, up 0.1 s
// later, at screen point (x, y). It is a left mouse click at the window point that shows (x, y),
// through the same event handler and Input (input.h, ADR 0047) as a real click.
// A scripted swipe (--swipe, #75) is a tap that moves: down at (x, y), `moves` drags evenly
// spaced in time and place to (x2, y2), and up there `seconds` after the down.
struct Tap {
    double at = 0;
    float x = 0, y = 0;
    float x2 = 0, y2 = 0; // where it ends (a tap: x, y)
    double seconds = 0.1; // down to up
    std::uint32_t moves = 0;
};

// A scripted screenshot (#60): at `at` seconds after the first run-loop turn, the composited
// screen (480x320 x render scale, what the device's screen shows) is written to
// <capture dir>/<name>.png.
struct Shot {
    double at = 0;
    std::string name;
};

// A scripted lifecycle change (#128, a test aid): `at` seconds after the first run-loop turn
// the window is minimised (--minimise T,S), or the app goes to the background with the events
// SDL sends on iOS for the home gesture (--background T,S), and `seconds` later it comes back.
// The events take the path of real ones (lifecycle.h, ADR 0063).
// --unfocus T,S (#145): the window loses focus at T and gets it back S seconds later
// (SDL_EVENT_WINDOW_FOCUS_LOST / _GAINED), which pauses the game only under pause_unfocused.
struct Suspend {
    double at = 0;
    double seconds = 0;
    bool background = false;
    bool unfocus = false;
};

struct GameOptions {
    // Every setting in effect (settings.ini with the command line over it, after the preset
    // picker; settings.h, ADR 0070). Settings added since #144 are read from here; the fields
    // below copy the older ones, which game.cpp reads (#89 moves them here as it makes them
    // live).
    Settings settings;
    std::string bundle_dir;         // --bundle (empty: MAPLETHIEF_BUNDLE_DIR or the dev default)
    std::string slice_path;         // --slice (empty: from the bundle's executable)
    std::uint32_t render_scale = 1; // render_scale -> mt_gles_config.render_scale
    ScaleMode scale_mode = ScaleMode::kInteger; // scale_mode (settings.ini, --scale-mode)
    Color border_color;                         // border_color (settings.ini, --border-color)
    // filter (settings.ini, --filter; #88): the present stage's shader filter. It needs SDL's GPU
    // renderer for the window (main.cpp asks for one); without it the scale mode's own filter
    // draws, logged.
    ShaderFilter filter = ShaderFilter::kNone;
    // hd_text (settings.ini, --hd-text; #88): the game's text textures are drawn at
    // render_scale times their size (maplethief/cg.h's text scale and replacement hook).
    bool hd_text = false;
    // --safe-area T,L,B,R: insets (window points) instead of the window's safe area (a test aid,
    // #73, ADR 0055); unset: the window's (zero on the desktop).
    std::optional<Insets> safe_area;
    std::uint64_t frames = 0; // --frames: quit after this many window presents (0: never)
    // --seconds: quit at the first run-loop turn this many seconds after the first one (the
    // run loop's time: real, or virtual with --fixed-step; 0: never). A soak's time limit.
    double seconds = 0;
    std::string capture_dir;               // --capture: read-back PNGs go here (empty: none)
    std::vector<std::uint64_t> capture_at; // --capture-at: window presents to capture
    std::vector<Tap> taps;                 // --tap and --swipe, by time
    // --fixed-step: run on the virtual clock (clock.h, ADR 0044), advanced by this many
    // microseconds per run-loop turn and never waited for (0: the host's real clock).
    std::uint64_t fixed_step_us = 0;
    std::vector<Shot> shots; // --shot (needs --capture); the run ends after the last one
    // Audio (audio_out.h, ADR 0045). The device plays unless --no-audio, --audio-capture or
    // --fixed-step (virtual time: playing it in real time would mean nothing).
    bool audio = true;         // false: --no-audio
    std::string audio_capture; // --audio-capture: the mix goes to this WAV instead
    // mute_unfocused (settings.ini, --mute-unfocused): mute while the window is unfocused
    // (FocusMute). Never with --audio-capture: a capture must not depend on the window's focus.
    bool mute_unfocused = true;
    // right_touch (settings.ini, --right-touch): the right mouse button as a touch at a fixed
    // screen point (input.h, #65).
    RightTouch right_touch;
    // rumble, rumble_strength (settings.ini, --rumble, --rumble-strength): the active gamepad
    // rumbles on the game's vibration, a hit and a level-up (rumble.h, #72).
    bool rumble = true;
    std::uint32_t rumble_strength = 100;
    // --portal-exit stay (temporary, until the settings registry, #144): the main menu's NGB
    // portal items return to the menu instead of quitting (the OpenPortalURLAndExit hook,
    // maplethief/hooks.h, #143, ADR 0069). Off (quit) is the original. Switchable at run time
    // with mt_hook_set_enabled.
    bool portal_no_exit = false;
    // hide_keypad (settings.ini, --hide-keypad): when the on-screen key pad is hidden
    // (keypad_hide.h, #71).
    KeypadMode hide_keypad = KeypadMode::kAuto;
    // The bindings (bindings.ini or --bindings, #70): the table and the face buttons, and the
    // file the remap overlay's Save writes ("" without a data directory).
    BindingSet bindings;
    std::string bindings_path;
    // --shot-gl: each --shot also writes the GL frame it shows, as <NAME>.gl.png (a test aid:
    // the game's own drawing without UIKit over it).
    bool shot_gl = false;
    // --minimise and --background, by time (lifecycle.h).
    std::vector<Suspend> suspends;
    // --quit T: the run quits T seconds after the first run-loop turn as when the window is
    // closed, with applicationWillTerminate: (lifecycle.h; below 0: never).
    double quit_at = -1;
    // Frame telemetry (frame_stats.h, ADR 0071). perf_overlay (settings.ini, --perf-overlay):
    // the frame-time overlay from the start (F3 toggles it; never in a run that writes images:
    // perf_overlay_allowed). --perf-csv: a row per window frame goes to this file.
    bool perf_overlay = false;
    std::string perf_csv;
    // The desktop conveniences (desktop.h, #145): main.cpp's, attached to the window; nullptr:
    // none (the hotkeys, the cursor's auto-hide and the screenshots are then off).
    Desktop *desktop = nullptr;
};

// Whether the frame-time overlay may be shown in this run: not when it writes images that must
// not depend on it (--fixed-step, --shot, --capture: goldens, shots and captures).
bool perf_overlay_allowed(const GameOptions &options);

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

// The scripted lifecycle events of --minimise, --background and --quit: (seconds after the
// first run-loop turn, SDL event type), by time. --background is SDL's iOS sequence: at T a
// minimise, WILL_ENTER_BACKGROUND and DID_ENTER_BACKGROUND; at T + S WILL_ENTER_FOREGROUND,
// DID_ENTER_FOREGROUND and a restore. --quit is SDL_EVENT_QUIT.
std::vector<std::pair<double, std::uint32_t>> lifecycle_script(const GameOptions &options);

// The data directory: MAPLETHIEF_DATA_DIR if set and not empty, else SDL_GetPrefPath("",
// "MapleThief") (which creates it). "" if neither gives one.
std::string data_dir();

// Where the game's Documents/ (its save files) goes when it is not <data dir>/Documents (#78,
// ADR 0074): on iOS, without MAPLETHIEF_DATA_DIR, the app's own Documents directory
// (SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)), which UIFileSharingEnabled shares with the Files
// app. "" everywhere else, and "" if `ios` is false (the parameter lets tests take either path).
std::string documents_dir_override(bool ios);

// Reads <dir>/settings.ini over `settings`, logging each problem (never fatal). A missing file
// leaves `settings` as it is. Returns the file's path ("" without a directory); `exists` (if
// not null) says whether the file was there (the preset picker's "Keep my settings", #144).
std::string load_settings(const std::string &dir, Settings *settings, bool *exists = nullptr);

// Writes `settings` to <dir>/settings.ini (serialise_settings over the file as it is, so its
// comments and unknown lines stay), through a temporary file renamed over it. False (logged)
// without a directory or if the write fails; the old file is then unchanged.
bool save_settings(const std::string &dir, const Settings &settings);

// Boots and runs the game in the window until the window closes (or the user or the system
// quits: the delegate gets applicationWillTerminate: first), the guest ends, options.frames
// presents are done or options.seconds have passed. Returns an ExitStatus.
int run_game(const GameProgram &program, SDL_Window *window, SDL_Renderer *renderer, bool vsync,
             const GameOptions &options);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GAME_H
