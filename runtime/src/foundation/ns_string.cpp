// NSString and NSMutableString (#49, ADR 0035): Foundation's selectors on CoreFoundation's
// string storage (#48, ADR 0032). Every NSString is an __NSCFString or a constant string, so a
// CFStringRef and an NSString * are the same object; +alloc of NSString or NSMutableString
// returns an empty __NSCFString that the -init... methods fill. The selectors are host method
// sets on NSString and NSMutableString (registered in runtime/src/objc/classes.cpp), so UIKit's
// drawing additions (#51) can add theirs to the same class from their own file.
//
// Also here: -[NSObject description], the %@ conversion of +stringWithFormat: /
// -initWithFormat: / -appendFormat:, and NSClassFromString.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "hle/format.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace mt_ns {

namespace {

using mt_cf::u32;

// NSStringCompareOptions (NSString.h).
constexpr u32 k_case_insensitive = 1u;
constexpr u32 k_numeric = 64u;

std::u16string from_utf8(const std::string &s) {
    std::u16string out;
    if (!mt_cf::decode(reinterpret_cast<const std::uint8_t *>(s.data()), s.size(),
                       mt_cf::k_enc_utf8, out)) {
        out.clear();
        mt_cf::decode(reinterpret_cast<const std::uint8_t *>(s.data()), s.size(),
                      mt_cf::k_enc_iso_latin1, out);
    }
    return out;
}

// The text of a string argument; a nil or non-string one raises, as Foundation does.
const std::u16string &text_arg(mt_cpu *cpu, u32 obj, const char *method) {
    const std::u16string *s = obj != 0 ? mt_cf::string_chars(cpu, obj) : nullptr;
    if (s == nullptr) {
        raise(cpu, method, obj == 0 ? "nil argument" : "not an NSString", obj);
    }
    return *s;
}

// Whether a string made by a class method of `cls` is mutable.
bool wants_mutable(mt_cpu *cpu, u32 cls) {
    return inherits(cpu, cls, class_named(cpu, "NSMutableString"));
}

void return_autoreleased(mt_cpu *cpu, u32 obj) {
    cpu->r[0] = obj != 0 ? mt_objc_autorelease(cpu, obj) : 0u;
    cpu->r[1] = 0;
}

// -init... of a string from +alloc: fills it, or releases it and returns nil.
void finish_init(mt_cpu *cpu, u32 self, bool ok, std::u16string text) {
    if (ok && mt_cf::string_assign(cpu, self, std::move(text))) {
        cpu->r[0] = self;
        return;
    }
    mt_objc_release(cpu, self);
    cpu->r[0] = 0;
}

// --- formats -------------------------------------------------------------------------------

void object_hook(void *, mt_cpu *cpu, const mt_format_spec &spec, mt_args *args, std::string &out) {
    const u32 obj = mt_arg_u32(args);
    const std::string d = describe(cpu, obj);
    mt_format_pad_string(out, spec, d.data(), d.size());
}

// Formats with the format string `fmt` (an NSString) and the arguments after it. The cursor
// reads a copy of the registers: %@ sends -description, which clobbers r0-r3.
bool format(mt_cpu *cpu, u32 fmt, mt_args args, const char *method, std::u16string *out) {
    const std::string f = mt_cf::to_utf8(text_arg(cpu, fmt, method));
    const mt_format_hooks hooks = {"@", object_hook, nullptr};
    std::string text;
    if (mt_format(cpu, f.c_str(), &args, &hooks, text) < 0) {
        return false;
    }
    *out = from_utf8(text);
    return true;
}

// The arguments after the format of a method sent with objc_msgSend, read from a copy of the
// registers that outlives the messages %@ sends.
struct Varargs {
    mt_cpu regs;
    mt_args args;
    Varargs(mt_cpu *cpu, u32 fixed_before_format) : regs(*cpu) {
        args = mt_objc_args(&regs);
        for (u32 i = 0; i < fixed_before_format; i++) {
            (void)mt_arg_u32(&args);
        }
    }
};

// --- comparison ----------------------------------------------------------------------------

char16_t fold(char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c; }
bool is_digit(char16_t c) { return c >= u'0' && c <= u'9'; }

int compare(const std::u16string &a, const std::u16string &b, u32 options) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if ((options & k_numeric) != 0 && is_digit(a[i]) && is_digit(b[j])) {
            // Digit runs compare as numbers: skip leading zeros, then the longer run is larger,
            // then the first different digit decides.
            std::size_t ie = i, je = j;
            while (ie < a.size() && is_digit(a[ie])) {
                ie++;
            }
            while (je < b.size() && is_digit(b[je])) {
                je++;
            }
            std::size_t is = i, js = j;
            while (is + 1 < ie && a[is] == u'0') {
                is++;
            }
            while (js + 1 < je && b[js] == u'0') {
                js++;
            }
            if (ie - is != je - js) {
                return ie - is < je - js ? -1 : 1;
            }
            for (; is < ie; is++, js++) {
                if (a[is] != b[js]) {
                    return a[is] < b[js] ? -1 : 1;
                }
            }
            i = ie;
            j = je;
            continue;
        }
        char16_t x = a[i], y = b[j];
        if ((options & k_case_insensitive) != 0) {
            x = fold(x);
            y = fold(y);
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
        i++;
        j++;
    }
    if (i == a.size() && j == b.size()) {
        return 0;
    }
    return i == a.size() ? -1 : 1;
}

