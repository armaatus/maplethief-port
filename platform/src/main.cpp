// MapleThief frontend (ARCHITECTURE.md section 3, ADR 0042).
//
// Opens a window titled "MapleThief", 960x640 points unless --window says otherwise (the 480x320
// landscape screen at 2x). What it shows:
//
//   the game        (the default when the build links the generated code, maplethief_gen): boots
//                   the recompiled game from the extracted bundle and presents its frames
//                   (game.h, present.h)
//   the placeholder (the default without generated code, M0 #2): a solid clear colour
//   --gfx-prototype the rendering backend's prototype scene (#55, ADR 0039)
//
//   MapleThief [--version] [--smoke-frames N] [--gfx-prototype]
//              [--window WxH] [--scale-mode MODE] [--border-color #RRGGBB] [--safe-area T,L,B,R]
//              [--bundle DIR] [--slice FILE] [--render-scale N] [--frames N] [--seconds S]
//              [--capture DIR] [--capture-at N[,N...]] [--tap T,X,Y]...
//              [--fixed-step MS] [--shot T,NAME]... [--no-audio] [--audio-capture FILE]
//              [--audio-test-tone] [--master-volume N] [--music-volume N]
//              [--effects-volume N] [--mute-unfocused on|off] [--gapless-music on|off]
//              [--audio-mono on|off] [--audio-device default|NAME]
//              [--right-touch X,Y|off] [--hide-keypad auto|always|never] [--shot-gl]
//              [--rumble on|off] [--rumble-strength N]
//              [--minimise T,S]... [--background T,S]... [--unfocus T,S]... [--quit T]
//              [--save-settings]
//              [--gfx-backend software|gpu] [--filter none|xbr|crt|lcd] [--hd-text on|off]
//              [--bindings FILE] [--save-bindings] [--perf-overlay on|off] [--perf-csv FILE]
//              [--fix-original-bugs on|off] [--keypad-layout L] [--edit-keypad]
//              [--preset original|original_controller|enhanced|custom]
//              [--fullscreen off|desktop] [--vsync on|off] [--pause-unfocused on|off]
//              [--language en|ko]
//              [--texture-pack DIR] [--texture-filter game|nearest] [--dump-textures DIR]
//              [--border color|blur|image] [--border-image FILE]
//              [--game-speed game|65|50|40|33|25] [--fast-forward off|x2|x3]
//              [--hold-fast-forward T,S]... [--restore-backup T,N]...
//              [--autosave off|on_quit] [--import-ipa FILE]
//              [--print-data-dir] [--data-location game|user]
//              [--switch-data-location game|user]
//              [--migrate-from DIR [--force]]
//              [--safe-start] [--force-crash host|trap] [--crash-dialog FILE]
//
// --version prints "MapleThief <version>" (MT_VERSION, from the release tag, ADR 0024) to
// stdout and exits 0 without starting SDL.
// --smoke-frames N runs the placeholder (or, with --gfx-prototype, the prototype) for N
// presented frames, never the game. Before presenting the last one it reads the frame back and
// exits with status 3 unless the centre pixel has the clear colour (the prototype: unless the
// scene's probe pixels match). With SDL_VIDEO_DRIVER=dummy it runs headless; CTest and CI use
// that.
// Window options (every mode; scaling.h, ADR 0043):
//   --window WxH           the window's size in points (default 960x640)
//   --scale-mode MODE      integer (default), fit, smooth or sharp
//   --border-color #RRGGBB the bars around the image (default #000000)
//   --safe-area T,L,B,R    safe-area insets in window points (top, left, bottom, right) instead
//                          of the window's (SDL_GetWindowSafeArea: zero on the desktop): the
//                          image is placed clear of them, as around a phone's notch (a test aid,
//                          #73, ADR 0055)
//   --gfx-backend software|gpu  what draws the GL frames: the software rasterizer (default,
//                          the reference) or the GPU backend on SDL3 GPU, which also gives the
//                          window SDL's GPU renderer (gfx_backend.h, #120, ADR 0060)
// On iOS (#73, ADR 0055) the window is fullscreen (no status bar) and turns with the phone
// between landscape left and right (SDL_HINT_ORIENTATIONS); --window does not apply there.
// Audio options (every mode; settings.h, #64):
//   --master-volume N      the volume of everything, 0 to 100 percent (default 100)
//   --music-volume N       the music's volume, 0 to 100 (default 100)
//   --effects-volume N     the sound effects' volume, 0 to 100 (default 100)
//   --mute-unfocused on|off  silence while the window is unfocused (default on; the game only,
//                          never with --audio-capture)
//   --gapless-music on|off  loop the music without the digital silence at the start and end
//                          of its files (default off, as the device; ADR 0062)
//   --audio-mono on|off    both channels (L + R) / 2, for one ear or one speaker (default off;
//                          #195, ADR 0045's addendum; a capture too)
//   --audio-device default|NAME  the playback device by its name (default: the system's); one
//                          that is not there plays on the default (logged)
// Input options (the game; input.h, ADR 0047). The left mouse button is a touch, a touch
// screen's fingers are touches:
//   --right-touch X,Y|off  the right mouse button as a second touch at screen point (X, Y)
//                          (whole points, 0-479, 0-319; default off)
//   --hide-keypad auto|always|never  hide the on-screen key pad: auto (default) while a
//                          gamepad or the keyboard is used, shown again on a touch; always;
//                          never (keypad_hide.h, #71)
//   --rumble on|off        the active gamepad rumbles on the game's vibration, a hit and a
//                          level-up (default on; rumble.h, #72)
//   --rumble-strength N    how strongly, 0 to 100 percent (default 100)
//   --cash-shop off|meso   an Assist: the cash shop's items and the paid revives sold for the
//                          hero's meso at rising prices (default off; maplethief/cashshop.h,
//                          ADR 0072)
//   --menu-repeat on|off   a held direction repeats in the popup menu, NPC chat, the shop (its
//                          amount too) and the map (default off; menu_input.h, #147, ADR 0114)
//   --confirm-guard on|off  a popup that just opened ignores confirm and back for 0.3 s and while
//                          a button pressed in the field is still held (default off)
//   --focus-highlight on|off  with a gamepad or the keyboard, a frame around the focused button
//                          or chosen item (default on; prompts.h, #146)
//   --button-prompts on|off   the control's glyph next to each of the game's buttons (default
//                          off)
//   --game-speed game|65|50|40|33|25  an Assist: the game's tick length in ms (default game:
//                          its own Speed option; game_speed.h, ADR 0087)
//   --fast-forward off|x2|x3  an Assist: holding the fast_forward action (Tab, the right
//                          stick's click) runs the game 2 or 3 times as fast (default off)
//   --ui-animations normal|fast|instant  the speed of the menus' UIKit animations (default
//                          normal; #90, ADR 0092)
//   --turbo-attack on|off  an Assist: holding attack in the field keeps attacking (default off)
//   --auto-potion off|25|50|75  an Assist: under that percentage of max HP the hero drinks the
//                          HP potion on its quick slots (default off; assists.h)
//   --skip-intros on|off   scenes run 8 times as fast, each speech box tapped (default off;
//                          scene_skip.h, #174)
//   --auto-run on|off      an Assist: the auto_run action (R, the left stick's click) keeps the
//                          hero walking (default off; #172, ADR 0094)
//   --hold-toggle off|walk|fast_forward|all  an Assist: a press of these held actions starts
//                          them and the next one stops them (default off)
//   --pause-on-disconnect on|off  the gamepad in use disconnecting pauses the game until a pad
//                          connects or a key is pressed (default off; #184, disconnect.h)
//   MAPLETHIEF_SETTINGS="key = value; key = value"  settings applied after the file and
//                          --preset and before the other flags (#90, ADR 0092: a test suite
//                          run with a feature on); a bad part is reported and stops the list
//   MAPLETHIEF_MAIN_THREAD_PRIORITY=normal  a real-time run keeps the main thread's default
//                          priority instead of SDL's high one (#206, ADR 0120,
//                          thread_priority.h: an A/B measurement)
// The game reads settings.ini in its data directory first (settings.h: preset, scale_mode,
// border_color, render_scale, gfx_backend, filter, hd_text, the volumes, mute_unfocused,
// gapless_music, audio_mono, audio_device, audio_pack, text_pack, right_touch, hide_keypad, rumble,
// rumble_strength, perf_overlay, pacing, fullscreen, vsync, pause_unfocused, language, menu_repeat,
// confirm_guard, focus_highlight, button_prompts, button_labels, pad_pointer, border, border_image,
// game_speed, fast_forward, autosave, ui_animations, turbo_attack, auto_potion, skip_intros,
// minimap, exp_percent, auto_run, hold_toggle, auto_loot, save_states, pause_on_disconnect); the
// command line overrides it.
// Every setting's flag is its registry row's (settings.h), checked as its file line would be. The
// placeholder and the prototype (tests) read no settings file.
//   --preset P        (the game) original, original_controller, enhanced or custom (#144,
//                     ADR 0070): applies the preset over the file before the other flags (which
//                     then make it custom if they change a key it controls), and skips the
//                     first-launch picker. Without a preset in the file or here, an interactive
//                     run (no --frames, --seconds, --capture, --shot, --tap, --swipe,
//                     --fixed-step, --audio-capture, --minimise, --background or --quit, not
//                     SDL's dummy or offscreen driver) shows the picker before the game boots
//                     (preset_picker.h) and writes the choice to settings.ini; its second step
//                     offers the controls card (#193)
//   --save-settings   (the game) writes the settings in effect (the file's, with the command
//                     line's over them) back to settings.ini, keeping its comments, then runs
// The desktop conveniences (the game; desktop.h, #145, ADR 0077):
//   --fullscreen off|desktop  the window, or the whole display (default off); F11, Alt+Enter
//                     and Cmd+Ctrl+F toggle it and write the change to settings.ini
//   --fullscreen-display current|NAME  the display fullscreen goes to (#196; default current,
//                     the window's); one that is not connected is the window's (logged)
//   --vsync on|off    present in step with the display (default on); off: the run loop paces
//                     by the game's own timer
//   --pause-unfocused on|off  the window losing focus pauses the game as a minimise does
//                     (default off; lifecycle.h)
//   --language en|ko  the game's language (default en; MAPLETHIEF_LANGUAGE overrides it)
//   In an interactive run without --window, <data dir>/window.ini keeps the window's size and
//   position from one run to the next. F12, a gamepad's Share button or R3 + Back write a
//   screenshot to <data dir>/screenshots/; the cursor hides while the keyboard or a gamepad
//   is used; the mouse wheel moves a row in the game's menus and lists.
// Bindings (the game; bindings.h, #70, docs/controls/remapping.md): bindings.ini in the data
// directory overrides the default keys and gamepad controls; the settings menu's Controls ->
// Bindings row opens the remap overlay (F1, Guide or both stick clicks open the menu; in a
// scripted run they open the overlay), whose Save writes the file.
//   --bindings FILE   read (and let the overlay save to) FILE instead of <data dir>/bindings.ini
//   --save-bindings   write the bindings in effect (every action, with the defaults for what
//                     the file does not change) to the bindings file, then run
// Game options (exit status 4 without generated code):
//   --bundle DIR      the extracted Payload/MapleThief.app (else MAPLETHIEF_BUNDLE_DIR, else the
//                     build's work/ipa/Payload/MapleThief.app; in the macOS game app the copy
//                     in its Contents/Resources/Game, ADR 0064; else game/MapleThief.app next
//                     to the executable; else the imported <data dir>/game; else, in an
//                     interactive run, the import screen asks for the IPA: import_screen.h,
//                     #182)
//   --slice FILE      a thin armv7 Mach-O instead of the bundle's executable
//   --render-scale N  the renderbuffers' internal scale, 1 to 8, or auto (default 1; auto:
//                     the largest that fits the frame budget and the window, render_scale.h)
//   --filter none|xbr|crt|lcd  a shader filter over the scaled image (default none; #88,
//                     ADR 0066): xBR-style edge smoothing, CRT scanlines, an LCD grid. It needs
//                     SDL's GPU renderer for the window, which is asked for whatever
//                     --gfx-backend is; without one the scale mode's filter draws (logged)
//   --hd-text on|off  draw the game's text textures at the render scale (default off)
//   --fix-original-bugs on|off  fix the original game's crashes and dead ends (default off,
//                     the original; on in the Enhanced preset): guest function hooks
//                     (maplethief/hooks.h) listed in docs/re/original-bugs.md (#155)
//   --border color|blur|image  what fills the window around the picture (default color:
//                     --border-color; blur: the game's frame stretched, blurred and dimmed;
//                     image: --border-image's PNG scaled to cover; border.h, #157, ADR 0085)
//   --border-image FILE  the PNG of --border image (a relative path is from the working
//                     directory; one that cannot be read keeps the border colour, logged)
//   --autosave off|on_quit  on_quit: the game's own save pair runs when the game quits or
//                     goes to the background, where its menu Save would be allowed (default
//                     off, the original; on_quit in the Enhanced preset; autosave.h, #150)
//   --frames N        quit after N window frames; the last is read back and must show the
//                     composited screen (else exit status 3)
//   --seconds S       quit at the first run-loop turn S seconds (above 0, at most a week)
//                     after the first one: real time, or virtual with --fixed-step; the exit
//                     log has the audio output's numbers (a soak's time limit, #124)
//   --capture DIR    write read-backs of window frames as PNGs: the first, the first that is
//                     not black, the last of --frames and those of --capture-at
//   --tap T,X,Y       a scripted tap at screen point (X, Y), T seconds after the first run-loop
//                     turn: a left click at the window point that shows it, through the same
//                     path as the mouse's (a test aid)
//   --hold T,X,Y,S    the same touch held down for S seconds (a key pad button: walking, #87)
//   --fixed-step MS   run on a virtual clock (ADR 0044): every run-loop turn advances it by MS
//                     milliseconds (0.001 to 1000) and nothing waits, so a scripted run
//                     reaches the same frames bit for bit every time
//   --shot T,NAME     write the composited 480x320 screen at T seconds after the first turn to
//                     <--capture DIR>/NAME.png; the run ends after the last shot
//   --shot-gl         each --shot also writes the GL frame it shows to NAME.gl.png
//   --minimise T,S    the window is minimised T seconds after the first run-loop turn and
//                     restored S seconds later: the app delegate resigns and becomes active
//                     again, and the game waits meanwhile (lifecycle.h, ADR 0063; a test aid,
//                     the only lifecycle events a --fixed-step run takes)
//   --background T,S  the same with the events SDL sends on iOS for the home gesture and back
//   --unfocus T,S     the window loses focus at T and gets it back S seconds later: the game
//                     pauses meanwhile only under pause_unfocused (#145; a test aid)
//   --quit T          quit at T seconds as when the window is closed: the delegate gets
//                     applicationWillTerminate:, then the run ends
//   --restore-backup T,N  at T seconds, the settings menu's Backups row N (1 = the newest) is
//                     chosen and confirmed: restored at the title, refused with a character
//                     loaded (#192; a test aid)
//   --hold-fast-forward T,S  the fast_forward action held from T for S seconds (#151; a test
//                     aid: it does something only with fast_forward on, in real time)
//   --no-audio        no audio device (the game runs silent)
//   --audio-capture FILE  write the mix to FILE as a 16-bit stereo 44.1 kHz WAV instead of
//                     playing it (audio_out.h); with --fixed-step the same bytes every run
//   --audio-pack DIR  the music and sound files in DIR replace the game's by file name
//                     (audio_pack, maplethief/audio.h, ADR 0106); python3 -m mtport.audiopack
//                     init DIR lists the names
//   --text-pack DIR   the text in DIR (UTF-8 .strings files) replaces the game's dialogue,
//                     table names and descriptions and the port's own text by key (text_pack,
//                     maplethief/textpack.h, ADR 0108); python3 -m mtport.textpack export DIR
//                     writes the text to translate
//   --perf-overlay on|off  the frame-time overlay from the start (default off; F3 shows or
//                     hides it; never with --fixed-step, --shot or --capture; perf_overlay.h)
//   --pacing display|wall  frame pacing in real time (default display: each game tick lasts
//                     whole display refreshes when the refresh rate allows; wall: the wall
//                     clock; pacing.h, ADR 0075; the fixed-step mode ignores it)
//   --perf-csv FILE   a CSV row per window frame: its present time, interval, tick interval,
//                     vblanks and the split of its time (frame_stats.h, ADR 0071); the exit
//                     log always has the tick intervals, the split and the pacing
//   --dump-textures DIR  every texture the game uploads and every UIKit image it loads by
//                     name, once, as PNGs with a manifest (maplethief/texpack.h, ADR 0081): the
//                     input of python3 -m mtport.texpack, which makes a texture_pack. Game art:
//                     keep it to yourself
//   --keypad-layout original|GROUP=S,DX,DY,O;...  the touch key pad's groups (dpad, actions,
//                     slots) resized (S %), moved (DX, DY points) and faded (O %); default
//                     original (keypad_layout.h, ADR 0086)
//   --edit-keypad     opens the key pad editor once the game shows its pad (keypad_editor.h):
//                     drag, pinch, Save writes keypad_layout to settings.ini
// --audio-test-tone (with the placeholder, or alone) plays a 440 Hz tone for 1.5 s through the
// default audio device and the mixer (#61, the manual device check) and quits after 2 s (once
// the stream has pulled the tone; 10 s at the latest); exit status 3 if no device opened or its
// stream did not pull the tone's frames.
// The save tools (#148, ADR 0078, save_tools.h) run alone, print to stdout and exit (status 3
// when a check fails or nothing was done); --save-dir DIR and --save-backups N go with them:
//   --export-saves ZIP|DIR  copy the game's save files to a zip (a name ending in .zip) or a
//                     folder
//   --import-saves ZIP|DIR  check a zip or folder of save files and, only if every file
//                     passes, copy them into Documents/ after backing up what they replace
//   --check-saves ZIP|DIR   each file's verdict ("theif0: ok", "config: bad_config: ...")
//   --list-backups    every backup folder, newest first
// The IPA import (#182, ADR 0100, ipa_import.h) runs alone too, without a window:
//   --import-ipa FILE check FILE (your own MapleThief-1.5.ipa: its size and SHA-256) and
//                     extract its Payload/MapleThief.app into the bundle folder (<game
//                     folder>/game, or the data folder's game/), replacing the one there; prints
//                     "imported: <folder>" (status 0) or "error: <reason>" (status 3). The first
//                     start without a bundle shows the same import as a screen (a file dialog, or
//                     drop the file on the window).
// The data folder (data_location.h, ADR 0127) is MAPLETHIEF_DATA_DIR when set (a fresh one for a
// test, say), else --data-location's for this run, else the per-user folder
// (SDL_GetPrefPath("", "MapleThief")) when user-data.txt is next to the game or (macOS) the app
// is in an Applications folder, else <game folder>/data when it can be written, else the
// per-user folder. An old layout there (Documents/, data/game/) is migrated at start. A new,
// empty game folder gets the old per-user folder's saves and settings copied in on its first
// start (data_copy.h, ADR 0127's migration (b)), with a notice on the title screen.
//   --print-data-dir  prints the data folder, its location and reason (and a fallback's why),
//                     saves/ and the bundle folder, one "key: value" a line, without starting
//                     SDL's video, and exits 0
//   --data-location game|user  this run's location (beats user-data.txt; writes nothing)
//   --switch-data-location game|user  does what the settings menu's Saves > Data location does
//                     (#263, data_switch.h): copies the data to the other folder (a destination
//                     that has data is used as it is), writes or removes user-data.txt, prints
//                     what it did and exits: 0 done (or already there), 1 refused, with why
//   --migrate-from DIR [--force]  copies the player data in DIR (a backup, another machine's or
//                     an old kit's data/, an old per-user folder; old or new layout) into the
//                     data folder, as the first start does: the saves checked first, nothing
//                     copied if they fail. Refuses a data folder that has data unless --force,
//                     which first backs up every file it replaces to backups/import/<time>/.
//                     Prints what it did; exits 0, or 3 with "error: <reason>"
// portable.txt next to the game is accepted and does nothing (logged once).
// The log and the crash report (player_log.h, crash_report.h, #198): a game run tees every log
// line into <data dir>/logs/MapleThief-<time>.log (the newest 5 are kept); a guest trap, a host
// signal or the guest ending by itself writes <data dir>/crashes/<time>.txt and shows a dialog.
//   --safe-start      this run with the default settings (no packs, the software renderer),
//                     settings.ini untouched; what the offer after a crash chooses
//   --force-crash host|trap  crash 2 s after the first run-loop turn: a bad host write, or a
//                     guest trap (to see the dialog: a test aid)
//   --crash-dialog FILE  shows the crash dialog for FILE and exits: what a crashed process
//                     starts, since it cannot use SDL any more
// Exit statuses: game.h (ExitStatus).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "audio_out.h"
#include "bindings.h"
#include "build_info.h"
#include "crash_report.h"
#include "data_copy.h"
#include "data_location.h"
#include "data_switch.h"
#include "desktop.h"
#include "game.h"
#include "gfx_backend.h"
#include "gfx_prototype.h"
#include "import_screen.h"
#include "ipa_import.h"
#include "maplethief/audio.h"
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/runtime.h"
#include "player_log.h"
#include "present.h"
#include "preset_picker.h"
#include "save_tools.h"
#include "scaling.h"
#include "settings.h"
#include "thread_priority.h"

