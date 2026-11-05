// CFString (#48, ADR 0032): the storage of __NSCFString and the 367 constant strings of
// __cfstring, and the CFString imports of the game.
//
// A string's text is host state (UTF-16). A constant string is the compiler's literal
// {isa = ___CFConstantStringClassReference, flags, chars, length} in guest memory: flags
// 0x7C8 for 8-bit text (366 literals, all ASCII), 0x7D0 for UTF-16 (the Korean terms of
// service). Its text is decoded into the same state on first use; it is immortal.
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "snapshot/snapshot_internal.h"

namespace mt_cf {

namespace {

constexpr u32 k_flags_8bit = 0x7C8u;    // what clang emits for an 8-bit literal
constexpr u32 k_flag_unicode = 0x10u;   // __kCFIsUnicode: the chars are UTF-16
constexpr u32 k_max_literal = 1u << 24; // sanity limit for a literal's length

struct CString {
    u32 encoding;
    u32 guest;
    u32 version; // the text version it was made from
};

struct StringState {
    std::u16string *text; // nullptr until set (a constant: until decoded)
    std::vector<CString> *cstrings;
    u32 version; // bumped by every mutation
    bool is_mutable;
};

void destroy_string(mt_cpu *cpu, u32, void *p) {
    auto *s = static_cast<StringState *>(p);
    delete s->text;
    if (s->cstrings != nullptr) {
        for (const CString &c : *s->cstrings) {
            mt_heap_free(heap(cpu), c.guest);
        }
        delete s->cstrings;
    }
}

const mt_objc_state_type k_string_state = {"CFString", sizeof(StringState), destroy_string};

bool get32(const mt_cpu *cpu, u32 addr, u32 *v) {
    if (addr < MT_GUEST_PAGE_ZERO_END || static_cast<std::uint64_t>(addr) + 4 > cpu->mem_size) {
        return false;
    }
    std::memcpy(v, cpu->mem + addr, 4);
    return true;
}

// Decodes a constant string's literal into `out`; false if it is malformed.
bool decode_literal(mt_cpu *cpu, u32 obj, std::u16string &out) {
    u32 flags, chars, length;
    if (!get32(cpu, obj + 4, &flags) || !get32(cpu, obj + 8, &chars) ||
        !get32(cpu, obj + 12, &length) || length > k_max_literal) {
        return false;
    }
    const u32 bytes = (flags & k_flag_unicode) != 0 ? length * 2u : length;
    if (chars < MT_GUEST_PAGE_ZERO_END ||
        static_cast<std::uint64_t>(chars) + bytes > cpu->mem_size) {
        return false;
    }
    const std::uint8_t *p = cpu->mem + chars;
    if ((flags & k_flag_unicode) != 0) {
        return decode(p, bytes, k_enc_utf16le, out);
    }
    // 8-bit literals are ASCII in this game; anything else is read as UTF-8, else Latin-1.
    return decode(p, bytes, k_enc_utf8, out) ||
           (out.clear(), decode(p, bytes, k_enc_iso_latin1, out));
}

StringState *state_of(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0 || !is_string(cpu, obj)) {
        return nullptr;
    }
    auto *s = static_cast<StringState *>(mt_objc_state(rt, obj, &k_string_state));
    if (s == nullptr) {
        return nullptr;
    }
    if (s->text == nullptr && mt_objc_class_of(rt, obj) == classes(cpu).constant_string) {
        std::u16string text;
        if (!decode_literal(cpu, obj, text)) {
            log(cpu, "malformed constant string at 0x%08X", static_cast<unsigned>(obj));
            return nullptr;
        }
        s->text = new (std::nothrow) std::u16string(std::move(text));
    }
    return s->text != nullptr ? s : nullptr;
}

} // namespace

bool is_string(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    const u32 cls = classes(cpu).string;
    return rt != nullptr && obj != 0 && cls != 0 && mt_objc_is_kind_of(rt, obj, cls) != 0;
}