// --- NSString class methods ------------------------------------------------------------------

// +allocWithZone: (and so +alloc): an empty __NSCFString for the -init... methods.
void string_alloc(mt_cpu *cpu) {
    cpu->r[0] = mt_cf::string_create(cpu, std::u16string(), wants_mutable(cpu, cpu->r[0]));
}

// +string
void string_string(mt_cpu *cpu) {
    return_autoreleased(cpu,
                        mt_cf::string_create(cpu, std::u16string(), wants_mutable(cpu, cpu->r[0])));
}

// +stringWithString:
void string_with_string(mt_cpu *cpu) {
    const std::u16string &t = text_arg(cpu, cpu->r[2], "+[NSString stringWithString:]");
    return_autoreleased(cpu, mt_cf::string_create(cpu, t, wants_mutable(cpu, cpu->r[0])));
}

// +stringWithUTF8String: nil if the bytes are not UTF-8 (NULL raises).
void string_with_utf8(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], p = cpu->r[2];
    if (p == 0) {
        raise(cpu, "+[NSString stringWithUTF8String:]", "NULL", 0);
    }
    const char *s = mt_g2h_str(cpu, p);
    return_autoreleased(cpu, mt_cf::string_create_bytes(
                                 cpu, reinterpret_cast<const std::uint8_t *>(s), std::strlen(s),
                                 mt_cf::k_enc_utf8, wants_mutable(cpu, cls)));
}

// Decodes the C string at guest `p` in NSStringEncoding `ns_encoding`; false if it does not.
bool decode_cstring(mt_cpu *cpu, u32 p, u32 ns_encoding, const char *method, std::u16string *out) {
    if (p == 0) {
        raise(cpu, method, "NULL", 0);
    }
    const char *s = mt_g2h_str(cpu, p);
    const u32 enc = mt_cf::ns_to_cf_encoding(ns_encoding);
    if (!mt_cf::encoding_supported(enc)) {
        log(cpu, "%s: encoding 0x%08X is not supported: nil", method,
            static_cast<unsigned>(ns_encoding));
        return false;
    }
    return mt_cf::decode(reinterpret_cast<const std::uint8_t *>(s), std::strlen(s), enc, *out);
}

// +stringWithCString:encoding:
void string_with_cstring(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], p = cpu->r[2], enc = cpu->r[3];
    std::u16string t;
    const bool ok = decode_cstring(cpu, p, enc, "+[NSString stringWithCString:encoding:]", &t);
    return_autoreleased(cpu,
                        ok ? mt_cf::string_create(cpu, std::move(t), wants_mutable(cpu, cls)) : 0u);
}

// +stringWithFormat:
void string_with_format(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], fmt = cpu->r[2];
    Varargs va(cpu, 1);
    std::u16string t;
    const bool ok = format(cpu, fmt, va.args, "+[NSString stringWithFormat:]", &t);
    return_autoreleased(cpu,
                        ok ? mt_cf::string_create(cpu, std::move(t), wants_mutable(cpu, cls)) : 0u);
}

// --- NSString instance methods ---------------------------------------------------------------

// -initWithCString:encoding:
void string_init_cstring(mt_cpu *cpu) {
    const u32 self = cpu->r[0], p = cpu->r[2], enc = cpu->r[3];
    std::u16string t;
    const bool ok = decode_cstring(cpu, p, enc, "-[NSString initWithCString:encoding:]", &t);
    finish_init(cpu, self, ok, std::move(t));
}

// -initWithFormat:
void string_init_format(mt_cpu *cpu) {
    const u32 self = cpu->r[0], fmt = cpu->r[2];
    Varargs va(cpu, 1);
    std::u16string t;
    const bool ok = format(cpu, fmt, va.args, "-[NSString initWithFormat:]", &t);
    finish_init(cpu, self, ok, std::move(t));
}