#if defined(MT_HAVE_GENERATED_CODE)
#include "maplethief/hle/imports.h"
#include "maplethief/recomp/imports.h"
#else
#include <cstdlib>

#include "maplethief/recomp/calls.h"

// Without generated code there is no dispatcher (maplethief_gen compiles
// runtime/src/recomp/dispatch.c), but the boot that game.cpp links refers to it. run_game never
// loads a process in such a build, so this is never called.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t target, std::uint32_t) noexcept {
    std::fprintf(stderr, "mt_dispatch(0x%X) in a build without generated code\n",
                 static_cast<unsigned>(target));
    std::abort();
}
#endif

namespace {

using namespace maplethief::frontend;

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 640;
constexpr int kMinWindowSide = 16;
constexpr int kMaxWindowSide = 16384;
constexpr double kMaxSeconds = 7 * 24 * 3600.0; // --seconds: a week at most

constexpr SDL_Color kClearColor = {0x1E, 0x2A, 0x3A, SDL_ALPHA_OPAQUE};

// Frame pacing when vsync is unavailable (e.g. the dummy driver): about 60 Hz, so the loop
// doesn't spin a core.
constexpr Uint32 kNoVsyncFrameDelayMs = 16;

struct Options {
    std::uint64_t smoke_frames = 0; // 0: run until quit
    bool version = false;           // --version: print the version and exit
    bool gfx_prototype = false;     // --gfx-prototype: show mt_gfx's prototype scene
    bool audio_test_tone = false;   // --audio-test-tone: play a tone through the device
    bool game_option = false;       // a game-only option was given
    int window_w = kWindowWidth;    // --window
    int window_h = kWindowHeight;
    bool window_set = false; // --window given: window.ini is neither read nor written (#145)
    // The command line's overrides of settings.ini: --preset first, then every other setting's
    // flag (the registry's, settings.h) as a settings.ini line (key, value), validated, applied
    // in command-line order.
    std::optional<Preset> preset; // --preset
    std::vector<std::pair<std::string, std::string>> setting_flags;
    bool save_settings = false; // --save-settings
    std::string bindings_file;  // --bindings (empty: <data dir>/bindings.ini)
    bool save_bindings = false; // --save-bindings
    // The save tools (#148, save_tools.h): one of them runs, prints to stdout and exits.
    enum class SaveAction { kNone, kExport, kImport, kCheck, kListBackups };
    SaveAction save_action = SaveAction::kNone;
    std::string save_action_path; // --export-saves, --import-saves, --check-saves
    std::string import_ipa;       // --import-ipa FILE (#182): import it and exit
    bool print_data_dir = false;  // --print-data-dir (#261): print the data folder and exit
    std::optional<DataPlace> data_location;        // --data-location game|user (ADR 0127)
    std::optional<DataPlace> switch_data_location; // --switch-data-location game|user (#263)
    std::string migrate_from;                      // --migrate-from DIR (#262): copy it in and exit
    bool force = false;                            // --force (with --migrate-from)
    bool safe_start = false;                       // --safe-start (#198)
    std::string crash_dialog;                      // --crash-dialog FILE: show it and exit
    GameOptions game;
};

namespace proto = maplethief::gfx_prototype;

// The generated program, if this build links it (main.cpp is the only unit that sees
// maplethief_gen and its MT_MEM_CHECKED).
GameProgram generated_program() {
    GameProgram g;
#if defined(MT_HAVE_GENERATED_CODE)
    static const mt_program program = MT_PROGRAM_GENERATED;
    g.program = &program;
    g.reservation = MT_MEM_RESERVE_FOR_BUILD;
#if defined(MT_EMBEDDED_BUNDLE)
    // The macOS game app (cmake/MacApp.cmake, ADR 0064) carries the IPA's bundle in
    // MapleThief.app/Contents/Resources/<MT_EMBEDDED_BUNDLE>; in an app bundle SDL_GetBasePath()
    // is that Resources directory, wherever the app was moved.
    static std::string embedded;
    if (const char *base = SDL_GetBasePath(); base != nullptr) {
        embedded = std::string(base) + MT_EMBEDDED_BUNDLE;
        g.dev_bundle_dir = embedded.c_str();
    }
#elif defined(MT_DEV_BUNDLE_DIR)
    g.dev_bundle_dir = MT_DEV_BUNDLE_DIR;
#endif
#endif
    return g;
}

// --gfx-prototype: the mt_gfx target, shown through the presenter. With --gfx-backend gpu the
// scene draws on the GPU, and on SDL's GPU renderer the presenter draws it through the present
// stage's identity pass (present_copy.frag), so the check covers the whole GPU path.
struct GfxPrototype {
    mt_gfx_target *target = nullptr;
    Presenter presenter;
    PresentPass pass;
};

// Before the renderer goes.
void stop_gfx_prototype(GfxPrototype &gfx) {
    gfx.presenter.stop();
    gfx.pass.close();
    mt_gfx_target_destroy(gfx.target);
    gfx.target = nullptr;
}

bool start_gfx_prototype(SDL_Renderer *renderer, GfxPrototype &gfx, const GameOptions &scaling) {
    gfx.target = mt_gfx_target_create(proto::kWidth, proto::kHeight, 1);
    if (gfx.target == nullptr) {
        SDL_Log("gfx-prototype: no target");
        return false;
    }
    if (!gfx.presenter.start(renderer, proto::kWidth, proto::kHeight)) {
        return false;
    }
    gfx.presenter.set_scaling(scaling.settings.scale_mode, scaling.settings.border_color);
    gfx.presenter.set_forced_insets(scaling.safe_area);
    if (SDL_GetGPURendererDevice(renderer) != nullptr) {
        if (!gfx.pass.open(renderer, "present_copy.frag")) {
            return false;
        }
        gfx.presenter.set_render_state(gfx.pass.state());
        SDL_Log("gfx-prototype: presenting through the present stage's identity pass");
    }
    return true;
}

// Renders the scene with mt_gfx, uploads it and draws it.
bool render_gfx_prototype(GfxPrototype &gfx, std::uint64_t frame) {
    const Uint64 start = SDL_GetTicksNS();
    if (!proto::render(gfx.target)) {
        SDL_Log("gfx-prototype: the scene did not render");
        return false;
    }
    mt_gfx_end_frame();
    const Uint64 rendered = SDL_GetTicksNS();
    if (!gfx.presenter.upload(mt_gfx_target_cpixels(gfx.target), proto::kWidth * 4) ||
        !gfx.presenter.draw()) {
        return false;
    }
    if (frame == 0) {
        const mt_gfx_stats stats = mt_gfx_get_stats();
        SDL_Log("gfx-prototype: frame 1: %llu draws, %llu pixels, rendered in %.3f ms, uploaded "
                "in %.3f ms",
                static_cast<unsigned long long>(stats.total.draws),
                static_cast<unsigned long long>(stats.total.pixels),
                static_cast<double>(rendered - start) / 1e6,
                static_cast<double>(SDL_GetTicksNS() - rendered) / 1e6);
    }
    return true;
}

// Reads the presented frame back and compares the scene's probe pixels (mapped into the
// presenter's rectangle) with their expected colours and with the mt_gfx target. The window has
// no alpha: RGB only. On the GPU backend the target may differ from the expected colours by ADR
// 0060's channel tolerance (2); the window still shows the target exactly.
bool check_gfx_prototype(SDL_Renderer *renderer, const GfxPrototype &gfx) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        SDL_Log("gfx-prototype: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return false;
    }
    const Rect &r = gfx.presenter.placement().rect;
    bool ok = true;
    const std::uint8_t *pixels = mt_gfx_target_cpixels(gfx.target);
    for (const proto::Probe &probe : proto::kProbes) {
        // The centre of the probe's pixel, in the presenter's rectangle.
        const int sx = static_cast<int>(r.x + (static_cast<float>(probe.x) + 0.5f) * r.w /
                                                  static_cast<float>(proto::kWidth));
        const int sy = static_cast<int>(r.y + (static_cast<float>(probe.y) + 0.5f) * r.h /
                                                  static_cast<float>(proto::kHeight));
        Uint8 cr = 0;
        Uint8 cg = 0;
        Uint8 cb = 0;
        Uint8 ca = 0;
        const bool read = SDL_ReadSurfacePixel(surface, sx, sy, &cr, &cg, &cb, &ca);
        const std::uint8_t *t = pixels + (probe.y * proto::kWidth + probe.x) * 4;
        const int tolerance = mt_gfx_gpu_installed() != 0 ? 2 : 0;
        bool near = true;
        for (std::size_t c = 0; c < 4; c++) {
            near = near && std::abs(t[c] - probe.rgba[c]) <= tolerance;
        }
        const bool match = read && cr == t[0] && cg == t[1] && cb == t[2] && near;
        SDL_Log("gfx-prototype: %s at (%u, %u) -> (%d, %d) of %dx%d: #%02X%02X%02X, expected "
                "#%02X%02X%02X%s",
                probe.what, static_cast<unsigned>(probe.x), static_cast<unsigned>(probe.y), sx, sy,
                surface->w, surface->h, cr, cg, cb, probe.rgba[0], probe.rgba[1], probe.rgba[2],
                match ? "" : "  MISMATCH");
        ok = ok && match;
    }
    SDL_DestroySurface(surface);
    return ok;
}

