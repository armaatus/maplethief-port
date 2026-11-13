// bindings.ini (platform/src/bindings.cpp, #70, ADR 0054): parse_bindings over the defaults,
// and the round trip its header promises: parse_bindings(serialise_bindings(s)) gives s's
// bindings back.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bindings.h"

namespace mf = maplethief::frontend;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char *>(data), size);
    mf::BindingSet set;
    (void)mf::parse_bindings(text, "bindings.ini", &set);
    mf::BindingSet back;
    (void)mf::parse_bindings(mf::serialise_bindings(set), "bindings.ini", &back);
    if (!mf::same_bindings(back.table, set.table) || back.face != set.face) {
        __builtin_trap();
    }
    return 0;
}
