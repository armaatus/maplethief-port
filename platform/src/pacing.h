// Display-locked frame pacing (#80, ADR 0075): the real-time guest clock, stepped at the
// display's refreshes so that every game tick lasts the same whole number of them.
//
// The game ticks when its 1 ms run-loop timer finds `now - lastFrame >= interval` on the clock
// (docs/re/timing-threading.md). On the wall clock a 50 ms tick lasts 50.0-51.0 ms (the 1 ms
// poll, GetTimeMs' whole milliseconds), so its frames drift against a 60 Hz display's 16.7 ms
// refreshes: most are shown for 3 refreshes, every second or so one for 4 (judder). At 144 Hz,
// where a tick is 7.2 refreshes, a quarter alternate between 7 and 8.
//
// PacedClock is the guest's clock in real time (pacing = display), as VirtualClock (clock.h,
// ADR 0044) is in the fixed-step mode: gettimeofday, CFAbsoluteTimeGetCurrent and GCD's clock all
// read it. It maps host time to guest time in one of two ways:
//
//   wall    guest time = host time (since the mapping began)
//   locked  guest time stands still between the display's refreshes and steps at each one by
//           exactly interval / n (n refreshes a tick: 3 at 60 Hz, 6 at 120 Hz for 50 ms), so the
//           game's gate opens at every n-th refresh and nowhere else
//
// plan_pacing() picks one: locked when vsync is on and refresh x interval is within
// kLockTolerance of a whole n (60, 120, 100 Hz at the default 50 ms); the wall clock otherwise
// (144 Hz, 75 Hz, the 40 and 65 ms game speeds at 60 Hz, no vsync, an unknown refresh rate).
//
// Locked, the guest clock is corrected against the wall clock: at refresh k it reads
// round(k x refresh period / step) steps, so when the display is not exactly n x the tick rate
// (59.94 Hz, 60.06 Hz) a step is now and then held or doubled (that tick lasts n + 1 or n - 1
// refreshes) instead of the game drifting from the wall clock. The correction is against the
// clock's start, so plan changes (each rounds the guest clock down to a refresh) never add up.
// Over a run the game's speed is the wall clock's; at any moment the guest clock is within half a
// step plus one refresh of it.
// There is no catch-up to bound: after an overrun or a stall the guest clock simply reads the
// wall clock's time (rounded down to a refresh), and the game's own `lastFrame = now` ticks once
// (no burst), as on the device.
//
// Fast-forward (#151, ADR 0087): while the fast_forward key is held the plan is the wall clock
// at `rate` (2 or 3) times its speed: guest time = guest0 + rate x (host time - host0), the
// game's gate, its real-time timers and GCD's clock all run that much faster. A display-locked
// plan falls back to it while the key is held and locks again when it is let go; set_plan's
// rebase keeps the guest time continuous at both ends. The locked plans' correction is then
// against the wall clock plus the time fast-forward gained (the guest clock's reference), so
// a lock after a hold does not hold the clock still until the wall clock catches up.
//
// The refresh grid is the host's (refresh k at origin + k x period): the display's own vblank
// phase is not known (SDL_RenderPresent with vsync returns at once on Metal, ADR 0071). A frame
// is shown at the display's first vblank after its present, so a present whose time wanders
// (the tick's work takes 7 ms, then 9) can cross a vblank the next one does not: judder again.
// PresentHold takes that out: the first present after each wake waits for a refresh boundary of
// the grid (the first one after the recent ticks' longest work), so that every tick's frame is
// presented at the same point of the display's refresh, whatever its phase.
//
// A machine starved of CPU loses more speed locked than on the wall clock (#163, ADR 0130): a turn
// that wakes more than a refresh late starts its tick a whole refresh late, and the game's
// `lastFrame = now` keeps that. OverloadGuard watches the tick intervals of a locked run, takes the
// plan back to the wall clock while a large share of them are late, then tries the lock again.
//
// Pure: no SDL. The host's monotonic clock and sleep come in as functions (SDL_GetTicksNS and
// SDL_DelayPrecise in game.cpp, a fake in the tests).
#ifndef MAPLETHIEF_PLATFORM_PACING_H
#define MAPLETHIEF_PLATFORM_PACING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "settings.h"

