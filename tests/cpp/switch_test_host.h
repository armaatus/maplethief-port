// Every test NRO's main on the Nintendo Switch (#236; doctest_main.cpp calls it, nothing
// elsewhere). Output goes over nxlink when scripts/switch-run (scripts/switch-test) sent the NRO,
// else to the console on the screen, and in both cases to a results file on the SD card:
// sdmc:/switch/maplethief-tests/results.txt for MapleThiefTests.nro, <program>-results.txt for
// the other test NROs. The first lines say how the program runs (Horizon's version, applet mode
// or title takeover, the process's memory). TMPDIR is sdmc:/switch/maplethief-tests/tmp, so the
// tests' scratch files are on the SD card's file system, like the saves.
#ifndef MAPLETHIEF_TESTS_CPP_SWITCH_TEST_HOST_H
#define MAPLETHIEF_TESTS_CPP_SWITCH_TEST_HOST_H

#include <ostream>

namespace mt_test_switch {

// Connects the output and opens the results file; the stream to hand to doctest.
std::ostream *begin(int argc, char **argv);

// Whether argv asks for --crash-test, and the forced guest fault (MapleThiefTests.nro only:
// tests/cpp/switch_crash_test.cpp). It does not return when the fault happens.
bool crash_test_requested(int argc, char **argv);
int run_crash_test();

// The last line, the results file closed; on the console, waits for + before returning.
int end(int result);

} // namespace mt_test_switch

#endif // MAPLETHIEF_TESTS_CPP_SWITCH_TEST_HOST_H
