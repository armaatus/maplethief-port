// The census of the port's ways into the guest (#405, ADR 0156's addendum): every place in the
// frontend that calls a guest function, writes guest memory, hooks a guest function, changes how
// a touch reaches a view or gives the guest input, and what the run recorder does about it.
//
//   recorded  it reaches the guest through the run tape's boundary (maplethief/runtape.h): the
//             tape holds it, and while a tape plays only the tape's reach the guest
//   pure      it depends only on guest state and the settings (which the run file holds, with
//             every change at its turn), so a replay with the same settings does the same; or
//             it only reads (an observer, a pass-through hook)
//   refused   neither: the recording ends before it touches the guest (run_refuse with `refuse`)
//
// tests/test_run_census.py scans platform/src for those calls and fails, by file and call, on
// one this table does not list (and on a listed one that is gone), and checks that each refused
// site's file calls run_refuse with its name. A new way into the guest must be added here.
#ifndef MAPLETHIEF_PLATFORM_RUN_CENSUS_H
#define MAPLETHIEF_PLATFORM_RUN_CENSUS_H

#include <span>

namespace maplethief::frontend {

enum class CensusKind { kRecorded, kPure, kRefused };

struct CensusSite {
    const char *file; // under platform/src
    const char *call; // the runtime call, or "guest_memory" for a write through cpu->mem
    CensusKind kind;
    const char *refuse; // kRefused: the name run_refuse gets (the run file's end detail)
    const char *why;
};

std::span<const CensusSite> run_census();
const char *census_kind_name(CensusKind kind);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_RUN_CENSUS_H
