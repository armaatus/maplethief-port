// MapleThief frontend (ARCHITECTURE.md section 3, ADR 0042).
//
// Opens a 960x640 window titled "MapleThief" (the 480x320 landscape screen at 2x). What it shows:
//
//   the game        (the default when the build links the generated code, maplethief_gen): boots
//                   the recompiled game from the extracted bundle and presents its frames
//                   (game.h, present.h)
//   the placeholder (the default without generated code, M0 #2): a solid clear colour
//   --gfx-prototype the rendering backend's prototype scene (#55, ADR 0039)
//
//   MapleThief [--version] [--smoke-frames N] [--gfx-prototype]
//              [--bundle DIR] [--slice FILE] [--render-scale N] [--frames N]
//              [--capture DIR] [--capture-at N[,N...]] [--tap T,X,Y]...
//
// --version prints "MapleThief <version>" (MT_VERSION, from the release tag, ADR 0024) to
// stdout and exits 0 without starting SDL.
// --smoke-frames N runs the placeholder (or, with --gfx-prototype, the prototype) for N
// presented frames, never the game. Before presenting the last one it reads the frame back and
// exits with status 3 unless the centre pixel has the clear colour (the prototype: unless the
// scene's probe pixels match). With SDL_VIDEO_DRIVER=dummy it runs headless; CTest and CI use
// that.
// Game options (exit status 4 without generated code):
//   --bundle DIR      the extracted Payload/MapleThief.app (else MAPLETHIEF_BUNDLE_DIR, else the
//                     build's work/ipa/Payload/MapleThief.app)
//   --slice FILE      a thin armv7 Mach-O instead of the bundle's executable
//   --render-scale N  the renderbuffers' internal scale, 1 to 8 (default 1)
//   --frames N        quit after N window frames; the last is read back and must show the
//                     composited screen (else exit status 3)
//   --capture DIR     write read-backs of window frames as PNGs: the first, the first that is
//                     not black, the last of --frames and those of --capture-at
//   --tap T,X,Y       a scripted tap at screen point (X, Y), T seconds after the first run-loop
//                     turn (a test aid until mouse input, #65)
// Exit statuses: game.h (ExitStatus).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "game.h"
#include "gfx_prototype.h"
#include "maplethief/gfx.h"
#include "maplethief/runtime.h"
#include "present.h"

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

constexpr SDL_Color kClearColor = {0x1E, 0x2A, 0x3A, SDL_ALPHA_OPAQUE};

// Frame pacing when vsync is unavailable (e.g. the dummy driver): about 60 Hz, so the loop
// doesn't spin a core.
constexpr Uint32 kNoVsyncFrameDelayMs = 16;

struct Options {
    std::uint64_t smoke_frames = 0; // 0: run until quit
    bool version = false;           // --version: print the version and exit
    bool gfx_prototype = false;     // --gfx-prototype: show mt_gfx's prototype scene
    bool game_option = false;       // a game-only option was given
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

bool start_gfx_prototype(SDL_Renderer *renderer, GfxPrototype &gfx) {
    gfx.target = mt_gfx_target_create(proto::kWidth, proto::kHeight, 1);
    if (gfx.target == nullptr) {
        SDL_Log("gfx-prototype: no target");
        return false;
    }
    return gfx.presenter.start(renderer, proto::kWidth, proto::kHeight);
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
    SDL_Log("usage: MapleThief [--version] [--smoke-frames N] [--gfx-prototype] [--bundle DIR] "
            "[--slice FILE] [--render-scale N] [--frames N] [--capture DIR] "
            "[--capture-at N[,N...]] [--tap T,X,Y]...");
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
        } else if (arg == "--render-scale" && has_value) {
            std::uint64_t scale = 0;
            if (!parse_uint(argv[++i], &scale) || scale < 1 || scale > MT_GFX_MAX_SCALE) {
                SDL_Log("--render-scale needs an integer from 1 to %u, got '%s'", MT_GFX_MAX_SCALE,
                        argv[i]);
                return false;
            }
            options.game.render_scale = static_cast<std::uint32_t>(scale);
            options.game_option = true;
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
            options.game.taps.push_back(Tap{t, static_cast<float>(x), static_cast<float>(y)});
            options.game_option = true;
        } else {
            SDL_Log("unknown argument '%s'", argv[i]);
            return false;
        }
    }
    return true;
}

// The placeholder and the prototype (no game): the M0 loop.
int run_placeholder(SDL_Renderer *renderer, bool vsync, const Options &options) {
    GfxPrototype gfx;
    if (options.gfx_prototype && !start_gfx_prototype(renderer, gfx)) {
        stop_gfx_prototype(gfx);
        return kExitSdlInitFailed;
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
    }

    SDL_Log("quit after %llu frames", static_cast<unsigned long long>(frames));
    stop_gfx_prototype(gfx);
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
                      (program.program != nullptr || options.game_option);
    if (options.game_option && (options.smoke_frames != 0 || options.gfx_prototype)) {
        SDL_Log("the game options do not go with --smoke-frames or --gfx-prototype");
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

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("MapleThief", kWindowWidth, kWindowHeight,
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
            "vsync %s, %s",
            mt_runtime_version(), SDL_VERSIONNUM_MAJOR(sdl_version),
            SDL_VERSIONNUM_MINOR(sdl_version), SDL_VERSIONNUM_MICRO(sdl_version),
            SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer), width, height, pixel_width,
            pixel_height, vsync ? "on" : "off",
            game ? "the game" : (options.gfx_prototype ? "the gfx prototype" : "the placeholder"));

    const int status = game ? run_game(program, window, renderer, vsync, options.game)
                            : run_placeholder(renderer, vsync, options);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
