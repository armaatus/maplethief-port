// Frame telemetry (#137, ADR 0071): the tick rate, the frame pacing and where each window
// frame's time goes, for the exit log, --perf-csv, the frame-time overlay (perf_overlay.h) and
// the frontend's own decisions at run time (display-locked pacing #80, the game-speed check
// #151, the frame-budget guard and render_scale = auto #152).
//
// Pure bookkeeping, no SDL and no clock of its own: the caller passes host timestamps
// (SDL_GetTicksNS in game.cpp, synthetic ones in the tests). It never reads or changes the
// guest's clock, so a --fixed-step run is the same bytes with or without it.
//
// The game.cpp hooks feed it:
//
//   start(now, refresh_hz, vsync, gfx_ns)  before mt_process_run: the guest runs from here
//   enter_host(now)                        a hook starts: the guest ran since leave_host
//   leave_host(now)                        the hook returns to the guest
//   gl_present(now)                        -presentRenderbuffer: (the GL present hook): a tick
//   add(Phase, ns)                         time in a frontend phase (take, compose, draw, ...)
//   frame(now, index, gl_frame, tick, gfx_ns)  SDL_RenderPresent returned: one window frame
//   gap(now, gfx_ns)                       a pause (hidden, the remap overlay): what spans it
//                                          is not an interval
//
// A tick is a GL present: the game presents once per logic tick (50 ms at its default speed),
// and while it loads a map it presents from inside the load. Tick intervals are measured
// between GL present hooks; window frames between SDL_RenderPresent returns. A window frame
// may show no new GL frame (a UIKit animation, the 0.25 s refresh, an event): its `tick` is
// false.
//
// mt_gfx's draws run inside the guest's GL calls; frame() takes mt_gfx_submit's running total
// (mt_gfx_stats.total.ns) and moves that much of the guest's time to `gfx`.
#ifndef MAPLETHIEF_PLATFORM_FRAME_STATS_H
#define MAPLETHIEF_PLATFORM_FRAME_STATS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace maplethief::frontend {

// The frontend's phases of a window frame (time the caller measures and hands to add()).
enum class Phase : std::uint8_t {
    kTake,    // taking the GL frame: Screen::take_gl_frame (mt_gfx's read-back) or the GPU copy
    kCompose, // the compositor: the CPU composite, or UIKit's overlay layer on the GPU
    kDraw,    // uploading and drawing the screen image, the focus frame and the overlay
    kPresent, // SDL_RenderPresent (with vsync: the wait for the display)
    kWait,    // the turn hook's wait for the run loop's next wake or an event
};

// One window frame: when it reached the display and where the time since the previous one
// went. Times in nanoseconds; the split adds up to interval_ns (or, for the first frame and the
// first after a gap, to the time since start() or gap()).
struct FrameRecord {
    std::uint64_t index = 0;       // the window frame, from 1
    std::uint32_t gl_frame = 0;    // the GL frame it shows
    bool tick = false;             // it shows a GL frame the previous one did not (a tick)
    std::uint64_t at_ns = 0;       // when SDL_RenderPresent returned, since start()
    std::uint64_t interval_ns = 0; // since the previous window frame (0: first, or after a gap)
    std::uint64_t tick_ns = 0;     // a tick: since the previous GL present (0: none to measure)
    // The display refreshes since the previous window frame's: how long that one was shown
    // (scan-out simulated from the present times, frame()); 0: replaced before it was shown;
    // -1: unknown (no vsync or refresh rate, the first frame, after a gap).
    std::int32_t vblanks = -1;
    std::uint64_t guest_ns = 0;   // the guest's code and its HLE calls, without mt_gfx
    std::uint64_t gfx_ns = 0;     // mt_gfx_submit: rasterizing (software) or recording (GPU)
    std::uint64_t take_ns = 0;    // Phase::kTake
    std::uint64_t compose_ns = 0; // Phase::kCompose
    std::uint64_t draw_ns = 0;    // Phase::kDraw
    std::uint64_t present_ns = 0; // Phase::kPresent
    std::uint64_t wait_ns = 0;    // Phase::kWait
    std::uint64_t other_ns = 0;   // the rest: the hooks' own work (events, input, lifecycle)

    // The frontend's cost of the frame without the display's wait: take + compose + draw (what
    // the exit log's "frame time" line sums, and what a frame budget limits).
    std::uint64_t frontend_ns() const { return take_ns + compose_ns + draw_ns; }
};

// The tick intervals (GL present to GL present, gaps left out).
struct TickStats {
    std::uint64_t ticks = 0;     // GL presents
    std::uint64_t intervals = 0; // intervals measured
    double seconds = 0;          // their sum
    double per_second = 0;       // intervals / seconds: GL frames a second
    double mean_ms = 0, p50_ms = 0, p99_ms = 0, max_ms = 0;
    std::uint64_t overruns = 0; // intervals over overrun_ns()
};

// Means over the window frames of about the last second (the overlay, and run-time decisions).
struct RecentStats {
    std::uint32_t frames = 0; // window frames in the window
    double seconds = 0;       // the time they span
    double fps = 0;           // window frames a second
    double tps = 0;           // ticks a second
    double tick_ms = 0;       // mean tick interval
    double max_tick_ms = 0;   // longest tick interval
    double guest_ms = 0;      // means per window frame
    double gfx_ms = 0;
    double take_ms = 0;
    double compose_ms = 0;
    double draw_ms = 0;
    double present_ms = 0;
    double wait_ms = 0;
    double other_ms = 0;
    double frontend_ms = 0;     // take + compose + draw
    double max_frontend_ms = 0; // the costliest frame's
};

