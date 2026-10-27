// MapleThief frontend: placeholder SDL3 main loop (M0, issue #2).
//
// Opens a 960x640 window titled "MapleThief", clears it to a solid colour every frame and
// exits with status 0 on SDL_EVENT_QUIT (window close, Cmd-Q, SIGINT/SIGTERM). The game is
// 480x320 landscape, so the renderer uses that as its letterboxed logical size.
//
//   MapleThief [--version] [--smoke-frames N]
//
// --version prints "MapleThief <version>" (MT_VERSION, from the release tag, ADR 0024) to
// stdout and exits 0 without starting SDL.
// --smoke-frames N quits by itself after N presented frames. Before presenting the last one it
// reads the frame back and exits with status 3 unless the centre pixel has the clear colour.
// With SDL_VIDEO_DRIVER=dummy it runs headless; CTest and CI use that.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>

#include "maplethief/runtime.h"

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 640;
constexpr int kLogicalWidth = 480;
constexpr int kLogicalHeight = 320;

constexpr SDL_Color kClearColor = {0x1E, 0x2A, 0x3A, SDL_ALPHA_OPAQUE};

// Exit statuses (ADR 0004).
constexpr int kExitOk = 0;            // quit cleanly (and, with --smoke-frames, the check passed)
constexpr int kExitSdlInitFailed = 1; // SDL_Init or window/renderer creation failed
constexpr int kExitUsage = 2;         // invalid command line
constexpr int kExitSmokeMismatch = 3; // --smoke-frames read back the wrong colour

// Frame pacing when vsync is unavailable (e.g. the dummy driver): about 60 Hz, so the loop
// doesn't spin a core.
constexpr Uint32 kNoVsyncFrameDelayMs = 16;

struct Options {
    std::uint64_t smoke_frames = 0; // 0: run until quit
    bool version = false;           // --version: print the version and exit
};

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

void print_usage() { SDL_Log("usage: MapleThief [--version] [--smoke-frames N]"); }

// Returns false (after logging why) if the command line is invalid.
bool parse_options(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version") {
            options.version = true;
        } else if (arg == "--smoke-frames" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            std::uint64_t frames = 0;
            const auto [end, ec] =
                std::from_chars(value.data(), value.data() + value.size(), frames);
            if (ec != std::errc() || end != value.data() + value.size() || frames == 0) {
                SDL_Log("--smoke-frames needs a positive integer, got '%s'", argv[i]);
                return false;
            }
            options.smoke_frames = frames;
        } else {
            SDL_Log("unknown argument '%s'", argv[i]);
            return false;
        }
    }
    return true;
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
    SDL_SetRenderLogicalPresentation(renderer, kLogicalWidth, kLogicalHeight,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    const bool vsync = SDL_SetRenderVSync(renderer, 1);

    const int sdl_version = SDL_GetVersion();
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Log("%s, SDL %d.%d.%d, video driver %s, renderer %s, window %dx%d, vsync %s",
            mt_runtime_version(), SDL_VERSIONNUM_MAJOR(sdl_version),
            SDL_VERSIONNUM_MINOR(sdl_version), SDL_VERSIONNUM_MICRO(sdl_version),
            SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer), width, height,
            vsync ? "on" : "off");

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

        SDL_SetRenderDrawColor(renderer, kClearColor.r, kClearColor.g, kClearColor.b,
                               kClearColor.a);
        SDL_RenderClear(renderer);
        const bool last_smoke_frame =
            options.smoke_frames != 0 && frames + 1 == options.smoke_frames;
        if (last_smoke_frame) {
            smoke_ok = check_clear_color(renderer);
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
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return smoke_ok ? kExitOk : kExitSmokeMismatch;
}