// Reads back the centre pixel of the cleared frame and checks that it has the clear colour.
bool check_clear_color(SDL_Renderer *renderer) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        SDL_Log("smoke: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return false;
    }
    Uint8 r = 0;
    Uint8 g = 0;
    Uint8 b = 0;
    Uint8 a = 0;
    const bool read = SDL_ReadSurfacePixel(surface, surface->w / 2, surface->h / 2, &r, &g, &b, &a);
    SDL_Log("smoke: %dx%d frame, centre pixel #%02X%02X%02X", surface->w, surface->h, r, g, b);
    SDL_DestroySurface(surface);
    return read && r == kClearColor.r && g == kClearColor.g && b == kClearColor.b;
}

void print_usage() {
    SDL_Log("usage: MapleThief [--version] [--smoke-frames N] [--gfx-prototype] [--window WxH] "
            "[--scale-mode integer|fit|smooth|sharp] [--border-color #RRGGBB] "
            "[--safe-area T,L,B,R] [--bundle DIR] "
            "[--slice FILE] [--render-scale N] [--frames N] [--capture DIR] "
            "[--capture-at N[,N...]] [--tap T,X,Y]... [--swipe T,X1,Y1,X2,Y2]... "
            "[--fixed-step MS] [--shot T,NAME]... "
            "[--no-audio] [--audio-capture FILE] [--audio-test-tone] [--master-volume N] "
            "[--music-volume N] [--effects-volume N] [--mute-unfocused on|off] "
            "[--gapless-music on|off] [--audio-mono on|off] [--audio-device default|NAME] "
            "[--audio-pack DIR] "
            "[--right-touch X,Y|off] [--hide-keypad auto|always|never] "
            "[--shot-gl] [--rumble on|off] [--rumble-strength N] "
            "[--minimise T,S]... [--background T,S]... [--unfocus T,S]... [--quit T] "
            "[--save-settings] "
            "[--gfx-backend software|gpu] [--filter none|xbr|crt|lcd] [--hd-text on|off] "
            "[--bindings FILE] [--save-bindings] [--perf-overlay on|off] [--perf-csv FILE] "
            "[--fix-original-bugs on|off] [--cash-shop off|meso] "
            "[--preset original|original_controller|enhanced|custom] [--pacing display|wall] "
            "[--save-dir DIR] [--save-backups N] [--export-saves ZIP|DIR] "
            "[--import-saves ZIP|DIR] [--check-saves ZIP|DIR] [--list-backups] "
            "[--fullscreen off|desktop] [--vsync on|off] [--pause-unfocused on|off] "
            "[--language en|ko] "
            "[--texture-pack DIR] [--texture-filter game|nearest] [--dump-textures DIR] "
            "[--menu-repeat on|off] [--confirm-guard on|off] "
            "[--focus-highlight on|off] [--button-prompts on|off] "
            "[--keypad-layout original|GROUP=S,DX,DY,O;...] [--edit-keypad] "
            "[--border color|blur|image] [--border-image FILE] "
            "[--game-speed game|65|50|40|33|25] [--fast-forward off|x2|x3] "
            "[--hold-fast-forward T,S]... [--restore-backup T,N]... "
            "[--autosave off|on_quit] [--import-ipa FILE] [--print-data-dir] "
            "[--data-location game|user] [--switch-data-location game|user] "
            "[--migrate-from DIR [--force]] [--safe-start] [--force-crash host|trap]");
}

