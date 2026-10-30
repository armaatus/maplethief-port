// The game (#58, ADR 0042); see game.h.
#include "game.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "actions.h"
#include "audio_out.h"
#include "clock.h"
#include "focus.h"
#include "gamepad.h"
#include "gfx_backend.h"
#include "input.h"
#include "keyboard.h"
#include "keypad_hide.h"
#include "lifecycle.h"
#include "maplethief/audio.h"
#include "maplethief/cf.h"
#include "maplethief/cg.h"
#include "maplethief/diag.h"
#include "maplethief/gfx.h"
#include "maplethief/gles.h"
#include "maplethief/hle/imports.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "present.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

// A path from a UTF-8 string, and back (on Windows a narrow string is not UTF-8).
fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

std::string choose_bundle_dir(const std::string &flag, const char *env, const char *dev_default,
                              std::string *why) {
    std::string dir;
    const char *source = nullptr;
    if (!flag.empty()) {
        dir = flag;
        source = "--bundle";
    } else if (env != nullptr && *env != '\0') {
        dir = env;
        source = "MAPLETHIEF_BUNDLE_DIR";
    } else if (dev_default != nullptr && *dev_default != '\0') {
        dir = dev_default;
        source = "the build's default";
    } else {
        if (why != nullptr) {
            *why = "no bundle: pass --bundle <dir> or set MAPLETHIEF_BUNDLE_DIR to the extracted "
                   "Payload/MapleThief.app";
        }
        return "";
    }
    std::error_code ec;
    if (!fs::is_directory(utf8_path(dir), ec)) {
        if (why != nullptr) {
            *why = "the bundle " + dir + " (" + source + ") is not a directory";
        }
        return "";
    }
    return dir;
}

bool valid_shot_name(const std::string &name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

namespace {

std::uint32_t be32(const std::uint8_t *p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}
std::uint32_t le32(const std::uint8_t *p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}

constexpr std::uint32_t kFatMagic = 0xCAFEBABEu;  // big-endian on disk
constexpr std::uint32_t kMachMagic = 0xFEEDFACEu; // 32-bit, little-endian on disk
constexpr std::uint32_t kCpuArm = 12;
constexpr std::uint32_t kCpuSubArmV7 = 9;
constexpr std::uint32_t kMaxFatArchs = 64; // the game has 2; real files have a handful
constexpr std::size_t kMachHeaderSize = 28;

// A thin 32-bit ARM Mach-O header of subtype armv7 at p (size bytes available).
bool is_thin_armv7(const std::uint8_t *p, std::size_t size) {
    return size >= kMachHeaderSize && le32(p) == kMachMagic && le32(p + 4) == kCpuArm &&
           le32(p + 8) == kCpuSubArmV7;
}

} // namespace

bool find_armv7_slice(const std::uint8_t *file, std::size_t size, std::size_t *offset,
                      std::size_t *length, std::string *error) {
    auto fail = [&](const char *what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };
    if (file == nullptr || size < 8) {
        return fail("too short for a Mach-O");
    }
    if (is_thin_armv7(file, size)) {
        *offset = 0;
        *length = size;
        return true;
    }
    if (be32(file) != kFatMagic) {
        return fail("neither a fat file nor a thin armv7 Mach-O");
    }
    const std::uint32_t count = be32(file + 4);
    if (count == 0 || count > kMaxFatArchs) {
        return fail("fat header: implausible architecture count");
    }
    if (8 + std::size_t{count} * 20 > size) {
        return fail("fat header: the architecture table runs past the end of the file");
    }
    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint8_t *a = file + 8 + std::size_t{i} * 20;
        if (be32(a) != kCpuArm || (be32(a + 4) & 0x00FFFFFFu) != kCpuSubArmV7) {
            continue;
        }
        const std::size_t off = be32(a + 8);
        const std::size_t len = be32(a + 12);
        if (off > size || len > size - off) {
            return fail("fat header: the armv7 slice lies outside the file");
        }
        if (!is_thin_armv7(file + off, len)) {
            return fail("fat header: the armv7 entry is not a thin armv7 Mach-O");
        }
        *offset = off;
        *length = len;
        return true;
    }
    return fail("no armv7 slice in the fat file");
}

std::vector<std::pair<double, std::uint32_t>> lifecycle_script(const GameOptions &options) {
    std::vector<std::pair<double, std::uint32_t>> script;
    for (const Suspend &s : options.suspends) {
        if (s.background) {
            // What SDL 3.4 sends on iOS for the home gesture and back (SDL_video.c's
            // SDL_OnApplication* handlers, SDL_uikitappdelegate.m).
            for (const std::uint32_t type :
                 {SDL_EVENT_WINDOW_MINIMIZED, SDL_EVENT_WILL_ENTER_BACKGROUND,
                  SDL_EVENT_DID_ENTER_BACKGROUND}) {
                script.emplace_back(s.at, type);
            }
            for (const std::uint32_t type :
                 {SDL_EVENT_WILL_ENTER_FOREGROUND, SDL_EVENT_DID_ENTER_FOREGROUND,
                  SDL_EVENT_WINDOW_RESTORED}) {
                script.emplace_back(s.at + s.seconds, type);
            }
        } else {
            script.emplace_back(s.at, SDL_EVENT_WINDOW_MINIMIZED);
            script.emplace_back(s.at + s.seconds, SDL_EVENT_WINDOW_RESTORED);
        }
    }
    if (options.quit_at >= 0) {
        script.emplace_back(options.quit_at, SDL_EVENT_QUIT);
    }
    std::stable_sort(script.begin(), script.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    return script;
}

std::string data_dir() {
    const char *env = SDL_getenv("MAPLETHIEF_DATA_DIR");
    if (env != nullptr && *env != '\0') {
        return env;
    }
    std::string dir;
    if (char *pref = SDL_GetPrefPath("", "MapleThief")) {
        dir = pref;
        SDL_free(pref);
    }
    return dir;
}

std::string load_settings(const std::string &dir, Settings *settings) {
    if (dir.empty()) {
        SDL_Log("settings: no data directory, so no %s: the defaults", kSettingsFileName);
        return "";
    }
    const std::string path = path_utf8(utf8_path(dir) / kSettingsFileName);
    std::error_code ec;
    const fs::path file = utf8_path(path);
    if (!fs::exists(file, ec)) {
        SDL_Log("settings: no %s: the defaults", path.c_str());
        return path;
    }
    // A settings file is small: refuse to read a big one (parse_settings would ignore it anyway).
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kMaxSettingsBytes) {
        SDL_Log("settings: %s: %s, ignored", path.c_str(),
                ec ? "cannot get its size" : "too large");
        return path;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        SDL_Log("settings: cannot read %s, ignored", path.c_str());
        return path;
    }
    for (const std::string &problem : parse_settings(text, settings)) {
        SDL_Log("settings: %s: %s", path.c_str(), problem.c_str());
    }
    SDL_Log("settings: read %s", path.c_str());
    return path;
}

