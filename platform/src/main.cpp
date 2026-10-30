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
//              [--window WxH] [--scale-mode MODE] [--border-color #RRGGBB]
//              [--bundle DIR] [--slice FILE] [--render-scale N] [--frames N] [--seconds S]
//              [--capture DIR] [--capture-at N[,N...]] [--tap T,X,Y]...
//              [--fixed-step MS] [--shot T,NAME]... [--no-audio] [--audio-capture FILE]
//              [--audio-test-tone] [--master-volume N] [--music-volume N]
//              [--effects-volume N] [--mute-unfocused on|off] [--gapless-music on|off]
//              [--right-touch X,Y|off] [--hide-keypad auto|always|never] [--shot-gl]
//              [--save-settings]
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
// Audio options (every mode; settings.h, #64):
//   --master-volume N      the volume of everything, 0 to 100 percent (default 100)
//   --music-volume N       the music's volume, 0 to 100 (default 100)
//   --effects-volume N     the sound effects' volume, 0 to 100 (default 100)
//   --mute-unfocused on|off  silence while the window is unfocused (default on; the game only,
//                          never with --audio-capture)
//   --gapless-music on|off  loop the music without the digital silence at the start and end
//                          of its files (default off, as the device; ADR 0062)
// Input options (the game; input.h, ADR 0047). The left mouse button is a touch, a touch
// screen's fingers are touches:
//   --right-touch X,Y|off  the right mouse button as a second touch at screen point (X, Y)
//                          (whole points, 0-479, 0-319; default off)
//   --hide-keypad auto|always|never  hide the on-screen key pad: auto (default) while a
//                          gamepad or the keyboard is used, shown again on a touch; always;
//                          never (keypad_hide.h, #71)
// The game reads settings.ini in its data directory first (settings.h: scale_mode, border_color,
// render_scale, the volumes, mute_unfocused, gapless_music, right_touch, hide_keypad); the
// command line overrides it. The placeholder and the prototype (tests) read no settings file.
//   --save-settings   (the game) writes the settings in effect (the file's, with the command
//                     line's over them) back to settings.ini, keeping its comments, then runs
// Game options (exit status 4 without generated code):
//   --bundle DIR      the extracted Payload/MapleThief.app (else MAPLETHIEF_BUNDLE_DIR, else the
//                     build's work/ipa/Payload/MapleThief.app)
//   --slice FILE      a thin armv7 Mach-O instead of the bundle's executable
//   --render-scale N  the renderbuffers' internal scale, 1 to 8 (default 1)
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
//   --fixed-step MS   run on a virtual clock (ADR 0044): every run-loop turn advances it by MS
//                     milliseconds (0.001 to 1000) and nothing waits, so a scripted run
//                     reaches the same frames bit for bit every time
//   --shot T,NAME     write the composited 480x320 screen at T seconds after the first turn to
//                     <--capture DIR>/NAME.png; the run ends after the last shot
//   --shot-gl         each --shot also writes the GL frame it shows to NAME.gl.png
//   --no-audio        no audio device (the game runs silent)
//   --audio-capture FILE  write the mix to FILE as a 16-bit stereo 44.1 kHz WAV instead of
//                     playing it (audio_out.h); with --fixed-step the same bytes every run
// --audio-test-tone (with the placeholder, or alone) plays a 440 Hz tone for 1.5 s through the
// default audio device and the mixer (#61, the manual device check) and quits after 2 s (once
// the stream has pulled the tone; 10 s at the latest); exit status 3 if no device opened or its
// stream did not pull the tone's frames.
// Exit statuses: game.h (ExitStatus).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "audio_out.h"
#include "game.h"
#include "gfx_prototype.h"
#include "maplethief/audio.h"
#include "maplethief/gfx.h"
#include "maplethief/runtime.h"
#include "present.h"
#include "scaling.h"
#include "settings.h"

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
    // The command line's overrides of settings.ini.
    std::optional<ScaleMode> scale_mode;       // --scale-mode
    std::optional<Color> border_color;         // --border-color
    std::optional<std::uint32_t> render_scale; // --render-scale
    // The audio and input flags as settings.ini lines (key, value), validated, applied in order.
    std::vector<std::pair<std::string, std::string>> setting_flags;
    bool save_settings = false; // --save-settings
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
#if defined(MT_DEV_BUNDLE_DIR)
    g.dev_bundle_dir = MT_DEV_BUNDLE_DIR;
