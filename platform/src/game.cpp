// The game (#58, ADR 0042); see game.h.
#include "game.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "actions.h"
#include "audio_out.h"
#include "clock.h"
#include "desktop.h"
#include "focus.h"
#include "frame_stats.h"
#include "gamepad.h"
#include "gfx_backend.h"
#include "gpu_screen.h"
#include "input.h"
#include "keyboard.h"
#include "keypad_hide.h"
#include "lifecycle.h"
#include "maplethief/audio.h"
#include "maplethief/cashshop.h"
#include "maplethief/cf.h"
#include "maplethief/cg.h"
#include "maplethief/diag.h"
#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/gles.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"
#include "pacing.h"
#include "perf_overlay.h"
#include "present.h"
#include "price_line.h"
#include "remap.h"
#include "render_scale.h"
#include "rumble.h"
#include "save_tools.h"

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

bool perf_overlay_allowed(const GameOptions &options) {
    return options.fixed_step_us == 0 && options.shots.empty() && options.capture_dir.empty();
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
        if (s.unfocus) {
            script.emplace_back(s.at, SDL_EVENT_WINDOW_FOCUS_LOST);
            script.emplace_back(s.at + s.seconds, SDL_EVENT_WINDOW_FOCUS_GAINED);
        } else if (s.background) {
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
#if !defined(SDL_PLATFORM_IOS)
    // Portable mode (#148, ADR 0078): portable.txt next to the executable (or MapleThief.app).
    if (const char *base = SDL_GetBasePath(); base != nullptr) {
        if (std::string dir = portable_data_dir(portable_base_dir(base)); !dir.empty()) {
            std::error_code ec;
            fs::create_directories(utf8_path(dir), ec);
            return dir;
        }
    }
#endif
    std::string dir;
    if (char *pref = SDL_GetPrefPath("", "MapleThief")) {
        dir = pref;
        SDL_free(pref);
    }
    return dir;
}

std::string documents_dir_override(bool ios) {
    const char *env = SDL_getenv("MAPLETHIEF_DATA_DIR");
    if (!ios || (env != nullptr && *env != '\0')) {
        return std::string();
    }
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return docs != nullptr ? std::string(docs) : std::string();
}

std::string load_settings(const std::string &dir, Settings *settings, bool *exists) {
    if (exists != nullptr) {
        *exists = false;
    }
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
    if (exists != nullptr) {
        *exists = true;
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
    SDL_Log("settings: read %s (preset %s)", path.c_str(),
            settings->preset ? preset_name(*settings->preset) : "none chosen yet");
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
    // GPU presentation (#130, ADR 0068): the GL frame stays on the GPU and UIKit's views are an
    // overlay texture. Screen then only serves the downloads of the test aids (shots, checks).
    GpuScreen gpu;
    bool gpu_present = false;
    // Whether each present finds out if the screen is black (the first-lit log and captures).
    // On the GPU that is a download, so only with --capture.
    bool track_lit = true;
    std::vector<std::uint8_t> gpu_px, gpu_gl; // GPU presentation's downloads

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
    PacedClock *paced = nullptr;  // real time with pacing = display (pacing.h), or nullptr
    bool locked = false;          // the paced clock is locked to the display
    std::int64_t tick_ms = 0;     // the game's tick interval (0: not read yet)
    Uint64 last_present_host = 0; // when the last window present returned (SDL_GetTicksNS)
    PresentHold hold;             // locked: the present point after each wake (pacing.h)
    std::vector<Shot> shots;      // options->shots by time
    std::size_t shots_done = 0;
    Uint64 compose_ns = 0; // time spent compositing, and uploading + drawing
    Uint64 draw_ns = 0;
    Uint64 render_present_ns = 0; // in SDL_RenderPresent (with vsync: waiting for the display)
    Uint64 take_ns = 0;           // taking the GL frames (the read-back, or the GPU copy)
    std::uint64_t gl_takes = 0;
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
    Rumble rumble{&gamepads};            // vibrate, hero damage, level-up -> the pad (rumble.h)
    Keyboard keyboard{&actions};         // SDL keys -> controls (keyboard.h, #68)
    Focus focus{&input};                 // menu navigation over UIKit controls (focus.h)
    Remap remap{&actions, &gamepads};    // the remap overlay (remap.h, #70)
    std::vector<std::uint8_t> overlaid;  // the screen with the overlay drawn over it
    KeypadHider keypad;                  // hide_keypad (keypad_hide.h, #71)
    std::uint64_t keypad_changes = 0;    // the hides and shows logged
    Lifecycle lifecycle;                 // the delegate's lifecycle (lifecycle.h, #128)
    std::vector<std::pair<double, std::uint32_t>> script; // --minimise, --background, --quit
    std::size_t script_index = 0;
    Uint64 quit_ns = 0;          // when the quit came (SDL_GetTicksNS), 0: none
    std::uint64_t held_back = 0; // presents not made while hidden
    double hidden_seconds = 0;   // the run's time spent hidden (waiting)
    FrameStats stats;            // frame telemetry (frame_stats.h, ADR 0071)
    PerfCsv csv;                 // --perf-csv
    PerfOverlay perf;            // the frame-time overlay (perf_overlay.h)
    // render_scale = auto and the frame-budget guard (#152, render_scale.h): not in a run that
    // writes images. A new scale waits for the next run-loop turn.
    RenderScaleGovernor scale_gov;
    bool govern = false;
    std::uint32_t pending_scale = 0; // 0: none
    std::uint32_t scale_changes = 0;
    std::string notice; // the guard's warning in the window, until notice_until (SDL ns)
    Uint64 notice_until = 0;
    // The desktop conveniences (desktop.h, #145): the settings in effect now (changed live by
    // the fullscreen hotkeys and #89's menu), the wheel's notches, a screenshot asked for, the
    // window's last frame to redraw while paused unfocused.
    Desktop *desktop = nullptr;
    Settings settings;
    WheelSteps wheel;
    bool screenshot_wanted = false;
    bool redraw_paused = false;
    std::uint64_t screenshots = 0;
    std::uint64_t wheel_steps = 0;

    explicit Game(std::uint32_t scale) : screen(scale) { focus.attach(actions); }
};

// mt_gfx_submit's running total (frame_stats.h moves it from the guest's time to mt_gfx's).
std::uint64_t gfx_total_ns() { return mt_gfx_get_stats().total.ns; }

// The refresh rate of the window's display (0: unknown, e.g. the dummy driver).
double display_refresh_hz(SDL_Window *window) {
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (m == nullptr) {
        return 0;
    }
    if (m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0) {
        return static_cast<double>(m->refresh_rate_numerator) / m->refresh_rate_denominator;
    }
    return static_cast<double>(m->refresh_rate);
}

// The game's tick interval in ms: CMTimer's frame interval, the i64 at 0x11A478
// (docs/re/timing-threading.md: 10 at boot, then 65, 50 or 40 from the game speed). 0 when the
// guest's image does not map it (a test's guest).
constexpr std::uint32_t kFrameIntervalGlobal = 0x11A478;
std::int64_t game_tick_ms(const mt_cpu *cpu) {
    if (cpu == nullptr || cpu->mem == nullptr ||
        std::uint64_t{kFrameIntervalGlobal} + 8 > cpu->mem_size) {
        return 0;
    }
    if (const mt_mem *mem = mt_mem_from_cpu(cpu)) {
        std::uint32_t n = 0;
        const mt_segment *segs = mt_mem_segments(mem, &n);
        bool mapped = false;
        for (std::uint32_t i = 0; i < n && !mapped; i++) {
            mapped = kFrameIntervalGlobal >= segs[i].vmaddr &&
                     kFrameIntervalGlobal + 8ull <= std::uint64_t{segs[i].vmaddr} + segs[i].vmsize;
        }
        if (!mapped) {
            return 0;
        }
    }
    std::int64_t v = 0;
    std::memcpy(&v, cpu->mem + kFrameIntervalGlobal, 8);
    return v;
}

// The paced clock's host (pacing.h): SDL's monotonic clock and precise sleep.
std::uint64_t host_now_ns(void *) { return SDL_GetTicksNS(); }
void host_sleep_ns(void *, std::uint64_t ns) { SDL_DelayPrecise(ns); }

// A host hook's span for the frame telemetry: the guest ran until it started and runs again
// when it returns.
class HostCall {
  public:
    explicit HostCall(FrameStats &stats) : stats_(stats) { stats_.enter_host(SDL_GetTicksNS()); }
    ~HostCall() { stats_.leave_host(SDL_GetTicksNS()); }
    HostCall(const HostCall &) = delete;
    HostCall &operator=(const HostCall &) = delete;

  private:
    FrameStats &stats_;
};

// The index of the last GL frame taken.
std::uint32_t gl_frame(const Game &g) {
    return g.gpu_present ? g.gpu.gl_frame() : g.screen.gl_frame();
}

// The settings that apply at once (#145's rows with Applies::kAtOnce; #89's menu calls this after
// it changed `s` with apply_setting or step_setting): fullscreen and vsync at the window
// (desktop.h), pause_unfocused at the lifecycle (a change while unfocused pauses or resumes at
// once). `s` becomes the settings in effect; the caller writes the file (save_settings).
void apply_settings_at_once(Game &g, const Settings &s) {
    g.settings = s;
    if (g.desktop != nullptr) {
        g.desktop->apply(s);
        if (g.desktop->vsync() != g.vsync) {
            g.vsync = g.desktop->vsync();
            g.stats.set_vsync(g.vsync);
        }
    }
    g.lifecycle.set_pause_unfocused(s.pause_unfocused);
}

// `fullscreen` as the window now is (a hotkey asked for it, or the system changed it: macOS's
// green button), into the settings and settings.ini. The preset moves to custom if it set the
// other value (ADR 0070). The window's resize re-checks render_scale = auto at the next window
// frame (govern_render_scale gets the new placement).
void set_fullscreen_setting(Game &g, bool on) {
    if ((g.settings.fullscreen == Fullscreen::kDesktop) == on) {
        return;
    }
    Settings s = g.settings;
    apply_setting(&s, "fullscreen", on ? "desktop" : "off", nullptr);
    apply_settings_at_once(g, s);
    save_settings(data_dir(), g.settings);
}

// The mouse wheel (#145): in a menu, a notch is the gamepad's Up or Down (lists, shops and the
// UIKit screens' focus move a row); nothing on the field. At most three steps an event.
void wheel(Game &g, const SDL_MouseWheelEvent &w) {
    if (w.which == SDL_TOUCH_MOUSEID || w.which == SDL_PEN_MOUSEID) {
        return;
    }
    const int steps = g.wheel.add(w.y);
    if (steps == 0 || g.actions.context() != Context::kMenu) {
        return;
    }
    const Control c{ControlKind::kDirection,
                    static_cast<int>(steps > 0 ? Direction::kUp : Direction::kDown)};
    for (int i = 0; i < std::min(std::abs(steps), 3); i++) {
        g.actions.control(InputSource::kMouse, c, true);
        g.actions.control(InputSource::kMouse, c, false);
        g.wheel_steps++;
    }
}

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
    if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED && g.stats.started()) {
        g.stats.set_refresh(display_refresh_hz(g.window));
        SDL_Log("perf: the window moved to a %.2f Hz display", g.stats.refresh_hz());
    }
    if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        e.type == SDL_EVENT_WINDOW_EXPOSED) {
        g.redraw_paused = true; // the last frame again, if paused unfocused (wait_hidden)
    }
    // The desktop's hotkeys and screenshot buttons (#145), before every input handler (they
    // never reach the game), and the cursor's auto-hide.
    if (g.desktop != nullptr) {
        const Desktop::Request r = g.desktop->handle(e);
        if (r.toggle_fullscreen) {
            set_fullscreen_setting(g, g.settings.fullscreen != Fullscreen::kDesktop);
        }
        if (r.fullscreen_changed) {
            set_fullscreen_setting(g, *r.fullscreen_changed);
        }
        if (r.screenshot) {
            g.screenshot_wanted = true;
        }
        if (r.consumed) {
            return;
        }
    }
    // The remap overlay (#70) takes what opens it and, while open, every input event; F3 is
    // the frame-time overlay's (#137).
    if (!g.remap.handle(e, static_cast<double>(SDL_GetTicksNS()) / 1e9) && !g.perf.handle(e)) {
        g.input.handle(e);
        g.gamepads.handle(e);
        g.keyboard.handle(e);
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            wheel(g, e.wheel);
        }
    }
    g.remap.track(e);
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g.rumble.stop();
    }
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
            from - g.first_turn, static_cast<unsigned long long>(g.presents), gl_frame(g));
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
            // Paused because the window lost focus (pause_unfocused, #145): it is still on
            // screen, so a resize or an expose shows the last frame again (no guest call).
            if (g.redraw_paused && g.lifecycle.paused_visible() && g.presenter.draw()) {
                SDL_RenderPresent(g.renderer);
            }
            g.redraw_paused = false;
            if (g.clock == nullptr && g.capture != nullptr &&
                !g.capture->render_to(SDL_GetTicksNS() - g.capture_start_ns)) {
                g.failed = true;
            }
        }
        run_script(g, mt_cf_absolute_time() - g.first_turn);
    }
    const double to = mt_cf_absolute_time();
    g.hidden_seconds += to - from;
    g.stats.gap(SDL_GetTicksNS(), gfx_total_ns()); // a pause, not a slow frame
    SDL_Log("lifecycle: %s at %.3f s, %.3f s after it was hidden (window frame %llu, GL frame "
            "%u)",
            g.lifecycle.hidden() ? "the wait ends" : "shown again", to - g.first_turn, to - from,
            static_cast<unsigned long long>(g.presents), gl_frame(g));
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