bool save_settings(const std::string &dir, const Settings &settings) {
    if (dir.empty()) {
        SDL_Log("settings: no data directory: %s not written", kSettingsFileName);
        return false;
    }
    const fs::path file = utf8_path(dir) / kSettingsFileName;
    const std::string path = path_utf8(file);
    std::string old_text;
    std::error_code ec;
    if (fs::exists(file, ec) && fs::file_size(file, ec) <= kMaxSettingsBytes && !ec) {
        std::ifstream in(file, std::ios::binary);
        old_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string text = serialise_settings(settings, old_text);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            SDL_Log("settings: cannot write %s", path_utf8(tmp).c_str());
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec); // replaces the old file (MoveFileEx on Windows)
    if (ec) {
        SDL_Log("settings: cannot replace %s: %s", path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    SDL_Log("settings: wrote %s", path.c_str());
    return true;
}

namespace {

bool read_file(const fs::path &path, std::vector<std::uint8_t> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Seconds the turn hook waits at most, so that it presents at least this often (a view change
// the compositor has not seen), and at most while a UIKit animation runs (about 60 Hz).
constexpr double kRefreshSeconds = 0.25;
constexpr double kAnimationSeconds = 1.0 / 60;
// A quit waits for the turn hook to send applicationWillTerminate: (lifecycle.h); a guest call
// that keeps presenting without returning to the run loop ends the run without it after this.
constexpr Uint64 kQuitGraceNs = 2000000000ull;

// The run's state, shared by the hooks (one thread).
struct Game {
    const GameOptions *options = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool vsync = false;
    Screen screen;
    Presenter presenter;
    PresentPass filter_pass; // the shader filter's (#88), if one is chosen and can run

    bool quit = false;           // the window closed or the user quit
    bool host_ended = false;     // the turn hook ended the run
    bool failed = false;         // a check failed or presentation broke
    bool event_seen = false;     // an SDL event since the last present
    bool told_no_window = false; // logged that the compositor has no window yet
    std::uint64_t turns = 0;
    std::uint64_t presents = 0; // window presents
    std::uint32_t shown_gl = 0; // the GL frame index of the last window present
    double first_turn = -1;     // `now` of the first turn
    double last_present = -1e300;
    std::uint64_t first_lit_present = 0;    // the first window present that is not black
    std::uint64_t first_lit_gl_present = 0; // the first that shows a GL frame that is not black
    std::size_t tap_index = 0;              // options->taps: the next one to deliver
    std::uint32_t tap_phase = 0;            // and its next phase (0 down, drags, up)
    VirtualClock *clock = nullptr;          // the fixed-step mode's clock, or nullptr
    std::vector<Shot> shots;                // options->shots by time
    std::size_t shots_done = 0;
    Uint64 compose_ns = 0; // time spent compositing, and uploading + drawing
    Uint64 draw_ns = 0;
    AudioDevice *audio = nullptr;    // the device stream, or nullptr
    AudioCapture *capture = nullptr; // --audio-capture, or nullptr
    Uint64 capture_start_ns = 0;     // the real-time capture's origin (SDL_GetTicksNS)
    Uint64 audio_start_ns = 0;       // when the device stream was resumed (SDL_GetTicksNS)
    bool logged_placement = false;
    Rect last_rect;
    FocusMute focus_mute;                // mute_unfocused (off under --audio-capture)
    Input input;                         // mouse and touch -> UITouches (input.h, ADR 0047)
    Actions actions{&input};             // controls -> game actions (actions.h, ADR 0048)
    Gamepads gamepads{&actions, &input}; // SDL gamepads -> controls (gamepad.h)
    Keyboard keyboard{&actions};         // SDL keys -> controls (keyboard.h, #68)
    Focus focus{&input};                 // menu navigation over UIKit controls (focus.h)
    KeypadHider keypad;                  // hide_keypad (keypad_hide.h, #71)
    std::uint64_t keypad_changes = 0;    // the hides and shows logged
    std::uint64_t vibrations = 0;        // AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
    Lifecycle lifecycle;                 // the delegate's lifecycle (lifecycle.h, #128)
    std::vector<std::pair<double, std::uint32_t>> script; // --minimise, --background, --quit
    std::size_t script_index = 0;
    Uint64 quit_ns = 0;          // when the quit came (SDL_GetTicksNS), 0: none
    std::uint64_t held_back = 0; // presents not made while hidden
    double hidden_seconds = 0;   // the run's time spent hidden (waiting)

    explicit Game(std::uint32_t scale) : screen(scale) { focus.attach(actions); }
};

// `scripted`: from the script (--minimise, --background, --quit), which the fixed-step mode's
// lifecycle takes where it ignores the window's own events (lifecycle.h).
void handle_event(Game &g, const SDL_Event &e, bool scripted = false) {
    g.event_seen = true;
    // The quits too: the run ends at the next turn hook, after applicationWillTerminate:.
    g.lifecycle.handle(e, scripted);
    if ((e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
         e.type == SDL_EVENT_TERMINATING) &&
        g.quit_ns == 0) {
        g.quit_ns = SDL_GetTicksNS();
    }
    g.focus_mute.on_event(e.type);
    g.input.handle(e);
    g.gamepads.handle(e);
    g.keyboard.handle(e);
}

// The game's vibration (CMSoundPlayer::Vibrate, with its option on): counted and logged at
// debug level. #72 rumbles the active gamepad (g.gamepads.active()) here.
void on_vibrate(void *user) {
    Game &g = *static_cast<Game *>(user);
    g.vibrations++;
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "audio: vibrate (%llu)",
                 static_cast<unsigned long long>(g.vibrations));
}

void pump_events(Game &g) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        handle_event(g, e);
    }
}

// The scripted lifecycle events that are due at run time t, as SDL would send them, through
// handle_event (a window event to the window, as the input handlers expect).
void run_script(Game &g, double t) {
    while (g.script_index < g.script.size() && t >= g.script[g.script_index].first) {
        const std::uint32_t type = g.script[g.script_index++].second;
        SDL_Event e{};
        e.type = type;
        e.common.timestamp = SDL_GetTicksNS();
        if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST) {
            e.window.windowID = SDL_GetWindowID(g.window);
        }
        SDL_Log("game: scripted %s at %.3f s", lifecycle_event_name(type), t);
        handle_event(g, e, true);
    }
}

// While the window is minimised (the app in the background): nothing is presented and the
// guest waits here, as iOS suspends an app, until it is shown again, the run quits or ends
// (--seconds). Real time: the wait is for SDL's events. The fixed-step mode: the virtual clock
// goes straight to the next scripted event (or --seconds), in whole steps, with the capture's
// audio up to it; with nothing scripted to come, only a quit ends the wait.
void wait_hidden(Game &g) {
    const double from = mt_cf_absolute_time();
    SDL_Log("lifecycle: hidden at %.3f s: nothing is presented, the guest waits (window frame "
            "%llu, GL frame %u)",
            from - g.first_turn, static_cast<unsigned long long>(g.presents), g.screen.gl_frame());
    while (g.lifecycle.hidden() && !g.lifecycle.terminating() && !g.quit && !g.failed) {
        const double t = mt_cf_absolute_time() - g.first_turn;
        if (g.options->seconds > 0 && t >= g.options->seconds) {
            SDL_Log("game: --seconds %g: the run ends %.3f s after the first run-loop turn",
                    g.options->seconds, t);
            g.quit = true;
            break;
        }
        double due = g.script_index < g.script.size() ? g.script[g.script_index].first : -1;
        if (g.options->seconds > 0 && (due < 0 || g.options->seconds < due)) {
            due = g.options->seconds;
        }
        if (g.clock != nullptr && due >= 0) {
            const double step = static_cast<double>(g.options->fixed_step_us) / 1e6;
            const double steps = std::max(1.0, std::ceil((due - t) / step - 1e-9));
            g.clock->advance(static_cast<std::uint64_t>(steps) * g.options->fixed_step_us * 1000u);
            if (g.capture != nullptr && !g.capture->render_to(g.clock->elapsed_ns())) {
                g.failed = true;
            }
            pump_events(g);
        } else {
            double wait = 0.1;
            if (g.clock == nullptr && due >= 0) {
                wait = std::clamp(due - t, 0.001, 0.1);
            }
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, static_cast<Sint32>(std::ceil(wait * 1000)))) {
                handle_event(g, e);
            }
            pump_events(g);
            if (g.clock == nullptr && g.capture != nullptr &&
                !g.capture->render_to(SDL_GetTicksNS() - g.capture_start_ns)) {
                g.failed = true;
            }
        }
        run_script(g, mt_cf_absolute_time() - g.first_turn);
    }
    const double to = mt_cf_absolute_time();
    g.hidden_seconds += to - from;
    SDL_Log("lifecycle: %s at %.3f s, %.3f s after it was hidden (window frame %llu, GL frame "
            "%u)",
            g.lifecycle.hidden() ? "the wait ends" : "shown again", to - g.first_turn, to - from,
            static_cast<unsigned long long>(g.presents), g.screen.gl_frame());
}

