// Display-locked frame pacing (#80, ADR 0075); see pacing.h.
#include "pacing.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "clock.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/libc.h"

namespace maplethief::frontend {

namespace {

std::uint64_t libc_clock(void *user) { return static_cast<PacedClock *>(user)->unix_us(); }

std::uint64_t gcd_clock(void *user) { return static_cast<PacedClock *>(user)->gcd_ns(); }

void gcd_sleep_until(void *user, std::uint64_t deadline) {
    if (deadline > PacedClock::kGcdOrigin) {
        static_cast<PacedClock *>(user)->sleep_until(deadline - PacedClock::kGcdOrigin);
    }
}

// Guest nanoseconds of `steps` steps of tick_ms / vblanks milliseconds (exact: `vblanks` steps
// are tick_ms milliseconds to the nanosecond).
std::uint64_t steps_ns(std::uint64_t steps, const PacingPlan &p) {
    const std::uint64_t tick_ns = std::uint64_t{p.tick_ms} * 1000000u;
    return steps / p.vblanks * tick_ns + steps % p.vblanks * tick_ns / p.vblanks;
}

} // namespace

PacingPlan plan_pacing(PacingMode mode, double refresh_hz, bool vsync, std::int64_t tick_ms) {
    PacingPlan p;
    if (mode == PacingMode::kWall) {
        p.why = "pacing = wall";
        return p;
    }
    if (!vsync) {
        p.why = "vsync off";
        return p;
    }
    if (!(refresh_hz >= 1 && refresh_hz <= 1000)) {
        p.why = "the refresh rate is unknown";
        return p;
    }
    if (tick_ms <= 0 || tick_ms > 1000) {
        p.why = "the game's tick interval is not known yet";
        return p;
    }
    const double exact = refresh_hz * static_cast<double>(tick_ms) / 1000.0;
    const double n = std::round(exact);
    if (n < 1 || std::fabs(exact / n - 1) > kLockTolerance) {
        p.why = "the refresh rate is not a multiple of the tick rate";
        return p;
    }
    p.locked = true;
    p.vblanks = static_cast<std::uint32_t>(n);
    p.refresh_hz = refresh_hz;
    p.tick_ms = static_cast<std::uint32_t>(tick_ms);
    return p;
}

std::string describe(const PacingPlan &plan) {
    char b[160];
    if (plan.locked) {
        std::snprintf(b, sizeof b, "locked to the display: %.2f Hz, %u refreshes a %u ms tick",
                      plan.refresh_hz, plan.vblanks, plan.tick_ms);
    } else {
        std::snprintf(b, sizeof b, "the wall clock (%s)", plan.why);
    }
    return b;
}

std::uint64_t paced_start_unix_us(std::uint64_t wall_us) {
    return wall_us - LaunchClock::shift_for(wall_us);
}

PacedClock::PacedClock(NowFn now, SleepFn sleep, void *user, std::uint64_t start_unix_us)
    : now_(now), sleep_(sleep), user_(user), start_unix_us_(start_unix_us), host_start_(now(user)) {
    m_.host0 = host_start_;
    m_.plan.why = "not planned yet";
}

bool PacedClock::install() {
    mt_gcd_config gcd = mt_gcd_config_default();
    gcd.clock = gcd_clock;
    gcd.clock_user = this;
    gcd.sleep_until = gcd_sleep_until;
    gcd.sleep_user = this;
    if (mt_gcd_configure(&gcd) != 0) {
        return false;
    }
    mt_libc_config libc = mt_libc_config_default();
    libc.clock = libc_clock;
    libc.clock_user = this;
    mt_libc_configure(&libc);
    installed_ = true;
    return true;
}

void PacedClock::uninstall() {
    if (!installed_) {
        return;
    }
    mt_libc_configure(nullptr);
    mt_gcd_configure(nullptr);
    installed_ = false;
}

std::uint64_t PacedClock::steps_at(const Mapping &m, std::uint64_t k) {
    const long long s = std::llround(static_cast<double>(k) * m.r + m.off);
    return s > 0 ? static_cast<std::uint64_t>(s) : 0;
}

std::uint64_t PacedClock::vblank_start(const Mapping &m, std::uint64_t k) {
    return m.host0 +
           static_cast<std::uint64_t>(std::ceil(static_cast<double>(k) * 1e9 / m.plan.refresh_hz));
}

std::uint64_t PacedClock::vblank_index(const Mapping &m, std::uint64_t host_ns) {
    if (host_ns <= m.host0) {
        return 0;
    }
    std::uint64_t k = static_cast<std::uint64_t>(
        std::floor(static_cast<double>(host_ns - m.host0) * m.plan.refresh_hz / 1e9));
    // The rounding of the double, settled against vblank_start itself.
    while (k > 0 && vblank_start(m, k) > host_ns) {
        k--;
    }
    while (vblank_start(m, k + 1) <= host_ns) {
        k++;
    }
    return k;
}

std::uint64_t PacedClock::at(const Mapping &m, std::uint64_t host_ns) {
    if (host_ns <= m.host0) {
        return m.guest0;
    }
    if (!m.plan.locked) {
        return m.guest0 + (host_ns - m.host0);
    }
    return m.guest0 + steps_ns(steps_at(m, vblank_index(m, host_ns)), m.plan);
}

bool PacedClock::set_plan(const PacingPlan &plan) {
    const std::uint64_t now = now_(user_);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (plan == m_.plan) {
        // The same mapping. A new reason for the wall clock (vsync switched off before the
        // game's tick interval was known, say) is kept for the log and the exit line.
        if (std::strcmp(plan.why, m_.plan.why) == 0) {
            return false;
        }
        m_.plan.why = plan.why;
        return true;
    }
    std::uint64_t guest = at(m_, now);
    const std::uint64_t seen = last_.load(std::memory_order_relaxed);
    guest = guest > seen ? guest : seen;
    if (m_.plan.locked && now > m_.host0) {
        locked_before_ += now - m_.host0;
    }
    m_.plan = plan;
    m_.host0 = now;
    m_.guest0 = guest;
    m_.r = 1;
    m_.off = 0;
    if (plan.locked) {
        // The refresh period over a step: n x 1000 / (refresh x interval), 1 when the display is
        // exactly n x the tick rate.
        m_.r = static_cast<double>(plan.vblanks) * 1000.0 /
               (plan.refresh_hz * static_cast<double>(plan.tick_ms));
        // How far the guest clock is behind the wall clock now, in steps (the last plan's
        // rounding down to a refresh): the correction makes it up, so that plan changes never
        // add up to a drift.
        const double step_ns = static_cast<double>(plan.tick_ms) * 1e6 / plan.vblanks;
        m_.off = (static_cast<double>(now - host_start_) - static_cast<double>(guest)) / step_ns;
    }
    return true;
}

PacingPlan PacedClock::plan() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return m_.plan;
}

