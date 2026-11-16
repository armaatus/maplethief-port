// The save files' checks and sanitiser (runtime/src/foundation/saves.cpp, docs/formats/save.md,
// #148, #323) and the cash shop's counters file (mt_cash_counters_parse). The first byte picks
// the file name (its low 5 bits: k_names, which seeds/saves.py mirrors) and the size of the
// reason's buffer (its top 3 bits); the rest is the file. The reason is NUL-terminated inside
// its buffer and a refusal has one; the verdict has a name and does not depend on that buffer;
// the sanitiser changes only config, at most two bytes, never one the checks pass, and a second
// pass changes nothing.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/cashshop.h"
#include "maplethief/saves.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

// Every name the checks know, two near misses and none (the first byte's low 5 bits).
const char *const k_names[] = {
    "theif0",
    "theif1",
    "theif2",
    "quest0",
    "quest1",
    "quest2",
    "config",
    "cupon",
    "CTerms",
    "crestore",
    "riprestore1",
    "riprestore2",
    "totalrestore",
    "maplethief-cashshop0",
    "maplethief-cashshop1",
    "maplethief-cashshop2",
    "thief0",
    "theif3",
    "",
};
constexpr unsigned k_name_count = sizeof k_names / sizeof k_names[0];
const std::size_t k_why_sizes[8] = {0, 1, 2, 8, 24, 64, 160, 512};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const char *name = k_names[(data[0] & 0x1Fu) % k_name_count];
    const std::size_t why_size = k_why_sizes[data[0] >> 5];
    const Bytes file(data + 1, data + size);
    std::vector<char> why(why_size + 16, '\x7F');
    const mt_saves_verdict v = mt_saves_check(name, file.data(), file.size(),
                                              why_size > 0 ? why.data() : nullptr, why_size);
    if (std::strcmp(mt_saves_verdict_name(v), "?") == 0) {
        __builtin_trap();
    }
    if (why_size > 0) {
        const void *nul = std::memchr(why.data(), '\0', why_size);
        if (nul == nullptr) {
            __builtin_trap();
        }
        if (why[why_size] != '\x7F') {
            __builtin_trap();
        }
        if (v != MT_SAVES_OK && why_size > 1 && why[0] == '\0') {
            __builtin_trap();
        }
    }
    if ((v == MT_SAVES_OK) !=
        (mt_saves_check(name, file.data(), file.size(), nullptr, 0) == MT_SAVES_OK)) {
        __builtin_trap();
    }

    Bytes copy = file;
    const std::uint32_t changed = mt_saves_sanitise(name, copy.data(), copy.size());
    if (changed > 2 || (changed > 0 && std::strcmp(name, "config") != 0)) {
        __builtin_trap();
    }
    if (mt_saves_sanitise(name, copy.data(), copy.size()) != 0) {
        __builtin_trap();
    }
    if (std::strcmp(name, "config") == 0 && v == MT_SAVES_OK && changed != 0) {
        __builtin_trap();
    }

    if (std::strncmp(name, MT_SAVES_COUNTERS_PREFIX, std::strlen(MT_SAVES_COUNTERS_PREFIX)) == 0) {
        const std::string text(file.begin(), file.end());
        mt_cash_counters c{};
        const int rc = mt_cash_counters_parse(text.data(), text.size(), &c);
        (void)rc;
    }
    return 0;
}