// The scripted taps and swipes (--tap, --swipe) take real input's path (ADR 0047): a left-button
// down, drags with the button held (a swipe) and an up at the window points that show the screen
// points (#59), handed to handle_event like SDL events, in the turn they are due (so a fixed-step
// run's touches land in the same turns as before). Before the first draw there is no placement
// (no window point shows the screen yet): the touch goes to Input::touch at the screen point.
void deliver_taps(Game &g, double t) {
    const std::vector<Tap> &taps = g.options->taps;
    while (g.tap_index < taps.size()) {
        const Tap &tap = taps[g.tap_index];
        const std::uint32_t last = tap.moves + 1; // phase 0 down, 1..moves drags, last up
        const std::uint32_t phase = g.tap_phase;
        const double due = tap.at + tap.seconds * phase / last;
        if (t < due) {
            break;
        }
        const float f = tap.moves == 0 ? (phase == 0 ? 0.0f : 1.0f)
                                       : static_cast<float>(std::min(phase, tap.moves)) /
                                             static_cast<float>(tap.moves);
        const float x = tap.x + (tap.x2 - tap.x) * f;
        const float y = tap.y + (tap.y2 - tap.y) * f;
        const char *what = phase == 0 ? "touch down" : phase == last ? "touch up" : "touch drag";
        const Placement &pl = g.presenter.placement();
        if (pl.rect.w > 0 && pl.rect.h > 0) {
            float wx = -1;
            float wy = -1;
            screen_to_window_point(g.window, pl, x, y, &wx, &wy);
            SDL_Event e{};
            if (phase == 0 || phase == last) {
                const bool down = phase == 0;
                e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.timestamp = SDL_GetTicksNS();
                e.button.windowID = SDL_GetWindowID(g.window);
                e.button.which = 0;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = down;
                e.button.clicks = 1;
                e.button.x = wx;
                e.button.y = wy;
            } else {
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.timestamp = SDL_GetTicksNS();
                e.motion.windowID = SDL_GetWindowID(g.window);
                e.motion.which = 0;
                e.motion.state = SDL_BUTTON_LMASK;
                e.motion.x = wx;
                e.motion.y = wy;
            }
            const ScreenPoint sp = window_point_to_screen(g.window, pl, wx, wy);
            handle_event(g, e);
            SDL_Log("game: %s at (%g, %g) (window point (%g, %g)), %.2f s", what,
                    static_cast<double>(sp.x), static_cast<double>(sp.y), static_cast<double>(wx),
                    static_cast<double>(wy), t);
        } else {
            const std::uint32_t uphase = phase == 0      ? MT_UIKIT_TOUCH_BEGAN
                                         : phase == last ? MT_UIKIT_TOUCH_ENDED
                                                         : MT_UIKIT_TOUCH_MOVED;
            g.input.touch(InputSource::kScripted, 0, uphase, x, y);
            SDL_Log("game: %s at (%g, %g) (no window placement yet), %.2f s", what,
                    static_cast<double>(x), static_cast<double>(y), t);
        }
        if (phase == last) {
            g.tap_index++;
            g.tap_phase = 0;
        } else {
            g.tap_phase++;
        }
    }
}

