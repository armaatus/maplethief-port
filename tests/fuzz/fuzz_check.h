// FUZZ_FAIL(): a wrong answer from a parser is a finding too. Prints where and aborts, which
// libFuzzer catches and saves the input for (on arm64 __builtin_trap raises SIGTRAP, which
// libFuzzer does not catch, so the input would be lost).
#ifndef MAPLETHIEF_TESTS_FUZZ_FUZZ_CHECK_H
#define MAPLETHIEF_TESTS_FUZZ_FUZZ_CHECK_H

#include <cstdio>
#include <cstdlib>

#define FUZZ_FAIL()                                                                                \
    do {                                                                                           \
        std::fprintf(stderr, "FUZZ_FAIL at %s:%d\n", __FILE__, __LINE__);                          \
        std::abort();                                                                              \
    } while (0)

#endif // MAPLETHIEF_TESTS_FUZZ_FUZZ_CHECK_H
