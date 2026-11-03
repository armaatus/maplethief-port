// Frame telemetry (#137, ADR 0071); see frame_stats.h.
#include "frame_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace maplethief::frontend {

namespace {

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

// The value at fraction q (0..1) of the values: the nearest rank (partially sorts `v`).
std::uint32_t percentile(std::vector<std::uint32_t> &v, double q) {
    if (v.empty()) {
        return 0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
    const std::size_t k = std::min(v.size() - 1, rank == 0 ? 0 : rank - 1);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

void add_split(FrameRecord &to, const FrameRecord &r) {
    to.guest_ns += r.guest_ns;
    to.gfx_ns += r.gfx_ns;
    to.take_ns += r.take_ns;
    to.compose_ns += r.compose_ns;
    to.draw_ns += r.draw_ns;
    to.present_ns += r.present_ns;
    to.wait_ns += r.wait_ns;
    to.other_ns += r.other_ns;
}

// A CSV cell in milliseconds with microsecond precision.
void cell_ms(std::string &out, std::uint64_t ns) {
    char b[32];
    std::snprintf(b, sizeof b, ",%.3f", ms(ns));
    out += b;
}

// "3 vblanks 95.50% (1910), 4 vblanks 3.00% (60), ...": the counts by frequency, most common
// first.
std::string histogram_text(const std::array<std::uint64_t, 16> &counts) {
    std::uint64_t total = 0;
    std::vector<std::pair<std::uint64_t, std::size_t>> order;
    for (std::size_t i = 0; i < counts.size(); i++) {
        total += counts[i];
        if (counts[i] != 0) {
            order.emplace_back(counts[i], i);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto &a, const auto &c) { return a.first > c.first; });
    std::string text;
    for (const auto &[count, vblanks] : order) {
        char part[96];
        std::snprintf(part, sizeof part, "%s%s%zu vblank%s %.2f%% (%llu)", text.empty() ? "" : ", ",
                      vblanks == counts.size() - 1 ? ">=" : "", vblanks, vblanks == 1 ? "" : "s",
                      100.0 * static_cast<double>(count) / static_cast<double>(total),
                      static_cast<unsigned long long>(count));
        text += part;
    }
    return text;
}

} // namespace

void FrameStats::start(std::uint64_t now_ns, double refresh_hz, bool vsync, std::uint64_t gfx_ns) {
    const std::uint64_t overrun = overrun_ns_;
    *this = FrameStats{};
    overrun_ns_ = overrun;
    started_ = true;
    refresh_hz_ = refresh_hz;
    vsync_ = vsync;
    start_ns_ = now_ns;
    host_depth_ = 0;
    left_ns_ = now_ns;
    gfx_mark_ = gfx_ns;
    prev_frame_ns_ = now_ns;
    after_gap_ = true;
    ring_.reserve(kRecentFrames);
}

void FrameStats::enter_host(std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    // A hook inside a hook (a guest call the turn hook makes presents): the guest code between
    // them counts as the outer hook's.
    if (host_depth_++ == 0 && now_ns > left_ns_) {
        pending_.guest_ns += now_ns - left_ns_;
    }
}

void FrameStats::leave_host(std::uint64_t now_ns) {
    if (!started_ || host_depth_ == 0) {
        return;
    }
    if (--host_depth_ == 0) {
        left_ns_ = now_ns;
    }
}

void FrameStats::gl_present(std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    gl_presents_++;
    if (prev_gl_ns_ != 0 && now_ns >= prev_gl_ns_) {
        const std::uint64_t d = now_ns - prev_gl_ns_;
        // Microseconds (a 32-bit count holds 71 minutes: plenty for one interval).
        tick_us_.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(d / 1000, std::numeric_limits<std::uint32_t>::max())));
        tick_sum_ns_ += d;
        tick_max_ns_ = std::max(tick_max_ns_, d);
        overruns_ += d > overrun_ns_ ? 1 : 0;
        pending_tick_ns_ = d;
    } else {
        pending_tick_ns_ = 0;
    }
    // 0 means "none": a GL present at exactly the start time still counts as one.
    prev_gl_ns_ = std::max<std::uint64_t>(now_ns, 1);
}

