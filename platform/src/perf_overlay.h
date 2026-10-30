// The frame-time overlay (#137, ADR 0071): the port's own debug read-out of FrameStats::recent()
// in the window's top-left corner: window frames and ticks a second, the tick interval, and the
// split of a frame (guest, mt_gfx, taking the GL frame, compositor, upload and draw,
// SDL_RenderPresent, waiting).
//
//   perf_overlay = on | off   settings.ini (off by default), --perf-overlay on|off
//   F3                        shows or hides it while the game runs
//
// It is drawn with SDL's renderer (SDL_RenderDebugText) straight into the window, after the
// screen image, every read-back and capture, like the controller focus's frame: it is never
// part of the composited screen, so never of a --shot or a golden, nor of a --capture read-back
// or the --frames check. A run that writes such images (--fixed-step, --shot, --capture) turns
// it off altogether (`allowed` false): F3 then does nothing but say so in the log.
#ifndef MAPLETHIEF_PLATFORM_PERF_OVERLAY_H
#define MAPLETHIEF_PLATFORM_PERF_OVERLAY_H

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "frame_stats.h"

namespace maplethief::frontend {

class PerfOverlay {
  public:
    static constexpr SDL_Keycode kHotkey = SDLK_F3;

    PerfOverlay() = default;
    // `on`: the setting; `allowed`: false in runs that write images (it stays hidden).
    PerfOverlay(bool on, bool allowed) : on_(on), allowed_(allowed) {}

    // F3 pressed (no repeat, no modifier but Shift or Num Lock): toggles it. Returns true if it
    // took the event (then nothing else gets it); also when not allowed, without a toggle.
    bool handle(const SDL_Event &e);
    void toggle() { on_ = !on_; }
    // perf_overlay changed while the game runs (the settings menu, #89).
    void set_on(bool on) { on_ = on; }
    bool on() const { return on_; }
    bool visible() const { return on_ && allowed_; }
    bool allowed() const { return allowed_; }
    // How often F3 was pressed (the log, tests).
    unsigned presses() const { return presses_; }

    // The text it shows for these numbers.
    static std::vector<std::string> lines(const RecentStats &r, const FrameRecord *last);
    // Draws it at the window's top left, each font pixel `scale` x `scale` output pixels, if
    // visible. Leaves the renderer's draw colour, blend mode and scale as they were.
    void draw(SDL_Renderer *renderer, const FrameStats &stats, int scale) const;

  private:
    bool on_ = false;
    bool allowed_ = true;
    unsigned presses_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PERF_OVERLAY_H
