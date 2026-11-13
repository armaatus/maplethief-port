// settings.ini (platform/src/settings.cpp, #66, ADR 0092): parse_settings over the defaults,
// and the round trip its header promises: parse_settings(serialise_settings(s, t)) gives s,
// with the input as the old text t and without one.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "settings.h"

namespace mf = maplethief::frontend;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char *>(data), size);
    mf::Settings s;
    (void)mf::parse_settings(text, &s);
    for (const std::string &out : {mf::serialise_settings(s, text), mf::serialise_settings(s)}) {
        mf::Settings back;
        (void)mf::parse_settings(out, &back);
        if (!(back == s)) {
            __builtin_trap();
        }
    }
    return 0;
}