// -UTF8String: a NUL-terminated guest copy owned by the string (NULL if it has a lone
// surrogate).
void string_utf8(mt_cpu *cpu) {
    cpu->r[0] = mt_cf::string_guest_cstring(cpu, cpu->r[0], mt_cf::k_enc_utf8);
}

// -length: UTF-16 code units.
void string_length(mt_cpu *cpu) {
    cpu->r[0] = static_cast<u32>(text_arg(cpu, cpu->r[0], "-[NSString length]").size());
}

// -isEqualToString:
void string_is_equal_to_string(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2];
    const std::u16string *a = mt_cf::string_chars(cpu, self);
    const std::u16string *b = other != 0 ? mt_cf::string_chars(cpu, other) : nullptr;
    cpu->r[0] = a != nullptr && b != nullptr && *a == *b ? 1u : 0u;
}

// -isEqual: a string with the same text; NO for anything else.
void string_is_equal(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2];
    const std::u16string *a = mt_cf::string_chars(cpu, self);
    const std::u16string *b =
        other != 0 && mt_cf::is_string(cpu, other) ? mt_cf::string_chars(cpu, other) : nullptr;
    cpu->r[0] = self == other || (a != nullptr && b != nullptr && *a == *b) ? 1u : 0u;
}

// -hash: of the text (equal strings, equal hashes; FNV-1a).
void string_hash(mt_cpu *cpu) {
    u32 h = 2166136261u;
    for (const char16_t c : text_arg(cpu, cpu->r[0], "-[NSString hash]")) {
        h = (h ^ static_cast<u32>(c)) * 16777619u;
    }
    cpu->r[0] = h;
}

// -compare:options: (NSCaseInsensitiveSearch, NSNumericSearch; NSLiteralSearch is what this
// does anyway) -> NSComparisonResult. A nil argument compares as greater on iOS 4.
void string_compare_options(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2], options = cpu->r[3];
    const std::u16string &a = text_arg(cpu, self, "-[NSString compare:options:]");
    if (other == 0) {
        cpu->r[0] = 1u;
        return;
    }
    const std::u16string &b = text_arg(cpu, other, "-[NSString compare:options:]");
    cpu->r[0] = static_cast<u32>(compare(a, b, options));
}

// -stringByAppendingPathComponent: one '/' between the two, trailing slashes of the receiver
// and leading slashes of the component dropped (NSPathUtilities).
void string_append_path(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2];
    std::u16string base = text_arg(cpu, self, "-[NSString stringByAppendingPathComponent:]");
    std::u16string comp = text_arg(cpu, other, "-[NSString stringByAppendingPathComponent:]");
    while (base.size() > 1 && base.back() == u'/') {
        base.pop_back();
    }
    std::size_t start = 0;
    while (start < comp.size() && comp[start] == u'/') {
        start++;
    }
    comp.erase(0, start);
    while (!comp.empty() && comp.back() == u'/') {
        comp.pop_back();
    }
    if (!comp.empty()) {
        if (!base.empty() && base.back() != u'/') {
            base.push_back(u'/');
        }
        base += comp;
    }
    return_autoreleased(cpu, mt_cf::string_create(cpu, std::move(base), false));
}

// -description: the string itself.
void string_description(mt_cpu *) {}

// -copyWithZone: the same string for an immutable one, else an immutable copy.
void string_copy(mt_cpu *cpu) {
    const u32 self = cpu->r[0];
    if (!mt_cf::string_is_mutable(cpu, self)) {
        cpu->r[0] = mt_objc_retain(cpu, self);
        return;
    }
    cpu->r[0] = mt_cf::string_create(cpu, text_arg(cpu, self, "-[NSString copy]"), false);
}

// -mutableCopyWithZone:
void string_mutable_copy(mt_cpu *cpu) {
    cpu->r[0] =
        mt_cf::string_create(cpu, text_arg(cpu, cpu->r[0], "-[NSString mutableCopy]"), true);
}

// --- NSMutableString -------------------------------------------------------------------------

std::u16string *mutable_text(mt_cpu *cpu, u32 self, const char *method) {
    std::u16string *t = mt_cf::string_mutable_chars(cpu, self);
    if (t == nullptr) {
        raise(cpu, method, "attempt to mutate an immutable string", self);
    }
    return t;
}

// -initWithCapacity: an empty string from +alloc.
void mstring_init_capacity(mt_cpu *) {}

// -appendFormat:
void mstring_append_format(mt_cpu *cpu) {
    const u32 self = cpu->r[0], fmt = cpu->r[2];
    Varargs va(cpu, 1);
    std::u16string t;
    if (format(cpu, fmt, va.args, "-[NSMutableString appendFormat:]", &t)) {
        *mutable_text(cpu, self, "-[NSMutableString appendFormat:]") += t;
    }
}