bool parse_uint(std::string_view value, std::uint64_t *out) {
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), *out);
    return ec == std::errc() && end == value.data() + value.size();
}

// (std::from_chars for double needs macOS 26.)
bool parse_double(std::string_view value, double *out) {
    const std::string s(value);
    char *end = nullptr;
    *out = SDL_strtod(s.c_str(), &end);
    return !s.empty() && end == s.c_str() + s.size() && std::isfinite(*out);
}

// "WxH" (decimal, kMinWindowSide to kMaxWindowSide each).
bool parse_size(std::string_view value, int *w, int *h) {
    const std::size_t x = value.find('x');
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    if (x == std::string_view::npos || !parse_uint(value.substr(0, x), &a) ||
        !parse_uint(value.substr(x + 1), &b) || a < kMinWindowSide || a > kMaxWindowSide ||
        b < kMinWindowSide || b > kMaxWindowSide) {
        return false;
    }
    *w = static_cast<int>(a);
    *h = static_cast<int>(b);
    return true;
}

// Splits "a,b,c" at commas.
std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = s.find(',', start);
        parts.push_back(s.substr(start, comma == std::string_view::npos ? comma : comma - start));
        if (comma == std::string_view::npos) {
            return parts;
        }
        start = comma + 1;
    }
}

// Returns false (after logging why) if the command line is invalid.
bool parse_options(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--version") {
            options.version = true;
        } else if (arg == "--gfx-prototype") {
            options.gfx_prototype = true;
        } else if (const SettingInfo *info = find_setting_flag(arg); info != nullptr && has_value) {
            // A setting's flag (the registry, settings.h): "--music-volume 50" is the line
            // "music_volume = 50", checked as settings.ini would check it.
            options.game_option = options.game_option || info->game_only;
            Settings check;
            std::string why;
            if (!apply_setting(&check, info->key, argv[++i], &why)) {
                SDL_Log("%s: %s", std::string(arg).c_str(), why.c_str());
                return false;
            }
            options.setting_flags.emplace_back(info->key, argv[i]);
        } else if (arg == kPresetFlag && has_value) {
            Preset preset{};
            if (!parse_preset(argv[++i], &preset)) {
                SDL_Log("--preset needs original, original_controller, enhanced or custom, got "
                        "'%s'",
                        argv[i]);
                return false;
            }
            options.preset = preset;
            options.game_option = true;
        } else if (arg == "--perf-csv" && has_value) {
            options.game.perf_csv = argv[++i];
            if (options.game.perf_csv.empty()) {
                SDL_Log("--perf-csv needs a file name");
                return false;
            }
            options.game_option = true;
        } else if (arg == "--dump-textures" && has_value) {
            options.game.dump_textures = argv[++i];
            if (options.game.dump_textures.empty()) {
                SDL_Log("--dump-textures needs a directory");
                return false;
            }
            options.game_option = true;
        } else if (arg == "--shot-gl") {
            options.game.shot_gl = true;
            options.game_option = true;
        } else if (arg == "--save-settings") {
            options.save_settings = true;
            options.game_option = true;
        } else if (arg == "--edit-keypad") {
            options.game.edit_keypad = true;
            options.game_option = true;
        } else if (arg == "--bindings" && has_value) {
            options.bindings_file = argv[++i];
            if (options.bindings_file.empty()) {
                SDL_Log("--bindings needs a file name");
                return false;
            }
            options.game_option = true;
        } else if (arg == "--save-bindings") {
            options.save_bindings = true;
            options.game_option = true;
        } else if ((arg == "--export-saves" || arg == "--import-saves" || arg == "--check-saves") &&
                   has_value) {
            if (options.save_action != Options::SaveAction::kNone) {
                SDL_Log("only one of --export-saves, --import-saves, --check-saves and "
                        "--list-backups");
                return false;
            }
            options.save_action = arg == "--export-saves"   ? Options::SaveAction::kExport
                                  : arg == "--import-saves" ? Options::SaveAction::kImport
                                                            : Options::SaveAction::kCheck;
            options.save_action_path = argv[++i];
            if (options.save_action_path.empty()) {
                SDL_Log("%s needs a path", std::string(arg).c_str());
                return false;
            }
        } else if (arg == "--import-ipa" && has_value) {
            options.import_ipa = argv[++i];
        } else if (arg == "--print-data-dir") {
            options.print_data_dir = true;
        } else if (arg == "--migrate-from" && has_value) {
            options.migrate_from = argv[++i];
            if (options.migrate_from.empty()) {
                SDL_Log("--migrate-from needs a folder");
                return false;
            }
        } else if (arg == "--force") {
            options.force = true;
        } else if (arg == "--safe-start") {
            options.safe_start = true;
            options.game_option = true;
        } else if (arg == "--crash-dialog" && has_value) {
            options.crash_dialog = argv[++i];
        } else if (arg == "--force-crash" && has_value) {
            const std::string_view value = argv[++i];
            if (value != "host" && value != "trap") {
                SDL_Log("--force-crash needs host or trap, got '%s'", argv[i]);
                return false;
            }
            options.game.force_crash = value;
            options.game_option = true;
        } else if (arg == "--data-location" && has_value) {
            options.data_location = parse_data_place(argv[++i]);
            if (!options.data_location) {
                SDL_Log("--data-location needs game or user, got '%s'", argv[i]);
                return false;
            }
        } else if (arg == "--switch-data-location" && has_value) {
            options.switch_data_location = parse_data_place(argv[++i]);
            if (!options.switch_data_location) {
                SDL_Log("--switch-data-location needs game or user, got '%s'", argv[i]);
                return false;
            }
        } else if (arg == "--list-backups") {
            if (options.save_action != Options::SaveAction::kNone) {
                SDL_Log("only one of --export-saves, --import-saves, --check-saves and "
                        "--list-backups");
                return false;
            }
            options.save_action = Options::SaveAction::kListBackups;
        } else if (arg == "--audio-test-tone") {
            options.audio_test_tone = true;
        } else if (arg == "--no-audio") {
            options.game.audio = false;
            options.game_option = true;
        } else if (arg == "--audio-capture" && has_value) {
            options.game.audio_capture = argv[++i];
            if (options.game.audio_capture.empty()) {
                SDL_Log("--audio-capture needs a file name");
                return false;
            }
            options.game_option = true;
        } else if (arg == "--smoke-frames" && has_value) {
            const std::string_view value = argv[++i];
            if (!parse_uint(value, &options.smoke_frames) || options.smoke_frames == 0) {
                SDL_Log("--smoke-frames needs a positive integer, got '%s'", argv[i]);
                return false;
            }
        } else if (arg == "--frames" && has_value) {
            if (!parse_uint(argv[++i], &options.game.frames) || options.game.frames == 0) {
                SDL_Log("--frames needs a positive integer, got '%s'", argv[i]);
                return false;
            }
            options.game_option = true;
        } else if (arg == "--seconds" && has_value) {
            double seconds = 0;
            if (!parse_double(argv[++i], &seconds) || !(seconds > 0) || seconds > kMaxSeconds) {
                SDL_Log("--seconds needs seconds above 0, at most %g, got '%s'", kMaxSeconds,
                        argv[i]);
                return false;
            }
            options.game.seconds = seconds;
            options.game_option = true;
        } else if (arg == "--window" && has_value) {
            if (!parse_size(argv[++i], &options.window_w, &options.window_h)) {
                SDL_Log("--window needs WxH (points, %d to %d each), got '%s'", kMinWindowSide,
                        kMaxWindowSide, argv[i]);
                return false;
            }
            options.window_set = true;
        } else if (arg == "--safe-area" && has_value) {
            Insets insets;
            if (!parse_insets(argv[++i], &insets)) {
                SDL_Log("--safe-area needs T,L,B,R (window points, 0 to 4096 each), got '%s'",
                        argv[i]);
                return false;
            }
            options.game.safe_area = insets;
        } else if (arg == "--bundle" && has_value) {
            options.game.bundle_dir = argv[++i];
            options.game_option = true;
        } else if (arg == "--slice" && has_value) {
            options.game.slice_path = argv[++i];
            options.game_option = true;
        } else if (arg == "--capture" && has_value) {
            options.game.capture_dir = argv[++i];
            options.game_option = true;
        } else if (arg == "--capture-at" && has_value) {
            for (const std::string_view part : split(argv[++i])) {
                std::uint64_t n = 0;
                if (!parse_uint(part, &n) || n == 0) {
                    SDL_Log("--capture-at needs positive integers, got '%s'", argv[i]);
                    return false;
                }
                options.game.capture_at.push_back(n);
            }
            options.game_option = true;
        } else if (arg == "--tap" && has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double t = 0, x = 0, y = 0;
            if (parts.size() != 3 || !parse_double(parts[0], &t) || !parse_double(parts[1], &x) ||
                !parse_double(parts[2], &y) || t < 0) {
                SDL_Log("--tap needs T,X,Y (seconds, screen points), got '%s'", argv[i]);
                return false;
            }
            Tap tap{t, static_cast<float>(x), static_cast<float>(y)};
            tap.x2 = tap.x;
            tap.y2 = tap.y;
            options.game.taps.push_back(tap);
            options.game_option = true;
        } else if (arg == "--hold" && has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double v[4] = {};
            bool ok = parts.size() == 4;
            for (std::size_t k = 0; ok && k < 4; k++) {
                ok = parse_double(parts[k], &v[k]);
            }
            if (!ok || v[0] < 0 || !(v[3] > 0) || v[3] > kMaxSeconds) {
                SDL_Log("--hold needs T,X,Y,S (seconds, screen points, seconds above 0), got '%s'",
                        argv[i]);
                return false;
            }
            Tap hold{v[0], static_cast<float>(v[1]), static_cast<float>(v[2])};
            hold.x2 = hold.x;
            hold.y2 = hold.y;
            hold.seconds = v[3];
            options.game.taps.push_back(hold);
            options.game_option = true;
        } else if (arg == "--swipe" && has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double v[5] = {};
            bool ok = parts.size() == 5;
            for (std::size_t k = 0; ok && k < 5; k++) {
                ok = parse_double(parts[k], &v[k]);
            }
            if (!ok || v[0] < 0) {
                SDL_Log("--swipe needs T,X1,Y1,X2,Y2 (seconds, screen points), got '%s'", argv[i]);
                return false;
            }
            Tap swipe{v[0], static_cast<float>(v[1]), static_cast<float>(v[2])};
            swipe.x2 = static_cast<float>(v[3]);
            swipe.y2 = static_cast<float>(v[4]);
            swipe.seconds = 0.3;
            swipe.moves = 8;
            options.game.taps.push_back(swipe);
            options.game_option = true;
        } else if ((arg == "--minimise" || arg == "--background" || arg == "--unfocus") &&
                   has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double t = 0, s = 0;
            if (parts.size() != 2 || !parse_double(parts[0], &t) || !parse_double(parts[1], &s) ||
                t < 0 || t > kMaxSeconds || !(s > 0) || s > kMaxSeconds) {
                SDL_Log("%s needs T,S (seconds: at T, for S above 0), got '%s'",
                        std::string(arg).c_str(), argv[i]);
                return false;
            }
            options.game.suspends.push_back(
                Suspend{t, s, arg == "--background", arg == "--unfocus"});
            options.game_option = true;
        } else if (arg == "--hold-fast-forward" && has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double t = 0, s = 0;
            if (parts.size() != 2 || !parse_double(parts[0], &t) || !parse_double(parts[1], &s) ||
                t < 0 || t > kMaxSeconds || !(s > 0) || s > kMaxSeconds) {
                SDL_Log("--hold-fast-forward needs T,S (seconds: at T, for S above 0), got '%s'",
                        argv[i]);
                return false;
            }
            options.game.fast_forward_holds.emplace_back(t, s);
            options.game_option = true;
        } else if (arg == "--restore-backup" && has_value) {
            const std::vector<std::string_view> parts = split(argv[++i]);
            double t = 0, n = 0;
            if (parts.size() != 2 || !parse_double(parts[0], &t) || !parse_double(parts[1], &n) ||
                t < 0 || t > kMaxSeconds || n < 1 || n > 1000 || n != std::floor(n)) {
                SDL_Log("--restore-backup needs T,N (seconds, then a row from 1), got '%s'",
                        argv[i]);
                return false;
            }
            options.game.restore_backups.emplace_back(t, static_cast<int>(n));
            options.game_option = true;
        } else if (arg == "--quit" && has_value) {
            double t = 0;
            if (!parse_double(argv[++i], &t) || t < 0 || t > kMaxSeconds) {
                SDL_Log("--quit needs seconds from 0, at most %g, got '%s'", kMaxSeconds, argv[i]);
                return false;
            }
            options.game.quit_at = t;
            options.game_option = true;
        } else if (arg == "--fixed-step" && has_value) {
            double ms = 0;
            if (!parse_double(argv[++i], &ms) || ms < 0.001 || ms > 1000) {
                SDL_Log("--fixed-step needs milliseconds from 0.001 to 1000, got '%s'", argv[i]);
                return false;
            }
            options.game.fixed_step_us = static_cast<std::uint64_t>(std::llround(ms * 1000));
            options.game_option = true;
        } else if (arg == "--shot" && has_value) {
            const std::string_view value = argv[++i];
            const std::size_t comma = value.find(',');
            double t = 0;
            const std::string name =
                comma == std::string_view::npos ? "" : std::string(value.substr(comma + 1));
            if (comma == std::string_view::npos || !parse_double(value.substr(0, comma), &t) ||
                t < 0 || !valid_shot_name(name)) {
                SDL_Log("--shot needs T,NAME (seconds; a name of [A-Za-z0-9_-]), got '%s'",
                        argv[i]);
                return false;
            }
            options.game.shots.push_back(Shot{t, name});
            options.game_option = true;
        } else {
            SDL_Log("unknown argument '%s'", argv[i]);
            return false;
        }
    }
    // Taps and swipes are delivered one after the other, in time order (game.cpp).
    std::stable_sort(options.game.taps.begin(), options.game.taps.end(),
                     [](const Tap &a, const Tap &b) { return a.at < b.at; });
    if (!options.game.shots.empty() && options.game.capture_dir.empty()) {
        SDL_Log("--shot needs --capture DIR");
        return false;
    }
    return true;
}

