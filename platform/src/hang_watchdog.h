// The hang watchdog (#337, ADR 0195): a frozen game leaves a report with the guest's position,
// as a crash does (crash_report.h).
//
//   the beat     hang_watchdog_beat(): every run-loop turn (game.cpp's on_turn), every pass of a
//                loop that holds the turn (the overlays, the key pad editor), and every event SDL
//                hands the main thread (an event watch: a macOS window drag runs its own loop).
//   the stall    no beat for `seconds` outside a pause: the watchdog's thread samples the guest's
//                thread (maplethief/diag.h: mt_hang_report, the host PC's guest function, the
//                registers and the symbolized r7 backtrace), writes the hang report
//                (hang_report_write) and asks Keep waiting or Quit (ask_hang_choice). Once per
//                stall: the next beat ends it. Quit ends the process with kExitHang; the crash
//                marker stays, so the next start offers the safe start.
//   the pauses   HangPause: while the window is hidden (the background) and inside a system
//                dialog the time does not count. The watch runs with the game (run_game), so the
//                import screen is never watched.
//   when         hang_watch_seconds: kHangSeconds, none in a --fixed-step run (its virtual clock
//                is not real time) unless --hang-seconds or --force-hang asks, and none on iOS,
//                Android and the Switch, where the functions compile and do nothing.
#ifndef MAPLETHIEF_PLATFORM_HANG_WATCHDOG_H
#define MAPLETHIEF_PLATFORM_HANG_WATCHDOG_H

#include <cstdint>

#include "maplethief/recomp/cpu.h"

namespace maplethief::frontend {

// Seconds without a beat that make a hang. The longest real-asset turn measured is far below it
// (ADR 0195).
inline constexpr double kHangSeconds = 15.0;

// Whether this platform watches at all (desktops only).
bool hang_watchdog_supported();

// The watch of a run, in seconds (0: none). `asked`: --hang-seconds (negative: not given);
// `fixed_step`: a --fixed-step run; `forced`: --force-hang.
double hang_watch_seconds(double asked, bool fixed_step, bool forced);

// Starts the watch of the calling thread, which runs `cpu`, with its first beat now. 0 seconds,
// or an unsupported platform: nothing. Stop it before `cpu` goes away.
void hang_watchdog_start(mt_cpu *cpu, double seconds);
void hang_watchdog_stop();
bool hang_watchdog_running();

void hang_watchdog_beat();

// While one exists the time does not count (nested pauses are fine). The main thread's.
class HangPause {
  public:
    HangPause();
    ~HangPause();
    HangPause(const HangPause &) = delete;
    HangPause &operator=(const HangPause &) = delete;
};

struct HangStats {
    double longest_gap = 0;  // seconds: the longest time between two beats without a pause
    std::uint32_t hangs = 0; // stalls reported
    double seconds = 0;      // the watch's limit (0: none)
};
// The current (or last) watch's numbers.
HangStats hang_watchdog_stats();

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_HANG_WATCHDOG_H