// Composites the screen at `now`, with the key pad's opacity at `now` on the game pad (#71):
// on the CPU (Screen, FULL), or with GPU presentation into the screen texture (overlay mode).
bool compose(Game &g, mt_cpu *cpu, double now) {
    apply_keypad_opacity(g.keypad.opacity(now));
    return g.gpu_present ? g.gpu.compose_uikit(cpu, now) : g.screen.compose(cpu, now);
}

// GPU presentation: the composed screen texture into g.gpu_px.
bool read_gpu_screen(Game &g) { return g.gpu.read_screen(&g.gpu_px); }

// GPU presentation: the GL frame shown (as Screen keeps it) into g.screen, for the test aids.
bool read_gpu_gl(Game &g) {
    if (!g.gpu.read_gl(&g.gpu_gl)) {
        return false;
    }
    g.screen.set_gl_frame(g.gpu.gl_frame(), g.gpu_gl.data(), g.gpu.gl_width(), g.gpu.gl_height());
    return true;
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
        // With GPU presentation the shot is the screen texture as the GPU composed it, read
        // back: what the window shows.
        const std::vector<std::uint8_t> &px = g.gpu_present ? g.gpu_px : g.screen.pixels();
        if ((g.gpu_present && !read_gpu_screen(g)) ||
            !write_png(path, px.data(), g.screen.width(), g.screen.height())) {
            SDL_Log("game: shot %s: cannot write %s: %s", shot.name.c_str(), path.c_str(),
                    SDL_GetError());
            return false;
        }
        if (g.options->shot_gl) {
            const std::string gl_path =
                path_utf8(utf8_path(g.options->capture_dir) / (shot.name + ".gl.png"));
            if (gl_frame(g) == 0 || (g.gpu_present && !read_gpu_gl(g)) ||
                !write_png(gl_path, g.screen.gl_pixels().data(), g.screen.gl_width(),
                           g.screen.gl_height())) {
                SDL_Log("game: shot %s: cannot write its GL frame to %s", shot.name.c_str(),
                        gl_path.c_str());
                return false;
            }
        }
        SDL_Log("game: shot %s at %.3f s (turn %llu, GL frame %u) written to %s", shot.name.c_str(),
                t, static_cast<unsigned long long>(g.turns), gl_frame(g), path.c_str());
    }
    return true;
}