// --audio-test-tone: how long the tone plays and when the run ends: after 2 s once the stream
// has pulled the tone's frames, and after 10 s at the latest (a slow emulated host's dummy
// driver can fall behind the wall clock).
constexpr std::uint32_t kToneMs = 1500;
constexpr Uint64 kToneRunNs = 2000000000ull;
constexpr Uint64 kToneDeadlineNs = 10000000000ull;
constexpr float kToneHz = 440.0f;

bool tone_pulled() {
    return mt_audio_get_output_stats().frames >= std::uint64_t{MT_AUDIO_RATE} * kToneMs / 1000;
}

// --audio-test-tone: a device must have opened and pulled at least the tone's frames.
bool check_tone(bool device_opened) {
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    const std::uint64_t want = std::uint64_t{MT_AUDIO_RATE} * kToneMs / 1000;
    const bool ok = device_opened && st.frames >= want;
    SDL_Log("audio-test-tone: the device stream pulled %llu frames (the tone has %llu) in %llu "
            "calls, longest render %u us, %llu late callbacks%s",
            static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(want),
            static_cast<unsigned long long>(st.renders), st.longest_render_us,
            static_cast<unsigned long long>(st.late),
            ok ? "" : (device_opened ? "  TOO FEW" : "  NO DEVICE"));
    return ok;
}

