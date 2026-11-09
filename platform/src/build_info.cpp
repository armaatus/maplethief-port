// The start-up build line and the Debug build's behind-the-tick warning; see build_info.h.
#include "build_info.h"

#include <cstdio>

namespace maplethief::frontend {

std::string build_description() { return std::string(MT_BUILD_TYPE) + " (" + MT_BUILD_OPT + ")"; }

bool TickLag::tick(std::uint64_t now_ns, std::uint64_t limit_ns) {
    const std::uint64_t prev = prev_ns_;
    prev_ns_ = now_ns;
    if (fired_ || prev == 0 || now_ns <= prev) {
        return false;
    }
    const std::uint64_t interval = now_ns - prev;
    if (interval > kGapNs) {
        sum_ns_ = 0;
        count_ = 0;
        over_ = 0;
        return false;
    }
    sum_ns_ += interval;
    over_ += interval > limit_ns ? 1 : 0;
    if (++count_ < kTicks) {
        return false;
    }
    const std::uint64_t mean = sum_ns_ / count_;
    const bool behind = over_ >= kOver;
    sum_ns_ = 0;
    count_ = 0;
    over_ = 0;
    if (!behind) {
        return false;
    }
    fired_ = true;
    mean_ms_ = static_cast<double>(mean) / 1e6;
    return true;
}

void warn_if_debug_behind_tick([[maybe_unused]] TickLag &lag, [[maybe_unused]] std::uint64_t now_ns,
                               [[maybe_unused]] std::uint64_t limit_ns) {
#ifndef NDEBUG
    if (lag.tick(now_ns, limit_ns)) {
        std::fprintf(stderr,
                     "MapleThief: this Debug build is falling behind the game's tick (%.1f ms a "
                     "tick over the last %u ticks). To judge or play the game, build a Release: "
                     "just build (docs/build/).\n",
                     lag.mean_ms(), TickLag::kTicks);
    }
#endif
}

} // namespace maplethief::frontend