std::uint64_t PacedClock::guest_at(std::uint64_t host_ns) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return at(m_, host_ns);
}

std::uint64_t PacedClock::guest_ns() {
    const std::uint64_t host = now_(user_);
    std::uint64_t g = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        g = at(m_, host);
    }
    // Never back (a plan's first refresh, a host clock read on another thread).
    std::uint64_t seen = last_.load(std::memory_order_relaxed);
    while (seen < g && !last_.compare_exchange_weak(seen, g, std::memory_order_relaxed)) {
    }
    return g > seen ? g : seen;
}

std::uint64_t PacedClock::host_when(std::uint64_t guest_ns) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const Mapping &m = m_;
    if (guest_ns <= m.guest0) {
        return m.host0;
    }
    const std::uint64_t d = guest_ns - m.guest0;
    if (!m.plan.locked) {
        return m.host0 + d;
    }
    // The steps it takes (steps_ns(s) >= d), then the first refresh with that many.
    const std::uint64_t tick_ns = std::uint64_t{m.plan.tick_ms} * 1000000u;
    std::uint64_t s = d / tick_ns * m.plan.vblanks;
    while (steps_ns(s, m.plan) < d) {
        s++;
    }
    const double guess = (static_cast<double>(s) - 0.5 - m.off) / m.r;
    std::uint64_t k = guess > 0 ? static_cast<std::uint64_t>(std::ceil(guess)) : 0;
    while (k > 0 && steps_at(m, k - 1) >= s) {
        k--;
    }
    while (steps_at(m, k) < s) {
        k++;
    }
    return vblank_start(m, k);
}

std::uint64_t PacedClock::vblank_at(std::uint64_t host_ns) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return m_.plan.locked ? vblank_index(m_, host_ns) : 0;
}

std::uint64_t PacedClock::vblank_host(std::uint64_t k) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return m_.plan.locked ? vblank_start(m_, k) : m_.host0;
}

void PacedClock::sleep_until(std::uint64_t guest_ns_target) {
    while (guest_ns() < guest_ns_target) {
        const std::uint64_t when = host_when(guest_ns_target);
        const std::uint64_t now = now_(user_);
        // At least a little: a plan that changed meanwhile is read again.
        sleep_(user_, when > now ? when - now : 100000u);
    }
}

std::uint64_t PacedClock::locked_ns() const {
    const std::uint64_t now = now_(user_);
    const std::lock_guard<std::mutex> lock(mutex_);
    return locked_before_ + (m_.plan.locked && now > m_.host0 ? now - m_.host0 : 0);
}

std::uint64_t PresentHold::lead_ns() const {
    std::uint64_t lead = 0;
    for (const std::uint64_t w : works_) {
        lead = w > lead ? w : lead;
    }
    return lead;
}

std::uint64_t PresentHold::present_at(const PacedClock &clock, std::uint64_t ready) {
    if (!pending_) {
        return ready;
    }
    pending_ = false;
    const PacingPlan plan = clock.plan();
    // One refresh a tick: the next boundary is the next tick's wake, nothing to hold for.
    if (!plan.locked || plan.vblanks < 2 || ready < start_) {
        return ready;
    }
    // The present point stays before the next tick's wake (n refreshes on): a hold that long
    // would push that tick a refresh later. Longer works (a stall) are not counted.
    const auto period = static_cast<std::uint64_t>(1e9 / plan.refresh_hz);
    const std::uint64_t most = (plan.vblanks - 1) * period - kMarginNs - 1;
    const std::uint64_t work = ready - start_;
    if (work > most) {
        return ready;
    }
    works_[next_] = work;
    next_ = (next_ + 1) % kWorks;
    const std::uint64_t point = start_ + lead_ns() + kMarginNs;
    const std::uint64_t at = clock.vblank_host(clock.vblank_at(point) + 1);
    return at > ready ? at : ready;
}

} // namespace maplethief::frontend