// The placeholder and the prototype (no game): the M0 loop.
int run_placeholder(SDL_Renderer *renderer, bool vsync, const Options &options) {
    GfxPrototype gfx;
    if (options.gfx_prototype && !start_gfx_prototype(renderer, gfx, options.game)) {
        stop_gfx_prototype(gfx);
        return kExitSdlInitFailed;
    }
    AudioDevice tone_device;
    bool tone_device_opened = false;
    const Uint64 started = SDL_GetTicksNS();
    if (options.audio_test_tone) {
        mt_audio_reset_output_stats();
        tone_device_opened = tone_device.open();
        if (tone_device_opened) {
            mt_audio_test_tone(kToneHz, kToneMs);
            tone_device.resume();
            SDL_Log("audio-test-tone: playing %g Hz for %u ms", static_cast<double>(kToneHz),
                    kToneMs);
        }
    }

    std::uint64_t frames = 0;
    bool smoke_ok = true;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        if (options.gfx_prototype) {
            if (!render_gfx_prototype(gfx, frames)) {
                smoke_ok = false;
                break;
            }
        } else {
            SDL_SetRenderDrawColor(renderer, kClearColor.r, kClearColor.g, kClearColor.b,
                                   kClearColor.a);
            SDL_RenderClear(renderer);
        }
        const bool last_smoke_frame =
            options.smoke_frames != 0 && frames + 1 == options.smoke_frames;
        if (last_smoke_frame) {
            smoke_ok = options.gfx_prototype ? check_gfx_prototype(renderer, gfx)
                                             : check_clear_color(renderer);
        }
        SDL_RenderPresent(renderer);
        ++frames;
        if (!vsync) {
            SDL_Delay(kNoVsyncFrameDelayMs);
        }

        if (last_smoke_frame) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        if (options.audio_test_tone) {
            const Uint64 elapsed = SDL_GetTicksNS() - started;
            if ((elapsed >= kToneRunNs && tone_pulled()) || elapsed >= kToneDeadlineNs) {
                running = false;
            }
        }
    }

    SDL_Log("quit after %llu frames", static_cast<unsigned long long>(frames));
    stop_gfx_prototype(gfx);
    if (options.audio_test_tone) {
        tone_device.close();
        smoke_ok = check_tone(tone_device_opened) && smoke_ok;
    }
    return smoke_ok ? kExitOk : kExitCheckMismatch;
}

// --export-saves, --import-saves, --check-saves, --list-backups (#148, save_tools.h): the
// settings (save_dir, save_backups) from settings.ini and the command line, then the action. It
// prints to stdout (scripts and the parity test read it) and exits: kExitOk, or
// kExitCheckMismatch when a check found a problem or the action failed.
int run_save_action(const Options &options) {
    Settings settings;
    load_settings(data_dir(), &settings);
    if (options.preset) {
        apply_preset(&settings, *options.preset);
    }
    for (const auto &[key, value] : options.setting_flags) {
        apply_setting(&settings, key, value, nullptr); // checked by parse_options
    }
    const SaveLocation loc = configure_save_location(settings, data_location());
    const std::string &path = options.save_action_path;
    std::string why;
    bool ok = false;
    switch (options.save_action) {
    case Options::SaveAction::kCheck: {
        SaveSet files;
        std::vector<std::string> ignored;
        if (!read_save_source(path, &files, &ignored, &why)) {
            break;
        }
        const std::vector<SaveProblem> problems = check_save_set(files);
        for (const SaveFile &f : files) {
            const auto bad = std::find_if(problems.begin(), problems.end(),
                                          [&](const SaveProblem &p) { return p.file == f.name; });
            std::printf("%s\n",
                        bad != problems.end() ? bad->line().c_str() : (f.name + ": ok").c_str());
        }
        for (const SaveProblem &p : problems) {
            if (p.file == "set") {
                std::printf("%s\n", p.line().c_str());
            }
        }
        for (const std::string &name : ignored) {
            std::printf("ignored: %s\n", name.c_str());
        }
        ok = problems.empty();
        if (files.empty()) {
            why = "no save files in " + path;
            ok = false;
        }
        break;
    }
    case Options::SaveAction::kExport: {
        std::vector<std::string> exported;
        ok = export_saves(loc, path, &exported, &why);
        for (const std::string &name : exported) {
            std::printf("exported: %s\n", name.c_str());
        }
        if (ok) {
            std::printf("from %s to %s\n", loc.documents_dir.c_str(), path.c_str());
        }
        break;
    }
    case Options::SaveAction::kImport: {
        ImportResult result;
        ok = import_saves(loc, path, backup_stamp_now(), &result, &why);
        for (const SaveProblem &p : result.problems) {
            std::printf("rejected: %s\n", p.line().c_str());
        }
        for (const std::string &name : result.ignored) {
            std::printf("ignored: %s\n", name.c_str());
        }
        for (const std::string &name : result.imported) {
            std::printf("imported: %s\n", name.c_str());
        }
        for (const std::string &name : result.removed) {
            std::printf("removed: %s\n", name.c_str());
        }
        if (ok) {
            std::printf("into %s\n", loc.documents_dir.c_str());
            if (!result.backup.empty()) {
                std::printf("the files it replaced are in %s\n", result.backup.c_str());
            }
        }
        break;
    }
    case Options::SaveAction::kListBackups:
        for (const BackupInfo &b : list_backups(loc)) {
            std::string files;
            for (const std::string &f : b.files) {
                files += (files.empty() ? "" : " ") + f;
            }
            std::printf("%s %s %s (%s)\n",
                        b.kind == "import" ? "import" : ("slot " + b.kind).c_str(), b.stamp.c_str(),
                        b.path.c_str(), files.c_str());
        }
        ok = true;
        break;
    case Options::SaveAction::kNone:
        break;
    }
    if (!ok && !why.empty()) {
        std::printf("error: %s\n", why.c_str());
    }
    std::fflush(stdout);
    return ok ? kExitOk : kExitCheckMismatch;
}

// A path from a UTF-8 string (on Windows a narrow string is not UTF-8).
std::filesystem::path utf8_path(const std::string &s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// The data folder resolved (and logged) and an old layout in it migrated (data_location.h,
// ADR 0127's migration (a)): before settings.ini or any other file is opened. Then, unless
// `copy` is false, the first-start copy from the old per-user folder (data_copy.h, migration
// (b)): only for the game folder without data, so the per-user folder is asked for only then.
void prepare_data_folder(bool copy = true) {
    const DataLocation &loc = data_location();
    for (const std::string &line : migrate_data_layout(loc)) {
        SDL_Log("%s", line.c_str());
    }
    if (copy && loc.place == DataPlace::kGame && !loc.dir.empty() &&
        !data_dir_has_player_data(utf8_path(loc.dir))) {
        first_start_copy(loc, user_data_dir(false));
    }
}

// What the log file must not name (player_log.h): the data folder, the game folder and the
// home folder, whichever way round the slashes are.
Redactions log_redactions(const DataLocation &loc) {
    Redactions r;
    const auto add = [&r](std::string path, const char *to) {
        while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
            path.pop_back();
        }
        if (path.empty()) {
            return;
        }
        std::string other = path;
        std::replace(other.begin(), other.end(), '\\', '/');
        if (other == path) {
            std::replace(other.begin(), other.end(), '/', '\\');
        }
        r.emplace_back(std::move(path), to);
        r.emplace_back(std::move(other), to);
    };
    add(loc.dir, "<data>");
    add(loc.base, "<game folder>");
    if (const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME); home != nullptr) {
        add(home, "~");
    }
    return r;
}

// Ends the crash marker and the log file on every way out of main() but a crash: the marker
// stays when this run wrote a report (or the process dies, which skips destructors), so the
// next start offers a safe start.
struct GameRunEnd {
    std::string crashes_dir;
    bool active = false;
    GameRunEnd() = default;
    GameRunEnd(const GameRunEnd &) = delete;
    GameRunEnd &operator=(const GameRunEnd &) = delete;
    ~GameRunEnd() {
        if (active) {
            if (!crash_reported()) {
                crash_marker_end(crashes_dir);
            }
            player_log_close();
        }
    }
};

// --migrate-from DIR [--force] (#262, data_copy.h): the copy without a window; prints to stdout.
int run_migrate_from(const std::string &from, bool force) {
    const DataLocation &loc = data_location();
    if (loc.dir.empty()) {
        std::printf("error: there is no data folder to copy into\n");
        std::fflush(stdout);
        return kExitCheckMismatch;
    }
    std::printf("copying %s into %s\n", from.c_str(), loc.dir.c_str());
    const DataCopyResult r =
        copy_player_data(utf8_path(from), utf8_path(loc.dir), utf8_path(loc.bundle_dir), force);
    for (const std::string &line : r.copied) {
        std::printf("copied: %s\n", line.c_str());
    }
    for (const std::string &line : r.replaced) {
        std::printf("replaced: %s\n", line.c_str());
    }
    if (!r.backup.empty()) {
        std::printf("backup: %s\n", r.backup.c_str());
    }
    for (const std::string &line : r.skipped) {
        std::printf("skipped: %s\n", line.c_str());
    }
    if (r.ok) {
        std::printf("migrated: %s\n", loc.dir.c_str());
    } else {
        std::printf("error: %s\n", r.error.c_str());
    }
    std::fflush(stdout);
    return r.ok ? kExitOk : kExitCheckMismatch;
}

