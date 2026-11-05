// Game speed and fast-forward (#151, ADR 0087): two Assists, off in every preset.
//
// game_speed (settings.h's GameSpeed). The game ticks when CMTimer's interval (the i64 at
// 0x11A478) has passed on its clock; CDataMgr::ApplyGameSpeed 0x37C41 sets it, from
// LoadConfig at boot and from the options menu, to g_gameSpeedIntervals[config[1]]: the i32
// table {65, 50, 40, -1} at 0x108C58, indexed by the Speed option's byte of the game's config
// (docs/re/timing-threading.md). A game_speed other than `game` writes its tick length into the
// table's three entries:
//   - at the start, after the process loads and before it runs (patch_game_speed): LoadConfig's
//     ApplyGameSpeed then sets it, as it sets the game's own;
//   - while the game runs (apply_game_speed, from the turn hook, for the settings menu): the
//     table again, and CMTimer's interval directly, as ApplyGameSpeed's SetFrameInterval
//     0x21DA1 would (`game` puts back the game's table and the Speed option's own length).
// The game's Speed option then changes nothing (each of its values reads the same length), and
// the config file keeps the option's own index (0 to 2): a port-only length never reaches it,
// and #78's clamp of a bad index (mt_saves_sanitise) is untouched. `game` writes nothing at the
// start: the run is the original's, byte for byte.
//
// fast_forward (settings.h's FastForward): fast_forward_rate() is the guest clock's rate while
// the fast_forward action is held (actions.h: a port action, never sent to the guest). game.cpp
// passes it to the paced clock's plan (pacing.h).
#ifndef MAPLETHIEF_PLATFORM_GAME_SPEED_H
#define MAPLETHIEF_PLATFORM_GAME_SPEED_H

#include <cstddef>
#include <cstdint>

#include "maplethief/recomp/cpu.h"
#include "settings.h"

