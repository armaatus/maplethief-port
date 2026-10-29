// The virtual clock of the fixed-step mode (#60, ADR 0044): one time source behind every clock
// the guest can read, advanced by the host instead of by the wall clock, so that a scripted run
// reaches the same frame bit for bit every time.
//
//   the guest's clocks                     where they read the time
//     gettimeofday, CFAbsoluteTimeGetCurrent  mt_libc_config.clock (Unix microseconds)
//     UIKit's animations, the run loop        mt_cf_absolute_time (the same libc clock)
//     dispatch_time, dispatch_after           mt_gcd_config.clock (monotonic nanoseconds)
//     dispatch_group_wait's sleep             mt_gcd_config.sleep_until
//
// VirtualClock::install() points all of them at one counter of virtual nanoseconds since the
// start: Unix time is kStartUnixUs plus it, GCD time is kGcdOrigin plus it. Nothing advances it
// but the host: the frontend's turn hook calls advance() once per run-loop turn, and GCD's
// sleep_until moves it to the deadline (a wait on the main thread costs no host time). Inside a
// turn the time stands still, as if every guest call took no time at all.
//
// The start, 2025-01-01 00:00:00 UTC, puts the game's 32-bit GetTimeMs at 522,812,416
// (docs/re/timing-threading.md): positive and 18.8 days before its 2^31 jump, which would
// freeze the game's ticks. The random seed (the low 32 bits of GetTimeMs at boot) is the same
// on every run.
#ifndef MAPLETHIEF_PLATFORM_CLOCK_H
#define MAPLETHIEF_PLATFORM_CLOCK_H

#include <atomic>
#include <cstdint>

namespace maplethief::frontend {

class VirtualClock {
  public:
    // 2025-01-01T00:00:00Z in microseconds since the Unix epoch.
    static constexpr std::uint64_t kStartUnixUs = 1735689600ull * 1000000ull;
    // GCD's clock at the start: positive and well away from DISPATCH_TIME_NOW (0), as the
    // host clock it replaces.
    static constexpr std::uint64_t kGcdOrigin = 1000000000ull;

    VirtualClock() = default;
    VirtualClock(const VirtualClock &) = delete;
    VirtualClock &operator=(const VirtualClock &) = delete;
    ~VirtualClock() { uninstall(); }

    // Makes this clock the libc and GCD clock (mt_libc_configure, mt_gcd_configure). Call it
    // before the process loads (GCD accepts a configuration only while it has no worker);
    // returns false if GCD refused.
    bool install();
    // Gives the libc and GCD clocks back to the host (their defaults). The process must be
    // gone (mt_process_destroy resets GCD's configuration anyway).
    void uninstall();
    bool installed() const { return installed_; }

    // Virtual nanoseconds since the start.
    std::uint64_t elapsed_ns() const { return ns_.load(std::memory_order_relaxed); }
    // Moves the time forward by `ns`.
    void advance(std::uint64_t ns) { ns_.fetch_add(ns, std::memory_order_relaxed); }
    // Moves the time forward to `elapsed` nanoseconds since the start (never back).
    void advance_to(std::uint64_t elapsed);

    // The clocks as the guest sees them.
    std::uint64_t unix_us() const { return kStartUnixUs + elapsed_ns() / 1000u; }
    std::uint64_t gcd_ns() const { return kGcdOrigin + elapsed_ns(); }

  private:
    // Read from GCD's worker threads too (each runs only while it holds GCD's baton).
    std::atomic<std::uint64_t> ns_{0};
    bool installed_ = false;
};

// The game's GetTimeMs at a Unix time: (int32)(tv_sec * 1000 + tv_usec / 1000), the 32-bit
// multiply-add of 0xA0D5.
std::int32_t guest_time_ms(std::uint64_t unix_us);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CLOCK_H