void FrameStats::add(Phase phase, std::uint64_t ns) {
    if (!started_) {
        return;
    }
    switch (phase) {
    case Phase::kTake:
        pending_.take_ns += ns;
        break;
    case Phase::kCompose:
        pending_.compose_ns += ns;
        break;
    case Phase::kDraw:
        pending_.draw_ns += ns;
        break;
    case Phase::kPresent:
        pending_.present_ns += ns;
        break;
    case Phase::kWait:
        pending_.wait_ns += ns;
        break;
    }
}

const FrameRecord &FrameStats::frame(std::uint64_t now_ns, std::uint64_t index,
                                     std::uint32_t gl_frame, bool tick, std::uint64_t gfx_ns) {
    FrameRecord r = pending_;
    pending_ = FrameRecord{};
    if (!started_) {
        start(now_ns, 0, false, gfx_ns);
    }
    r.index = index;
    r.gl_frame = gl_frame;
    r.tick = tick;
    r.at_ns = now_ns - start_ns_;
    r.interval_ns = after_gap_ ? 0 : now_ns - prev_frame_ns_;
    r.tick_ns = tick ? pending_tick_ns_ : 0;
    if (tick) {
        pending_tick_ns_ = 0;
    }
    // mt_gfx's draws ran inside the guest's calls: that much of the guest's time is mt_gfx's.
    const std::uint64_t gfx = gfx_ns >= gfx_mark_ ? gfx_ns - gfx_mark_ : 0;
    gfx_mark_ = gfx_ns;
    r.gfx_ns = std::min(gfx, r.guest_ns);
    r.guest_ns -= r.gfx_ns;
    const std::uint64_t span = now_ns >= prev_frame_ns_ ? now_ns - prev_frame_ns_ : 0;
    const std::uint64_t split =
        r.guest_ns + r.gfx_ns + r.take_ns + r.compose_ns + r.draw_ns + r.present_ns + r.wait_ns;
    r.other_ns = span > split ? span - split : 0;
    // Scan-out, simulated: a frame presented at t is first shown at the next vblank, and stays
    // until the next frame is. The vblanks are taken at whole refresh periods from start() (the
    // display's phase is not known; the counts' distribution does not depend on it).
    if (vsync_ && refresh_hz_ > 0) {
        const auto vblank = static_cast<std::int64_t>(
            std::floor(static_cast<double>(r.at_ns) * refresh_hz_ / 1e9) + 1);
        if (!after_gap_ && prev_vblank_ >= 0) {
            const std::int64_t held = std::max<std::int64_t>(0, vblank - prev_vblank_);
            r.vblanks = static_cast<std::int32_t>(std::min<std::int64_t>(held, 1'000'000));
            held_[std::min<std::size_t>(static_cast<std::size_t>(r.vblanks), held_.size() - 1)]++;
        }
        prev_vblank_ = vblank;
        if (tick) {
            if (!after_gap_ && prev_tick_vblank_ >= 0) {
                const std::int64_t shown = std::max<std::int64_t>(0, vblank - prev_tick_vblank_);
                tick_held_[std::min<std::size_t>(static_cast<std::size_t>(shown),
                                                 tick_held_.size() - 1)]++;
            }
            prev_tick_vblank_ = vblank;
        }
    }
    prev_frame_ns_ = now_ns;
    after_gap_ = false;
    frames_++;
    add_split(totals_, r);
    totals_.interval_ns += r.interval_ns;
    if (ring_.size() < kRecentFrames) {
        ring_.push_back(r);
    } else {
        ring_[ring_next_] = r;
    }
    ring_next_ = (ring_next_ + 1) % kRecentFrames;
    return back(0);
}

void FrameStats::gap(std::uint64_t now_ns, std::uint64_t gfx_ns) {
    if (!started_) {
        return;
    }
    // What the pause spanned belongs to no frame: the split starts again from here.
    pending_ = FrameRecord{};
    pending_tick_ns_ = 0;
    prev_gl_ns_ = 0;
    prev_frame_ns_ = now_ns;
    after_gap_ = true;
    prev_tick_vblank_ = -1;
    gfx_mark_ = gfx_ns;
    left_ns_ = now_ns;
}

const FrameRecord &FrameStats::back(std::size_t n) const {
    // ring_next_ is the slot after the newest (the oldest, once the ring is full).
    return ring_[(ring_next_ + 2 * ring_.size() - 1 - n) % ring_.size()];
}