namespace maplethief::frontend {

// The game's addresses (armv7 VM addresses; docs/re/timing-threading.md).
inline constexpr std::uint32_t kGameSpeedTable = 0x108C58; // g_gameSpeedIntervals, i32[4]
inline constexpr std::uint32_t kFrameInterval = 0x11A478;  // CMTimer's interval, i64 ms
inline constexpr std::uint32_t kConfigPointer = 0x11A98C;  // the config's 6 bytes (a pointer)
// The table as the game ships it: 65, 50 and 40 ms, then the popup id that a speed byte of 3
// reads (-1).
inline constexpr std::int32_t kGameSpeedLengths[3] = {65, 50, 40};

// The tick length of a game_speed in ms; 0 for `game` (the game's own option decides).
std::uint32_t game_speed_ms(GameSpeed speed);

// Writes `speed`'s table into guest memory: its length three times, or for `game` the game's
// own {65, 50, 40}. False (nothing written) when the guest does not map the table or the table
// is not the game's ({65, 50, 40, -1}, or a length written before, three times, then -1).
bool patch_game_speed(mt_cpu *cpu, GameSpeed speed);

// A change while the game runs: patch_game_speed, then CMTimer's interval, once the game has
// its config (the pointer at 0x11A98C set): `speed`'s length, or for `game` the game's own
// length for its Speed option (config byte 1, clamped to 2 as mt_saves_sanitise reads it).
// Returns the interval written (ms), or 0 if none was (no table, no config yet).
std::int64_t apply_game_speed(mt_cpu *cpu, GameSpeed speed);

// The guest clock's rate: 2 or 3 while the fast-forward key is held with fast_forward on, else 1.
std::uint32_t fast_forward_rate(FastForward ff, bool held);

// The tick intervals over this are overruns (FrameStats::set_overrun_ns): 1.1 x the tick's
// length on the host's clock (interval / rate). The default's 55 ms for an unknown interval.
std::uint64_t overrun_ns_for(std::int64_t tick_ms, std::uint32_t rate);

// CMTimer's lastFrame (i64, GetTimeMs at the last tick's gate; docs/re/timing-threading.md).
inline constexpr std::uint32_t kLastFrame = 0x11A4D8;
// No gate to wait for: the guest does not map CMTimer's globals, or its interval is not a tick's.
inline constexpr std::int64_t kNoGate = INT64_MAX;

// The guest time until the game's tick gate opens, in microseconds, at the guest's Unix time
// `unix_us` (the libc clock's): CMTimer::OnRunLoopTimer ticks at the first poll where
// GetTimeMs - lastFrame >= interval, so that is lastFrame + interval less GetTimeMs now, in whole
// milliseconds, less the fraction of the current one. 0 or less: the gate is open. kNoGate when
// the guest does not map lastFrame and the interval, or the interval is outside (0, 1000].
std::int64_t gate_in_us(const mt_cpu *cpu, std::uint64_t unix_us);

// Fast-forward's wait for the game's gate (ADR 0087 addendum). The game polls its gate from a
// 1 ms run-loop timer re-armed 1 ms after each callout, so a tick starts at the first poll after
// the gate opens: up to a poll late, plus the host time of a turn, which counts 2 or 3 times
// over on the guest's clock while fast-forwarding (a whole millisecond and more of a 25 or
// 16.7 ms tick). When the gate opens after the next timer but within kGatePollUs of it, the
// turn hook waits for the gate instead (plus kGateMarginUs, so that the wake never lands a
// hair before it): the timer, due by then, fires once and finds the gate open. Only the poll
// that would find the gate shut is left out; a gate further off keeps the 1 ms polls.
inline constexpr std::int64_t kGatePollUs = 1000;
inline constexpr std::int64_t kGateMarginUs = 5;
// The guest seconds to wait: `timer_s` to the run loop's next wake, or to the gate `gate_us`
// guest microseconds from now (gate_in_us) when it opens within kGatePollUs after the wake.
double ff_gate_wait(double timer_s, std::int64_t gate_us);

// Fast-forward's window presents (ADR 0087 addendum). The game's gate takes
// `lastFrame = now` with no catch-up, so any host work that runs past the moment the gate opens
// makes that tick, and every later one, late: the game slows down. A window present is such work
// (the CPU composite alone is 5 ms in Release and 18 ms in Debug on the dummy driver). At 1x a
// tick's frame is presented inside the tick, as the device presents, with most of the 50 ms tick
// still ahead; at 2x or 3x the tick lasts 25 or 16.7 ms of host time, and a present that starts
// late in it (a frame held back by the one-a-refresh limit, a UIKit animation's) runs over the
// next gate.
//
// So while fast-forwarding a present starts only when it ends before the gate opens: the
// longest of the last kCosts presents (plus kMarginNs) fits in the host time left. Where none
// fits (a present longer than the tick's slack: Debug's CPU composite at 2x and 3x) the window
// would freeze; a lateness budget lets one through now and then instead. The budget grows by
// kShare of the host time and pays the overrun each such present is expected to cost, so the
// presents slow the game by at most about kShare. A refresh's limit stays: at most one present a
// display refresh. The cost is the present's whole host time, a SDL_RenderPresent that blocks
// for vsync included: that too is time the gate waits.
//
// Pure (no SDL): host times come in as nanoseconds.
class FastForwardPresents {
  public:
    static constexpr std::size_t kCosts = 8;
    static constexpr std::uint64_t kMarginNs = 1'000'000;
    static constexpr double kShare = 0.01;

    // A window present took `ns`, from its start to SDL_RenderPresent's return.
    void presented(std::uint64_t ns);
    // The expected cost of the next present: the longest recent one plus the margin.
    std::uint64_t cost_ns() const;
    // A fast-forward begins (or ends): no budget, no accrual since an earlier one.
    void restart();
    // Whether a present may start at host time `now`, `since` host nanoseconds after the last
    // present ended, at most one a `refresh_ns` (the display's period), with the game's gate
    // opening `gate_ns` host nanoseconds from now (0 or less: open; kNoGate: no gate known).
    bool allow(std::uint64_t now, std::uint64_t since, std::uint64_t refresh_ns,
               std::int64_t gate_ns);
    // The budget, in host nanoseconds of lateness; the presents allowed, and of them those the
    // budget let through (both over every fast-forward of the run).
    double budget_ns() const { return budget_; }
    std::uint64_t allowed() const { return allowed_; }
    std::uint64_t overran() const { return overran_; }

  private:
    std::uint64_t costs_[kCosts] = {};
    std::size_t next_ = 0;
    double budget_ = 0;
    std::uint64_t last_ = 0; // the host time of the last allow (0: none since restart)
    std::uint64_t allowed_ = 0;
    std::uint64_t overran_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GAME_SPEED_H