// Composites the screen at `now`, with the key pad's opacity at `now` on the game pad (#71).
bool compose(Game &g, mt_cpu *cpu, double now) {
    apply_keypad_opacity(g.keypad.opacity(now));
    return g.screen.compose(cpu, now);
}

// Writes an RGBA8 image (top row first, w * 4 bytes per row) as a PNG.
bool write_png(const std::string &path, const std::uint8_t *rgba, std::uint32_t w,
               std::uint32_t h) {
    SDL_Surface *surface =
        SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_ABGR8888,
                              const_cast<std::uint8_t *>(rgba), static_cast<int>(w * 4));
    const bool ok = surface != nullptr && SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    return ok;
}

// Writes the shots that are due at run time t: the screen composited at `now`, as the device's
// screen would show it (no window, no scaling). Returns false if one could not be written.
bool take_shots(Game &g, mt_cpu *cpu, double now, double t) {
    while (g.shots_done < g.shots.size() && t >= g.shots[g.shots_done].at) {
        const Shot &shot = g.shots[g.shots_done++];
        const std::string path =
            path_utf8(utf8_path(g.options->capture_dir) / (shot.name + ".png"));
        if (!compose(g, cpu, now)) {
            SDL_Log("game: shot %s at %.3f s: the compositor has no window", shot.name.c_str(), t);
            return false;
        }
        if (!write_png(path, g.screen.pixels().data(), g.screen.width(), g.screen.height())) {
            SDL_Log("game: shot %s: cannot write %s: %s", shot.name.c_str(), path.c_str(),
                    SDL_GetError());
            return false;
        }
        if (g.options->shot_gl) {
            const std::string gl_path =
                path_utf8(utf8_path(g.options->capture_dir) / (shot.name + ".gl.png"));
            if (g.screen.gl_frame() == 0 || !write_png(gl_path, g.screen.gl_pixels().data(),
                                                       g.screen.gl_width(), g.screen.gl_height())) {
                SDL_Log("game: shot %s: cannot write its GL frame to %s", shot.name.c_str(),
                        gl_path.c_str());
                return false;
            }
        }
        SDL_Log("game: shot %s at %.3f s (turn %llu, GL frame %u) written to %s", shot.name.c_str(),
                t, static_cast<unsigned long long>(g.turns), g.screen.gl_frame(), path.c_str());
    }
    return true;
}

// Logs the placement when it changes (the first frame, a resize).
void log_placement(Game &g) {
    const Placement &p = g.presenter.placement();
    const Rect &r = p.rect;
    if (g.logged_placement && r.x == g.last_rect.x && r.y == g.last_rect.y &&
        r.w == g.last_rect.w && r.h == g.last_rect.h) {
        return;
    }
    g.logged_placement = true;
    g.last_rect = r;
    int ow = 0;
    int oh = 0;
    SDL_GetCurrentRenderOutputSize(g.renderer, &ow, &oh);
    float dx = 1;
    float dy = 1;
    window_density(g.window, &dx, &dy);
    char k[32] = "";
    if (p.integer_scale != 0) {
        std::snprintf(k, sizeof k, " (%dx)", p.integer_scale);
    } else if (p.mode == ScaleMode::kInteger) {
        std::snprintf(k, sizeof k, " (under 1x: fit)");
    }
    SDL_Log("game: drawable %dx%d pixels (density %gx%g), %s%s: the %ux%u image at (%g, %g) "
            "size %gx%g",
            ow, oh, static_cast<double>(dx), static_cast<double>(dy), scale_mode_name(p.mode), k,
            g.screen.width(), g.screen.height(), static_cast<double>(r.x), static_cast<double>(r.y),
            static_cast<double>(r.w), static_cast<double>(r.h));
}

// The shader filter (#88, ADR 0066): its pass on SDL's GPU renderer, or the scale mode's own
// filter (logged) when the window has no GPU renderer.
void start_filter(Game &g, ShaderFilter filter) {
    if (filter == ShaderFilter::kNone) {
        return;
    }
    if (g.filter_pass.open_filter(g.renderer, filter)) {
        g.presenter.set_stage(g.filter_pass.stage());
        SDL_Log("present: filter %s (%s)", shader_filter_name(filter),
                shader_filter_shader(filter));
    } else {
        SDL_Log("present: filter %s needs SDL's GPU renderer, and the window's renderer is %s: "
                "the image is drawn with the scale mode's filter (%s)",
                shader_filter_name(filter), SDL_GetRendererName(g.renderer),
                scale_mode_name(g.presenter.scale_mode()));
    }
}

// hd_text (#88, ADR 0066): the game's text textures at the render scale, through mt_gfx's
// replacement hook.
void start_hd_text(const GameOptions &o) {
    if (!o.hd_text) {
        return;
    }
    if (o.render_scale <= 1) {
        SDL_Log("text: hd_text is on, but at render scale 1 the text textures are already at the "
                "output's resolution: nothing to do");
        return;
    }
    mt_cg_set_text_scale(o.render_scale);
    mt_gfx_set_replace_hook(mt_cg_text_replace, nullptr);
    SDL_Log("text: hd_text: the game's text textures are drawn at %ux",
            static_cast<unsigned>(o.render_scale));
}