#endif
#endif
    return g;
}

// --gfx-prototype: the mt_gfx target, shown through the presenter.
struct GfxPrototype {
    mt_gfx_target *target = nullptr;
    Presenter presenter;
};

// Before the renderer goes.
void stop_gfx_prototype(GfxPrototype &gfx) {
    gfx.presenter.stop();
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
    gfx.presenter.set_scaling(scaling.scale_mode, scaling.border_color);
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
// no alpha: RGB only.
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
        const bool match = read && cr == probe.rgba[0] && cg == probe.rgba[1] &&
                           cb == probe.rgba[2] && t[0] == probe.rgba[0] && t[1] == probe.rgba[1] &&
                           t[2] == probe.rgba[2] && t[3] == probe.rgba[3];
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
            "[--scale-mode integer|fit|smooth|sharp] [--border-color #RRGGBB] [--bundle DIR] "
            "[--slice FILE] [--render-scale N] [--frames N] [--capture DIR] "
            "[--capture-at N[,N...]] [--tap T,X,Y]... [--swipe T,X1,Y1,X2,Y2]... "
            "[--fixed-step MS] [--shot T,NAME]... "
            "[--no-audio] [--audio-capture FILE] [--audio-test-tone] [--master-volume N] "
            "[--music-volume N] [--effects-volume N] [--mute-unfocused on|off] "
            "[--gapless-music on|off] [--right-touch X,Y|off] [--hide-keypad auto|always|never] "
            "[--shot-gl] "
            "[--save-settings]");
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
        } else if ((arg == "--master-volume" || arg == "--music-volume" ||
                    arg == "--effects-volume" || arg == "--mute-unfocused" ||
                    arg == "--gapless-music" || arg == "--right-touch" || arg == "--hide-keypad") &&
                   has_value) {
            // "--music-volume" -> "music_volume", checked as settings.ini would check it.
            std::string key(arg.substr(2));
            for (char &c : key) {
                c = c == '-' ? '_' : c;
            }
            Settings check;
            std::string why;
            if (!apply_setting(&check, key, argv[++i], &why)) {
                SDL_Log("%s: %s", std::string(arg).c_str(), why.c_str());
                return false;
            }
            options.setting_flags.emplace_back(key, argv[i]);
        } else if (arg == "--shot-gl") {
            options.game.shot_gl = true;
            options.game_option = true;
        } else if (arg == "--save-settings") {
            options.save_settings = true;
            options.game_option = true;
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
        } else if (arg == "--render-scale" && has_value) {
            std::uint64_t scale = 0;
            if (!parse_uint(argv[++i], &scale) || scale < 1 || scale > MT_GFX_MAX_SCALE) {
                SDL_Log("--render-scale needs an integer from 1 to %u, got '%s'", MT_GFX_MAX_SCALE,
                        argv[i]);
                return false;
            }
            options.render_scale = static_cast<std::uint32_t>(scale);
            options.game_option = true;
        } else if (arg == "--window" && has_value) {
            if (!parse_size(argv[++i], &options.window_w, &options.window_h)) {
                SDL_Log("--window needs WxH (points, %d to %d each), got '%s'", kMinWindowSide,
                        kMaxWindowSide, argv[i]);
                return false;
            }
        } else if (arg == "--scale-mode" && has_value) {
            ScaleMode mode{};
            if (!parse_scale_mode(argv[++i], &mode)) {
                SDL_Log("--scale-mode needs integer, fit, smooth or sharp, got '%s'", argv[i]);
                return false;
            }
            options.scale_mode = mode;
        } else if (arg == "--border-color" && has_value) {
            Color color;
            if (!parse_color(argv[++i], &color)) {
                SDL_Log("--border-color needs #RRGGBB, got '%s'", argv[i]);
                return false;
            }
            options.border_color = color;
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

} // namespace

