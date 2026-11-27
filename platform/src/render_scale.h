// render_scale = auto and the frame-budget guard (#152, ADR 0080).
//
// The render scale N draws the game's GL layer, and composites UIKit's views, at 480N x 320N.
// Its cost grows with N squared: the software rasterizer's time in mt_gfx and the compositor's.
// RenderScaleGovernor watches FrameStats::recent() (frame_stats.h) once a second and
//
//   - with `auto`, picks the largest N that keeps a window frame within the budget and does not
//     exceed the window: it starts at 1, climbs one N at a time while the measured cost, scaled
//     by ((N + 1) / N)^2, stays within the budget at N + 1, and drops as soon as two seconds in
//     a row go over it (to the largest smaller N whose scaled cost fits). After a drop it does
//     not climb again for kRetrySeconds. The window's image size caps N: no point drawing more
//     texels than the drawable has pixels (render_scale_cap). A resize re-checks it at once.
//   - at a fixed N > 1, warns once when kWarnSeconds in a row go over the budget: the frontend
//     logs it, shows it in the frame-time overlay and as a short notice in the window.
//
// The budget per window frame (the means over the last second):
//   frontend (taking the GL frame, the compositor, upload and draw)  <= kFrontendBudgetMs
//   frontend + mt_gfx (the GL draws: rasterizing on the software backend)  <= kRenderBudgetMs
//   the GPU's time per GL frame (RecentStats::gpu_ms, GpuFrameTimer)  <= kRenderBudgetMs
// The CPU and the GPU work side by side, so the render budget holds the larger of the two
// (#365, ADR 0203). On the GPU backend mt_gfx's CPU time is recording only and the cost is the
// GPU's; without a GPU time (software backend, too few samples) the CPU's alone counts.
//
// Pure logic, no SDL and no clock: game.cpp passes recent() and SDL_GetTicksNS seconds, the
// tests pass made-up numbers. The frontend applies the scale the governor returns
// (mt_gles_set_render_scale, then its Screen, textures and text scale) between two run-loop
// turns. In a run that writes images (--fixed-step, --shot, --capture) the governor is not used
// and `auto` is 1: those runs must be the same bytes on every machine.
#ifndef MAPLETHIEF_PLATFORM_RENDER_SCALE_H
#define MAPLETHIEF_PLATFORM_RENDER_SCALE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

#include "frame_stats.h"

namespace maplethief::frontend {

inline constexpr double kFrontendBudgetMs = 10.0;   // #152: the frontend's time per frame
inline constexpr double kRenderBudgetMs = 20.0;     // with mt_gfx: 40% of the game's 50 ms tick
inline constexpr std::uint32_t kMaxRenderScale = 8; // MT_GFX_MAX_SCALE

// The smallest N whose 480N x 320N image has at least as many texels across and down as the
// image's place in the drawable (Placement::rect, pixels), 1 to kMaxRenderScale; 1 for an empty
// place.
std::uint32_t render_scale_cap(double image_w, double image_h);

class RenderScaleGovernor {
  public:
    static constexpr double kEvalSeconds = 1.0;    // how often it looks
    static constexpr double kSettleSeconds = 1.25; // after a change, before it looks again
    static constexpr std::uint32_t kMinFrames = 8; // window frames a second must have to count
    static constexpr std::uint32_t kDropAfter = 2; // auto: seconds over budget before a drop
    static constexpr std::uint32_t kWarnAfter = 3; // fixed N: seconds over budget before a warning
    static constexpr double kRetrySeconds = 30;    // auto: no climbing this long after a drop

    RenderScaleGovernor() = default;
    // `automatic`: render_scale = auto (starting at `scale`, normally 1); otherwise the fixed N.
    RenderScaleGovernor(bool automatic, std::uint32_t scale);

    // The image's place in the drawable is now `cap` (render_scale_cap) at time `now` (seconds).
    // Returns the scale to switch to (auto above the new cap: the cap), or 0 for none. A larger
    // cap ends the wait after a drop: the new size is worth a new look.
    std::uint32_t set_cap(std::uint32_t cap, double now);
    // After a window frame, with FrameStats::recent() and the time. Returns the scale to switch
    // to, or 0 for none; reason() then says why.
    std::uint32_t update(const RecentStats &recent, double now);
    // The frontend could not switch: it stays at `scale`.
    void failed(std::uint32_t scale);

    bool automatic() const { return automatic_; }
    std::uint32_t scale() const { return scale_; }
    std::uint32_t cap() const { return cap_; }
    // Why the last change was made, e.g. "frontend 1.4 ms, with mt_gfx 3.2 ms a frame: 2x
    // should fit (budget 10 / 20 ms)".
    const std::string &reason() const { return reason_; }
    // The warning (fixed N over the budget), once: true the first time it is asked for after
    // it arose, with its text.
    bool take_warning(std::string *text);
    bool warned() const { return !warning_.empty(); }
    const std::string &warning() const { return warning_; }
    // One line for the frame-time overlay: "render 3x auto (window 4x)", "render 4x" or the
    // warning.
    std::string status() const;

  private:
    bool automatic_ = false;
    std::uint32_t scale_ = 1;
    std::uint32_t cap_ = kMaxRenderScale;
    double last_eval_ = -1e300;
    double last_change_ = -1e300;
    double retry_at_ = -1e300; // auto: no climbing before this
    std::uint32_t over_ = 0;   // seconds over budget in a row
    std::string reason_;
    std::string warning_;
    bool warning_taken_ = false;
};

// The GPU's time per GL frame for the governor (#365, ADR 0203). mt_gfx's GPU backend times a
// frame by waiting for its flushes' fence (mt_gfx_gpu_set_timing), which stalls the CPU, so the
// frontend times one GL frame in kEvery: after a window frame it asks arm() whether to turn
// timing on for the next GL frame, and when that frame has arrived it hands its GPU time
// (mt_gfx_gpu_stats::last_frame_gpu_ns) to add(). ms() is the median of the samples of the last
// kWindowSeconds, or 0 (unknown) with fewer than kMinSamples. Pure logic: game.cpp passes
// SDL_GetTicksNS seconds and the backend's numbers, the tests made-up ones.
class GpuFrameTimer {
  public:
    static constexpr std::uint32_t kEvery = 4;       // GL frames: one timed in this many
    static constexpr double kWindowSeconds = 1.0;    // the samples ms() looks at
    static constexpr std::uint32_t kMinSamples = 2;  // fewer: unknown
    static constexpr std::size_t kMaxSamples = 64;   // kept at most

    // A window frame showed a new GL frame (a tick): whether to time the next GL frame. Every
    // kEvery-th tick, and not while a timed frame is still to arrive.
    bool arm();
    // The timed frame arrived at `now` (seconds) with the GPU's time for it; ns 0 (no draws
    // timed) is no sample.
    void add(std::uint64_t ns, double now);
    // Whether a timed frame is still to arrive (timing is on).
    bool armed() const { return armed_; }
    // The median GPU time of the last kWindowSeconds before `now`, in ms; 0 if unknown.
    double ms(double now) const;

  private:
    std::uint32_t ticks_ = 0;
    bool armed_ = false;
    std::deque<std::pair<double, double>> samples_; // (when, ms)
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RENDER_SCALE_H
