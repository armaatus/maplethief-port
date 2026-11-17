// The runtime refers to mt_dispatch, which the generated code (or a test's double) defines.
// Nothing a fuzz target parses runs guest code, so reaching it is a bug in the target.
#include <cstdint>
#include <cstdlib>

#include "maplethief/recomp/calls.h"

extern "C" mt_guest_fn mt_dispatch(mt_cpu *, std::uint32_t, std::uint32_t) noexcept {
    std::abort();
}