const FrameRecord *FrameStats::last() const { return ring_.empty() ? nullptr : &back(0); }

TickStats FrameStats::ticks() const {
    TickStats t;
    t.ticks = gl_presents_;
    t.intervals = tick_us_.size();
    t.seconds = static_cast<double>(tick_sum_ns_) / 1e9;
    t.overruns = overruns_;
    if (t.intervals == 0) {
        return t;
    }
    t.per_second = t.seconds > 0 ? static_cast<double>(t.intervals) / t.seconds : 0;
    t.mean_ms = ms(tick_sum_ns_) / static_cast<double>(t.intervals);
    t.max_ms = ms(tick_max_ns_);
    std::vector<std::uint32_t> v = tick_us_;
    t.p50_ms = static_cast<double>(percentile(v, 0.50)) / 1000;
    t.p99_ms = static_cast<double>(percentile(v, 0.99)) / 1000;
    return t;
}

RecentStats FrameStats::recent() const {
    RecentStats s;
    const FrameRecord *newest = last();
    if (newest == nullptr) {
        return s;
    }
    FrameRecord sum;
    std::uint64_t intervals = 0;
    std::uint64_t span = 0;
    std::uint64_t ticks = 0;
    std::uint64_t tick_sum = 0;
    std::uint64_t tick_max = 0;
    std::uint64_t frontend_max = 0;
    // From the newest back, while within kRecentNs of it, and not past a gap.
    for (std::size_t n = 0; n < ring_.size(); n++) {
        const FrameRecord &r = back(n);
        if (newest->at_ns - r.at_ns > kRecentNs) {
            break;
        }
        s.frames++;
        add_split(sum, r);
        frontend_max = std::max(frontend_max, r.frontend_ns());
        if (r.tick_ns != 0) {
            ticks++;
            tick_sum += r.tick_ns;
            tick_max = std::max(tick_max, r.tick_ns);
        }
        if (r.interval_ns == 0) {
            break; // the first frame after a gap (or the run's first)
        }
        intervals++;
        span += r.interval_ns;
    }
    const double n = s.frames;
    s.seconds = static_cast<double>(span) / 1e9;
    if (span != 0) {
        s.fps = static_cast<double>(intervals) / s.seconds;
        s.tps = static_cast<double>(ticks) / s.seconds;
    }
    s.tick_ms = ticks != 0 ? ms(tick_sum) / static_cast<double>(ticks) : 0;
    s.max_tick_ms = ms(tick_max);
    s.guest_ms = ms(sum.guest_ns) / n;
    s.gfx_ms = ms(sum.gfx_ns) / n;
    s.take_ms = ms(sum.take_ns) / n;
    s.compose_ms = ms(sum.compose_ns) / n;
    s.draw_ms = ms(sum.draw_ns) / n;
    s.present_ms = ms(sum.present_ns) / n;
    s.wait_ms = ms(sum.wait_ns) / n;
    s.other_ms = ms(sum.other_ns) / n;
    s.frontend_ms = ms(sum.frontend_ns()) / n;
    s.max_frontend_ms = ms(frontend_max);
    return s;
}

