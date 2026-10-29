// The test executable's mt_trap (maplethief/recomp/calls.h): it records the trap and, inside
// mt_test::stops, jumps back to the test instead of ending the process.
#ifndef MAPLETHIEF_TESTS_CPP_GUEST_TRAP_H
#define MAPLETHIEF_TESTS_CPP_GUEST_TRAP_H

#include <cstdint>

namespace mt_test {

struct Trap {
    std::uint32_t kind;
    std::uint32_t pc;
    std::uint32_t detail;
};

// The last trap of this executable.
extern Trap g_trap;

// Runs fn(arg) and returns true if it stopped through mt_trap (the trap is in g_trap). A
// trap outside stops() aborts. No C++ object with a destructor may live in fn's frames
// across the trap: the stop is a longjmp.
bool stops(void (*fn)(void *), void *arg);

} // namespace mt_test

#endif // MAPLETHIEF_TESTS_CPP_GUEST_TRAP_H