// --import-ipa FILE (#182, ipa_import.h): the import without a window; prints to stdout.
int run_import_ipa(const std::string &path) {
    const std::string dir = data_location().bundle_parent();
    std::printf("importing %s into %s\n", path.c_str(), imported_game_dir(dir).c_str());
    std::fflush(stdout);
    const IpaResult r = import_ipa(path, dir, ipa_expected());
    if (r.ok()) {
        std::printf("imported: %s\n", r.game_dir.c_str());
    } else {
        std::printf("error: %s (%s)\n", r.why.c_str(), ipa_problem_name(r.problem));
    }
    std::fflush(stdout);
    return r.ok() ? kExitOk : kExitCheckMismatch;
}

} // namespace

int main(int argc, char *argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return kExitUsage;
    }
    if (!options.crash_dialog.empty()) {
        return run_crash_dialog_tool(options.crash_dialog); // a crashed process's helper
    }
    if (options.version) {
        // stdout, not SDL_Log (stderr): scripts read it. A Windows GUI program prints only
        // when its output is redirected, e.g. `MapleThief.exe --version | Out-String`.
        std::fputs("MapleThief " MT_VERSION "\n", stdout);
        return std::fflush(stdout) == 0 ? kExitOk : kExitUsage;
    }
    // The data folder (data_location.h, ADR 0127): --data-location first, then resolved on the
    // first use. Without SDL's video: SDL_GetBasePath and SDL_GetPrefPath need none.
    set_data_location_flag(options.data_location);
    if (options.print_data_dir) {
        std::fputs(describe_data_location(data_location()).c_str(), stdout);
        return std::fflush(stdout) == 0 ? kExitOk : kExitUsage;
    }
    if (options.switch_data_location) {
        // The menu's switch from the command line (#263): never a running game to flush.
        DataSwitchInputs in;
        in.user_dir = [] { return user_data_dir(false); };
        std::vector<std::string> lines;
        const int status = switch_data_location_command(data_location(),
                                                        *options.switch_data_location, in, &lines);
        for (const std::string &line : lines) {
            std::fputs((line + "\n").c_str(), stdout);
        }
        std::fflush(stdout);
        return status;
    }
    if (options.force && options.migrate_from.empty()) {
        SDL_Log("--force goes only with --migrate-from");
        print_usage();
        return kExitUsage;
    }
    if (!options.migrate_from.empty()) {
        if (options.game_option || options.smoke_frames != 0 || options.gfx_prototype ||
            options.audio_test_tone || !options.setting_flags.empty() || options.preset ||
            options.save_action != Options::SaveAction::kNone || !options.import_ipa.empty()) {
            SDL_Log("--migrate-from runs alone (with --force or --data-location)");
            print_usage();
            return kExitUsage;
        }
        prepare_data_folder(false); // the copy is this one, from DIR
        return run_migrate_from(options.migrate_from, options.force);
    }
    if (options.save_action != Options::SaveAction::kNone) {
        if (options.game_option || options.smoke_frames != 0 || options.gfx_prototype ||
            options.audio_test_tone) {
            SDL_Log("--export-saves, --import-saves, --check-saves and --list-backups go only "
                    "with settings (e.g. --save-dir)");
            print_usage();
            return kExitUsage;
        }
        prepare_data_folder();
        return run_save_action(options);
    }
    if (!options.import_ipa.empty()) {
        if (options.game_option || options.smoke_frames != 0 || options.gfx_prototype ||
            options.audio_test_tone || !options.setting_flags.empty() || options.preset) {
            SDL_Log("--import-ipa runs alone");
            print_usage();
            return kExitUsage;
        }
        prepare_data_folder();
        return run_import_ipa(options.import_ipa);
    }
    const GameProgram program = generated_program();
    const bool game = options.smoke_frames == 0 && !options.gfx_prototype &&
                      !options.audio_test_tone &&
                      (program.program != nullptr || options.game_option);
    if (options.game_option &&
        (options.smoke_frames != 0 || options.gfx_prototype || options.audio_test_tone)) {
        SDL_Log("the game options do not go with --smoke-frames, --gfx-prototype or "
                "--audio-test-tone");
        print_usage();
        return kExitUsage;
    }
    if (options.audio_test_tone && options.smoke_frames != 0) {
        SDL_Log("--audio-test-tone ends by itself after 2 s: not with --smoke-frames");
        print_usage();
        return kExitUsage;
    }
    if (game && program.program == nullptr) {
        SDL_Log("this build has no generated code, so it cannot run the game (work/gen: python -m "
                "recomp all, then reconfigure)");
        return kExitNoGame;
    }

    // The settings: the defaults, the game's settings.ini over them, the command line over that
    // (--preset first, ADR 0070); before SDL_Init, which reads fullscreen_mode on macOS.
    Settings settings;
    bool settings_file = false;
    GameRunEnd run_end;
    if (game) {
        prepare_data_folder(); // the old layout migrated before anything opens a file
        // The log file and the crash report (#198), through the resolver's layout: logs/ and
        // crashes/ (data_location.h, ADR 0127).
        const DataLocation &loc = data_location();
        set_log_redactions(log_redactions(loc));
        player_log_open(loc.logs);
        for (const std::string &line : data_location_log(loc)) {
            player_log_line(line.c_str()); // logged by the resolver before the file was open
        }
        crash_report_start(loc.crashes);
        run_end.crashes_dir = loc.crashes;
        run_end.active = true;
        load_settings(data_dir(), &settings, &settings_file);
    }
    if (options.preset) {
        apply_preset(&settings, *options.preset);
    }
    if (const char *list = SDL_getenv("MAPLETHIEF_SETTINGS"); game && list != nullptr) {
        std::string why;
        if (!apply_setting_list(&settings, list, &why)) {
            SDL_Log("MAPLETHIEF_SETTINGS: %s: the rest is ignored", why.c_str());
        } else {
            SDL_Log("MAPLETHIEF_SETTINGS: %s", list);
        }
    }
    for (const auto &[key, value] : options.setting_flags) {
        apply_setting(&settings, key, value, nullptr); // checked by parse_options
    }

    // The last run's crash (#198): offer a safe start, before anything uses the settings. The
    // marker is set now, so a crash during the start counts too.
    bool safe_start = false;
    if (game) {
        // What the settings name is the player's too: log lines quote these as they use them.
        // (Very short values would mangle unrelated text.)
        const auto hide = [](const std::string &value, const char *to) {
            if (value.size() >= 4 && value != "default") {
                add_log_redaction(value, to);
            }
        };
        hide(settings.save_dir, "<saves>");
        hide(settings.audio_device, "<audio device>");
        hide(settings.audio_pack, "<pack>");
        hide(settings.texture_pack, "<pack>");
        hide(settings.text_pack, "<pack>");
        hide(settings.border_image, "<image>");
        const LastRun last = crash_marker_begin(run_end.crashes_dir);
        const bool interactive = is_interactive_run(options.game, SDL_getenv("SDL_VIDEO_DRIVER"));
        set_crash_ui(interactive);
        safe_start = decide_safe_start(options.safe_start, last, interactive);
        if (safe_start) {
            settings = safe_start_settings(settings);
            set_settings_write_blocked(true);
            SDL_Log("safe start: default settings for this run (no texture or audio packs, the "
                    "software renderer); settings.ini is not changed");
        }
        if (last.crashed) {
            SDL_Log("the last run did not end cleanly (report: %s)",
                    last.crash_file.empty() ? "none" : last.crash_file.c_str());
        }
    }

#if defined(SDL_PLATFORM_MACOS)
    // fullscreen_mode (#179, ADR 0099): whether fullscreen is a Space of its own (desktop: the
    // system's, the green button's) or the window over the desktop (borderless). SDL reads it
    // once, at video init, so a change counts from the next start.
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES,
                settings.fullscreen_mode == FullscreenMode::kBorderless ? "0" : "1");
#endif
    SDL_SetAppMetadata("MapleThief", MT_VERSION, MT_APP_ID);
#if defined(SDL_PLATFORM_IOS)
    // Both landscape orientations (#73, ADR 0055), set before SDL_Init. The window is resizable,
    // so without the hint SDL would let it turn to portrait too. SDL rotates the window's
    // coordinates with the phone; the safe area's insets swap sides and the placement follows
    // (scaling.h). The game itself always sees landscape left (uikit.h). Info.plist's
    // UISupportedInterfaceOrientations must list both as well (#83): SDL intersects the two.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return kExitSdlInitFailed;
    }
    // Gamepads (#67, ADR 0048): the game plays without them if SDL cannot have them.
    if (game && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SDL_Log("gamepads: off: %s", SDL_GetError());
    }

    // The window, and its renderer from the gfx backend's choice (gfx_backend.h, #120).
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_IOS)
    // The whole screen without the status bar (SDL hides it for a fullscreen window), and the
    // home indicator dimmed with edge swipes deferred (SDL_HINT_IOS_HIDE_HOME_INDICATOR's
    // default for fullscreen: the first swipe shows it, the second goes home) (#73).
    window_flags |= SDL_WINDOW_FULLSCREEN;
    constexpr bool ios = true;
#else
    // fullscreen = on (#145, desktop.h): the window starts over the whole display.
    if (settings.fullscreen) {
        window_flags |= SDL_WINDOW_FULLSCREEN;
    }
    constexpr bool ios = false;