const std::u16string *string_chars(mt_cpu *cpu, u32 obj) {
    const StringState *s = state_of(cpu, obj);
    return s != nullptr ? s->text : nullptr;
}

std::u16string *string_mutable_chars(mt_cpu *cpu, u32 obj) {
    StringState *s = state_of(cpu, obj);
    if (s == nullptr || !s->is_mutable) {
        return nullptr;
    }
    s->version++;
    return s->text;
}

bool string_is_mutable(mt_cpu *cpu, u32 obj) {
    const StringState *s = state_of(cpu, obj);
    return s != nullptr && s->is_mutable;
}

u32 string_create(mt_cpu *cpu, std::u16string chars, bool is_mutable) {
    const u32 obj = create(cpu, classes(cpu).string);
    auto *s = obj != 0
                  ? static_cast<StringState *>(mt_objc_state(runtime(cpu), obj, &k_string_state))
                  : nullptr;
    if (s == nullptr) {
        return 0;
    }
    s->text = new (std::nothrow) std::u16string(std::move(chars));
    s->is_mutable = is_mutable;
    return obj;
}

bool string_assign(mt_cpu *cpu, u32 obj, std::u16string chars) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0 || mt_objc_class_of(rt, obj) == classes(cpu).constant_string) {
        return false;
    }
    StringState *s = state_of(cpu, obj);
    if (s == nullptr) {
        return false;
    }
    *s->text = std::move(chars);
    s->version++;
    return true;
}

u32 string_create_bytes(mt_cpu *cpu, const std::uint8_t *bytes, std::size_t n, u32 encoding,
                        bool is_mutable) {
    std::u16string text;
    if (!decode(bytes, n, encoding, text)) {
        return 0;
    }
    return string_create(cpu, std::move(text), is_mutable);
}

u32 string_guest_cstring(mt_cpu *cpu, u32 obj, u32 encoding) {
    StringState *s = state_of(cpu, obj);
    if (s == nullptr) {
        return 0;
    }
    if (s->cstrings != nullptr) {
        for (const CString &c : *s->cstrings) {
            if (c.encoding == encoding && c.version == s->version) {
                return c.guest;
            }
        }
    }
    std::string bytes;
    if (!encode(s->text->data(), s->text->size(), encoding, bytes)) {
        return 0;
    }
    // Two NULs: a terminator for 8-bit and UTF-16 text alike.
    const u32 size = static_cast<u32>(bytes.size()) + 2u;
    const u32 guest = mt_heap_calloc(heap(cpu), 1, size);
    if (guest == 0) {
        return 0;
    }
    std::memcpy(cpu->mem + guest, bytes.data(), bytes.size());
    if (s->cstrings == nullptr) {
        s->cstrings = new (std::nothrow) std::vector<CString>();
    }
    if (s->cstrings != nullptr) {
        s->cstrings->push_back(CString{encoding, guest, s->version});
    }
    return guest;
}

u32 string_create_constant(mt_cpu *cpu, const char *ascii) {
    const u32 cls = classes(cpu).constant_string;
    const u32 n = static_cast<u32>(std::strlen(ascii));
    const u32 obj = cls != 0 ? mt_heap_calloc(heap(cpu), 1, 16u + n + 1u) : 0;
    if (obj == 0) {
        return 0;
    }
    const u32 words[4] = {cls, k_flags_8bit, obj + 16u, n};
    std::memcpy(cpu->mem + obj, words, sizeof words);
    std::memcpy(cpu->mem + obj + 16u, ascii, n);
    return obj;
}

} // namespace mt_cf

using namespace mt_cf;

// CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *cStr,
//                                       CFStringEncoding encoding)
extern "C" void mt_hle_CFStringCreateWithCString(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a); // the allocator: every allocation is the guest heap's
    const u32 cstr = mt_arg_u32(&a);
    const u32 encoding = mt_arg_u32(&a);
    if (cstr == 0) {
        misuse(cpu, "CFStringCreateWithCString", "NULL C string", 0);
    }
    const char *s = mt_g2h_str(cpu, cstr);
    if (!encoding_supported(encoding)) {
        log(cpu, "CFStringCreateWithCString: encoding 0x%08X is not supported: NULL",
            static_cast<unsigned>(encoding));
        mt_ret_u32(cpu, 0);
        return;
    }
    const u32 obj = string_create_bytes(cpu, reinterpret_cast<const std::uint8_t *>(s),
                                        std::strlen(s), encoding, false);
    mt_ret_u32(cpu, obj);
}

// void CFStringAppendCharacters(CFMutableStringRef, const UniChar *chars, CFIndex numChars)
extern "C" void mt_hle_CFStringAppendCharacters(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 str = mt_arg_u32(&a);
    const u32 chars = mt_arg_u32(&a);
    const std::int32_t n = mt_arg_i32(&a);
    if (n < 0) {
        misuse(cpu, "CFStringAppendCharacters", "negative length", static_cast<u32>(n));
    }
    if (!string_is_mutable(cpu, str)) {
        misuse(cpu, "CFStringAppendCharacters", "not a mutable CFString", str);
    }
    const std::uint8_t *p = n > 0 ? mt_g2h(cpu, chars, static_cast<u32>(n) * 2u) : nullptr;
    std::u16string *text = string_mutable_chars(cpu, str);
    for (std::int32_t i = 0; i < n; i++) {
        text->push_back(static_cast<char16_t>(p[2 * i] | (p[2 * i + 1] << 8)));
    }
}

// NSStringEncoding CFStringConvertEncodingToNSStringEncoding(CFStringEncoding)
extern "C" void mt_hle_CFStringConvertEncodingToNSStringEncoding(mt_cpu *cpu,
                                                                 std::uint32_t) noexcept {
    mt_ret_u32(cpu, cf_to_ns_encoding(cpu->r[0]));
}

// --- save states (#202, ADR 0105) ---------------------------------------------------------------

namespace mt_cf {

namespace {

bool save_string(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const StringState *>(state);
    w.boolean(s->text != nullptr);
    if (s->text != nullptr) {
        w.u16str(*s->text);
    }
    w.boolean(s->cstrings != nullptr);
    if (s->cstrings != nullptr) {
        w.u32(static_cast<u32>(s->cstrings->size()));
        for (const CString &c : *s->cstrings) {
            w.u32(c.encoding);
            w.u32(c.guest);
            w.u32(c.version);
        }
    }
    w.u32(s->version);
    w.boolean(s->is_mutable);
    return true;
}

bool load_string(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<StringState *>(state);
    bool has_text = false, has_cstrings = false;
    std::u16string text;
    std::vector<CString> cstrings;
    if (!r.boolean(&has_text) || (has_text && !r.u16str(&text)) || !r.boolean(&has_cstrings)) {
        return false;
    }
    if (has_cstrings) {
        u32 n = 0;
        if (!r.u32(&n) || !r.count_fits(n, 12)) {
            return false;
        }
        cstrings.resize(n);
        for (CString &c : cstrings) {
            r.u32(&c.encoding);
            r.u32(&c.guest);
            r.u32(&c.version);
        }
    }
    if (!r.u32(&s->version) || !r.boolean(&s->is_mutable)) {
        return false;
    }
    s->text = has_text ? new std::u16string(std::move(text)) : nullptr;
    s->cstrings = has_cstrings ? new std::vector<CString>(std::move(cstrings)) : nullptr;
    return true;
}

void drop_string(void *state) {
    auto *s = static_cast<StringState *>(state);
    delete s->text;
    delete s->cstrings;
}

} // namespace

extern const mt_snap::StateOps k_snap_string;
const mt_snap::StateOps k_snap_string = {&k_string_state, "CFString", save_string, load_string,
                                         drop_string, nullptr};

} // namespace mt_cf