class FrameStats {
  public:
    static constexpr std::uint64_t kDefaultOverrunNs = 55'000'000; // 55 ms: 1.1 x the 50 ms tick
    static constexpr std::uint64_t kRecentNs = 1'000'000'000;      // RecentStats' window
    static constexpr std::size_t kRecentFrames = 256;              // frames kept for it

    // The run starts (the guest runs from `now_ns`). `refresh_hz`: the display's refresh rate
    // (0: unknown); `vsync`: SDL_RenderPresent waits for it. Vblank counts need both.
    // `gfx_ns`: mt_gfx_submit's running total now. Clears everything.
    void start(std::uint64_t now_ns, double refresh_hz, bool vsync, std::uint64_t gfx_ns);
    // The window moved to another display.
    void set_refresh(double refresh_hz) {
        refresh_hz_ = refresh_hz;
        prev_vblank_ = -1;
    }
    // vsync switched while the game runs (#145's `vsync`, applied at once).
    void set_vsync(bool vsync) {
        vsync_ = vsync;
        prev_vblank_ = -1;
    }
    // Tick intervals over this are overruns (default 55 ms).
    void set_overrun_ns(std::uint64_t ns) { overrun_ns_ = ns; }

    void enter_host(std::uint64_t now_ns);
    void leave_host(std::uint64_t now_ns);
    void gl_present(std::uint64_t now_ns);
    void add(Phase phase, std::uint64_t ns);
    // A window frame: SDL_RenderPresent returned at `now_ns`. `tick`: it shows a new GL frame.
    // Returns its record (also last()).
    const FrameRecord &frame(std::uint64_t now_ns, std::uint64_t index, std::uint32_t gl_frame,
                             bool tick, std::uint64_t gfx_ns);
    void gap(std::uint64_t now_ns, std::uint64_t gfx_ns);

    // --- reading -------------------------------------------------------------------------
    bool started() const { return started_; }
    double refresh_hz() const { return refresh_hz_; }
    bool vsync() const { return vsync_; }
    std::uint64_t overrun_ns() const { return overrun_ns_; }
    std::uint64_t frames() const { return frames_; }
    // The last window frame, or nullptr before the first.
    const FrameRecord *last() const;
    TickStats ticks() const;
    // The window frames whose present is within kRecentNs of the last one's.
    RecentStats recent() const;
    // Vblank counts: held[n] is how many window frames were shown for n refreshes (n < 16; the
    // last slot counts 15 and more; 0: replaced before it was shown). Zero without vsync or a
    // refresh rate.
    const std::array<std::uint64_t, 16> &held() const { return held_; }
    // The per-frame totals (sums of every record since start).
    const FrameRecord &totals() const { return totals_; }

    // The exit log's lines (game.cpp logs them with a "game: " prefix): the ticks, the split
    // per window frame, the pacing. `fixed_step`: the run was on the virtual clock (its wall
    // clock times say nothing about the game's speed; the lines say so).
    std::vector<std::string> summary(bool fixed_step) const;

    // --perf-csv: the header line and one row per window frame (no line ends).
    static std::string csv_header();
    static std::string csv_row(const FrameRecord &r);

  private:
    // The n-th newest frame of the ring (0: the newest); n < ring_.size().
    const FrameRecord &back(std::size_t n) const;

    bool started_ = false;
    double refresh_hz_ = 0;
    bool vsync_ = false;
    std::uint64_t overrun_ns_ = kDefaultOverrunNs;
    std::uint64_t start_ns_ = 0;

    std::uint32_t host_depth_ = 0;    // hooks running (0: the guest runs)
    std::uint64_t left_ns_ = 0;       // when the guest last resumed
    std::uint64_t gfx_mark_ = 0;      // mt_gfx's total at the last frame (or start, gap)
    std::uint64_t prev_frame_ns_ = 0; // the last frame's present, start() or gap()
    bool after_gap_ = true;           // no interval to the next frame
    std::uint64_t prev_gl_ns_ = 0;    // the last GL present (0: none since start or a gap)
    std::uint64_t pending_tick_ns_ = 0;
    std::int64_t prev_vblank_ = -1; // the vblank the last frame was first shown at (-1: none)
    FrameRecord pending_;           // the split so far of the frame to come

    std::uint64_t frames_ = 0;
    std::uint64_t gl_presents_ = 0;
    std::vector<std::uint32_t> tick_us_; // every tick interval, microseconds
    std::uint64_t tick_sum_ns_ = 0;
    std::uint64_t tick_max_ns_ = 0;
    std::uint64_t overruns_ = 0;
    std::array<std::uint64_t, 16> held_{};
    FrameRecord totals_;
    std::vector<FrameRecord> ring_; // the last kRecentFrames frames
    std::size_t ring_next_ = 0;
};

// --perf-csv FILE: FrameStats::csv_header, then a row per window frame, buffered.
class PerfCsv {
  public:
    PerfCsv() = default;
    ~PerfCsv() { close(); }
    PerfCsv(const PerfCsv &) = delete;
    PerfCsv &operator=(const PerfCsv &) = delete;

    // Creates (truncates) the file at the UTF-8 path and writes the header; false if it cannot.
    bool open(const std::string &path);
    bool is_open() const { return file_ != nullptr; }
    void write(const FrameRecord &r);
    // Flushes and closes; false if a write failed.
    bool close();
    std::uint64_t rows() const { return rows_; }

  private:
    std::FILE *file_ = nullptr;
    bool failed_ = false;
    std::uint64_t rows_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_FRAME_STATS_H
