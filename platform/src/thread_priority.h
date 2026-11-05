// The main thread's priority in a real-time run (#206, ADR 0120).
//
// The game's tick gate takes `lastFrame = now` with no catch-up (ADR 0075), so a wake that comes
// late because another process holds the core makes that tick late for good. In a real-time run
// of the game the main thread (the guest's run loop, the render and the presents) asks for SDL's
// SDL_THREAD_PRIORITY_HIGH at start-up:
//
//   macOS, iOS  SCHED_RR at priority 45 (pthread_setschedparam): a fixed priority above the
//               default 31 that other processes' threads start from and decay below, and below
//               SDL's audio device thread (SDL_THREAD_PRIORITY_TIME_CRITICAL: SCHED_RR 47).
//   Windows     THREAD_PRIORITY_HIGHEST (+2); the audio thread is THREAD_PRIORITY_TIME_CRITICAL.
//   Linux       nice -10 for the thread (setpriority), else through RealtimeKit (D-Bus); without
//               the rights for either it is refused and the thread keeps its nice value.
//
// The loop sleeps between ticks (the gate's 1 ms polls), so a raised thread takes a core only
// for a tick's work. A fixed-step run (--fixed-step) never waits and keeps the default: it would
// hold a core at the raised priority for the whole run. MAPLETHIEF_MAIN_THREAD_PRIORITY=normal
// leaves the priority alone (an A/B measurement, or a machine where the raise misbehaves).
#ifndef MAPLETHIEF_PLATFORM_THREAD_PRIORITY_H
#define MAPLETHIEF_PLATFORM_THREAD_PRIORITY_H

#include <string>

namespace maplethief::frontend {

// The calling thread's scheduling as the platform reports it.
struct ThreadPriority {
    bool known = false;      // false: the platform does not say (nothing below is set)
    const char *policy = ""; // "SCHED_OTHER", "SCHED_RR", "SCHED_FIFO", "nice" or "Windows"
    int value = 0;           // the POSIX priority, the nice value or GetThreadPriority's
};

// The calling thread's priority now.
ThreadPriority current_thread_priority();

// "SCHED_RR priority 45", "nice -10", "Windows priority 2 (THREAD_PRIORITY_HIGHEST)", or
// "unknown".
std::string describe(const ThreadPriority &priority);

// What raise_main_thread_priority did.
struct PriorityRaise {
    bool asked = false;  // false: not asked for (`why_not` says why)
    std::string why_not; // "a fixed-step run", "MAPLETHIEF_MAIN_THREAD_PRIORITY=normal"
    bool raised = false; // SDL accepted SDL_THREAD_PRIORITY_HIGH
    ThreadPriority before;
    ThreadPriority after;
    std::string error; // SDL's error when refused
};

// Whether MAPLETHIEF_MAIN_THREAD_PRIORITY's value (nullptr: unset) asks for the raise: anything
// but "normal" (ASCII case-insensitively) does.
bool main_thread_priority_wanted(const char *env_value);

// Raises the calling thread to SDL_THREAD_PRIORITY_HIGH unless `why_not` is non-empty (then it
// only reads the priority). A refusal is not an error: the thread keeps what it had.
PriorityRaise raise_main_thread_priority(const std::string &why_not);

// The start-up log's line (without the "game: " prefix the caller adds), e.g.
// "main thread: SCHED_RR priority 45 (SDL's high priority; it was SCHED_OTHER priority 31)",
// "main thread: SDL's high priority refused (setpriority() failed); it keeps nice 0", or
// "main thread: SCHED_OTHER priority 31, the default (a fixed-step run)".
std::string describe_raise(const PriorityRaise &raise);

} // namespace maplethief::frontend

#endif