namespace maplethief::frontend {

// How far refresh x interval / 1000 may be from a whole number of refreshes for the lock: 0.5 %
// (59.94 and 60.06 Hz lock; the guest clock's correction then holds or doubles a step about once
// every 1 / 0.001 refreshes).
inline constexpr double kLockTolerance = 0.005;

// What the guest clock follows.
struct PacingPlan {
    bool locked = false;       // stepped at the refreshes; false: the wall clock
    std::uint32_t vblanks = 0; // refreshes a tick (locked)
    double refresh_hz = 0;     // the display's refresh rate (locked)
    std::uint32_t tick_ms = 0; // the game's tick interval (locked)
    const char *why = "";      // why it is not locked (wall)
    std::uint32_t rate = 1;    // guest seconds a host second: 2 or 3 while fast-forwarding (wall)

    bool operator==(const PacingPlan &o) const {
        return locked == o.locked && vblanks == o.vblanks && refresh_hz == o.refresh_hz &&
               tick_ms == o.tick_ms && rate == o.rate;
    }
};

// The plan for a pacing setting, the display (refresh rate, 0: unknown; vsync), the game's tick
// interval in ms (CMTimer's, 0x11A478; 0 or less, or over 1000: unknown) and the fast-forward's
// rate (1: none; more: the wall clock that many times as fast, whatever the rest says).
PacingPlan plan_pacing(PacingMode mode, double refresh_hz, bool vsync, std::int64_t tick_ms,
                       std::uint32_t rate = 1);

// One line for the log: "locked to the display: 60.00 Hz, 3 refreshes a 50 ms tick", "the wall
// clock (vsync off)" or "the wall clock x2 (fast-forward)".
std::string describe(const PacingPlan &plan);

// The paced clock's start for a run launched at Unix time `wall_us` (#80 x #155): the wall clock
// less LaunchClock::shift_for(wall_us) (clock.h), so that the game's GetTimeMs starts in
// [0, 2^30), clear of its 2^31 jump. run_game constructs the paced clock with it and installs no
// LaunchClock beside it.
std::uint64_t paced_start_unix_us(std::uint64_t wall_us);

class PacedClock {
  public:
    // GCD's clock at the start, as VirtualClock's.
    static constexpr std::uint64_t kGcdOrigin = 1000000000ull;

    // The host's monotonic clock in nanoseconds, and a sleep of that many nanoseconds.
    using NowFn = std::uint64_t (*)(void *user);
    using SleepFn = void (*)(void *user, std::uint64_t ns);

    // `start_unix_us`: the guest's Unix time at the start (the host's wall clock when the run
    // starts: the game's random seed still differs per launch). Starts on the wall clock. A
    // launch offset goes here: run_game passes paced_start_unix_us(wall clock), #155's shift,
    // and does not install the LaunchClock too (both are the libc clock). The GCD clock is from
    // kGcdOrigin and needs no offset.
    PacedClock(NowFn now, SleepFn sleep, void *user, std::uint64_t start_unix_us);
    PacedClock(const PacedClock &) = delete;
    PacedClock &operator=(const PacedClock &) = delete;
    ~PacedClock() { uninstall(); }

    // Makes this clock the libc and GCD clock (as VirtualClock::install: before the process
    // loads; false if GCD refused), and gives them back.
    bool install();
    void uninstall();
    bool installed() const { return installed_; }

    // Follows `plan` from now on; the guest time goes on from where it is (continuous, never
    // back). Returns true if the plan changed: its mapping, or only the reason it is the wall
    // clock (`why`, kept without a rebase).
    bool set_plan(const PacingPlan &plan);
    PacingPlan plan() const;