// The player's screenshot (#145: F12, the pad's Share button or R3 + Back): the screen composed
// at `now`, as --shot writes it, to <data dir>/screenshots/ (desktop.h); never into --capture.
void take_screenshot(Game &g, mt_cpu *cpu, double now) {
    if (!compose(g, cpu, now)) {
        SDL_Log("desktop: screenshot: the compositor has no window yet");
        return;
    }
    if (g.gpu_present && !read_gpu_screen(g)) {
        SDL_Log("desktop: screenshot: the GPU's screen cannot be read back");
        return;
    }
    const std::vector<std::uint8_t> &px = g.gpu_present ? g.gpu_px : g.screen.pixels();
    const std::string path =
        write_screenshot(data_dir(), px.data(), g.screen.width(), g.screen.height());
    if (!path.empty()) {
        g.screenshots++;
        SDL_Log("desktop: screenshot (%ux%u, GL frame %u) written to %s", g.screen.width(),
                g.screen.height(), gl_frame(g), path.c_str());
    }
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
    char safe[160] = "";
    const Insets &in = g.presenter.insets();
    if (in.any()) {
        // The safe area (#73, ADR 0055): insets in window points, the area in pixels.
        std::snprintf(safe, sizeof safe,
                      ", safe area insets %g,%g,%g,%g points (T,L,B,R%s): %gx%g at (%g, %g)",
                      static_cast<double>(in.top), static_cast<double>(in.left),
                      static_cast<double>(in.bottom), static_cast<double>(in.right),
                      g.options->safe_area ? ", forced" : "", static_cast<double>(p.safe.w),
                      static_cast<double>(p.safe.h), static_cast<double>(p.safe.x),
                      static_cast<double>(p.safe.y));
    }
    SDL_Log("game: drawable %dx%d pixels (density %gx%g), %s%s: the %ux%u image at (%g, %g) "
            "size %gx%g%s",
            ow, oh, static_cast<double>(dx), static_cast<double>(dy), scale_mode_name(p.mode), k,
            g.screen.width(), g.screen.height(), static_cast<double>(r.x), static_cast<double>(r.y),
            static_cast<double>(r.w), static_cast<double>(r.h), safe);
}

