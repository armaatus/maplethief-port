// The NIBArchive reader (#54): see nib.h and docs/formats/nibarchive.md.
#include "uikit/nib.h"

#include <cstdio>
#include <cstring>

namespace mt_ui::nib {

namespace {

constexpr char k_magic[] = "NIBArchive";
constexpr std::size_t k_magic_size = sizeof k_magic - 1;
constexpr std::size_t k_header_size = k_magic_size + 10 * 4;

// A bounds-checked cursor over the file. Every read sets `failed` (and the first error) instead
// of reading past the end.
struct Reader {
    const u8 *p;
    std::size_t size, pos = 0;
    std::string *error;
    bool failed = false;

    bool fail(const char *what) {
        if (!failed) {
            char line[160];
            std::snprintf(line, sizeof line, "%s (at byte %zu of %zu)", what, pos, size);
            *error = line;
        }
        failed = true;
        return false;
    }
    bool need(std::size_t n, const char *what) {
        return !failed && (n <= size - pos || fail(what));
    }
    u8 byte(const char *what) {
        if (!need(1, what)) {
            return 0;
        }
        return p[pos++];
    }
    u32 u32le(const char *what) {
        if (!need(4, what)) {
            return 0;
        }
        const u32 v = static_cast<u32>(p[pos]) | static_cast<u32>(p[pos + 1]) << 8 |
                      static_cast<u32>(p[pos + 2]) << 16 | static_cast<u32>(p[pos + 3]) << 24;
        pos += 4;
        return v;
    }
    std::uint64_t u64le(const char *what) {
        const std::uint64_t lo = u32le(what);
        const std::uint64_t hi = u32le(what);
        return lo | hi << 32;
    }
    // 7 bits per byte, least significant group first; the byte with the high bit set is the
    // last (the opposite of LEB128). At most 5 bytes, and the value must fit 32 bits.
    u32 varint(const char *what) {
        std::uint64_t value = 0;
        for (int i = 0; i < 5; i++) {
            const u8 b = byte(what);
            if (failed) {
                return 0;
            }
            value |= static_cast<std::uint64_t>(b & 0x7Fu) << (7 * i);
            if ((b & 0x80u) != 0) {
                if (value > 0xFFFFFFFFull) {
                    fail("varint does not fit 32 bits");
                    return 0;
                }
                return static_cast<u32>(value);
            }
        }
        fail("varint longer than 5 bytes");
        return 0;
    }
    bool bytes(std::size_t n, std::string *out, const char *what) {
        if (!need(n, what)) {
            return false;
        }
        out->assign(reinterpret_cast<const char *>(p + pos), n);
        pos += n;
        return true;
    }
};

bool fail(std::string *error, const char *fmt, unsigned a, unsigned b) {
    char line[160];
    std::snprintf(line, sizeof line, fmt, a, b);
    *error = line;
    return false;
}

const std::string k_empty;

} // namespace

const std::string &Archive::class_name(u32 index) const {
    if (index >= objects.size() || objects[index].cls >= classes.size()) {
        return k_empty;
    }
    return classes[objects[index].cls];
}

const Value *Archive::find(u32 index, const char *key) const {
    if (index >= objects.size()) {
        return nullptr;
    }
    const Object &o = objects[index];
    for (u32 i = 0; i < o.count; i++) {
        const Value &v = values[o.first + i];
        if (keys[v.key] == key) {
            return &v;
        }
    }
    return nullptr;
}

std::vector<u32> Archive::elements(u32 index) const {
    std::vector<u32> out;
    if (index >= objects.size()) {
        return out;
    }
    const Object &o = objects[index];
    for (u32 i = 0; i < o.count; i++) {
        const Value &v = values[o.first + i];
        if (v.type == k_object && keys[v.key] == "UINibEncoderEmptyKey") {
            out.push_back(v.object);
        }
    }
    return out;
}

const u8 *Archive::data(const Value &v) const {
    return v.type == k_data ? bytes.data() + v.data_offset : nullptr;
}

bool Archive::floats(const Value &v, float *out, u32 count) const {
    if (v.type != k_data || v.data_length != 1 + 4 * count || bytes[v.data_offset] != 0x06) {
        return false;
    }
    const u8 *p = bytes.data() + v.data_offset + 1;
    for (u32 i = 0; i < count; i++) {
        const u32 bits = static_cast<u32>(p[4 * i]) | static_cast<u32>(p[4 * i + 1]) << 8 |
                         static_cast<u32>(p[4 * i + 2]) << 16 |
                         static_cast<u32>(p[4 * i + 3]) << 24;
        std::memcpy(&out[i], &bits, 4);
    }
    return true;
}

bool Archive::number(const Value &v, double *out) const {
    switch (v.type) {
    case k_i8:
    case k_i16:
    case k_i32:
    case k_i64:
    case k_true:
    case k_false:
        *out = static_cast<double>(v.integer);
        return true;
    case k_f32:
    case k_f64:
        *out = v.real;
        return true;
    default:
        return false;
    }
}

bool parse(const u8 *bytes, std::size_t size, Archive *out, std::string *error) {
    *out = Archive();
    Reader r{bytes, size, 0, error};
    std::string magic;
    if (!r.bytes(k_magic_size, &magic, "truncated header") || magic != k_magic) {
        if (!r.failed) {
            *error = "not a NIBArchive (bad magic)";
        }
        return false;
    }
    out->format_version = r.u32le("truncated header");
    out->coder_version = r.u32le("truncated header");
    u32 counts[4], offsets[4];
    for (int i = 0; i < 4; i++) {
        counts[i] = r.u32le("truncated header");
        offsets[i] = r.u32le("truncated header");
    }
    if (r.failed) {
        return false;
    }
    // Each record takes at least one byte (an object three, a value two): a count larger than
    // the rest of the file is refused before anything is allocated for it.
    static const char *const k_tables[4] = {"objects", "keys", "values", "classes"};
    static const u32 k_min_record[4] = {3, 1, 2, 2};
    std::size_t expected = k_header_size;
    auto seek = [&](int table) {
        char line[160];
        if (offsets[table] != expected) {
            std::snprintf(line, sizeof line,
                          "%s table at %u, expected %zu (the tables must be contiguous)",
                          k_tables[table], static_cast<unsigned>(offsets[table]), expected);
            *error = line;
            return false;
        }
        if (counts[table] > (size - expected) / k_min_record[table]) {
            std::snprintf(line, sizeof line, "%s: %u records do not fit the file", k_tables[table],
                          static_cast<unsigned>(counts[table]));
            *error = line;
            return false;
        }
        r.pos = expected;
        return true;
    };
    if (!seek(0)) {
        return false;
    }
    out->objects.resize(counts[0]);
    for (Object &o : out->objects) {
        o.cls = r.varint("truncated objects table");
        o.first = r.varint("truncated objects table");
        o.count = r.varint("truncated objects table");
    }
    if (r.failed) {
        return false;
    }
    expected = r.pos;
    if (!seek(1)) {
        return false;
    }
    out->keys.resize(counts[1]);
    for (std::string &k : out->keys) {
        r.bytes(r.varint("truncated keys table"), &k, "truncated key");
    }
    if (r.failed) {
        return false;
    }
    expected = r.pos;
    if (!seek(2)) {
        return false;
    }
    out->values.resize(counts[2]);
    for (u32 i = 0; i < counts[2] && !r.failed; i++) {
        Value &v = out->values[i];
        v.key = r.varint("truncated values table");
        if (!r.failed && v.key >= counts[1]) {
            return fail(error, "value %u: key index %u out of range", i, v.key);
        }
        v.type = r.byte("truncated values table");
        switch (v.type) {
        case k_i8:
            v.integer = static_cast<std::int8_t>(r.byte("truncated value"));
            break;
        case k_i16: {
            const u32 lo = r.byte("truncated value"), hi = r.byte("truncated value");
            v.integer = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | hi << 8));
            break;
        }
        case k_i32:
            v.integer = static_cast<std::int32_t>(r.u32le("truncated value"));
            break;
        case k_i64:
            v.integer = static_cast<std::int64_t>(r.u64le("truncated value"));
            break;
        case k_true:
        case k_false:
            v.integer = v.type == k_true ? 1 : 0;
            break;
        case k_f32: {
            const u32 bits = r.u32le("truncated value");
            float f;
            std::memcpy(&f, &bits, 4);
            v.real = f;
            break;
        }
        case k_f64: {
            const std::uint64_t bits = r.u64le("truncated value");
            std::memcpy(&v.real, &bits, 8);
            break;
        }
        case k_data:
            v.data_length = r.varint("truncated value");
            if (r.need(v.data_length, "truncated data value")) {
                v.data_offset = static_cast<u32>(r.pos);
                r.pos += v.data_length;
            }
            break;
        case k_nil:
            break;
        case k_object:
            v.object = r.u32le("truncated value");
            if (!r.failed && v.object >= counts[0]) {
                return fail(error, "value %u: object index %u out of range", i, v.object);
            }
            break;
        default:
            if (!r.failed) {
                return fail(error, "value %u: unknown type %u", i, v.type);
            }
        }
    }
    if (r.failed) {
        return false;
    }
    expected = r.pos;
    if (!seek(3)) {
        return false;
    }
    out->classes.resize(counts[3]);
    for (u32 i = 0; i < counts[3] && !r.failed; i++) {
        const u32 length = r.varint("truncated classes table");
        const u32 fallbacks = r.varint("truncated classes table");
        if (!r.failed && fallbacks > (size - r.pos) / 4) {
            return fail(error, "class %u: %u fallbacks past the end of the file", i, fallbacks);
        }
        for (u32 f = 0; f < fallbacks && !r.failed; f++) {
            const u32 fb = r.u32le("truncated class fallback");
            if (!r.failed && fb >= counts[3]) {
                return fail(error, "class %u: fallback %u out of range", i, fb);
            }
        }
        std::string &name = out->classes[i];
        if (!r.bytes(length, &name, "truncated class name")) {
            break;
        }
        if (name.empty() || name.back() != '\0' || name.find('\0') != name.size() - 1) {
            return fail(error, "class %u: the name is not one NUL-terminated string (%u bytes)", i,
                        length);
        }
        name.pop_back();
    }
    if (r.failed) {
        return false;
    }
    if (r.pos != size) {
        return fail(error, "%u trailing bytes after the classes table (%u)",
                    static_cast<unsigned>(size - r.pos), static_cast<unsigned>(r.pos));
    }
    // The objects' value runs must cover the values table in order.
    u32 next = 0;
    for (u32 i = 0; i < counts[0]; i++) {
        const Object &o = out->objects[i];
        if (o.cls >= counts[3]) {
            return fail(error, "object %u: class index %u out of range", i, o.cls);
        }
        if (o.first != next || o.count > counts[2] - o.first) {
            return fail(error, "object %u: values %u are not the next run", i, o.first);
        }
        next = o.first + o.count;
    }
    if (next != counts[2]) {
        return fail(error, "%u values belong to no object (of %u)", counts[2] - next, counts[2]);
    }
    out->bytes.assign(bytes, bytes + size);
    return true;
}

} // namespace mt_ui::nib
