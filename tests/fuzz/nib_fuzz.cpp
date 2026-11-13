// The NIBArchive reader (runtime/src/uikit/nib.cpp, docs/formats/nibarchive.md, #54, #323):
// parse, then the accessors uikit/app.cpp reads an archive with (class names, values by key, an
// array's elements, geometry floats, numbers, data). A refusal has a reason, and every index an
// accepted archive holds is in range.
#include <cstddef>
#include <cstdint>
#include <string>

#include "uikit/nib.h"

namespace nib = mt_ui::nib;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    nib::Archive a;
    std::string why;
    if (!nib::parse(data, size, &a, &why)) {
        if (why.empty()) {
            __builtin_trap();
        }
        return 0;
    }
    for (const nib::Value &v : a.values) {
        if (v.key >= a.keys.size() || (v.type == nib::k_object && v.object >= a.objects.size()) ||
            (v.type == nib::k_data && v.data_offset + std::uint64_t{v.data_length} > size)) {
            __builtin_trap();
        }
        double d;
        float f[4];
        (void)a.number(v, &d);
        (void)a.floats(v, f, 2);
        (void)a.floats(v, f, 4);
        if (const std::uint8_t *p = a.data(v); p != nullptr && v.data_length > 0) {
            volatile std::uint8_t last = p[v.data_length - 1];
            (void)last;
        }
    }
    const auto n = static_cast<std::uint32_t>(a.objects.size());
    for (std::uint32_t i = 0; i <= n; i++) { // one past the end, too
        (void)a.class_name(i);
        (void)a.find(i, "UINibTopLevelObjectsKey");
        (void)a.find(i, "UIBounds");
        for (const std::uint32_t e : a.elements(i)) {
            if (e >= n) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
