// The game (#58, ADR 0042): the frontend boots the recompiled game from the extracted bundle and
// runs it in the SDL window, on one thread.
//
//   main()  ->  run_game()
//     the bundle      --bundle <dir>, else MAPLETHIEF_BUNDLE_DIR, else the build's dev default
//                     (work/ipa/Payload/MapleThief.app, compiled in only with generated code),
//                     else <game folder>/game/MapleThief.app (placed by hand), else the
//                     imported bundle folder: <game folder>/game, or the data folder's game/
//                     outside the game folder (find_bundle, ADR 0127; main.cpp shows the
//                     import screen when none is there)
//     the slice       the armv7 slice of <bundle>/MapleThief (a fat Mach-O), or --slice <file>
//     the data dir    data_location() (data_location.h, ADR 0127): MAPLETHIEF_DATA_DIR, else
//                     --data-location, else user-data.txt or an Applications folder (the
//                     per-user folder), else <game folder>/data if it can be written, else the
//                     per-user folder; its settings.ini (settings.h, ADR 0043) is read by
//                     main.cpp before this; saves/, save_dir and the backups: save_tools.h
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
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bindings.h"
#include "data_location.h"
#include "maplethief/boot.h"
#include "scaling.h"
#include "settings.h"
#include "switch_applet.h"

struct SDL_Window;
struct SDL_Renderer;

namespace maplethief::frontend {

enum class PromptDevice; // prompts.h

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
    // Every setting in effect at the start (settings.ini with the command line over it, after
    // the preset picker; settings.h, ADR 0070). game.cpp reads every setting from here (its
    // live copy, which the settings menu changes, #89, ADR 0076); render_scale and hd_text are
    // also copied below.
    Settings settings;
    std::string bundle_dir;         // --bundle (empty: find_bundle's other places)
    std::string slice_path;         // --slice (empty: from the bundle's executable)
    std::uint32_t render_scale = 1; // render_scale -> mt_gles_config.render_scale
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
    // The bindings (bindings.ini or --bindings, #70): the table and the face buttons, and the
    // file the remap overlay's Save writes ("" without a data directory).
    BindingSet bindings;
    std::string bindings_path;
    // The first-launch preset picker's "Show controls" (#193): the controls card opens once the
    // game is up (kCardAfterSeconds after the first run-loop turn). Never in a scripted run.
    bool show_controls_card = false;
    PromptDevice controls_card_device{}; // the device it was answered with (none: touch)
    // --shot-gl: each --shot also writes the GL frame it shows, as <NAME>.gl.png (a test aid:
    // the game's own drawing without UIKit over it).
    bool shot_gl = false;
    // --minimise and --background, by time (lifecycle.h).
    std::vector<Suspend> suspends;
    // --quit T: the run quits T seconds after the first run-loop turn as when the window is
    // closed, with applicationWillTerminate: (lifecycle.h; below 0: never).
    double quit_at = -1;
    // Frame telemetry (frame_stats.h, ADR 0071): settings.perf_overlay shows the frame-time
    // overlay from the start (F3 toggles it; never in a run that writes images:
    // perf_overlay_allowed). --perf-csv: a row per window frame goes to this file.
    std::string perf_csv;
    // The desktop conveniences (desktop.h, #145): main.cpp's, attached to the window; nullptr:
    // none (the hotkeys, the cursor's auto-hide and the screenshots are then off).
    Desktop *desktop = nullptr;
    // The Nintendo Switch's applet side (#240, switch_applet.h): main.cpp's, from
    // platform/switch/switch_app.h. kNotSwitch elsewhere: AppletWatch does nothing.
    SwitchRunMode applet_mode = SwitchRunMode::kNotSwitch;
    std::function<std::optional<MemorySample>()> applet_memory;
    std::function<void(bool)> applet_boost;
    // --dump-textures DIR (#153, ADR 0081): every upload and every UIKit image loaded by name
    // goes to DIR once (maplethief/texpack.h); empty: no dump.
    std::string dump_textures;
    // --edit-keypad (#159): the key pad editor opens once the game has its pad
    bool edit_keypad = false;
    // --hold-fast-forward T,S (#151, a test aid): the fast_forward action is held from T for S
    // seconds after the first run-loop turn (of the guest's clock), as if its key were; it does
    // something only with fast_forward on. Each is (T, S).
    std::vector<std::pair<double, double>> fast_forward_holds;
    // --force-crash host|trap (#198, a test aid: the crash report's dialog, and the safe start
    // after it): a bad host write, or a guest trap, at force_crash_at seconds after the first
    // run-loop turn. Empty: never.
    std::string force_crash;
    double force_crash_at = 2.0;
    // --restore-backup T,N (#192, a test aid): at T seconds after the first run-loop turn, the
    // settings menu's Backups list's row N (1 = the newest) is chosen and confirmed, as a player
    // does (save_menu.h's backups_list: refused with a character loaded). Each is (T, N).
    std::vector<std::pair<double, int>> restore_backups;
};

// Whether the frame-time overlay may be shown in this run: not when it writes images that must
// not depend on it (--fixed-step, --shot, --capture: goldens, shots and captures).
bool perf_overlay_allowed(const GameOptions &options);

// Whether the run is scripted (not a player's): --frames, --seconds, --capture, --capture-at,
// --shot, --tap / --swipe, --fixed-step, --audio-capture, --minimise, --background or --quit.
// Neither the first-launch preset picker (#144) nor the settings menu (#89) shows in one.
bool scripted_run(const GameOptions &options);

// Whether a shot name is a plain file name: 1 to 64 of [A-Za-z0-9_-].
bool valid_shot_name(const std::string &name);

// Where the game's bundle is found (#182, ADR 0100), in this order: --bundle, then
// MAPLETHIEF_BUNDLE_DIR (each must be a directory once given), then the build's own default
// (MT_DEV_BUNDLE_DIR, the bootstrap's work/ipa/Payload/MapleThief.app, or the macOS game app's
// MT_EMBEDDED_BUNDLE; a player build has neither), then game/MapleThief.app next to the
// executable (scripts/linux-game's folder), then the bundle the import put in the data
// directory (<data dir>/game, ipa_import.h). The last three are skipped when they are not
// there. With none of them the import screen shows (BundleChoice::import).
struct BundleSearch {
    std::string flag;                    // --bundle ("" if not given)
    const char *env = nullptr;           // MAPLETHIEF_BUNDLE_DIR (null or "": not set)
    const char *build_default = nullptr; // the build's default bundle (null: none)
    std::string placed;                  // <game folder>/game/MapleThief.app ("" if not known)
    std::string bundle_parent;           // the folder whose game/ is the import's ("" if none)
};
struct BundleChoice {
    std::string dir;    // the bundle ("" if none)
    std::string source; // where it came from, for the log ("--bundle", ...)
    std::string why;    // without one: why, in a sentence
    // No bundle anywhere (not one named by --bundle or MAPLETHIEF_BUNDLE_DIR that is missing):
    // the player's IPA is to be imported.
    bool import = false;
};
BundleChoice find_bundle(const BundleSearch &search);
// This run's search: `flag`, MAPLETHIEF_BUNDLE_DIR, `build_default`, then data_location()'s
// placed_bundle (not on iOS) and bundle folder.
BundleSearch bundle_search(const std::string &flag, const char *build_default);

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

// The data folder: data_location().dir (data_location.h, ADR 0127). "" if there is none.
std::string data_dir();

// Safe start (#198, crash_report.h): nothing in this run writes settings.ini, the player's file
// stays byte for byte as it was. save_settings logs and returns true.
void set_settings_write_blocked(bool blocked);
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
