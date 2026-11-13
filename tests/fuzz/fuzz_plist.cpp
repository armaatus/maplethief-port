// Fuzz target: the binary plist reader (runtime/src/foundation/plist.cpp, #323, ADR 0154).
// Oracles besides the sanitizers: a refusal has a reason, and an accepted file's expansion stays
// within the reader's bounds (objects and copied bytes, linear in the file's size).
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "foundation/plist.h"
#include "fuzz2.h"

namespace {

using mt_fuzz2::Bytes;

// A binary plist writer for the seeds: objects in order, then the offset table and the trailer,
// with the offset and reference sizes given.
struct Writer {
    std::vector<Bytes> objects;

    std::uint64_t add(Bytes o) {
        objects.push_back(std::move(o));
        return objects.size() - 1;
    }
    static void be(Bytes &o, std::uint64_t v, unsigned size) {
        for (unsigned k = size; k-- > 0;) {
            o.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    }
    static Bytes marker(std::uint8_t type, std::size_t n) {
        Bytes o;
        if (n < 15) {
            o.push_back(static_cast<std::uint8_t>(type | n));
        } else {
            o.push_back(static_cast<std::uint8_t>(type | 0x0F));
            o.push_back(0x13);
            be(o, n, 8);
        }
        return o;
    }
    std::uint64_t ascii(const char *s) {
        Bytes o = marker(0x50, std::strlen(s));
        o.insert(o.end(), s, s + std::strlen(s));
        return add(o);
    }
    std::uint64_t utf16(const std::u16string &s) {
        Bytes o = marker(0x60, s.size());
        for (const char16_t c : s) {
            be(o, c, 2);
        }
        return add(o);
    }
    // 2^log2 bytes; a 16-byte integer (log2 4) holds `v` in its low half.
    std::uint64_t integer(std::uint64_t v, unsigned log2) {
        Bytes o{static_cast<std::uint8_t>(0x10 | log2)};
        if (log2 == 4) {
            be(o, 0, 8);
        }
        be(o, v, log2 >= 3 ? 8 : 1u << log2);
        return add(o);
    }
    std::uint64_t raw(Bytes o) { return add(std::move(o)); }
    std::uint64_t collection(std::uint8_t type, const std::vector<std::uint64_t> &refs,
                             unsigned ref_size) {
        const std::size_t n = type == 0xD0 ? refs.size() / 2 : refs.size();
        Bytes o = marker(type, n);
        for (const std::uint64_t r : refs) {
            be(o, r, ref_size);
        }
        return add(o);
    }
    Bytes file(std::uint64_t top, unsigned offset_size, unsigned ref_size) const {
        Bytes f{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        std::vector<std::uint64_t> offsets;
        for (const Bytes &o : objects) {
            offsets.push_back(f.size());
            f.insert(f.end(), o.begin(), o.end());
        }
        const std::uint64_t table = f.size();
        for (const std::uint64_t o : offsets) {
            be(f, o, offset_size);
        }
        f.insert(f.end(), 6, 0);
        f.push_back(static_cast<std::uint8_t>(offset_size));
        f.push_back(static_cast<std::uint8_t>(ref_size));
        be(f, objects.size(), 8);
        be(f, top, 8);
        be(f, table, 8);
        return f;
    }
};

// Every kind of object once, in a dictionary, with refs of `ref_size` and offsets of
// `offset_size` bytes. Object 0 is the dictionary, filled in last.
Bytes every_kind(unsigned offset_size, unsigned ref_size) {
    Writer w;
    w.add({});
    std::vector<std::uint64_t> keys, values;
    const auto entry = [&](const char *key, std::uint64_t value) {
        keys.push_back(w.ascii(key));
        values.push_back(value);
    };
    entry("null", w.raw({0x00}));
    entry("false", w.raw({0x08}));
    entry("true", w.raw({0x09}));
    entry("i8", w.integer(200, 0));
    entry("i16", w.integer(0xBEEF, 1));
    entry("i32", w.integer(0xDEADBEEF, 2));
    entry("i64", w.integer(0xFFFFFFFFFFFFFFFEull, 3));
    entry("i128", w.integer(7, 4));
    entry("f32", w.raw({0x22, 0x3F, 0x00, 0x00, 0x00}));
    entry("f64", w.raw({0x23, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0}));
    entry("date", w.raw({0x33, 0x41, 0xC2, 0x6E, 0x5D, 0x00, 0, 0, 0}));
    entry("data", w.raw({0x43, 1, 2, 3}));
    entry("uid", w.raw({0x81, 0x01, 0x02}));
    entry("ascii", w.ascii("a string longer than fifteen bytes"));
    entry("utf16", w.utf16(u"브루스"));
    const std::uint64_t one = w.integer(1, 0);
    entry("array", w.collection(0xA0, {one, one, w.ascii("two")}, ref_size));
    entry("set", w.collection(0xC0, {one}, ref_size));
    entry("empty", w.collection(0xD0, {}, ref_size));
    std::vector<std::uint64_t> refs = keys;
    refs.insert(refs.end(), values.begin(), values.end());
    Bytes top = Writer::marker(0xD0, keys.size());
    for (const std::uint64_t r : refs) {
        Writer::be(top, r, ref_size);
    }
    w.objects[0] = top;
    return w.file(0, offset_size, ref_size);
}

// A .strings table, as the bundle's are: a dictionary of string keys and values.
Bytes strings_table() {
    Writer w;
    w.add({});
    const std::uint64_t k1 = w.ascii("Bruce.dialog.1"), k2 = w.ascii("empty"),
                        v1 = w.utf16(u"I'm Bruce."), v2 = w.ascii("");
    Bytes top = Writer::marker(0xD0, 2);
    for (const std::uint64_t r : {k1, k2, v1, v2}) {
        Writer::be(top, r, 1);
    }
    w.objects[0] = top;
    return w.file(0, 1, 1);
}

// Nested arrays that share their children: [x, x] ten levels deep, then one data object.
Bytes shared_levels() {
    Writer w;
    for (int level = 0; level < 10; level++) {
        w.collection(0xA0,
                     {static_cast<std::uint64_t>(level + 1), static_cast<std::uint64_t>(level + 1)},
                     1);
    }
    w.raw({0x48, 1, 2, 3, 4, 5, 6, 7, 8});
    return w.file(0, 1, 1);
}

struct Tally {
    std::uint64_t objects = 0, bytes = 0;
};

void walk(const mt_ns::PlistValue &v, Tally *t) {
    t->objects++;
    t->bytes += v.bytes.size() + v.s.size() * 2;
    for (const auto &item : v.items) {
        walk(item, t);
    }
    for (const auto &e : v.entries) {
        t->objects++; // the key
        t->bytes += e.first.size() * 2;
        walk(e.second, t);
    }
    if (v.kind == mt_ns::PlistValue::dict && !v.entries.empty()) {
        (void)v.find(v.entries.back().first);
    }
}

} // namespace

void mt_fuzz2::init() {}

std::vector<mt_fuzz2::Seed> mt_fuzz2::seeds() {
    return {
        {"every-kind-1-1", every_kind(1, 1)}, {"every-kind-2-1", every_kind(2, 1)},
        {"every-kind-4-2", every_kind(4, 2)}, {"every-kind-8-8", every_kind(8, 8)},
        {"strings-table", strings_table()},   {"shared-levels", shared_levels()},
    };
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    mt_ns::PlistValue v;
    std::string error;
    if (!mt_ns::plist_parse(data, size, &v, &error)) {
        if (error.empty()) {
            mt_fuzz2::broken("a plist refused without a reason");
        }
        return 0;
    }
    Tally t;
    walk(v, &t);
    // The reader's bounds (plist.h): at most count x 16 + 4,096 objects, count <= the size, and
    // at most size x 16 + 4,096 bytes of data and strings copied.
    if (t.objects > static_cast<std::uint64_t>(size) * 16u + 4096u ||
        t.bytes > static_cast<std::uint64_t>(size) * 16u + 4096u) {
        mt_fuzz2::broken("a plist expanded past the reader's bounds");
    }
    return 0;
}