int main(int argc, char *argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return kExitUsage;
    }
    if (options.version) {
        // stdout, not SDL_Log (stderr): scripts read it. A Windows GUI program prints only
        // when its output is redirected, e.g. `MapleThief.exe --version | Out-String`.
        std::fputs("MapleThief " MT_VERSION "\n", stdout);
        return std::fflush(stdout) == 0 ? kExitOk : kExitUsage;
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

    SDL_SetAppMetadata("MapleThief", MT_VERSION, MT_APP_ID);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return kExitSdlInitFailed;
    }
    // Gamepads (#67, ADR 0048): the game plays without them if SDL cannot have them.
    if (game && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SDL_Log("gamepads: off: %s", SDL_GetError());
    }

    // The settings: the defaults, the game's settings.ini over them, the command line over that.
    Settings settings;
    if (game) {
        load_settings(data_dir(), &settings);
    }
    if (options.scale_mode) {
        settings.scale_mode = *options.scale_mode;
    }
    if (options.border_color) {
        settings.border_color = *options.border_color;
    }
    if (options.render_scale) {
        settings.render_scale = *options.render_scale;
    }
    for (const auto &[key, value] : options.setting_flags) {
        apply_setting(&settings, key, value, nullptr); // checked by parse_options
    }
    options.game.scale_mode = settings.scale_mode;
    options.game.border_color = settings.border_color;
    options.game.render_scale = settings.render_scale;
    options.game.mute_unfocused = settings.mute_unfocused;
    options.game.right_touch = settings.right_touch;
    options.game.hide_keypad = settings.hide_keypad;
    if (game && options.save_settings) {
        save_settings(data_dir(), settings);
    }
    apply_audio_settings(settings);

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("MapleThief", options.window_w, options.window_h,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window,
                                     &renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        SDL_Quit();
        return kExitSdlInitFailed;
    }
    const bool vsync = SDL_SetRenderVSync(renderer, 1);

    const int sdl_version = SDL_GetVersion();
    int width = 0;
    int height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetCurrentRenderOutputSize(renderer, &pixel_width, &pixel_height);
    SDL_Log("%s, SDL %d.%d.%d, video driver %s, renderer %s, window %dx%d (%dx%d pixels), "
            "vsync %s, scale mode %s, border #%02X%02X%02X, render scale %u, volume %u%% (music "
            "%u%%, effects %u%%), mute unfocused %s, gapless music %s, %s",
            mt_runtime_version(), SDL_VERSIONNUM_MAJOR(sdl_version),
            SDL_VERSIONNUM_MINOR(sdl_version), SDL_VERSIONNUM_MICRO(sdl_version),
            SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer), width, height, pixel_width,
            pixel_height, vsync ? "on" : "off", scale_mode_name(settings.scale_mode),
            settings.border_color.r, settings.border_color.g, settings.border_color.b,
            static_cast<unsigned>(settings.render_scale),
            static_cast<unsigned>(settings.master_volume),
            static_cast<unsigned>(settings.music_volume),
            static_cast<unsigned>(settings.effects_volume), settings.mute_unfocused ? "on" : "off",
            settings.gapless_music ? "on" : "off",
            game ? "the game" : (options.gfx_prototype ? "the gfx prototype" : "the placeholder"));

    const int status = game ? run_game(program, window, renderer, vsync, options.game)
                            : run_placeholder(renderer, vsync, options);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