std::vector<std::string> FrameStats::summary(bool fixed_step) const {
    std::vector<std::string> lines;
    char b[512];
    const TickStats t = ticks();
    const char *clock = fixed_step ? " (wall clock: the fixed-step mode is not paced)" : "";
    if (t.intervals == 0) {
        std::snprintf(b, sizeof b, "ticks: %llu GL frames, no interval to measure%s",
                      static_cast<unsigned long long>(t.ticks), clock);
    } else {
        std::snprintf(b, sizeof b,
                      "ticks: %llu GL frames, %.2f a second%s; tick interval p50 %.2f ms, p99 "
                      "%.2f ms, max %.2f ms, mean %.2f ms; %llu over %.0f ms",
                      static_cast<unsigned long long>(t.ticks), t.per_second, clock, t.p50_ms,
                      t.p99_ms, t.max_ms, t.mean_ms, static_cast<unsigned long long>(t.overruns),
                      ms(overrun_ns_));
    }
    lines.emplace_back(b);
    if (frames_ != 0) {
        const double n = static_cast<double>(frames_);
        const FrameRecord &s = totals_;
        std::snprintf(b, sizeof b,
                      "frame split: %llu window frames; per frame guest %.2f ms, mt_gfx %.2f ms, "
                      "taking the GL frame %.2f ms, compositor %.2f ms, upload and draw %.2f ms, "
                      "SDL_RenderPresent %.2f ms, waiting %.2f ms, other %.2f ms",
                      static_cast<unsigned long long>(frames_), ms(s.guest_ns) / n,
                      ms(s.gfx_ns) / n, ms(s.take_ns) / n, ms(s.compose_ns) / n, ms(s.draw_ns) / n,
                      ms(s.present_ns) / n, ms(s.wait_ns) / n, ms(s.other_ns) / n);
        lines.emplace_back(b);
    }
    std::uint64_t held = 0;
    for (const std::uint64_t c : held_) {
        held += c;
    }
    if (held == 0) {
        std::snprintf(b, sizeof b, "pacing: no vblank counts (%s)",
                      !vsync_           ? "vsync off"
                      : refresh_hz_ > 0 ? "no frame interval"
                                        : "the refresh rate is unknown");
        lines.emplace_back(b);
        return lines;
    }
    std::snprintf(b, sizeof b,
                  "pacing: %.2f Hz display, vsync on, scan-out simulated from the present times: "
                  "%llu window frames shown for ",
                  refresh_hz_, static_cast<unsigned long long>(held));
    lines.emplace_back(std::string(b) + histogram_text(held_));
    std::uint64_t ticks = 0;
    for (const std::uint64_t c : tick_held_) {
        ticks += c;
    }
    if (ticks != 0) {
        std::snprintf(b, sizeof b, "pacing: %llu GL frames (ticks) shown for ",
                      static_cast<unsigned long long>(ticks));
        lines.emplace_back(std::string(b) + histogram_text(tick_held_));
    }
    return lines;
}

std::string FrameStats::csv_header() {
    return "frame,gl_frame,tick,present_s,interval_ms,tick_ms,vblanks,guest_ms,gfx_ms,take_ms,"
           "compose_ms,draw_ms,present_ms,wait_ms,other_ms";
}

std::string FrameStats::csv_row(const FrameRecord &r) {
    char b[160];
    std::snprintf(b, sizeof b, "%llu,%u,%d,%.6f", static_cast<unsigned long long>(r.index),
                  static_cast<unsigned>(r.gl_frame), r.tick ? 1 : 0,
                  static_cast<double>(r.at_ns) / 1e9);
    std::string out = b;
    // Empty cells: not measured (the first frame, after a gap, not a tick, no refresh rate).
    if (r.interval_ns != 0) {
        cell_ms(out, r.interval_ns);
    } else {
        out += ',';
    }
    if (r.tick_ns != 0) {
        cell_ms(out, r.tick_ns);
    } else {
        out += ',';
    }
    out += ',';
    if (r.vblanks >= 0) {
        out += std::to_string(r.vblanks);
    }
    for (const std::uint64_t ns : {r.guest_ns, r.gfx_ns, r.take_ns, r.compose_ns, r.draw_ns,
                                   r.present_ns, r.wait_ns, r.other_ns}) {
        cell_ms(out, ns);
    }
    return out;
}

bool PerfCsv::open(const std::string &path) {
    close();
    failed_ = false;
    rows_ = 0;
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
#if defined(_WIN32)
    if (_wfopen_s(&file_, p.c_str(), L"wb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(p.c_str(), "wb");
#endif
    if (file_ == nullptr) {
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 16);
    const std::string header = FrameStats::csv_header() + "\n";
    failed_ = std::fwrite(header.data(), 1, header.size(), file_) != header.size();
    return true;
}

void PerfCsv::write(const FrameRecord &r) {
    if (file_ == nullptr) {
        return;
    }
    const std::string row = FrameStats::csv_row(r) + "\n";
    failed_ = std::fwrite(row.data(), 1, row.size(), file_) != row.size() || failed_;
    rows_++;
}

bool PerfCsv::close() {
    if (file_ == nullptr) {
        return !failed_;
    }
    const bool ok = std::fclose(file_) == 0 && !failed_;
    file_ = nullptr;
    failed_ = !ok;
    return ok;
}

} // namespace maplethief::frontend
