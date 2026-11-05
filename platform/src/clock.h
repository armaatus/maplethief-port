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
#include <mutex>

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
    // Sets the time to the guest's Unix time `unix_us` (back or forward): a save state's restore
    // (#202, ADR 0105), after which the guest's time goes on from the state's. A time before
    // the start is the start.
    void rebase_to(std::uint64_t unix_us);

    // The clocks as the guest sees them.
    std::uint64_t unix_us() const { return kStartUnixUs + elapsed_ns() / 1000u; }
    std::uint64_t gcd_ns() const { return kGcdOrigin + elapsed_ns(); }

  private:
    // Read from GCD's worker threads too (each runs only while it holds GCD's baton).
    std::atomic<std::uint64_t> ns_{0};
    bool installed_ = false;
};

// The real-time mode's clock (#155, docs/re/original-bugs.md): the host's wall clock, moved back
// at launch by a whole number of 2^30 ms (0 to 3, at most 37.3 days) so that the game's
// GetTimeMs starts in [0, 2^30). The original freezes its ticks for 49.7 days when GetTimeMs
// jumps from +2^31 - 1 to -2^31 while it runs (docs/re/timing-threading.md); from such a start
// the jump is at least 2^30 ms (12.4 days) of running away. Everything else is the wall clock
// as the device's: the low 30 bits of GetTimeMs, so the random seed still differs per launch,
// the gaps of a suspend or a sleep, a clock set by hand. Only gettimeofday and
// CFAbsoluteTimeGetCurrent see it (mt_libc_config.clock); GCD keeps the host's clocks.
//
// On top of another clock source (#80's paced clock, or any Unix-microsecond clock): either
//   - install(source, user): this becomes the libc clock and reads source(user) less the shift
//     taken from source's first reading (install it after anything else that sets the libc
//     clock, since mt_libc_configure keeps only the last); or
//   - the other clock subtracts LaunchClock::shift_for(its Unix time at launch) itself, once, and
//     no LaunchClock is installed.
// Either way the shift is fixed for the run, so the guest's time moves exactly as the source's.
//
// Fast-forward (#151, ADR 0087), with pacing = wall: set_rate(2 or 3) runs the guest's time that
// many times as fast as the source's from now on, and set_rate(1) back at the source's speed;
// each change goes on from the time it reads then (continuous), and what the faster time gained
// stays (gained_us). GCD keeps the host's clocks: only Chartboost's request timeout uses them
// (docs/re/timing-threading.md).
class LaunchClock {
  public:
    // A Unix-microsecond clock (the shape of mt_libc_config.clock, maplethief/hle/libc.h).
    using Source = std::uint64_t (*)(void *user);

    // The shift for a launch at Unix time `wall_us`: the bits 30 and 31 of its GetTimeMs, in
    // microseconds. unix_us - shift_for(unix_us) has GetTimeMs in [0, 2^30).
    static std::uint64_t shift_for(std::uint64_t wall_us);

    LaunchClock() = default;
    LaunchClock(const LaunchClock &) = delete;
    LaunchClock &operator=(const LaunchClock &) = delete;
    ~LaunchClock() { uninstall(); }

    // Takes the shift from `source` now (nullptr: the host's wall clock) and makes this the
    // libc clock: source's time less the shift. Call it before the process loads (the random
    // seed is GetTimeMs at boot).
    void install(Source source = nullptr, void *user = nullptr);
    // Gives the libc clock back to the host.
    void uninstall();
    bool installed() const { return installed_; }

    // The clock as the guest sees it: the source's time now, less the shift (and, after a
    // fast-forward, plus what it gained).
    std::uint64_t unix_us() const;
    std::uint64_t shift_us() const { return shift_us_; }

    // The guest's time runs `rate` times as fast as the source's from now (1: as fast).
    void set_rate(std::uint32_t rate);
    // The guest's time is `guest_us` now and goes on from it at the current rate: a save
    // state's restore (#202, ADR 0105). What a fast-forward gained so far stays counted.
    void rebase_to(std::uint64_t guest_us);
    std::uint32_t rate() const;
    // The guest time the fast-forward gained on the source's (microseconds).
    std::uint64_t gained_us() const;

  private:
    std::uint64_t source_us() const;
    std::uint64_t unix_at(std::uint64_t source) const; // with mutex_ held

    Source source_ = nullptr;
    void *source_user_ = nullptr;
    std::uint64_t shift_us_ = 0;
    bool installed_ = false;
    // The fast-forward's mapping: guest = base_guest_ + rate_ x (source - base_source_) from the
    // last set_rate on (before any: source - shift). Read by the guest's threads.
    mutable std::mutex mutex_;
    bool rebased_ = false;
    std::uint32_t rate_ = 1;
    std::uint64_t base_source_ = 0;
    std::uint64_t base_guest_ = 0;
    std::int64_t gained_ = 0; // gained before the current mapping
};

// The game's GetTimeMs at a Unix time: (int32)(tv_sec * 1000 + tv_usec / 1000), the 32-bit
// multiply-add of 0xA0D5.
std::int32_t guest_time_ms(std::uint64_t unix_us);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_CLOCK_H
