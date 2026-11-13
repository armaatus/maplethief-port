// Fuzz target: the NIBArchive reader (runtime/src/uikit/nib.cpp, docs/formats/nibarchive.md,
// #323, ADR 0154). An accepted archive is then read the way uikit/app.cpp reads one: every
// object's class name, the values by key, an array's elements, geometry floats and numbers.
// Oracles besides the sanitizers: a refusal has a reason, and every index an accepted archive
// holds is in range.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "fuzz2.h"
#include "uikit/nib.h"

namespace {

using mt_fuzz2::Bytes;
namespace nib = mt_ui::nib;

// A NIBArchive writer for the seeds: the header, then the four tables back to back.
struct Writer {
    struct Val {
        std::uint32_t key;
        std::uint8_t type;
        Bytes payload; // after the type byte
    };
    struct Obj {
        std::uint32_t cls;
        std::vector<Val> values;
    };
    std::vector<std::string> keys, classes;
    std::vector<Obj> objects;

    static void varint(Bytes &b, std::uint32_t v) {
        while (v >= 0x80) {
            b.push_back(static_cast<std::uint8_t>(v & 0x7F));
            v >>= 7;
        }
        b.push_back(static_cast<std::uint8_t>(v | 0x80));
    }
    static void le32(Bytes &b, std::uint32_t v) {
        for (int k = 0; k < 4; k++) {
            b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    }
    std::uint32_t key(const char *k) {
        for (std::uint32_t i = 0; i < keys.size(); i++) {
            if (keys[i] == k) {
                return i;
            }
        }
        keys.emplace_back(k);
        return static_cast<std::uint32_t>(keys.size() - 1);
    }
    std::uint32_t cls(const char *c) {
        for (std::uint32_t i = 0; i < classes.size(); i++) {
            if (classes[i] == c) {
                return i;
            }
        }
        classes.emplace_back(c);
        return static_cast<std::uint32_t>(classes.size() - 1);
    }
    std::uint32_t object(const char *c) {
        objects.push_back(Obj{cls(c), {}});
        return static_cast<std::uint32_t>(objects.size() - 1);
    }
    void value(std::uint32_t obj, const char *k, std::uint8_t type, Bytes payload = {}) {
        objects[obj].values.push_back(Val{key(k), type, std::move(payload)});
    }
    void ref(std::uint32_t obj, const char *k, std::uint32_t target) {
        Bytes p;
        le32(p, target);
        value(obj, k, nib::k_object, p);
    }
    void floats(std::uint32_t obj, const char *k, std::initializer_list<float> fs) {
        Bytes p;
        varint(p, static_cast<std::uint32_t>(1 + 4 * fs.size()));
        p.push_back(0x06);
        for (const float f : fs) {
            std::uint32_t bits;
            std::memcpy(&bits, &f, 4);
            le32(p, bits);
        }
        value(obj, k, nib::k_data, p);
    }

    Bytes file() const {
        Bytes tables[4];
        std::uint32_t first = 0, nvalues = 0;
        for (const Obj &o : objects) {
            varint(tables[0], o.cls);
            varint(tables[0], first);
            varint(tables[0], static_cast<std::uint32_t>(o.values.size()));
            first += static_cast<std::uint32_t>(o.values.size());
        }
        for (const std::string &k : keys) {
            varint(tables[1], static_cast<std::uint32_t>(k.size()));
            tables[1].insert(tables[1].end(), k.begin(), k.end());
        }
        for (const Obj &o : objects) {
            for (const Val &v : o.values) {
                varint(tables[2], v.key);
                tables[2].push_back(v.type);
                tables[2].insert(tables[2].end(), v.payload.begin(), v.payload.end());
                nvalues++;
            }
        }
        for (std::uint32_t i = 0; i < classes.size(); i++) {
            varint(tables[3], static_cast<std::uint32_t>(classes[i].size() + 1));
            // UIProxyObject-like classes carry a fallback (to the first class).
            const bool fallback = i > 0 && classes[i] == "UIProxyObject";
            varint(tables[3], fallback ? 1u : 0u);
            if (fallback) {
                le32(tables[3], 0);
            }
            tables[3].insert(tables[3].end(), classes[i].begin(), classes[i].end());
            tables[3].push_back(0);
        }
        const std::string magic = "NIBArchive";
        Bytes f(magic.begin(), magic.end());
        le32(f, 1);  // format version
        le32(f, 10); // coder version
        const std::uint32_t counts[4] = {static_cast<std::uint32_t>(objects.size()),
                                         static_cast<std::uint32_t>(keys.size()), nvalues,
                                         static_cast<std::uint32_t>(classes.size())};
        std::uint32_t at = static_cast<std::uint32_t>(f.size() + 8 * 4);
        for (int t = 0; t < 4; t++) {
            le32(f, counts[t]);
            le32(f, at);
            at += static_cast<std::uint32_t>(tables[t].size());
        }
        for (const Bytes &t : tables) {
            f.insert(f.end(), t.begin(), t.end());
        }
        return f;
    }
};

// A MainWindow-like archive: the top-level objects array, a proxy, a window with its frame, a
// controller, and one value of every type.
Bytes main_window() {
    Writer w;
    const std::uint32_t root = w.object("NSObject");
    const std::uint32_t top = w.object("NSArray");
    const std::uint32_t owner = w.object("UIProxyObject");
    const std::uint32_t window = w.object("UIWindow");
    const std::uint32_t controller = w.object("UIViewController");
    w.ref(root, "UINibTopLevelObjectsKey", top);
    w.ref(root, "UINibObjectsKey", top);
    w.value(top, "NSInlinedValue", nib::k_true);
    for (const std::uint32_t o : {owner, window, controller}) {
        w.ref(top, "UINibEncoderEmptyKey", o);
    }
    Bytes id{'I', 'B', 'F', 'i', 'l', 'e', 's', 'O', 'w', 'n', 'e', 'r'};
    Bytes data;
    Writer::varint(data, static_cast<std::uint32_t>(id.size()));
    data.insert(data.end(), id.begin(), id.end());
    w.value(owner, "UIProxiedObjectIdentifier", nib::k_data, data);
    w.floats(window, "UIBounds", {0, 0, 320, 480});
    w.floats(window, "UICenter", {160, 240});
    w.value(window, "UIOpaque", nib::k_true);
    w.value(window, "UIHidden", nib::k_false);
    w.value(window, "UIAutoresizingMask", nib::k_i8, {0x12});
    w.value(window, "UITag", nib::k_i16, {0x34, 0x12});
    w.value(window, "UIContentMode", nib::k_i32, {1, 0, 0, 0});
    w.value(window, "UIBig", nib::k_i64, {1, 2, 3, 4, 5, 6, 7, 8});
    w.value(window, "UIAlpha", nib::k_f32, {0, 0, 0x80, 0x3F});
    w.value(window, "UIScale", nib::k_f64, {0, 0, 0, 0, 0, 0, 0xF0, 0x3F});
    w.value(window, "UIBackgroundColor", nib::k_nil);
    w.ref(controller, "UIParentViewController", window);
    w.ref(window, "UIRootViewController", controller);
    return w.file();
}

Bytes empty_archive() {
    Writer w;
    w.object("NSObject");
    return w.file();
}

} // namespace

void mt_fuzz2::init() {}

std::vector<mt_fuzz2::Seed> mt_fuzz2::seeds() {
    return {{"main-window", main_window()}, {"one-object", empty_archive()}};
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    nib::Archive a;
    std::string error;
    if (!nib::parse(data, size, &a, &error)) {
        if (error.empty()) {
            mt_fuzz2::broken("a NIBArchive refused without a reason");
        }
        return 0;
    }
    for (const nib::Value &v : a.values) {
        if (v.key >= a.keys.size() || (v.type == nib::k_object && v.object >= a.objects.size()) ||
            (v.type == nib::k_data && v.data_offset + std::uint64_t{v.data_length} > size)) {
            mt_fuzz2::broken("an accepted NIBArchive holds an index out of range");
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
                mt_fuzz2::broken("an array element out of range");
            }
        }
    }
    return 0;
}