#endif
    // window.ini (#145): the player's window size and position from the last run, in an
    // interactive run of the game without --window; the position only if it lands on a display.
    const bool remember_window = game && !ios && !options.window_set &&
                                 is_interactive_run(options.game, SDL_GetCurrentVideoDriver());
    std::optional<WindowState> window_state;
    if (remember_window) {
        window_state = load_window_state(data_dir(), kMinWindowSide, kMaxWindowSide);
    }
    // fullscreen_display (#196): a window that starts fullscreen opens on that display.
    const SDL_DisplayID start_display =
        game && !ios && settings.fullscreen ? find_display(settings.fullscreen_display) : 0;
    SDL_Window *window = nullptr;
    if (window_state) {
        std::vector<SDL_Rect> displays;
        int count = 0;
        if (SDL_DisplayID *ids = SDL_GetDisplays(&count)) {
            for (int i = 0; i < count; i++) {
                SDL_Rect r{};
                if (SDL_GetDisplayUsableBounds(ids[i], &r)) {
                    displays.push_back(r);
                }
            }
            SDL_free(ids);
        }
        const bool place = window_state_on_display(*window_state, displays);
        SDL_Log("desktop: window.ini: %dx%d%s%s", window_state->width, window_state->height,
                place ? " at the saved position" : " (the saved position is on no display)",
                window_state->maximized ? ", maximised" : "");
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "MapleThief");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              start_display != 0 ? SDL_WINDOWPOS_CENTERED_DISPLAY(start_display)
                              : place            ? window_state->x
                                                 : SDL_WINDOWPOS_CENTERED);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              start_display != 0 ? SDL_WINDOWPOS_CENTERED_DISPLAY(start_display)
                              : place            ? window_state->y
                                                 : SDL_WINDOWPOS_CENTERED);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_state->width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_state->height);
        SDL_SetNumberProperty(
            props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
            static_cast<Sint64>(window_flags |
                                (window_state->maximized ? SDL_WINDOW_MAXIMIZED : 0)));
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (!place) {
            window_state.reset(); // measured from the window instead
        }
    } else if (start_display != 0) {
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "MapleThief");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(start_display));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              SDL_WINDOWPOS_CENTERED_DISPLAY(start_display));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, options.window_w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, options.window_h);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                              static_cast<Sint64>(window_flags));
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
    } else {
        window = SDL_CreateWindow("MapleThief", options.window_w, options.window_h, window_flags);
    }
    // A player's window never smaller than the game's screen at 1x (ADR 0103): below 480 x 320
    // points the fit fallback drops whole rows and columns of the picture and of every menu's
    // text. A scripted run keeps the size it asked for.
    if (window != nullptr && game && !ios &&
        is_interactive_run(options.game, SDL_GetCurrentVideoDriver())) {
        SDL_SetWindowMinimumSize(window, kScreenWidth, kScreenHeight);
    }
    GfxBackendHost gfx_backend;
    // A shader filter (#88) or the colour step (the colour-blind filter, #158, or the picture
    // adjustments, #187) asks for SDL's GPU renderer; only the game has filters.
    const auto wants_gpu_renderer = [game](const Settings &s) {
        return game && (s.filter != ShaderFilter::kNone || !color_grade(s).is_identity());
    };
    SDL_Renderer *renderer = window != nullptr ? gfx_backend.open(window, settings.gfx_backend,
                                                                  wants_gpu_renderer(settings))
                                               : nullptr;
    // The bundle (#182, game.h's find_bundle): found, or imported now from the player's IPA by
    // the import screen in an interactive run; without one the game cannot start.
    if (game && renderer != nullptr) {
        const BundleChoice choice =
            find_bundle(bundle_search(options.game.bundle_dir, program.dev_bundle_dir));
        std::optional<std::string> bundle;
        if (!choice.dir.empty()) {
            SDL_Log("game: bundle %s (%s)", choice.dir.c_str(), choice.source.c_str());
            bundle = choice.dir;
        } else if (choice.import && is_interactive_run(options.game, SDL_GetCurrentVideoDriver())) {
            bool quit = false;
            bundle =
                run_import_screen(window, renderer, settings, data_dir(), &quit,
                                  [parent = data_location().bundle_parent()](
                                      const std::string &path, const IpaProgress &progress) {
                                      return import_ipa(path, parent, ipa_expected(), progress);
                                  });
            if (quit) {
                gfx_backend.close();
                SDL_DestroyWindow(window);
                SDL_Quit();
                return kExitOk;
            }
        } else {
            SDL_Log("game: %s", choice.why.c_str());
        }
        if (!bundle) {
            gfx_backend.close();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return kExitNoGame;
        }
        options.game.bundle_dir = *bundle;
    }
    if (renderer != nullptr && !safe_start &&
        wants_preset_picker(game, settings, options.game, SDL_GetCurrentVideoDriver())) {
        // The first-launch picker (#144): no preset chosen yet, an interactive run. The choice
        // is written to settings.ini at once, then the game starts with it.
        bool quit = false;
        const Settings before = settings;
        ControlsOffer offer;
        const std::optional<Preset> chosen =
            run_preset_picker(window, renderer, settings, settings_file, &quit, &offer);
        options.game.show_controls_card = offer.show; // the second step (#193)
        options.game.controls_card_device = offer.device;
        if (chosen) {
            apply_preset(&settings, *chosen);
            save_settings(data_dir(), settings);
            // A preset that changes what draws: the renderer again.
            if (settings.gfx_backend != before.gfx_backend ||
                wants_gpu_renderer(settings) != wants_gpu_renderer(before)) {
                gfx_backend.close();
                renderer =
                    gfx_backend.open(window, settings.gfx_backend, wants_gpu_renderer(settings));
            }
        }
        if (quit) {
            gfx_backend.close();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return kExitOk;
        }
    }
    if (renderer == nullptr) {
        SDL_Log("no window or renderer: %s", SDL_GetError());
        gfx_backend.close();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return kExitSdlInitFailed;
    }

    options.game.settings = settings;
    options.game.hd_text = settings.hd_text;
    options.game.render_scale = settings.render_scale;
    if (game && options.save_settings) {
        save_settings(data_dir(), settings);
    }
    // The bindings (#70): the defaults, bindings.ini (or --bindings) over them.
    if (game) {
        options.game.bindings_path = bindings_path(data_dir(), options.bindings_file);
        load_bindings(options.game.bindings_path, &options.game.bindings);
        if (options.save_bindings) {
            save_bindings(options.game.bindings_path, options.game.bindings);
        }
    }
    apply_audio_settings(settings);
    // The window's conveniences (desktop.h, #145): fullscreen (the picker's preset may have
    // changed it) and vsync now; the game applies later changes at once through it.
    Desktop desktop;
    desktop.attach(window, renderer, window_state);
    // Cmd+Ctrl+F (#179): the Window menu's on macOS in desktop mode, the port's otherwise.
    desktop.set_menu_fullscreen_key(kMacosHotkeys &&
                                    settings.fullscreen_mode == FullscreenMode::kDesktop);
    desktop.apply(settings);
    const bool vsync = desktop.vsync();
    if (game) {
        options.game.desktop = &desktop;
    }

    if (game) {
        // The log file's header (#198): everything a report needs is known now.
        LogHeader header;
        header.version = MT_VERSION;
        header.commit = player_log_commit();
        header.os = host_os_text();
        header.gpu = std::string(gfx_backend_name(settings.gfx_backend)) +
                     (gfx_backend.gpu() ? " (mt_gfx on the GPU)" : " (mt_gfx on the CPU)") +
                     ", renderer " + SDL_GetRendererName(renderer);
        header.settings = log_settings_text(settings) + (safe_start ? ", safe start" : "");
        header.ipa_check =
            bundle_hash_check(options.game.bundle_dir, !options.game.slice_path.empty());
        player_log_header(header);
        crash_report_set_header("version: " + header.version + " (" + header.commit +
                                ")\nos: " + header.os + "\ngpu: " + header.gpu + "\n");
    }

    const int sdl_version = SDL_GetVersion();
    int width = 0;
    int height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetCurrentRenderOutputSize(renderer, &pixel_width, &pixel_height);
    SDL_Log("%s, SDL %d.%d.%d, video driver %s, renderer %s, window %dx%d (%dx%d pixels), "
            "%svsync %s, scale mode %s, border #%02X%02X%02X, render scale %s, volume %u%% (music "
            "%u%%, effects %u%%), mute unfocused %s, gapless music %s, %s",
            mt_runtime_version(), SDL_VERSIONNUM_MAJOR(sdl_version),
            SDL_VERSIONNUM_MINOR(sdl_version), SDL_VERSIONNUM_MICRO(sdl_version),
            SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer), width, height, pixel_width,
            pixel_height, desktop.fullscreen() ? "fullscreen, " : "", vsync ? "on" : "off",
            scale_mode_name(settings.scale_mode), settings.border_color.r, settings.border_color.g,
            settings.border_color.b, setting_text(settings, *find_setting("render_scale")).c_str(),
            static_cast<unsigned>(settings.master_volume),
            static_cast<unsigned>(settings.music_volume),
            static_cast<unsigned>(settings.effects_volume), settings.mute_unfocused ? "on" : "off",
            settings.gapless_music ? "on" : "off",
            game ? "the game" : (options.gfx_prototype ? "the gfx prototype" : "the placeholder"));
    SDL_Log("build: %s", build_description().c_str()); // #136, ADR 0134

    // The main thread's priority (#206, ADR 0120): raised in a real-time run of the game, whose
    // loop sleeps between ticks; a fixed-step run never waits and keeps the default.
    if (game) {
        std::string why_not;
        if (options.game.fixed_step_us != 0) {
            why_not = "a fixed-step run";
        } else if (!main_thread_priority_wanted(SDL_getenv("MAPLETHIEF_MAIN_THREAD_PRIORITY"))) {
            why_not = "MAPLETHIEF_MAIN_THREAD_PRIORITY=normal";
        }
        SDL_Log("game: %s", describe_raise(raise_main_thread_priority(why_not)).c_str());
    }

    const int status = game ? run_game(program, window, renderer, vsync, options.game)
                            : run_placeholder(renderer, vsync, options);
    if (remember_window) {
        if (const std::optional<WindowState> state = desktop.state()) {
            save_window_state(data_dir(), *state);
        }
    }

    gfx_backend.close(); // the renderer, then the GPU backend's device
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
