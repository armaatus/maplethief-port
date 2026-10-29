// What the objc_test executable's test files share: its mt_trap / mt_dispatch test doubles
// (defined in objc_test.cpp).
#ifndef MAPLETHIEF_TESTS_CPP_OBJC_TEST_H
#define MAPLETHIEF_TESTS_CPP_OBJC_TEST_H

#include <cstdint>

#include "maplethief/recomp/recomp.h"

namespace objc_test {

// kind, pc, detail of the last mt_trap.
extern std::uint32_t g_trap[3];

// Makes mt_dispatch return `fn` for guest code address `imp` (a test's guest function).
void add_imp(std::uint32_t imp, mt_guest_fn fn);

// Runs fn(arg); returns true if it stopped through mt_trap. No C++ object with a destructor
// may live in fn's frames across the trap: the stop is a longjmp.
bool stops(void (*fn)(void *), void *arg);

} // namespace objc_test

#endif // MAPLETHIEF_TESTS_CPP_OBJC_TEST_H