// --- NSObject ----------------------------------------------------------------------------

// -[NSObject description]: "<ClassName: 0x...>", as Foundation prints it.
void object_description(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    const u32 self = cpu->r[0];
    const char *name = mt_objc_class_name(rt, mt_objc_class_of(rt, self));
    char text[160];
    std::snprintf(text, sizeof text, "<%s: 0x%x>", name != nullptr ? name : "?",
                  static_cast<unsigned>(self));
    return_autoreleased(cpu, string_from_utf8(cpu, text));
}

const mt_objc_host_method k_object_instance[] = {{"description", object_description}};

const mt_objc_host_method k_string_instance[] = {
    {"initWithCString:encoding:", string_init_cstring},
    {"initWithFormat:", string_init_format},
    {"UTF8String", string_utf8},
    {"length", string_length},
    {"isEqualToString:", string_is_equal_to_string},
    {"isEqual:", string_is_equal},
    {"hash", string_hash},
    {"compare:options:", string_compare_options},
    {"stringByAppendingPathComponent:", string_append_path},
    {"description", string_description},
    {"copyWithZone:", string_copy},
    {"mutableCopyWithZone:", string_mutable_copy},
};
const mt_objc_host_method k_string_class[] = {
    {"allocWithZone:", string_alloc},
    {"string", string_string},
    {"stringWithString:", string_with_string},
    {"stringWithUTF8String:", string_with_utf8},
    {"stringWithCString:encoding:", string_with_cstring},
    {"stringWithFormat:", string_with_format},
};
const mt_objc_host_method k_mutable_string_instance[] = {
    {"initWithCapacity:", mstring_init_capacity},
    {"appendFormat:", mstring_append_format},
};

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

} // namespace

const mt_objc_host_category k_nsobject_methods = {"NSObject", k_object_instance,
                                                  MT_COUNT(k_object_instance), nullptr, 0};
const mt_objc_host_category k_nsstring_methods = {"NSString", k_string_instance,
                                                  MT_COUNT(k_string_instance), k_string_class,
                                                  MT_COUNT(k_string_class)};
const mt_objc_host_category k_nsmutablestring_methods = {
    "NSMutableString", k_mutable_string_instance, MT_COUNT(k_mutable_string_instance), nullptr, 0};

// --- host API ------------------------------------------------------------------------------------

std::string string_utf8(mt_cpu *cpu, u32 str) {
    const std::u16string *s = str != 0 ? mt_cf::string_chars(cpu, str) : nullptr;
    return s != nullptr ? mt_cf::to_utf8(*s) : std::string();
}

u32 string_from_utf8(mt_cpu *cpu, const std::string &text) {
    return mt_cf::string_create(cpu, from_utf8(text), false);
}

std::string describe(mt_cpu *cpu, u32 obj) {
    if (obj == 0) {
        return "(null)";
    }
    if (mt_cf::is_string(cpu, obj)) {
        return string_utf8(cpu, obj);
    }
    const u32 d = mt_objc_send0(cpu, obj, "description");
    return d != 0 && mt_cf::is_string(cpu, d) ? string_utf8(cpu, d) : std::string("(null)");
}

bool objects_equal(mt_cpu *cpu, u32 a, u32 b) {
    if (a == b) {
        return true;
    }
    if (a == 0 || b == 0) {
        return false;
    }
    const bool sa = mt_cf::is_string(cpu, a), sb = mt_cf::is_string(cpu, b);
    if (sa || sb) {
        const std::u16string *x = sa ? mt_cf::string_chars(cpu, a) : nullptr;
        const std::u16string *y = sb ? mt_cf::string_chars(cpu, b) : nullptr;
        return x != nullptr && y != nullptr && *x == *y;
    }
    mt_cf::Number x, y;
    if (mt_cf::number_value(cpu, a, &x) && mt_cf::number_value(cpu, b, &y)) {
        return x.is_float || y.is_float ? (x.is_float ? x.f : static_cast<double>(x.i)) ==
                                              (y.is_float ? y.f : static_cast<double>(y.i))
                                        : x.i == y.i;
    }
    return mt_objc_send1(cpu, a, "isEqual:", b) != 0;
}

} // namespace mt_ns

using namespace mt_ns;

// Class NSClassFromString(NSString *): the class of that name (a host class, a placeholder or
// an image class), Nil for nil or an unknown name.
extern "C" void mt_hle_NSClassFromString(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 name = cpu->r[0];
    mt_objc *rt = runtime(cpu);
    cpu->r[0] =
        name != 0 && rt != nullptr ? mt_objc_class_named(rt, string_utf8(cpu, name).c_str()) : 0u;
}