// How the screen reaches the window (#130, ADR 0068). GPU presentation when mt_gfx draws on the
// GPU and the window's renderer is SDL's GPU renderer (GfxBackendHost puts them on one device),
// unless MAPLETHIEF_GPU_READBACK=1 asks for #120's read-back path; otherwise the CPU composite
// is uploaded. Leaves the Presenter without a texture (logged) if neither can start.
void start_presentation(Game &g, SDL_Renderer *renderer, const GameOptions &options) {
    const char *readback = SDL_getenv("MAPLETHIEF_GPU_READBACK");
    const bool asked_readback = readback != nullptr && std::strcmp(readback, "1") == 0;
    const bool gpu = mt_gfx_gpu_installed() != 0 && SDL_GetGPURendererDevice(renderer) != nullptr;
    if (gpu && !asked_readback) {
        if (g.presenter.start(renderer, g.screen.width(), g.screen.height(), true) &&
            g.gpu.start(renderer, g.presenter.texture(), g.screen.width(), g.screen.height())) {
            g.gpu_present = true;
            g.track_lit = !options.capture_dir.empty();
            SDL_Log("present: the GL frame stays on the GPU: UIKit's views are composited as an "
                    "overlay over it (read-backs only for shots%s)",
                    g.track_lit ? ", captures and checks" : " and checks");
            return;
        }
        SDL_Log("present: GPU presentation cannot start: the GL frames are read back");
        g.gpu.stop();
    } else if (gpu) {
        SDL_Log("present: MAPLETHIEF_GPU_READBACK=1: the GL frames are read back and composited "
                "on the CPU");
    }
    g.presenter.start(renderer, g.screen.width(), g.screen.height());
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
// replacement hook. Again when render_scale = auto raises the scale (#152); a lower one keeps
// the text scale it had.
void start_hd_text(const GameOptions &o, std::uint32_t scale) {
    if (!o.hd_text) {
        return;
    }
    if (scale <= 1) {
        SDL_Log("text: hd_text is on, but at render scale 1 the text textures are already at the "
                "output's resolution: nothing to do");
        return;
    }
    mt_cg_set_text_scale(scale);
    mt_gfx_set_replace_hook(mt_cg_text_replace, nullptr);
    SDL_Log("text: hd_text: the game's text textures are drawn at %ux",
            static_cast<unsigned>(scale));
}

void stop_hd_text() {
    if (mt_cg_text_scale() <= 1) {
        return;
    }
    const mt_cg_text_stats s = mt_cg_get_text_stats();
    SDL_Log("text: hd_text: %llu text draws at %ux (%llu shadows rebuilt, %llu drawn at 1x "
            "only), %llu text textures replaced",
            static_cast<unsigned long long>(s.shadowed_draws),
            static_cast<unsigned>(mt_cg_text_scale()), static_cast<unsigned long long>(s.rebuilds),
            static_cast<unsigned long long>(s.unshadowed),
            static_cast<unsigned long long>(s.replacements));
    mt_gfx_set_replace_hook(nullptr, nullptr);
    mt_cg_set_text_scale(1);
}

// How long the frame-budget guard's notice stays in the window.
constexpr Uint64 kNoticeNs = 8000000000ull;

// A new render scale while the game runs (render_scale = auto, #152, ADR 0080): mt_gles remakes
// the renderbuffers' targets (their frames resampled), then the screen image, the Presenter's
// texture and GPU presentation's textures follow. Between two run-loop turns. False (logged) if
// a texture cannot be made at the new size: the old scale is put back.
bool apply_render_scale(Game &g, std::uint32_t n) {
    const std::uint32_t old = g.screen.width() / MT_UIKIT_SCREEN_WIDTH;
    if (n == old) {
        return true;
    }
    const Uint64 t0 = SDL_GetTicksNS();
    auto resize = [&g](std::uint32_t scale) {
        mt_gles_set_render_scale(scale);
        g.screen.set_scale(scale);
        const std::uint32_t w = g.screen.width();
        const std::uint32_t h = g.screen.height();
        return g.presenter.start(g.renderer, w, h, g.gpu_present) &&
               (!g.gpu_present || g.gpu.resize(g.presenter.texture(), w, h));
    };
    if (!resize(n)) {
        SDL_Log("render: render scale %ux cannot be set: back to %ux", static_cast<unsigned>(n),
                static_cast<unsigned>(old));
        g.scale_gov.failed(old);
        if (!resize(old)) {
            g.failed = true;
        }
        return false;
    }
    if (n > 1 && n > mt_cg_text_scale()) {
        start_hd_text(*g.options, n);
    }
    g.scale_changes++;
    g.event_seen = true; // show it at once
    SDL_Log("render: render scale %ux -> %ux (%s), %.1f ms", static_cast<unsigned>(old),
            static_cast<unsigned>(n), g.scale_gov.reason().c_str(),
            static_cast<double>(SDL_GetTicksNS() - t0) / 1e6);
    return true;
}

// render_scale = auto and the frame-budget guard (#152): after a window frame, the governor
// gets the image's place (a resize) and the last second's telemetry.
void govern_render_scale(Game &g) {
    if (!g.govern) {
        return;
    }
    const Uint64 now_ns = SDL_GetTicksNS();
    const double now = static_cast<double>(now_ns) / 1e9;
    const Rect &r = g.presenter.placement().rect;
    std::uint32_t n = g.scale_gov.set_cap(render_scale_cap(r.w, r.h), now);
    if (n == 0) {
        n = g.scale_gov.update(g.stats.recent(), now);
    }
    if (n != 0) {
        g.pending_scale = n;
    }
    std::string warning;
    if (g.scale_gov.take_warning(&warning)) {
        SDL_Log("render: %s (%s)", warning.c_str(), g.scale_gov.reason().c_str());
        g.notice = warning;
        g.notice_until = now_ns + kNoticeNs;
    }
    g.perf.set_note(g.scale_gov.status());
}

std::string capture_path(const Game &g, std::uint64_t present, const char *what) {
    char name[64];
    std::snprintf(name, sizeof name, "window_%05llu%s.png",
                  static_cast<unsigned long long>(present), what);
    return path_utf8(utf8_path(g.options->capture_dir) / name);
}

// The screen to show: the composited one, with the remap overlay over it while it is open (the
// overlay is the window's only: --shot writes the screen without it).
const std::vector<std::uint8_t> &shown_pixels(Game &g) {
    if (!g.remap.is_open()) {
        return g.screen.pixels();
    }
    g.overlaid = g.screen.pixels();
    g.overlaid.resize(std::size_t{g.screen.width()} * g.screen.height() * 4, 0);
    g.remap.draw(g.overlaid.data(), g.screen.width(), g.screen.height(),
                 static_cast<int>(g.screen.width() / 480));
    return g.overlaid;
}

// GPU presentation's remap overlay (#70 with #130, ADR 0068's addendum): the screen texture
// composed again from the last compose's layers, the overlay drawn over UIKit's layer (which is
// premultiplied and transparent in the GL slot, so the overlay's backdrop also dims the GL
// frame). No guest call, so run_remap can redraw it while it holds the turn; the next compose
// (a present, a shot) is without it again. False (logged) if it cannot be drawn.
bool draw_remap_gpu(Game &g) {
    const std::uint32_t w = g.screen.width();
    const std::uint32_t h = g.screen.height();
    const bool ok = g.gpu.compose_over(
        [&g, w, h](std::uint8_t *rgba) { g.remap.draw(rgba, w, h, static_cast<int>(w / 480)); });
    if (!ok) {
        SDL_Log("game: the remap overlay cannot be drawn on the GPU's screen");
    }
    return ok;
}

// GPU presentation's end check (#130, ADR 0068), with g.gpu_px the screen texture at `now`:
// the CPU's FULL composite at the same time, from the GL frame the GPU shows (downloaded),
// must match it within ADR 0060's tolerance (both with the remap overlay while it is open).
// Returns false (logged) if it does not.
bool check_gpu_layers(Game &g, mt_cpu *cpu, double now, std::uint64_t index) {
    if (gl_frame(g) != 0 && !read_gpu_gl(g)) {
        return false;
    }
    if (!g.screen.compose(cpu, now)) {
        SDL_Log("game: GPU presentation check: the compositor has no window");
        return false;
    }
    const ScreenDiff d =
        diff_screens(g.gpu_px.data(), shown_pixels(g).data(), g.screen.width(), g.screen.height());
    SDL_Log("game: GPU presentation of window frame %llu (GL frame %u) against the CPU's "
            "composite: %llu of %llu pixels beyond +-%d (at most %g%%), max channel delta %d: %s",
            static_cast<unsigned long long>(index), gl_frame(g),
            static_cast<unsigned long long>(d.beyond), static_cast<unsigned long long>(d.pixels),
            ScreenDiff::kChannel, 100 * ScreenDiff::kRatio, d.max_delta,
            d.within() ? "within tolerance" : "BEYOND TOLERANCE");
    return d.within();
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
    // The screen image: the CPU's composite (with the remap overlay over it while it is
    // open), or with GPU presentation the screen texture's pixels, downloaded only when a
    // check needs them (below); the remap overlay is then a layer of the screen texture.
    if (g.gpu_present && g.remap.is_open() && !draw_remap_gpu(g)) {
        return false;
    }
    const std::vector<std::uint8_t> &px = g.gpu_present ? g.gpu_px : shown_pixels(g);
    const std::uint32_t stride = g.screen.width() * 4;
    const Uint64 composed = SDL_GetTicksNS();
    if ((!g.gpu_present && !g.presenter.upload(px.data(), stride)) || !g.presenter.draw()) {
        return false;
    }
    log_placement(g);
    const Uint64 drawn_image = SDL_GetTicksNS();
    g.compose_ns += composed - start;
    g.draw_ns += drawn_image - composed;
    g.stats.add(Phase::kCompose, composed - start);
    g.stats.add(Phase::kDraw, drawn_image - composed);
    const std::uint64_t index = ++g.presents;
    const GameOptions &o = *g.options;
    const bool last = o.frames != 0 && index == o.frames;
    bool lit = false;
    if (!g.gpu_present) {
        lit = !is_black(px.data(), g.screen.width(), g.screen.height(), stride);
    } else if (last || (g.track_lit && g.first_lit_present == 0)) {
        if (!read_gpu_screen(g)) {
            return false;
        }
        lit = !is_black(px.data(), g.screen.width(), g.screen.height(), stride);
    }
    const bool first_lit = lit && g.first_lit_present == 0;
    if (first_lit) {
        g.first_lit_present = index;
        SDL_Log("game: window frame %llu (GL frame %u) is the first that is not black",
                static_cast<unsigned long long>(index), gl_frame(g));
    }
    // The first window frame that shows a GL frame that is not black (the game's first GL
    // frame is its fade-in's black quad; the main menu is UIKit over that black).
    const bool first_lit_gl = g.first_lit_gl_present == 0 && g.screen.first_lit_gl_frame() != 0;
    if (first_lit_gl) {
        g.first_lit_gl_present = index;
        SDL_Log("game: window frame %llu shows GL frame %u; the first GL frame that is not black "
                "was %u",
                static_cast<unsigned long long>(index), gl_frame(g), g.screen.first_lit_gl_frame());
    }
    if (!o.capture_dir.empty()) {
        const bool asked =
            std::find(o.capture_at.begin(), o.capture_at.end(), index) != o.capture_at.end();
        if (asked || first_lit || first_lit_gl || last || index == 1) {
            const char *what = first_lit ? "_first_lit" : first_lit_gl ? "_first_lit_gl" : "";
            const std::string path = capture_path(g, index, what);
            if (g.presenter.save_readback(path)) {
                SDL_Log("game: window frame %llu (GL frame %u) read back to %s",
                        static_cast<unsigned long long>(index), gl_frame(g), path.c_str());
            }
        }
    }
    if (last) {
        std::string report;
        const long bad = g.presenter.check_readback(px.data(), stride, &report);
        SDL_Log("game: read-back of window frame %llu (GL frame %u): %s",
                static_cast<unsigned long long>(index), gl_frame(g), report.c_str());
        if (bad != 0) {
            g.failed = true;
        }
        if (g.gpu_present && !check_gpu_layers(g, cpu, now, index)) {
            g.failed = true;
        }
        // The window must show something: the game's frames, not a black window.
        if (!lit) {
            SDL_Log("game: window frame %llu is black", static_cast<unsigned long long>(index));
            g.failed = true;
        }
    }
    // The controller focus's frame: the window only, after the read-back and captures, so
    // the screen image (shots, goldens) never has it; not over the remap overlay (#70).
    const FocusTarget *f = g.remap.is_open() ? nullptr : g.focus.highlighted();
    const Uint64 overlays = SDL_GetTicksNS();
    if (f != nullptr) {
        g.presenter.draw_highlight(f->x, f->y, f->w, f->h);
    }
    // The frame-time overlay (#137) and the frame-budget guard's notice (#152): the window
    // only, after everything that reads it back.
    const bool notice = g.notice_until > overlays;
    if (g.perf.visible() || notice) {
        float dx = 1;
        float dy = 1;
        window_density(g.window, &dx, &dy);
        const int k = std::max(1, static_cast<int>(std::lround(dy)));
        g.perf.draw(g.renderer, g.stats, k);
        if (notice) {
            PerfOverlay::draw_notice(g.renderer, g.notice, k);
        }
    }
    // The cash shop's price line (#156, price_line.h): the window only, like the overlay.
    if (mt_cash_shop_mode() == MT_CASH_SHOP_MESO) {
        float dx = 1;
        float dy = 1;
        window_density(g.window, &dx, &dy);
        draw_price_line(g.renderer, mt_cash_shop_view(),
                        std::max(1, static_cast<int>(std::lround(dy))));
    }
    Uint64 drawn = SDL_GetTicksNS();
    g.stats.add(Phase::kDraw, drawn - overlays);
    // Locked to the display: the first present after the turn hook's wake waits for the refresh
    // boundary after the recent works (pacing.h's PresentHold), so every tick's frame is
    // presented at the same point of the display's refresh.
    if (g.locked && g.hold.pending()) {
        const Uint64 at = g.hold.present_at(*g.paced, drawn);
        if (at > drawn) {
            SDL_DelayPrecise(at - drawn);
            const Uint64 held = SDL_GetTicksNS();
            g.stats.add(Phase::kWait, held - drawn);
            drawn = held;
        }
    }
    SDL_RenderPresent(g.renderer);
    const Uint64 presented = SDL_GetTicksNS();
    g.render_present_ns += presented - drawn;
    g.last_present_host = presented;
    g.stats.add(Phase::kPresent, presented - drawn);
    const FrameRecord &record =
        g.stats.frame(presented, index, gl_frame(g), gl_frame(g) != g.shown_gl, gfx_total_ns());
    g.csv.write(record);
    govern_render_scale(g);
    g.shown_gl = gl_frame(g);
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
    const HostCall host(g.stats);
    const Uint64 t0 = SDL_GetTicksNS();
    g.stats.gl_present(t0); // a tick
    if (!g.gpu_present) {
        g.screen.take_gl_frame(*frame); // mt_gfx_target_cpixels: on the GPU backend, a read-back
    } else if (g.gpu.take_gl_frame(*frame) && g.track_lit && g.screen.first_lit_gl_frame() == 0) {
        read_gpu_gl(g); // the first-lit log (--capture): downloads until a GL frame is lit
    }
    const Uint64 took = SDL_GetTicksNS() - t0;
    g.take_ns += took;
    g.stats.add(Phase::kTake, took);
    g.gl_takes++;
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

// The remap overlay is open (#70, ADR 0054): the game is paused. This hook does not return, so
// the guest gets no run-loop turn (no timer, no tick, no UIKit animation), as while an iOS app
// is suspended; the engine folds the gap into its timers afterwards. The overlay is drawn over
// the last composited screen, which is not composited again (that could run guest drawing):
// on the CPU over its pixels, with GPU presentation over the screen texture's last layers
// (draw_remap_gpu).
//
// The lifecycle (#128): a minimise or the background while it is open keeps it open and the turn
// held. Nothing is drawn while hidden; it is drawn again when shown. Its messages wait in the
// queue: the turn hook delivers them when the overlay closes (a resign and its become-active,
// as for a minimise between two turns). A quit ends the hold; the turn hook then sends
// applicationWillTerminate: and the run ends.
void run_remap(Game &g) {
    const Uint64 started = SDL_GetTicksNS();
    g.rumble.stop(); // the game is paused: no motor keeps running (#72)
    bool redraw = true;
    while (g.remap.is_open() && !g.quit && !g.lifecycle.terminating()) {
        redraw = redraw || g.remap.dirty() || g.event_seen;
        g.event_seen = false;
        if (redraw && g.lifecycle.presenting()) {
            bool shown = false;
            if (g.gpu_present) {
                shown = draw_remap_gpu(g) && g.presenter.draw();
            } else {
                const std::vector<std::uint8_t> &px = shown_pixels(g);
                shown = !px.empty() && g.presenter.upload(px.data(), g.screen.width() * 4) &&
                        g.presenter.draw();
            }
            if (!shown) {
                SDL_Log("game: the remap overlay cannot be shown");
                g.remap.cancel();
                break;
            }
            SDL_RenderPresent(g.renderer);
            redraw = false;
        }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            handle_event(g, e);
            while (g.remap.is_open() && SDL_PollEvent(&e)) {
                handle_event(g, e);
            }
        }
        g.remap.tick(static_cast<double>(SDL_GetTicksNS()) / 1e9);
    }
    g.stats.gap(SDL_GetTicksNS(), gfx_total_ns()); // a pause, not a slow frame
    SDL_Log("game: the remap overlay was open %.1f s; %s",
            static_cast<double>(SDL_GetTicksNS() - started) / 1e9,
            g.remap.is_open() ? "the run quits" : "the game goes on");
    g.event_seen = true; // show the game again
}

// Locked to the display (pacing.h): the guest clock moves only at the display's refreshes, so the
// run loop's next timer is due at a refresh. Wait until then, handling events as they come but
// not turning before it, so that every turn (and the tick and present it may make) starts right
// at a refresh. The cap (the periodic refresh, a UIKit animation) ends the wait at the first
// refresh at or after it.
void wait_locked(Game &g, double next, double cap) {
    const Uint64 start = SDL_GetTicksNS();
    Uint64 deadline = start;
    const double guest_wait = next - mt_cf_absolute_time();
    if (guest_wait > 0) {
        // The guest reads whole microseconds.
        const std::uint64_t us = g.paced->guest_ns() / 1000u +
                                 static_cast<std::uint64_t>(std::ceil(guest_wait * 1e6 - 1e-3));
        deadline = g.paced->host_when(us * 1000u);
    }
    const Uint64 capped = start + static_cast<Uint64>(cap * 1e9);
    const std::uint64_t k = g.paced->vblank_at(capped);
    const Uint64 cap_at =
        g.paced->vblank_host(k) >= capped ? g.paced->vblank_host(k) : g.paced->vblank_host(k + 1);
    deadline = std::min(deadline, cap_at);
    for (;;) {
        const Uint64 t = SDL_GetTicksNS();
        if (t >= deadline || g.quit || g.remap.is_open() || g.lifecycle.hidden() ||
            g.lifecycle.terminating()) {
            break;
        }
        const Uint64 left = deadline - t;
        if (left > 2000000) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, static_cast<Sint32>((left - 1000000) / 1000000))) {
                handle_event(g, e);
            }
        } else {
            SDL_DelayPrecise(left);
        }
    }
    const Uint64 woke = SDL_GetTicksNS();
    g.stats.add(Phase::kWait, woke - start);
    g.hold.wake(g.paced->vblank_host(g.paced->vblank_at(woke)));
}