void stop_hd_text() {
    if (mt_cg_text_scale() <= 1) {
        return;
    }
    const mt_cg_text_stats s = mt_cg_get_text_stats();
    SDL_Log("text: hd_text: %llu text draws at %ux (%llu shadows rebuilt, %llu drawn at 1x "
            "only), %llu text textures replaced",
            static_cast<unsigned long long>(s.shadowed_draws),
            static_cast<unsigned>(mt_cg_text_scale()),
            static_cast<unsigned long long>(s.rebuilds),
            static_cast<unsigned long long>(s.unshadowed),
            static_cast<unsigned long long>(s.replacements));
    mt_gfx_set_replace_hook(nullptr, nullptr);
    mt_cg_set_text_scale(1);
}

std::string capture_path(const Game &g, std::uint64_t present, const char *what) {
    char name[64];
    std::snprintf(name, sizeof name, "window_%05llu%s.png",
                  static_cast<unsigned long long>(present), what);
    return path_utf8(utf8_path(g.options->capture_dir) / name);
}

// Composites, uploads, draws and presents the screen; captures and checks as asked.
bool present(Game &g, mt_cpu *cpu, double now) {
    if (!g.lifecycle.presenting()) {
        g.held_back++; // minimised or in the background: no upload, no present (lifecycle.h)
        return true;
    }
    const Uint64 start = SDL_GetTicksNS();
    if (!compose(g, cpu, now)) {
        if (!g.told_no_window) {
            SDL_Log("game: the compositor has no application or window yet");
            g.told_no_window = true;
        }
        return true; // not an error before the launch has a window
    }
    const std::vector<std::uint8_t> &px = g.screen.pixels();
    const std::uint32_t stride = g.screen.width() * 4;
    const Uint64 composed = SDL_GetTicksNS();
    if (!g.presenter.upload(px.data(), stride) || !g.presenter.draw()) {
        return false;
    }
    log_placement(g);
    g.compose_ns += composed - start;
    g.draw_ns += SDL_GetTicksNS() - composed;
    const std::uint64_t index = ++g.presents;
    const bool lit = !is_black(px.data(), g.screen.width(), g.screen.height(), stride);
    const bool first_lit = lit && g.first_lit_present == 0;
    if (first_lit) {
        g.first_lit_present = index;
        SDL_Log("game: window frame %llu (GL frame %u) is the first that is not black",
                static_cast<unsigned long long>(index), g.screen.gl_frame());
    }
    // The first window frame that shows a GL frame that is not black (the game's first GL
    // frame is its fade-in's black quad; the main menu is UIKit over that black).
    const bool first_lit_gl = g.first_lit_gl_present == 0 && g.screen.first_lit_gl_frame() != 0;
    if (first_lit_gl) {
        g.first_lit_gl_present = index;
        SDL_Log("game: window frame %llu shows GL frame %u; the first GL frame that is not black "
                "was %u",
                static_cast<unsigned long long>(index), g.screen.gl_frame(),
                g.screen.first_lit_gl_frame());
    }
    const GameOptions &o = *g.options;
    const bool last = o.frames != 0 && index == o.frames;
    if (!o.capture_dir.empty()) {
        const bool asked =
            std::find(o.capture_at.begin(), o.capture_at.end(), index) != o.capture_at.end();
        if (asked || first_lit || first_lit_gl || last || index == 1) {
            const char *what = first_lit ? "_first_lit" : first_lit_gl ? "_first_lit_gl" : "";
            const std::string path = capture_path(g, index, what);
            if (g.presenter.save_readback(path)) {
                SDL_Log("game: window frame %llu (GL frame %u) read back to %s",
                        static_cast<unsigned long long>(index), g.screen.gl_frame(), path.c_str());
            }
        }
    }
    if (last) {
        std::string report;
        const long bad = g.presenter.check_readback(px.data(), stride, &report);
        SDL_Log("game: read-back of window frame %llu (GL frame %u): %s",
                static_cast<unsigned long long>(index), g.screen.gl_frame(), report.c_str());
        if (bad != 0) {
            g.failed = true;
        }
        // The window must show something: the game's frames, not a black window.
        if (!lit) {
            SDL_Log("game: window frame %llu is black", static_cast<unsigned long long>(index));
            g.failed = true;
        }
    }
    // The controller focus's frame: the window only, after the read-back and captures, so
    // the screen image (shots, goldens) never has it.
    if (const FocusTarget *f = g.focus.highlighted()) {
        g.presenter.draw_highlight(f->x, f->y, f->w, f->h);
    }
    SDL_RenderPresent(g.renderer);
    g.shown_gl = g.screen.gl_frame();
    g.last_present = now;
    g.event_seen = false;
    return true;
}

// Presents if `due` and decides whether the run ends (the window closed, a check failed or
// --frames are done): returns true to end it.
bool present_and_decide(Game &g, mt_cpu *cpu, double now, bool due) {
    if (!g.quit && due && !present(g, cpu, now)) {
        g.failed = true;
    }
    if (g.failed || (g.options->frames != 0 && g.presents >= g.options->frames) ||
        (!g.shots.empty() && g.shots_done == g.shots.size())) {
        g.quit = true;
    }
    if (g.quit) {
        g.host_ended = true;
    }
    return g.quit;
}

