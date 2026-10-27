// The bridged classes of CoreFoundation and Foundation (#48, ADR 0032), as host classes of the
// ObjC runtime (maplethief/objc/host.h), registered in runtime/src/objc/classes.cpp:
//
//   NSObject
//     NSString                       (abstract)
//       NSMutableString              (abstract)
//         __NSCFString               every CFString / NSString object: string.cpp's state
//           __NSCFConstantString     the __cfstring literals; class object =
//                                    ___CFConstantStringClassReference (ADR 0032)
//     NSData                         (abstract)
//       NSMutableData                (abstract)
//         __NSCFData                 every CFData / NSData object: data.cpp's state
//     NSValue                        (abstract)
//       NSNumber                     (abstract)
//         __NSCFNumber               kCFNumberNaN / ...Infinity; numbers #49 creates
//         __NSCFBoolean              kCFBooleanTrue / kCFBooleanFalse
//     __NSCFType                     run loop, timer, bundle, URL
//
// The extension point of Foundation (#49): it adds its NS selectors (-length,
// -UTF8String, +stringWithFormat:, +allocWithZone: returning an __NSCFString, ...) to the
// method tables below, implemented on the storage of cf_objects.h. Today the tables hold
// what CF itself needs (constants ignore -retain/-release: CF's constant objects are
// immortal) and the two NSString methods the offline stubs (#52) send:
// +stringWithUTF8String: and -UTF8String. Everything else is NSObject's.
#include <cstdint>
#include <cstring>

#include "maplethief/hle/abi.h"

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"

namespace mt_cf {

namespace {

// [super <_cmd>] from a method of the receiver's class chain: the lookup starts above the
// class that defines this method.
void send_super(mt_cpu *cpu) {
    mt_objc *rt = mt_objc_current();
    const u32 self = mt_objc_self(cpu), sel = mt_objc_cmd(cpu);
    mt_objc_method m;
    if (rt == nullptr || mt_objc_lookup(rt, mt_objc_class_of(rt, self), sel, &m) == 0) {
        cpu->r[0] = 0;
        return;
    }
    mt_objc_msg_super(cpu, self, m.owner, sel, nullptr, 0);
}

void cf_retain(mt_cpu *cpu) {
    if (!is_immortal(cpu, mt_objc_self(cpu))) {
        send_super(cpu);
    } // else r0 is self already
}

void cf_release(mt_cpu *cpu) {
    if (!is_immortal(cpu, mt_objc_self(cpu))) {
        send_super(cpu);
    }
}

void cf_retain_count(mt_cpu *cpu) {
    if (is_immortal(cpu, mt_objc_self(cpu))) {
        cpu->r[0] = 0xFFFFFFFFu; // UINT_MAX, as for every object that is never deallocated
    } else {
        send_super(cpu);
    }
}

// +[NSString stringWithUTF8String:] (and NSMutableString's): an autoreleased __NSCFString;
// nil if the bytes are not UTF-8. NULL raises NSInvalidArgumentException on iOS: a misuse here.
void string_with_utf8(mt_cpu *cpu) {
    const u32 text = cpu->r[2];
    if (text == 0) {
        misuse(cpu, "+[NSString stringWithUTF8String:]", "NULL", 0);
    }
    const char *p = mt_g2h_str(cpu, text);
    const std::size_t n = std::strlen(p);
    mt_objc *rt = mt_objc_current();
    const bool is_mutable =
        rt != nullptr && cpu->r[0] == mt_objc_class_named(rt, "NSMutableString");
    const u32 s = string_create_bytes(cpu, reinterpret_cast<const std::uint8_t *>(p), n, k_enc_utf8,
                                      is_mutable);
    cpu->r[0] = s != 0 ? mt_objc_autorelease(cpu, s) : 0u;
}

// -[NSString UTF8String]: a NUL-terminated guest copy owned by the string.
void string_utf8(mt_cpu *cpu) { cpu->r[0] = string_guest_cstring(cpu, cpu->r[0], k_enc_utf8); }

const mt_objc_host_method k_string_methods[] = {{"UTF8String", string_utf8}};
const mt_objc_host_method k_string_class_methods[] = {{"stringWithUTF8String:", string_with_utf8}};

// The NSObject protocol's memory management of objects that may be immortal.
const mt_objc_host_method k_lifetime[] = {
    {"retain", cf_retain}, {"release", cf_release}, {"retainCount", cf_retain_count}};
constexpr u32 k_lifetime_count = sizeof k_lifetime / sizeof k_lifetime[0];

} // namespace

// Instance sizes: 4 (the isa) for the abstract classes, 8 (CFRuntimeBase: isa + info) for the
// concrete ones; a literal is 16 bytes ({isa, flags, chars, length}). Contents are host state.
const mt_objc_host_class k_nsstring = {
    "NSString", "NSObject", 4, k_string_methods, 1, k_string_class_methods, 1};
const mt_objc_host_class k_nsmutablestring = {"NSMutableString", "NSString", 4, nullptr, 0,
                                              nullptr,           0};
const mt_objc_host_class k_nscfstring = {
    "__NSCFString", "NSMutableString", 8, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nscfconstantstring = {
    "__NSCFConstantString", "__NSCFString", 16, k_lifetime, k_lifetime_count, nullptr, 0};
const mt_objc_host_class k_nsdata = {"NSData", "NSObject", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nsmutabledata = {"NSMutableData", "NSData", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nscfdata = {"__NSCFData", "NSMutableData", 8, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nsvalue = {"NSValue", "NSObject", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nsnumber = {"NSNumber", "NSValue", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_nscfnumber = {"__NSCFNumber",   "NSNumber", 8, k_lifetime,
                                         k_lifetime_count, nullptr,    0};
const mt_objc_host_class k_nscfboolean = {"__NSCFBoolean",  "NSNumber", 8, k_lifetime,
                                          k_lifetime_count, nullptr,    0};
const mt_objc_host_class k_nscftype = {"__NSCFType",     "NSObject", 8, k_lifetime,
                                       k_lifetime_count, nullptr,    0};

} // namespace mt_cf
