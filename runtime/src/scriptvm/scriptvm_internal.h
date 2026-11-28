// Shared by the host loop (scriptvm.cpp) and the shadow runs (shadow.cpp), runtime-internal.
#ifndef MAPLETHIEF_RUNTIME_SCRIPTVM_SCRIPTVM_INTERNAL_H
#define MAPLETHIEF_RUNTIME_SCRIPTVM_SCRIPTVM_INTERNAL_H

#include <string>
#include <vector>

namespace mt::scriptvm {

// While set, the host loop appends each line it would trace (every opcode and native call) to
// `lines`, the trace channel on or off: the shadow runs' report of the call. nullptr: none.
// Guest thread only.
void set_recorder(std::vector<std::string> *lines) noexcept;

} // namespace mt::scriptvm

#endif // MAPLETHIEF_RUNTIME_SCRIPTVM_SCRIPTVM_INTERNAL_H
