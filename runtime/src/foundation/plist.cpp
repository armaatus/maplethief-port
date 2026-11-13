// The binary property list reader (plist.h). Layout (CFBinaryPList.c):
//
//   "bplist00" | objects ... | offset table | trailer (32 bytes)
//   trailer: 6 unused bytes, offset int size (1-8), object ref size (1-8), object count (u64 BE),
//            top object (u64 BE), offset table offset (u64 BE)
//   object:  a marker byte, high nibble the type, low nibble a count (0xF: an integer object
//            with the count follows): 0x00 null, 0x08/0x09 false/true, 0x1n int of 2^n bytes,
//            0x2n real of 2^n bytes, 0x33 date, 0x4n data, 0x5n ASCII, 0x6n UTF-16BE,
//            0x8n UID of n+1 bytes, 0xAn array, 0xCn set, 0xDn dict (n key refs, n value refs)
//
// Every multi-byte number is big-endian.
#include "foundation/plist.h"

#include <cstring>
#include <utility>

namespace mt_ns {

namespace {

constexpr std::size_t k_trailer = 32;
constexpr unsigned k_max_depth = 64;

struct Reader {
    const std::uint8_t *p;
    std::size_t n;
    unsigned offset_size, ref_size;
    std::uint64_t count, offsets;
    std::vector<bool> active; // objects on the current path (cycle check)
    std::uint64_t parsed;     // objects parsed so far, shared references counted each time
    std::uint64_t copied;     // bytes of data and strings copied so far, likewise
    std::string error;

    bool fail(const char *what) {
        if (error.empty()) {
            error = what;
        }
        return false;
    }

    bool uint_at(std::size_t at, unsigned size, std::uint64_t *v) {
        if (size == 0 || size > 8 || at > n || n - at < size) {
            return fail("number out of bounds");
        }
        std::uint64_t x = 0;
        for (unsigned k = 0; k < size; k++) {
            x = (x << 8) | p[at + k];
        }
        *v = x;
        return true;
    }

    bool offset_of(std::uint64_t ref, std::size_t *at) {
        std::uint64_t off;
        if (ref >= count) {
            return fail("object reference out of range");
        }
        if (!uint_at(static_cast<std::size_t>(offsets + ref * offset_size), offset_size, &off)) {
            return false;
        }
        if (off < 8 || off >= n - k_trailer) {
            return fail("object offset out of bounds");
        }
        *at = static_cast<std::size_t>(off);
        return true;
    }

    // The count of a marker's low nibble (or the integer object after it); *body: where the
    // object's contents start.
    bool length(std::size_t at, std::uint64_t *len, std::size_t *body) {
        const unsigned low = p[at] & 0x0Fu;
        if (low != 0x0F) {
            *len = low;
            *body = at + 1;
            return true;
        }
        if (at + 1 >= n || (p[at + 1] & 0xF0u) != 0x10u) {
            return fail("bad length object");
        }
        const unsigned size = 1u << (p[at + 1] & 0x0Fu);
        if (size > 8 || !uint_at(at + 2, size, len)) {
            return fail("bad length object");
        }
        *body = at + 2 + size;
        return true;
    }

    bool span(std::size_t body, std::uint64_t bytes) {
        return body <= n && bytes <= n - body ? true : fail("object runs past the end");
    }

    // Each expanded copy of a data or string object costs its bytes: bound them as the objects
    // are, so that many references to one large object cannot take memory without end (#323).
    bool copy(std::uint64_t bytes) {
        copied += bytes;
        return copied <= static_cast<std::uint64_t>(n) * 16u + 4096u
                   ? true
                   : fail("shared references copy too many bytes");
    }

    bool object(std::uint64_t ref, PlistValue *out, unsigned depth) {
        if (depth > k_max_depth) {
            return fail("nested too deeply");
        }
        if (ref >= count) { // before `active` is indexed with it
            return fail("object reference out of range");
        }
        if (active[static_cast<std::size_t>(ref)]) {
            return fail("reference cycle");
        }
        // Shared references are expanded: bound the work, so that a small file of nested
        // arrays that each reference the same array many times cannot take for ever.
        if (++parsed > count * 16u + 4096u) {
            return fail("too many object references");
        }
        std::size_t at;
        if (!offset_of(ref, &at)) {
            return false;
        }
        active[static_cast<std::size_t>(ref)] = true;
        const bool ok = parse_at(at, out, depth);
        active[static_cast<std::size_t>(ref)] = false;
        return ok;
    }

