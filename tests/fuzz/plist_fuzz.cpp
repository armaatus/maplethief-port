// The binary plist reader (runtime/src/foundation/plist.cpp, #49, #323): plist_parse, as the
// bundle's Info.plist and .strings tables reach it. A refusal has a reason, and an accepted
// file's expansion stays inside the reader's bounds: at most the file's size x 16 + 4,096
// objects, and as many bytes of data and strings copied (plist.h).
#include <cstddef>
#include <cstdint>
#include <string>

#include "foundation/plist.h"

namespace {

struct Tally {
    std::uint64_t objects = 0, bytes = 0;
};

void walk(const mt_ns::PlistValue &v, Tally *t) {
    t->objects++;
    t->bytes += v.bytes.size() + v.s.size() * 2;
    for (const mt_ns::PlistValue &item : v.items) {
        walk(item, t);
    }
    for (const auto &[key, value] : v.entries) {
        t->objects++;
        t->bytes += key.size() * 2;
        walk(value, t);
    }
    if (v.kind == mt_ns::PlistValue::dict && !v.entries.empty() &&
        v.find(v.entries.back().first) == nullptr) {
        __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    mt_ns::PlistValue v;
    std::string why;
    if (!mt_ns::plist_parse(data, size, &v, &why)) {
        if (why.empty()) {
            __builtin_trap();
        }
        return 0;
    }
    Tally t;
    walk(v, &t);
    const std::uint64_t bound = static_cast<std::uint64_t>(size) * 16u + 4096u;
    if (t.objects > bound || t.bytes > bound) {
        __builtin_trap();
    }
    return 0;
}