// After every turn of the main run loop: delivers the scripted taps and shots, presents if
// something other than a GL frame changed (an event, a running UIKit animation, the periodic
// refresh), then waits, or in the fixed-step mode advances the virtual clock by one step.
int on_turn(void *user, mt_cpu *cpu, double now, double next) {
    Game &g = *static_cast<Game *>(user);
    const HostCall host(g.stats);
    if (g.turns++ == 0) {
        g.first_turn = now;
        if (g.audio != nullptr) {
            g.audio_start_ns = SDL_GetTicksNS();
            g.audio->resume(); // the game runs: start pulling the mixer
        }
    }
    pump_events(g);
    if (g.pending_scale != 0) {
        const std::uint32_t n = g.pending_scale;
        g.pending_scale = 0;
        apply_render_scale(g, n); // render_scale = auto (#152): between two turns
    }
    if (g.remap.is_open()) {
        run_remap(g); // the remap overlay holds the turn while it is open (#70)
    }
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
    g.rumble.turn();        // what the game's vibration and events asked for since the last turn
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
    // The game's tick interval, and in real time with pacing = display the clock's plan for it
    // and the display (pacing.h, ADR 0075).
    if (g.clock == nullptr) {
        g.tick_ms = game_tick_ms(cpu);
    }
    if (g.paced != nullptr) {
        const PacingPlan plan =
            plan_pacing(PacingMode::kDisplay, g.stats.refresh_hz(), g.vsync, g.tick_ms);
        if (g.paced->set_plan(plan)) {
            SDL_Log("pacing: %s, at %.3f s", describe(plan).c_str(), now - g.first_turn);
        }
        g.locked = plan.locked;
    }
    if (g.screenshot_wanted) {
        g.screenshot_wanted = false;
        take_screenshot(g, cpu, now);
    }
    // In the fixed-step mode window events do not make a present: what is presented (and
    // composited) then depends only on the virtual clock.
    // Nor does a UIKit animation more often than about 60 times a virtual second (a turn is
    // then far shorter than a display frame).
    const bool fixed = g.clock != nullptr;
    const bool animation_due = (mt_uikit_animation_count(now) > 0 || g.keypad.fading(now)) &&
                               (!fixed || now - g.last_present >= kAnimationSeconds);
    bool due = gl_frame(g) != g.shown_gl || (g.event_seen && !fixed) || animation_due ||
               now - g.last_present >= kRefreshSeconds;
    // Locked to the display: at most one window present a refresh, and none after the tick's in
    // its refresh, so that an event's or an animation's present never queues ahead of the next
    // tick's frame and pushes it a refresh later. It waits for the next refresh (event_seen and
    // the animation stay due).
    if (due && g.locked && gl_frame(g) == g.shown_gl && g.last_present_host != 0) {
        const std::uint64_t k = g.paced->vblank_at(SDL_GetTicksNS());
        due = g.last_present_host < g.paced->vblank_host(k);
    }
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
    if (g.locked) {
        wait_locked(g, next, cap);
        return 0;
    }
    const double wait = std::min(next - mt_cf_absolute_time(), cap);
    const Uint64 wait_start = SDL_GetTicksNS();
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
    g.stats.add(Phase::kWait, SDL_GetTicksNS() - wait_start);
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
    // mt_stubs_data_dir reads itself, else <exe dir>/data in portable mode, else SDL's per-user
    // directory for the app (the same directory as the stubs' default on macOS and Windows).
    // save_dir (#148) moves Documents/; the backup hook wraps the game's save (save_tools.h).
#if defined(SDL_PLATFORM_IOS)
    constexpr bool ios = true;
#else
    constexpr bool ios = false;
#endif
    const SaveLocation saves =
        configure_save_location(options.settings, data_dir(), documents_dir_override(ios));
    SDL_Log("game: data directory %s, saves in %s", saves.data_dir.c_str(),
            saves.documents_dir.c_str());
    install_save_backups();

    if (!options.capture_dir.empty()) {
        std::error_code ec;
        fs::create_directories(utf8_path(options.capture_dir), ec);
    }

    // render_scale = auto (#152): from 1x up, while the game runs; 1x in a run that writes
    // images, which must be the same bytes everywhere.
    const bool auto_scale = options.render_scale == kRenderScaleAuto;
    const std::uint32_t scale = auto_scale ? 1 : options.render_scale;
    Game g(scale);
    g.options = &options;
    g.scale_gov = RenderScaleGovernor(auto_scale, scale);
    g.govern = perf_overlay_allowed(options) && (auto_scale || scale > 1);
    if (auto_scale) {
        SDL_Log("render: render_scale auto: %s",
                g.govern ? "from 1x up to the largest that keeps a frame within the budget and "
                           "does not exceed the window"
                         : "1x in a run that writes images (--fixed-step, --shot, --capture)");
    }
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
    // Real time with pacing = display (pacing.h, ADR 0075): the paced clock, installed before
    // the process loads as the virtual clock is. It starts on the wall clock; the turn hook
    // locks it to the display once the game has set its tick interval.
    const auto wall_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    PacedClock paced(host_now_ns, host_sleep_ns, nullptr, wall_us);
    if (options.fixed_step_us == 0 && options.settings.pacing == PacingMode::kDisplay) {
        if (!paced.install()) {
            SDL_Log("game: GCD refused the paced clock");
            return kExitNoGame;
        }
        g.paced = &paced;
    }
    SDL_Log("game: pacing = %s%s", pacing_mode_name(options.settings.pacing),
            options.fixed_step_us != 0 ? " (not used: the fixed-step mode's virtual clock)"
            : g.paced != nullptr ? ": the paced clock (locked to the display when the refresh "
                                   "rate is a multiple of the tick rate)"
                                 : ": the wall clock");
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
    if (!options.perf_csv.empty() && !g.csv.open(options.perf_csv)) {
        SDL_Log("perf: cannot write --perf-csv %s", options.perf_csv.c_str());
        return kExitNoGame;
    }
    mt_audio_reset_output_stats();
    // A capture records the mix whatever the window's focus; a run that plays does not.
    g.focus_mute = FocusMute(options.mute_unfocused && g.capture == nullptr);
    g.window = window;
    g.renderer = renderer;
    g.vsync = vsync;
    start_presentation(g, renderer, options);
    if (g.presenter.texture() == nullptr) {
        return kExitSdlInitFailed;
    }
    g.presenter.set_scaling(options.scale_mode, options.border_color);
    g.presenter.set_forced_insets(options.safe_area);
    start_filter(g, options.filter);
    start_hd_text(options, scale);
    g.input.set_window(window, &g.presenter.placement());
    g.input.set_right_touch(options.right_touch);
    g.keypad = KeypadHider(options.hide_keypad);
    g.input.set_activity_listener(KeypadHider::listener, &g.keypad);
    SDL_Log("input: hide_keypad %s%s", keypad_mode_name(options.hide_keypad),
            options.hide_keypad == KeypadMode::kAlways ? ": the key pad is hidden" : "");
    // The bindings (bindings.ini or --bindings, read by main.cpp; #70) and the overlay's file.
    g.actions.bindings() = options.bindings.table;
    g.gamepads.set_face_buttons(options.bindings.face);
    g.remap.set_path(options.bindings_path);
    SDL_Log("input: %zu binding rows, Nintendo face buttons by %s; F1, Guide or L3+R3 opens the "
            "remap overlay",
            options.bindings.table.size(), face_buttons_name(options.bindings.face));
    g.gamepads.open_connected();
    // The lifecycle: the app events by SDL's event watch; in the fixed-step mode only the
    // script's (lifecycle.h).
    g.lifecycle.set_real(g.clock == nullptr);
    // The desktop conveniences (#145): the settings in effect, applied at once from here on
    // (the window's fullscreen and vsync are main.cpp's already; pause_unfocused here).
    g.desktop = options.desktop;
    apply_settings_at_once(g, options.settings);
    g.lifecycle.watch();
    g.script = lifecycle_script(options);
    g.rumble.configure(options.rumble, options.rumble_strength);
    // Frame telemetry (#137, ADR 0071): the overlay.
    g.perf = PerfOverlay(options.perf_overlay, perf_overlay_allowed(options));
    const double refresh_hz = display_refresh_hz(window);
    SDL_Log("perf: display %.2f Hz, vsync %s; the frame-time overlay is %s (F3%s)%s%s", refresh_hz,
            g.vsync ? "on" : "off", g.perf.visible() ? "on" : "off",
            g.perf.allowed() ? "" : ": never in a run that writes images",
            options.perf_csv.empty() ? "" : "; a row per window frame to ",
            options.perf_csv.c_str());
    SDL_Log("input: rumble %s, strength %u%%", options.rumble ? "on" : "off",
            static_cast<unsigned>(options.rumble_strength));
    if (options.right_touch.on) {
        SDL_Log("input: the right mouse button touches (%u, %u)",
                static_cast<unsigned>(options.right_touch.x),
                static_cast<unsigned>(options.right_touch.y));
    }

    // `language = ko` (#145) or MAPLETHIEF_LANGUAGE=ko, which overrides it, plays the Korean
    // build (NSLocale "KR": the terms screen, the Korean help pages and tables; #75). The
    // bundle is the one chosen above.
    mt_cf_config cf = mt_cf_config_default();
    const char *setting_language =
        kLanguageNames[static_cast<std::size_t>(options.settings.language)];
    std::memset(cf.language, 0, sizeof cf.language);
    std::memcpy(cf.language, setting_language, std::strlen(setting_language));
    if (mt_cf_config_from_env(&cf) != 0) {
        SDL_Log("game: %s must be en or ko: the setting's %s", MT_CF_LANGUAGE_ENV,
                setting_language);
    }
    cf.bundle_dir = bundle.c_str();
    if (std::strcmp(cf.language, "en") != 0 || std::strcmp(cf.language, setting_language) != 0) {
        SDL_Log("game: language %s (%s)", cf.language,
                std::strcmp(cf.language, setting_language) != 0 ? MT_CF_LANGUAGE_ENV
                                                                : "the language setting");
    }
    mt_cf_configure(&cf);
    mt_diag_configure_from_env();
    mt_gles_config gl{};
    gl.present = on_gl_present;
    gl.user = &g;
    gl.render_scale = scale;
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
    g.rumble.install();
    // Guest function hooks (maplethief/hooks.h, #143): registered for the whole run, each on or
    // off as its setting says.
    mt_hooks_install_portal_no_exit(options.portal_no_exit ? 1 : 0);
    SDL_Log("game: the main menu's portal items %s",
            options.portal_no_exit ? "return to the menu (hook)" : "quit, as the original");
    // The offline cash shop (#156, ADR 0072): sold for meso, or cancelled as the original
    // without a server (off, every preset).
    const bool cash_meso = options.settings.cash_shop == CashShop::kMeso;
    mt_cash_shop_install(cash_meso ? MT_CASH_SHOP_MESO : MT_CASH_SHOP_OFF);
    SDL_Log("game: the cash shop %s",
            cash_meso ? "sells for meso (cash_shop = meso)" : "is off: every payment is cancelled");
    mt_process *p = mt_process_load(&cfg, program.program, file.data() + offset, length);
    if (p == nullptr) {
        SDL_Log("game: the boot could not load the slice");
        status = kExitNoGame;
    } else {
        const Uint64 started = SDL_GetTicksNS();
        g.stats.start(started, refresh_hz, g.vsync, gfx_total_ns());
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
            SDL_Log("game: per window frame: compositing %.2f ms, upload and draw %.2f ms, "
                    "SDL_RenderPresent %.2f ms; per GL frame: taking it %.2f ms, %.2f ms in mt_gfx",
                    static_cast<double>(g.compose_ns) / 1e6 / n,
                    static_cast<double>(g.draw_ns) / 1e6 / n,
                    static_cast<double>(g.render_present_ns) / 1e6 / n,
                    g.gl_takes != 0
                        ? static_cast<double>(g.take_ns) / 1e6 / static_cast<double>(g.gl_takes)
                        : 0.0,
                    gfx.frames != 0
                        ? static_cast<double>(gfx.total.ns) / 1e6 / static_cast<double>(gfx.frames)
                        : 0.0);
            // The frontend's own time per window frame, without SDL_RenderPresent's wait for the
            // display: what GPU presentation (#130) shortens.
            SDL_Log("game: frame time: %.2f ms per window frame in the frontend (taking the GL "
                    "frames, compositing, upload and draw; %s)",
                    static_cast<double>(g.take_ns + g.compose_ns + g.draw_ns) / 1e6 / n,
                    g.gpu_present ? "GPU presentation" : "the CPU composite uploaded");
        }
        if (g.gpu_present) {
            const GpuScreen::Stats &s = g.gpu.stats();
            SDL_Log("game: GPU presentation: %llu GL frames copied on the GPU, %llu screens "
                    "composed, %llu overlay uploads (%llu unchanged), %llu downloads (%.1f MB) "
                    "for shots and checks",
                    static_cast<unsigned long long>(s.frames_taken),
                    static_cast<unsigned long long>(s.composes),
                    static_cast<unsigned long long>(s.overlay_uploads),
                    static_cast<unsigned long long>(s.composes -
                                                    std::min(s.composes, s.overlay_uploads)),
                    static_cast<unsigned long long>(s.downloads),
                    static_cast<double>(s.download_bytes) / 1e6);
        }
        if (g.govern) {
            SDL_Log("game: render scale %ux at the end (%s), %u changes%s%s",
                    static_cast<unsigned>(g.screen.width() / MT_UIKIT_SCREEN_WIDTH),
                    auto_scale ? "auto" : "fixed", static_cast<unsigned>(g.scale_changes),
                    g.scale_gov.warned() ? "; warned: " : "", g.scale_gov.warning().c_str());
        }
        // The tick intervals, the split of a window frame and the pacing (#137).
        for (const std::string &line : g.stats.summary(g.clock != nullptr)) {
            SDL_Log("game: %s", line.c_str());
        }
        // The game's speed against its own tick interval, and the paced clock (#80).
        if (g.clock == nullptr) {
            const TickStats t = g.stats.ticks();
            std::string speed = "no tick interval to compare";
            if (g.tick_ms > 0 && t.per_second > 0) {
                const double want = 1000.0 / static_cast<double>(g.tick_ms);
                char b[160];
                std::snprintf(
                    b, sizeof b,
                    "%.3f ticks a second against the game's %lld ms tick's %.3f (%+.2f %%)",
                    t.per_second, static_cast<long long>(g.tick_ms), want,
                    100.0 * (t.per_second / want - 1));
                speed = b;
            }
            if (g.paced != nullptr) {
                const double host = static_cast<double>(g.paced->host_elapsed_ns()) / 1e9;
                const double guest = static_cast<double>(g.paced->guest_ns()) / 1e9;
                SDL_Log("game: clock: pacing = display, %s at the end; locked %.1f s of %.1f s; "
                        "the guest clock is %+.3f s from the wall clock; %s",
                        describe(g.paced->plan()).c_str(),
                        static_cast<double>(g.paced->locked_ns()) / 1e9, host, guest - host,
                        speed.c_str());
            } else {
                SDL_Log("game: clock: pacing = wall, the host's clock; %s", speed.c_str());
            }
        }
        if (g.csv.is_open()) {
            const std::uint64_t rows = g.csv.rows();
            if (g.csv.close()) {
                SDL_Log("perf: %llu rows written to %s", static_cast<unsigned long long>(rows),
                        options.perf_csv.c_str());
            } else {
                SDL_Log("perf: writing %s failed", options.perf_csv.c_str());
            }
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
    g.rumble.uninstall();
    g.rumble.stop();
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
        if (g.desktop != nullptr &&
            (g.screenshots != 0 || g.wheel_steps != 0 || g.desktop->toggles() != 0)) {
            SDL_Log("game: desktop: %llu screenshots, %llu fullscreen toggles, %llu wheel steps "
                    "in menus, the cursor hidden %llu times",
                    static_cast<unsigned long long>(g.screenshots),
                    static_cast<unsigned long long>(g.desktop->toggles()),
                    static_cast<unsigned long long>(g.wheel_steps),
                    static_cast<unsigned long long>(g.desktop->cursor_hides()));
        }
        const RumbleStats &rs = g.rumble.stats();
        const mt_game_events_stats ev = mt_game_events_get_stats();
        SDL_Log(
            "game: %llu hits taken, %llu level-ups; rumble: %llu requests, %llu sent, "
            "%llu without a pad, %llu ignored (off), %llu failed",
            static_cast<unsigned long long>(ev.hero_damage),
            static_cast<unsigned long long>(ev.level_ups),
            static_cast<unsigned long long>(rs.requests), static_cast<unsigned long long>(rs.sent),
            static_cast<unsigned long long>(rs.no_pad), static_cast<unsigned long long>(rs.ignored),
            static_cast<unsigned long long>(rs.failed));
    }
    g.lifecycle.unwatch();
    g.gpu.stop();
    g.presenter.stop();
    g.filter_pass.close();
    stop_hd_text();
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    clock.uninstall();
    paced.uninstall();
    if (status == kExitOk && g.shots_done < g.shots.size()) {
        SDL_Log("game: the run ended after %zu of %zu shots", g.shots_done, g.shots.size());
        status = kExitCheckMismatch;
    }
    return status;
}

} // namespace maplethief::frontend