    bool parse_at(std::size_t at, PlistValue *out, unsigned depth) {
        const unsigned marker = p[at], type = marker >> 4, low = marker & 0x0Fu;
        std::uint64_t len, v;
        std::size_t body;
        switch (type) {
        case 0x0:
            if (marker == 0x00) {
                out->kind = PlistValue::null;
            } else if (marker == 0x08 || marker == 0x09) {
                out->kind = PlistValue::boolean;
                out->b = marker == 0x09;
            } else {
                return fail("unknown simple object");
            }
            return true;
        case 0x1: {
            const unsigned size = 1u << low;
            if (size > 16) {
                return fail("integer too wide");
            }
            // A 16-byte integer: keep the low 8 bytes.
            if (!uint_at(at + 1 + (size == 16 ? 8 : 0), size == 16 ? 8 : size, &v)) {
                return false;
            }
            out->kind = PlistValue::integer;
            // 1, 2 and 4 bytes are unsigned; 8 bytes are signed (CFBinaryPList.c).
            out->i = static_cast<std::int64_t>(v);
            return true;
        }
        case 0x2:
        case 0x3: {
            const unsigned size = type == 0x3 ? 8u : 1u << low;
            if ((type == 0x3 && low != 3) || (size != 4 && size != 8) ||
                !uint_at(at + 1, size, &v)) {
                return fail("bad real or date");
            }
            if (size == 4) {
                float f;
                const std::uint32_t w = static_cast<std::uint32_t>(v);
                std::memcpy(&f, &w, 4);
                out->f = f;
            } else {
                std::memcpy(&out->f, &v, 8);
            }
            out->kind = type == 0x3 ? PlistValue::date : PlistValue::real;
            return true;
        }
        case 0x4:
            if (!length(at, &len, &body) || !span(body, len) || !copy(len)) {
                return false;
            }
            out->kind = PlistValue::data;
            out->bytes.assign(p + body, p + body + len);
            return true;
        case 0x5:
            if (!length(at, &len, &body) || !span(body, len) || !copy(len * 2)) {
                return false;
            }
            out->kind = PlistValue::string;
            out->s.clear();
            for (std::uint64_t k = 0; k < len; k++) {
                if (p[body + k] > 0x7F) {
                    return fail("non-ASCII byte in an ASCII string");
                }
                out->s.push_back(static_cast<char16_t>(p[body + k]));
            }
            return true;
        case 0x6:
            if (!length(at, &len, &body) || len > (n / 2) || !span(body, len * 2)) {
                return fail("UTF-16 string out of bounds");
            }
            if (!copy(len * 2)) {
                return false;
            }
            out->kind = PlistValue::string;
            out->s.clear();
            for (std::uint64_t k = 0; k < len; k++) {
                out->s.push_back(
                    static_cast<char16_t>((p[body + 2 * k] << 8) | p[body + 2 * k + 1]));
            }
            return true;
        case 0x8:
            if (!uint_at(at + 1, low + 1, &v)) {
                return false;
            }
            out->kind = PlistValue::integer;
            out->i = static_cast<std::int64_t>(v);
            return true;
        case 0xA:
        case 0xC:
        case 0xD: {
            const std::uint64_t refs = type == 0xD ? 2u : 1u;
            if (!length(at, &len, &body) || len > n || !span(body, len * refs * ref_size)) {
                return fail("collection out of bounds");
            }
            out->kind = type == 0xD ? PlistValue::dict : PlistValue::array;
            for (std::uint64_t k = 0; k < len; k++) {
                std::uint64_t r;
                if (!uint_at(static_cast<std::size_t>(body + k * ref_size), ref_size, &r)) {
                    return false;
                }
                PlistValue item;
                if (!object(r, &item, depth + 1)) {
                    return false;
                }
                if (type != 0xD) {
                    out->items.push_back(std::move(item));
                    continue;
                }
                if (item.kind != PlistValue::string) {
                    return fail("dictionary key is not a string");
                }
                std::uint64_t vr;
                PlistValue value;
                if (!uint_at(static_cast<std::size_t>(body + (len + k) * ref_size), ref_size,
                             &vr) ||
                    !object(vr, &value, depth + 1)) {
                    return false;
                }
                out->entries.emplace_back(std::move(item.s), std::move(value));
            }
            return true;
        }
        default:
            return fail("unknown object type");
        }
    }
};

} // namespace

const PlistValue *PlistValue::find(const std::u16string &key) const {
    for (const auto &e : entries) {
        if (e.first == key) {
            return &e.second;
        }
    }
    return nullptr;
}

bool plist_parse(const std::uint8_t *bytes, std::size_t n, PlistValue *out, std::string *error) {
    Reader r{bytes, n, 0, 0, 0, 0, {}, 0, 0, {}};
    std::uint64_t top = 0;
    bool ok = n >= 8 + k_trailer && std::memcmp(bytes, "bplist00", 8) == 0;
    if (!ok) {
        r.fail("not a binary plist");
    } else {
        const std::size_t t = n - k_trailer;
        r.offset_size = bytes[t + 6];
        r.ref_size = bytes[t + 7];
        ok = r.uint_at(t + 8, 8, &r.count) && r.uint_at(t + 16, 8, &top) &&
             r.uint_at(t + 24, 8, &r.offsets);
        if (ok && (r.offset_size < 1 || r.offset_size > 8 || r.ref_size < 1 || r.ref_size > 8 ||
                   r.count == 0 || r.count > n || top >= r.count || r.offsets < 8 ||
                   r.offsets > t || (t - r.offsets) / r.offset_size < r.count)) {
            ok = r.fail("bad trailer");
        }
    }
    if (ok) {
        r.active.assign(static_cast<std::size_t>(r.count), false);
        *out = PlistValue{};
        ok = r.object(top, out, 0);
    }
    if (!ok && error != nullptr) {
        *error = r.error;
    }
    return ok;
}

} // namespace mt_ns
