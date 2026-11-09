// The build type and optimisation level at start-up, and the Debug build's warning when it
// falls behind the game's tick (#136, ADR 0134).
#pragma once

#include <cstdint>
#include <string>

namespace maplethief::frontend {

// The build type and optimisation level for the start-up log, e.g. "Release (-O3)" or
// "Debug (-O2 for the generated code, mt_gfx, CoreGraphics and the compositor, -O0 elsewhere)".
std::string build_description();

// Whether a GL-frame stream has been behind the tick for a while: tumbling windows of kTicks
// intervals whose mean is over `limit_ns` (the tick's overrun limit, FrameStats::overrun_ns), so
// that one slow frame, a load or a stall alone never counts. An interval over kGapNs (a loading
// screen, a hidden window) starts a new window instead of being counted. Fires at most once.
class TickLag {
  public:
    static constexpr std::uint32_t kTicks = 40;               // about 2 s at 20 ticks a second
    static constexpr std::uint64_t kGapNs = 1'000'000'000ull; // a pause, not a slow tick

    // A tick at `now_ns`: true the first time the window's mean exceeds `limit_ns`, and only
    // then.
    bool tick(std::uint64_t now_ns, std::uint64_t limit_ns);

    // The mean interval of the window that fired (0 before that), in ms.
    double mean_ms() const { return mean_ms_; }

  private:
    std::uint64_t prev_ns_ = 0;
    std::uint64_t sum_ns_ = 0;
    std::uint32_t count_ = 0;
    bool fired_ = false;
    double mean_ms_ = 0;
};

// In a Debug build: feeds `lag` the GL frame at `now_ns` and writes the one-line warning to
// stderr when it fires. In other builds nothing.
void warn_if_debug_behind_tick(TickLag &lag, std::uint64_t now_ns, std::uint64_t limit_ns);

} // namespace maplethief::frontend
