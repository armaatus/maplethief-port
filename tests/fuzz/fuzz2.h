// What every fuzz target of #323 (tests/fuzz/targets2.cmake) defines besides
// LLVMFuzzerTestOneInput, for the shared driver (fuzz2_driver.cpp):
//
//   init()   one-time setup (a synthetic guest for the state file); may do nothing
//   seeds()  the target's synthetic seed corpus, made in code: no seed is a checked-in binary,
//            and none comes from the IPA (ADR 0100)
//
// The driver writes the seeds (MT_FUZZ_WRITE_SEEDS=<dir> or --write-seeds <dir>) and, without
// libFuzzer, replays seeds, files and directories through the target (--seeds, paths).
#ifndef MAPLETHIEF_TESTS_FUZZ_FUZZ2_H
#define MAPLETHIEF_TESTS_FUZZ_FUZZ2_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mt_fuzz2 {

using Bytes = std::vector<std::uint8_t>;

struct Seed {
    std::string name; // a file name: [A-Za-z0-9._-]
    Bytes bytes;
};

void init();
std::vector<Seed> seeds();

// Aborts with `what` on stderr: a fuzz target's own check (an oracle) failed. libFuzzer and the
// replay driver both report the abort as a crash with its input.
[[noreturn]] void broken(const char *what);

inline Bytes bytes_of(const std::string &s) { return Bytes(s.begin(), s.end()); }

} // namespace mt_fuzz2

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

#endif // MAPLETHIEF_TESTS_FUZZ_FUZZ2_H
