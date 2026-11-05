// The main thread's priority (#206, thread_priority.h, ADR 0120): the opt-out's values, the
// descriptions of the platforms' priorities, the start-up line for each outcome, and a real raise
// on a thread of the test's own (raised, or refused and left as it was where the system says no:
// Linux without the rights). The game's side (the line in every real-time run's start-up log, the
// audio callback's thread above it) is platform.game_speed's and the exit log's.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <string>
#include <thread>

#include "thread_priority.h"

namespace {

namespace fe = maplethief::frontend;

fe::ThreadPriority priority(const char *policy, int value) {
    fe::ThreadPriority p;
    p.known = true;
    p.policy = policy;
    p.value = value;
    return p;
}

} // namespace

TEST_CASE("thread priority: MAPLETHIEF_MAIN_THREAD_PRIORITY=normal is the only opt-out") {
    CHECK(fe::main_thread_priority_wanted(nullptr));
    CHECK(fe::main_thread_priority_wanted(""));
    CHECK(fe::main_thread_priority_wanted("high"));
    CHECK(fe::main_thread_priority_wanted("normally"));
    CHECK_FALSE(fe::main_thread_priority_wanted("normal"));
    CHECK_FALSE(fe::main_thread_priority_wanted("Normal"));
    CHECK_FALSE(fe::main_thread_priority_wanted("NORMAL"));
}

TEST_CASE("thread priority: the descriptions") {
    CHECK(fe::describe(fe::ThreadPriority{}) == "unknown");
    CHECK(fe::describe(priority("SCHED_RR", 45)) == "SCHED_RR priority 45");
    CHECK(fe::describe(priority("SCHED_OTHER", 31)) == "SCHED_OTHER priority 31");
    CHECK(fe::describe(priority("nice", -10)) == "nice -10");
    CHECK(fe::describe(priority("Windows", 2)) == "Windows priority 2 (THREAD_PRIORITY_HIGHEST)");
    CHECK(fe::describe(priority("Windows", 15)) ==
          "Windows priority 15 (THREAD_PRIORITY_TIME_CRITICAL)");
    CHECK(fe::describe(priority("Windows", 7)) == "Windows priority 7");
}

TEST_CASE("thread priority: the start-up line for each outcome") {
    fe::PriorityRaise r;
    r.before = priority("SCHED_OTHER", 31);
    r.after = priority("SCHED_RR", 45);
    r.asked = true;
    r.raised = true;
    CHECK(fe::describe_raise(r) ==
          "main thread: SCHED_RR priority 45 (SDL's high priority; it was SCHED_OTHER priority "
          "31)");

    r.raised = false;
    r.error = "setpriority() failed";
    r.after = priority("nice", 0);
    CHECK(fe::describe_raise(r) ==
          "main thread: SDL's high priority refused (setpriority() failed); it keeps nice 0");
    r.error.clear();
    CHECK(fe::describe_raise(r) ==
          "main thread: SDL's high priority refused (no reason given); it keeps nice 0");

    fe::PriorityRaise off;
    off.why_not = "a fixed-step run";
    off.after = priority("SCHED_OTHER", 31);
    CHECK(fe::describe_raise(off) ==
          "main thread: SCHED_OTHER priority 31, the default (a fixed-step run)");
}

TEST_CASE("thread priority: a real raise, on a thread of its own") {
    // A thread of its own: the raise must not outlive the test on the runner's main thread.
    fe::PriorityRaise kept;
    fe::PriorityRaise raised;
    std::thread([&] {
        kept = fe::raise_main_thread_priority("a fixed-step run");
        raised = fe::raise_main_thread_priority("");
    }).join();

    // Not asked: only read, and nothing changes.
    CHECK_FALSE(kept.asked);
    CHECK_FALSE(kept.raised);
    CHECK(kept.before.known);
    CHECK(fe::describe(kept.before) == fe::describe(kept.after));

    CHECK(raised.asked);
    CHECK(raised.after.known);
    const std::string line = fe::describe_raise(raised);
    INFO(line);
    CHECK(line.rfind("main thread: ", 0) == 0);
    if (raised.raised) {
#if defined(__APPLE__)
        // SDL's high priority on Apple: SCHED_RR 45, under the audio thread's time-critical 47.
        CHECK(fe::describe(raised.after) == "SCHED_RR priority 45");
#elif defined(_WIN32)
        CHECK(fe::describe(raised.after) == "Windows priority 2 (THREAD_PRIORITY_HIGHEST)");
#elif defined(__linux__)
        CHECK(fe::describe(raised.after) == "nice -10");
#endif
    } else {
        // Refused (Linux without CAP_SYS_NICE or RealtimeKit): the thread keeps what it had.
        CHECK(line.find("refused") != std::string::npos);
        CHECK(fe::describe(raised.after) == fe::describe(raised.before));
    }
}