    // Guest nanoseconds since the start, now (never less than an earlier reading).
    std::uint64_t guest_ns();
    // The mapping, without reading the host clock: guest nanoseconds at host time `host_ns`
    // (not before the current plan began).
    std::uint64_t guest_at(std::uint64_t host_ns) const;
    // The earliest host time at which the guest clock reads at least `guest_ns`.
    std::uint64_t host_when(std::uint64_t guest_ns) const;
    // Locked: the index of the refresh interval holding host time `host_ns` (since the plan
    // began; a new plan starts a new count) and the host time refresh `k` begins. Wall: 0 and
    // the plan's start.
    std::uint64_t vblank_at(std::uint64_t host_ns) const;
    std::uint64_t vblank_host(std::uint64_t k) const;

    // The guest's clocks.
    std::uint64_t unix_us() {
        return start_unix_us_.load(std::memory_order_relaxed) + guest_ns() / 1000u;
    }
    // The guest's Unix time is `unix_us` now and goes on from it (back or forward): a save
    // state's restore (#204, ADR 0105's addendum). Only the Unix clock moves (its start); the
    // paced mapping, the GCD clock and the lock to the display go on as they were.
    void rebase_to(std::uint64_t unix_us);
    std::uint64_t gcd_ns() { return kGcdOrigin + guest_ns(); }
    // Blocks until the guest clock reads at least `guest_ns` (GCD's sleep_until).
    void sleep_until(std::uint64_t guest_ns);

    // Host nanoseconds since the start, and how long the guest clock has been locked.
    std::uint64_t host_elapsed_ns() const { return now_(user_) - host_start_; }
    std::uint64_t locked_ns() const;
    // Fast-forward: the host time spent at a rate over 1, and the guest time it gained (the
    // guest clock's lead on the wall clock it makes, exactly rate - 1 times that host time).
    std::uint64_t fast_ns() const;
    std::uint64_t gained_ns() const;

  private:
    struct Mapping {
        PacingPlan plan;
        std::uint64_t host0 = 0;  // host time the mapping began
        std::uint64_t guest0 = 0; // guest time then
        double r = 1;             // locked: refresh period / step
        double off = 0;           // locked: reference - guest time at host0, in steps
        // The reference at host0: host time since the start plus what fast-forward gained
        // before (the guest time the clock would read had it never been locked).
        std::uint64_t ref0 = 0;
    };
    // The reference at host time `host_ns` (not before m.host0).
    static std::uint64_t ref_at(const Mapping &m, std::uint64_t host_ns);
    static std::uint64_t at(const Mapping &m, std::uint64_t host_ns);
    static std::uint64_t vblank_index(const Mapping &m, std::uint64_t host_ns);
    static std::uint64_t vblank_start(const Mapping &m, std::uint64_t k);
    static std::uint64_t steps_at(const Mapping &m, std::uint64_t k);

    NowFn now_;
    SleepFn sleep_;
    void *user_;
    std::atomic<std::uint64_t> start_unix_us_; // read by the guest's threads too
    std::uint64_t host_start_;
    mutable std::mutex mutex_; // the mapping: set by the turn hook, read by guest threads too
    Mapping m_;
    std::uint64_t locked_before_ = 0; // locked time of the plans before the current one
    std::uint64_t fast_before_ = 0;   // fast-forward time of the plans before the current one
    std::atomic<std::uint64_t> last_{0};
    bool installed_ = false;
};

// The overload guard (#163, ADR 0130): whether a display-locked plan is costing the game speed.
// Pure: every time comes in. The turn hook passes each turn's plan through apply() and, while that
// plan is locked, feeds add() each tick's interval once per window frame. Real-time runs with
// pacing = display only: a fixed-step run and pacing = wall never reach it.
//
// A tick is late when it lasted a refresh or more over the interval (kLateRefreshes of one, since
// the tick's own work moves its GL present by some milliseconds either way). When kLateShare or
// more of the ticks of the last kWindowNs were late, apply() returns the wall clock with the
// reason, and goes on doing so for kBackoffNs; then it hands the locked plan back and the count
// starts over, so a machine that is still starved leaves the lock again within a window.
//
// An interval of more than twice the tick is not counted at all, neither way: a map load stalls
// the game too, and one tick lost to a stall costs the same on either clock. Nor is a window
// decided on fewer than kMinShareOfTicks of the ticks that fit in it, so one stall followed by a
// few ticks cannot unlock either, and neither can the first ticks after a retry.
class OverloadGuard {
  public:
    static constexpr std::uint64_t kWindowNs = 2'000'000'000;
    static constexpr std::uint64_t kBackoffNs = 30'000'000'000;
    static constexpr double kLateRefreshes = 0.75; // of a refresh, over the tick's interval
    static constexpr double kLateShare = 0.15;     // of the window's ticks
    static constexpr double kMinShareOfTicks = 0.5;
    static constexpr std::size_t kCapacity = 1024; // more ticks than 2 s holds (a load's presents)