// -[EAGLContext presentRenderbuffer:]: the frame goes to the window at once, as on the device.
// The game also presents from inside long calls without returning to the run loop (its loading
// screen animates while it loads), so waiting for the end of the turn would lose those frames.
// The window's events are pumped here too, so that it stays responsive (and can be closed)
// during such calls.
int on_gl_present(void *user, mt_cpu *cpu, const mt_gles_frame *frame) {
    Game &g = *static_cast<Game *>(user);
    g.screen.take_gl_frame(*frame);
    pump_events(g);
    // A quit waits for the turn hook, which sends applicationWillTerminate: first: the guest is
    // inside a call here. Unless it does not return to the run loop in time.
    if (g.lifecycle.terminating() && !g.quit && g.quit_ns != 0 &&
        SDL_GetTicksNS() - g.quit_ns > kQuitGraceNs) {
        SDL_Log("lifecycle: the game did not return to its run loop within %.0f s of the quit: "
                "the run ends without applicationWillTerminate:",
                static_cast<double>(kQuitGraceNs) / 1e9);
        g.quit = true;
    }
    return present_and_decide(g, cpu, mt_cf_absolute_time(), true) ? 1 : 0;
}

// After every turn of the main run loop: delivers the scripted taps and shots, presents if
// something other than a GL frame changed (an event, a running UIKit animation, the periodic
// refresh), then waits, or in the fixed-step mode advances the virtual clock by one step.
int on_turn(void *user, mt_cpu *cpu, double now, double next) {
    Game &g = *static_cast<Game *>(user);
    if (g.turns++ == 0) {
        g.first_turn = now;
        if (g.audio != nullptr) {
            g.audio_start_ns = SDL_GetTicksNS();
            g.audio->resume(); // the game runs: start pulling the mixer
        }
    }
    pump_events(g);
    // The lifecycle (lifecycle.h, ADR 0063): the messages since the last turn; while hidden the
    // guest waits here, then hears that it is active again; after a quit's
    // applicationWillTerminate: the run ends (the game has deleted its state).
    run_script(g, now - g.first_turn);
    g.lifecycle.deliver(cpu, now - g.first_turn);
    if (g.lifecycle.hidden() && !g.lifecycle.terminating() && !g.quit) {
        wait_hidden(g);
        now = mt_cf_absolute_time();
        g.lifecycle.deliver(cpu, now - g.first_turn);
    }
    if (g.lifecycle.terminating()) {
        g.lifecycle.deliver(cpu, now - g.first_turn);
        g.quit = true;
        g.host_ended = true;
        return 1;
    }
    g.actions.turn();       // the synthetic touches of actions without a key code
    g.focus.turn(cpu, now); // the controller focus on UIKit screens (ADR 0051)
    deliver_taps(g, now - g.first_turn);
    // The key pad's hiding (#71): what this turn's input asked for, once no touch is down.
    g.keypad.turn(now, g.input.live());
    if (g.keypad.hides() + g.keypad.shows() != g.keypad_changes) {
        g.keypad_changes = g.keypad.hides() + g.keypad.shows();
        SDL_Log("input: the key pad %s at %.3f s (hide_keypad %s)",
                g.keypad.hidden() ? "hides" : "shows", now - g.first_turn,
                keypad_mode_name(g.keypad.mode()));
    }
    // --seconds: the run ends at the first turn at or after its time (the time limit of a soak).
    if (g.options->seconds > 0 && !g.quit && now - g.first_turn >= g.options->seconds) {
        SDL_Log("game: --seconds %g: the run ends %.3f s after the first run-loop turn",
                g.options->seconds, now - g.first_turn);
        g.quit = true;
    }
    if (!take_shots(g, cpu, now, now - g.first_turn)) {
        g.failed = true;
    }
    // In the fixed-step mode window events do not make a present: what is presented (and
    // composited) then depends only on the virtual clock.
    // Nor does a UIKit animation more often than about 60 times a virtual second (a turn is
    // then far shorter than a display frame).
    const bool fixed = g.clock != nullptr;
    const bool animation_due = (mt_uikit_animation_count(now) > 0 || g.keypad.fading(now)) &&
                               (!fixed || now - g.last_present >= kAnimationSeconds);
    const bool due = g.screen.gl_frame() != g.shown_gl || (g.event_seen && !fixed) ||
                     animation_due || now - g.last_present >= kRefreshSeconds;
    if (present_and_decide(g, cpu, now, due)) {
        return 1;
    }
    if (fixed) {
        // No wait: the next turn's time is one step later, however long this one took. The
        // capture gets the audio of the step, after the turn's calls (and of any GCD sleep
        // since the last turn): the frames up to the virtual time, the remainder carried.
        g.clock->advance(g.options->fixed_step_us * 1000u);
        if (g.capture != nullptr && !g.capture->render_to(g.clock->elapsed_ns())) {
            g.failed = true;
        }
        return 0;
    }
    if (g.capture != nullptr && !g.capture->render_to(SDL_GetTicksNS() - g.capture_start_ns)) {
        g.failed = true;
    }
    // Wait until the run loop has work, an event arrives, or the next refresh is due. With
    // vsync, SDL_RenderPresent has already waited for the display.
    const bool animating = mt_uikit_animation_count(now) > 0 || g.keypad.fading(now);
    const double cap = animating ? kAnimationSeconds : kRefreshSeconds;
    const double wait = std::min(next - mt_cf_absolute_time(), cap);
    if (wait >= 0.002) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, static_cast<Sint32>(wait * 1000))) {
            handle_event(g, e);
        }
    } else if (wait > 0) {
        // On a map the game's timer fires about every millisecond: sleep the fraction too, so
        // the loop does not spin.
        SDL_DelayNS(static_cast<Uint64>(wait * 1e9));
    }
    return 0;
}

const char *outcome_name(std::uint32_t outcome) {
    switch (outcome) {
    case MT_PROCESS_LOADED:
        return "loaded";
    case MT_PROCESS_EXITED:
        return "exited";
    case MT_PROCESS_STOPPED:
        return "stopped";
    case MT_PROCESS_TRAPPED:
        return "trapped";
    default:
        return "?";
    }
}

} // namespace

