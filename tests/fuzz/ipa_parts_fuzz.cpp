// The IPA check's readers of what is inside the zip (platform/src/ipa_import.cpp, #182): the
// Info.plist's strings (binary or XML), the executable's cryptid, and an entry name's parts.
// The first byte picks one; the rest is the input. A zip's CRC-32 keeps the fuzzer from
// changing these inside the ipa target, so they get their own.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipa_import.h"

namespace mf = maplethief::frontend;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::uint8_t> rest(data + 1, size - 1);
    switch (data[0] % 3) {
    case 0: {
        // The keys the check reads.
        static constexpr std::string_view kKeys[] = {"CFBundleVersion",
                                                     "CFBundleShortVersionString",
                                                     "CFBundleIdentifier", "CFBundleExecutable"};
        std::string value;
        (void)mf::plist_string(rest, kKeys[(data[0] / 3) % 4], &value);
        break;
    }
    case 1: {
        std::uint32_t cryptid = 0;
        (void)mf::macho_cryptid(rest, &cryptid);
        break;
    }
    default: {
        std::vector<std::string> parts;
        std::string why;
        const std::string_view name(reinterpret_cast<const char *>(rest.data()), rest.size());
        if (mf::ipa_entry_parts(name, &parts, &why)) {
            for (const std::string &p : parts) {
                if (p.empty() || p == "." || p == ".." || p.find('/') != std::string::npos ||
                    p.find('\\') != std::string::npos) {
                    __builtin_trap();
                }
            }
        }
        break;
    }
    }
    return 0;
}
