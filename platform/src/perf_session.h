// The logged performance session (#244, ADR 0145): --perf-session logs one "perf:" line per
// second of play with what ADR 0071's telemetry measured in that second, as counts and sums, so
// that a host tool (python3 -m mtport.perfsession) can add any run of them up exactly and judge
// the pace: the ticks a second and the share of frames held 3 vblanks, per screen (handheld,
// docked) and per scene (a map, a load, fast-forward). scripts/switch-perf runs it on a Nintendo
// Switch over nxlink; it works the same on every platform.
//
// Pure bookkeeping, no SDL and no clock: game.cpp hands it each window frame's FrameRecord (frame
// telemetry, frame_stats.h) with the context it was shown in. It never touches the guest.
//
// A line (one window, key=value pairs separated by single spaces, in this order):
//
//   perf: t=12.345 s=1.002 display=handheld scene=map:2.0 ff=1 scale=1 backend=gpu frames=20
//         ticks=20 held=20 held3=20 over=0 tick_max=50.12 guest=14.20 gfx=0.80 take=1.00
//         compose=6.00 draw=0.20 present=4.00 wait=974.00 other=2.00
//
//   t          when the window's last frame was presented, seconds since FrameStats::start
//   s          the window's length: the sum of its frames' intervals, seconds
//   display    handheld | docked (the Switch's operation mode) | desktop
//   scene      map:AREA.MAP (the field, or its popup menu) | load (the game's modes 6 to 8) |
//              scene (modes 9, 10: the intro, story and boss scenes) | menu (any other mode, the
//              title) | none (the game is not running yet)
//   ff         the guest clock's rate: 1, or fast-forward's (ADR 0087)
//   scale      the render scale in effect; backend: software | gpu (what mt_gfx draws with)
//   frames     window frames with an interval; ticks: those that show a new GL frame
//   held       frames whose scan-out time is known; held3: those shown for exactly 3 vblanks
//   over       ticks longer than the overrun threshold (55 ms at the game's speed)
//   tick_max   the longest tick, ms
//   guest ...  the split, summed over the frames, ms (ADR 0071: guest, mt_gfx, taking the GL
//              frame, compositor, upload and draw, SDL_RenderPresent, waiting, other)
//
// A window ends after kWindowNs, at a change of context (no second mixes handheld with docked or
// a map with a load) and at a pause (a frame without an interval: HOME, sleep, a minimise).
#ifndef MAPLETHIEF_PLATFORM_PERF_SESSION_H
#define MAPLETHIEF_PLATFORM_PERF_SESSION_H

#include <cstdint>
#include <optional>
#include <string>

#include "frame_stats.h"

namespace maplethief::frontend {

// What a window frame was shown in.
struct PerfContext {
    std::string display = "desktop"; // handheld | docked | desktop
    std::string scene = "none";      // perf_scene()
    std::uint32_t ff = 1;            // the guest clock's rate
    std::uint32_t scale = 1;         // the render scale
    std::string backend = "software";
    std::uint64_t overrun_ns = FrameStats::kDefaultOverrunNs;

    bool operator==(const PerfContext &o) const {
        return display == o.display && scene == o.scene && ff == o.ff && scale == o.scale &&
               backend == o.backend;
    }
};

// The scene from the game's mode, area and map (autosave.h's check_autosave; -1: unknown):
// "map:2.0" for the field (mode 0) and its popup menu (1), "load" for 6 to 8, "scene" for 9 and
// 10, "none" for -1, "menu" otherwise (the title, the character select). A field mode without a
// map reads "menu".
std::string perf_scene(std::int32_t mode, std::int32_t area, std::int32_t map);

class PerfSession {
  public:
    static constexpr std::uint64_t kWindowNs = 1'000'000'000;

    // A window frame and its context. Returns the line of the window it closed, if it closed one
    // (the frame then starts the next window).
    std::optional<std::string> add(const FrameRecord &r, const PerfContext &ctx);
    // The run ends: the open window's line, if it has a frame.
    std::optional<std::string> flush();

    std::uint64_t lines() const { return lines_; }

  private:
    struct Window {
        PerfContext ctx;
        std::uint64_t end_ns = 0;
        std::uint64_t span_ns = 0;
        std::uint64_t frames = 0, ticks = 0, held = 0, held3 = 0, over = 0;
        std::uint64_t tick_max_ns = 0;
        std::uint64_t guest = 0, gfx = 0, take = 0, compose = 0, draw = 0, present = 0, wait = 0,
                      other = 0;
    };
    std::string line(const Window &w) const;

    std::optional<Window> open_;
    std::uint64_t lines_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PERF_SESSION_H