int run_game(const GameProgram &program, SDL_Window *window, SDL_Renderer *renderer, bool vsync,
             const GameOptions &options) {
    if (program.program == nullptr) {
        SDL_Log("game: this build has no generated code (work/gen: python -m recomp all)");
        return kExitNoGame;
    }
    std::string why;
    const std::string bundle = choose_bundle_dir(
        options.bundle_dir, SDL_getenv("MAPLETHIEF_BUNDLE_DIR"), program.dev_bundle_dir, &why);
    if (bundle.empty()) {
        SDL_Log("game: %s", why.c_str());
        return kExitNoGame;
    }
    // The slice: the bundle's executable (fat: armv6 + armv7) unless --slice names one.
    const fs::path slice_file = options.slice_path.empty() ? utf8_path(bundle) / "MapleThief"
                                                           : utf8_path(options.slice_path);
    std::vector<std::uint8_t> file;
    if (!read_file(slice_file, &file)) {
        SDL_Log("game: cannot read %s", path_utf8(slice_file).c_str());
        return kExitNoGame;
    }
    std::size_t offset = 0;
    std::size_t length = 0;
    if (!find_armv7_slice(file.data(), file.size(), &offset, &length, &why)) {
        SDL_Log("game: %s: %s", path_utf8(slice_file).c_str(), why.c_str());
        return kExitNoGame;
    }
    SDL_Log("game: bundle %s, armv7 slice of %s (%zu bytes at %zu)", bundle.c_str(),
            path_utf8(slice_file).c_str(), length, offset);

    // The data directory (Documents/, the saves, gamecenter.json): MAPLETHIEF_DATA_DIR, which
    // mt_stubs_data_dir reads itself, else SDL's per-user directory for the app (the same
    // directory as the stubs' default on macOS and Windows).
    const char *data_env = SDL_getenv("MAPLETHIEF_DATA_DIR");
    if (data_env == nullptr || *data_env == '\0') {
        const std::string dir = data_dir();
        if (!dir.empty()) {
            mt_stubs_set_data_dir(dir.c_str());
        }
    }
    SDL_Log("game: data directory %s", mt_stubs_data_dir());

    if (!options.capture_dir.empty()) {
        std::error_code ec;
        fs::create_directories(utf8_path(options.capture_dir), ec);
    }

    Game g(options.render_scale);
    g.options = &options;
    g.shots = options.shots;
    std::stable_sort(g.shots.begin(), g.shots.end(),
                     [](const Shot &a, const Shot &b) { return a.at < b.at; });
    // The fixed-step mode's clock: installed before the process loads, so the guest never sees
    // another time (the random seed comes from the clock at boot).
    VirtualClock clock;
    if (options.fixed_step_us != 0) {
        if (!clock.install()) {
            SDL_Log("game: GCD refused the virtual clock");
            return kExitNoGame;
        }
        g.clock = &clock;
        SDL_Log("game: fixed-step mode: virtual time from 2025-01-01T00:00:00Z, %.3f ms per "
                "run-loop turn",
                static_cast<double>(options.fixed_step_us) / 1000.0);
    }
    // Audio: a capture replaces the device (the mixer has one reader); the fixed-step mode's
    // time is virtual, so it plays nothing in real time.
    AudioDevice audio;
    AudioCapture capture;
    if (!options.audio_capture.empty()) {
        if (!capture.open(options.audio_capture)) {
            return kExitNoGame;
        }
        g.capture = &capture;
        g.capture_start_ns = SDL_GetTicksNS();
    } else if (!options.audio) {
        SDL_Log("audio: off (--no-audio)");
    } else if (g.clock != nullptr) {
        SDL_Log("audio: not played in the fixed-step mode (--audio-capture FILE.wav records it)");
    } else if (audio.open()) {
        g.audio = &audio;
    }
    mt_audio_reset_output_stats();
    // A capture records the mix whatever the window's focus; a run that plays does not.
    g.focus_mute = FocusMute(options.mute_unfocused && g.capture == nullptr);
    g.window = window;
    g.renderer = renderer;
    g.vsync = vsync;
    if (!g.presenter.start(renderer, g.screen.width(), g.screen.height())) {
        return kExitSdlInitFailed;
    }
    g.presenter.set_scaling(options.scale_mode, options.border_color);
    start_filter(g, options.filter);
    start_hd_text(options);
    g.input.set_window(window, &g.presenter.placement());
    g.input.set_right_touch(options.right_touch);
    g.keypad = KeypadHider(options.hide_keypad);
    g.input.set_activity_listener(KeypadHider::listener, &g.keypad);
    SDL_Log("input: hide_keypad %s%s", keypad_mode_name(options.hide_keypad),
            options.hide_keypad == KeypadMode::kAlways ? ": the key pad is hidden" : "");
    g.gamepads.open_connected();
    // The lifecycle: the app events by SDL's event watch; in the fixed-step mode only the
    // script's (lifecycle.h).
    g.lifecycle.set_real(g.clock == nullptr);
    g.lifecycle.watch();
    g.script = lifecycle_script(options);
    if (options.right_touch.on) {
        SDL_Log("input: the right mouse button touches (%u, %u)",
                static_cast<unsigned>(options.right_touch.x),
                static_cast<unsigned>(options.right_touch.y));
    }

    // MAPLETHIEF_LANGUAGE=ko plays the Korean build (NSLocale "KR": the terms screen, the
    // Korean help pages and tables; #75). The bundle is the one chosen above.
    mt_cf_config cf = mt_cf_config_default();
    if (mt_cf_config_from_env(&cf) != 0) {
        SDL_Log("game: %s must be en or ko: en", MT_CF_LANGUAGE_ENV);
    }
    cf.bundle_dir = bundle.c_str();
    if (std::strcmp(cf.language, "en") != 0) {
        SDL_Log("game: language %s (%s)", cf.language, MT_CF_LANGUAGE_ENV);
    }
    mt_cf_configure(&cf);
    mt_diag_configure_from_env();
    mt_gles_config gl{};
    gl.present = on_gl_present;
    gl.user = &g;
    gl.render_scale = options.render_scale;
    mt_gles_configure(&gl);

    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = on_turn;
    ui.user = &g;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = program.reservation;
    mt_import_config_from_env(&cfg.imports);
    cfg.app_main = mt_uikit_app_main;
    cfg.app_user = &ui;

    int status = kExitOk;
    mt_audio_set_vibrate_hook(on_vibrate, &g);
    mt_process *p = mt_process_load(&cfg, program.program, file.data() + offset, length);
    if (p == nullptr) {
        SDL_Log("game: the boot could not load the slice");
        status = kExitNoGame;
    } else {
        const Uint64 started = SDL_GetTicksNS();
        const std::uint32_t outcome = mt_process_run(p);
        const mt_process_result *r = mt_process_result_of(p);
        const double seconds = static_cast<double>(SDL_GetTicksNS() - started) / 1e9;
        const mt_gles_stats stats = mt_gles_get_stats();
        SDL_Log("game: %s after %.1f s: %llu turns, %llu window frames (first not black %llu, "
                "first with a GL frame not black %llu), %u GL frames (first with draws %u, first "
                "not black %u)",
                outcome_name(outcome), seconds, static_cast<unsigned long long>(g.turns),
                static_cast<unsigned long long>(g.presents),
                static_cast<unsigned long long>(g.first_lit_present),
                static_cast<unsigned long long>(g.first_lit_gl_present), stats.frames,
                stats.first_frame, g.screen.first_lit_gl_frame());
        if (g.clock != nullptr) {
            SDL_Log("game: virtual time at the end: %.3f s",
                    static_cast<double>(g.clock->elapsed_ns()) / 1e9);
        }
        {
            const mt_gfx_stats gfx = mt_gfx_get_stats();
            SDL_Log("game: GL draws: %u drawn, %u skipped, %u misuses; mt_gfx: %llu triangles, "
                    "%llu lines, %llu unsupported, %llu invalid",
                    stats.draws_drawn, stats.draws_skipped, stats.misuses,
                    static_cast<unsigned long long>(gfx.total.triangles),
                    static_cast<unsigned long long>(gfx.total.lines),
                    static_cast<unsigned long long>(gfx.total.unsupported),
                    static_cast<unsigned long long>(gfx.total.invalid));
        }
        if (g.presents != 0) {
            const mt_gfx_stats gfx = mt_gfx_get_stats();
            const double n = static_cast<double>(g.presents);
            SDL_Log("game: per window frame: compositing %.2f ms, upload and draw %.2f ms; per GL "
                    "frame: %.2f ms in mt_gfx",
                    static_cast<double>(g.compose_ns) / 1e6 / n,
                    static_cast<double>(g.draw_ns) / 1e6 / n,
                    gfx.frames != 0
                        ? static_cast<double>(gfx.total.ns) / 1e6 / static_cast<double>(gfx.frames)
                        : 0.0);
        }
        if (outcome == MT_PROCESS_STOPPED) {
            SDL_Log("game: stop reason: %s", r->stop_reason != nullptr ? r->stop_reason : "?");
        } else if (outcome == MT_PROCESS_EXITED) {
            SDL_Log("game: exit status %d", r->exit_status);
        } else if (outcome == MT_PROCESS_TRAPPED) {
            SDL_Log("game: trap kind %u at pc 0x%08X, detail 0x%08X", r->trap_kind, r->trap_pc,
                    r->trap_detail);
        }
        // The host ends the run from either hook: "UIApplicationMain: the host ended the run
        // loop" or "presentRenderbuffer: the host ended the run".
        const bool clean = (outcome == MT_PROCESS_STOPPED && g.host_ended) ||
                           (outcome == MT_PROCESS_EXITED && r->exit_status == 0);
        if (g.failed) {
            status = kExitCheckMismatch;
        } else if (!clean) {
            status = kExitGuestEnded;
        } else if (options.frames != 0 && g.presents < options.frames) {
            SDL_Log("game: the window closed after %llu of %llu frames",
                    static_cast<unsigned long long>(g.presents),
                    static_cast<unsigned long long>(options.frames));
        }
        mt_process_destroy(p);
    }
    // How long the device stream ran, for the frames it pulled (a soak, #124: they should
    // match within the device's buffering).
    const double streamed =
        g.audio_start_ns != 0 ? static_cast<double>(SDL_GetTicksNS() - g.audio_start_ns) / 1e9 : 0;
    audio.close();
    mt_audio_set_vibrate_hook(nullptr, nullptr);
    mt_audio_set_muted(false);
    if (g.capture != nullptr && !capture.close()) {
        status = status == kExitOk ? kExitCheckMismatch : status;
    }
    {
        const mt_audio_output_stats a = mt_audio_get_output_stats();
        SDL_Log("game: audio: %llu frames rendered in %llu calls (longest %u us), %llu late "
                "device callbacks",
                static_cast<unsigned long long>(a.frames),
                static_cast<unsigned long long>(a.renders), a.longest_render_us,
                static_cast<unsigned long long>(a.late));
        if (streamed > 0) {
            const double played = static_cast<double>(a.frames) / MT_AUDIO_RATE;
            SDL_Log("game: audio: the device stream ran %.3f s and pulled %.3f s of audio "
                    "(%+.3f s)",
                    streamed, played, played - streamed);
        }
        const std::vector<AppMessageRecord> sent = g.lifecycle.delivered();
        if (!sent.empty() || g.held_back != 0 || g.lifecycle.ignored() != 0) {
            SDL_Log("game: lifecycle: %zu messages sent, %.3f s hidden, %llu presents held back "
                    "while hidden, %llu window events ignored",
                    sent.size(), g.hidden_seconds, static_cast<unsigned long long>(g.held_back),
                    static_cast<unsigned long long>(g.lifecycle.ignored()));
        }
        if (g.vibrations != 0) {
            SDL_Log("game: %llu vibrations (no rumble yet: #72)",
                    static_cast<unsigned long long>(g.vibrations));
        }
    }
    g.lifecycle.unwatch();
    g.presenter.stop();
    g.filter_pass.close();
    stop_hd_text();
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    clock.uninstall();
    if (status == kExitOk && g.shots_done < g.shots.size()) {
        SDL_Log("game: the run ended after %zu of %zu shots", g.shots_done, g.shots.size());
        status = kExitCheckMismatch;
    }
    return status;
}

} // namespace maplethief::frontend
