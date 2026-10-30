// The virtual clock (#60, ADR 0044) and the real-time mode's launch clock (#155); see clock.h.
#include "clock.h"

#include <chrono>

#include "maplethief/gcd.h"
#include "maplethief/hle/libc.h"

namespace maplethief::frontend {

namespace {

std::uint64_t libc_clock(void *user) { return static_cast<const VirtualClock *>(user)->unix_us(); }

std::uint64_t gcd_clock(void *user) { return static_cast<const VirtualClock *>(user)->gcd_ns(); }

// A wait on the main thread (dispatch_group_wait with a timeout, a worker's timeout): the time
// jumps to the deadline, so the wait costs no host time and ends at the same virtual instant on
// every run.
void gcd_sleep_until(void *user, std::uint64_t deadline) {
    auto *clock = static_cast<VirtualClock *>(user);
    if (deadline > VirtualClock::kGcdOrigin) {
        clock->advance_to(deadline - VirtualClock::kGcdOrigin);
    }
}

// The host's wall clock in microseconds since the Unix epoch (the libc default's).
std::uint64_t host_wall_us() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

std::uint64_t launch_clock(void *user) { return static_cast<const LaunchClock *>(user)->unix_us(); }

} // namespace

void VirtualClock::advance_to(std::uint64_t elapsed) {
    std::uint64_t cur = ns_.load(std::memory_order_relaxed);
    while (cur < elapsed && !ns_.compare_exchange_weak(cur, elapsed, std::memory_order_relaxed)) {
    }
}

bool VirtualClock::install() {
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

void VirtualClock::uninstall() {
    if (!installed_) {
        return;
    }
    mt_libc_configure(nullptr);
    mt_gcd_configure(nullptr); // -1 (kept) only while a worker exists: the process is gone
    installed_ = false;
}

std::uint64_t LaunchClock::shift_for(std::uint64_t wall_us) {
    const std::uint32_t ms = static_cast<std::uint32_t>(wall_us / 1000u); // GetTimeMs, unsigned
    return static_cast<std::uint64_t>(ms & 0xC0000000u) * 1000u;
}

void LaunchClock::install() {
    shift_us_ = shift_for(host_wall_us());
    mt_libc_config libc = mt_libc_config_default();
    libc.clock = launch_clock;
    libc.clock_user = this;
    mt_libc_configure(&libc);
    installed_ = true;
}

void LaunchClock::uninstall() {
    if (!installed_) {
        return;
    }
    mt_libc_configure(nullptr);
    installed_ = false;
}

std::uint64_t LaunchClock::unix_us() const { return host_wall_us() - shift_us_; }

std::int32_t guest_time_ms(std::uint64_t unix_us) {
    const std::uint32_t sec = static_cast<std::uint32_t>(unix_us / 1000000u);
    const std::uint32_t usec = static_cast<std::uint32_t>(unix_us % 1000000u);
    return static_cast<std::int32_t>(sec * 1000u + usec / 1000u);
}

} // namespace maplethief::frontend