    // A tick of `tick_ns` ended at host time `now_ns` on `plan`. Ignored unless `plan` is locked
    // and the guard is not backing off. A new locked plan starts the count over.
    void add(std::uint64_t now_ns, std::uint64_t tick_ns, const PacingPlan &plan);
    // The plan to follow at host time `now_ns` for the plan the display allows: `plan` itself, or
    // the wall clock (`why` says so, with the numbers) while backing off. A plan that is not
    // locked goes through unchanged. The lock is retried here, when the back-off is over.
    PacingPlan apply(const PacingPlan &plan, std::uint64_t now_ns);

    bool backing_off() const { return retry_at_ != 0; }
    // How many times the guard left the lock, and why the last time ("" before the first).
    std::uint64_t unlocks() const { return unlocks_; }
    const char *reason() const { return reason_; }

  private:
    struct Tick {
        std::uint64_t at = 0;
        bool late = false;
    };
    void clear();
    void drop_oldest();

    PacingPlan plan_;            // the locked plan the count is for
    Tick ticks_[kCapacity] = {}; // the oldest at head_, count_ of them
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t late_ = 0;       // how many of them are late
    std::uint64_t retry_at_ = 0; // host time the lock is tried again (0: not backing off)
    std::uint64_t unlocks_ = 0;
    char reason_[128] = "";
};

// The present point (above): the turn hook wakes at a refresh boundary of the paced clock's grid
// (wake), and the first window present after it waits until the first boundary after the wake
// plus the longest work (wake to ready to present) of the last kWorks such presents, plus
// kMarginNs. At 60 Hz and the software rasterizer's ~9 ms that is one refresh after the wake.
// The point stays before the next tick's wake: a work longer than n - 1 refreshes (a stall, a
// map load's frame) is presented at once and not counted, and with one refresh a tick nothing
// is held.
class PresentHold {
  public:
    static constexpr std::size_t kWorks = 600;         // 30 s of ticks at 50 ms
    static constexpr std::uint64_t kMarginNs = 500000; // the wake's and the sleep's jitter

    // The turn hook woke at `refresh_start`, the host time the paced clock's refresh began.
    void wake(std::uint64_t refresh_start) {
        start_ = refresh_start;
        pending_ = true;
    }
    // Whether the next present is the first since a wake.
    bool pending() const { return pending_; }
    // The first present since the wake was held by another rule (frame interpolation's, #87):
    // nothing is held or counted for it.
    void skip() { pending_ = false; }
    // The first present since the wake is ready at host time `ready`: the host time to present
    // at (`ready` itself for any other present, or when the boundary has passed).
    std::uint64_t present_at(const PacedClock &clock, std::uint64_t ready);
    // The current lead: the longest recent work.
    std::uint64_t lead_ns() const;
    // The host time the wake began (the refresh boundary).
    std::uint64_t start() const { return start_; }
    // Frame interpolation (#87, ADR 0091): the grid boundary a present ready at `ready` is held
    // to, as present_at would return it (without counting the work), or the next boundary after
    // `ready` when it would not be held: the refresh its picture is shown from. Locked plans
    // only (`ready` itself otherwise).
    std::uint64_t shown_at(const PacedClock &clock, std::uint64_t ready) const;

  private:
    std::uint64_t start_ = 0;
    bool pending_ = false;
    std::uint64_t works_[kWorks] = {};
    std::size_t next_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PACING_H
