// The CoreFoundation object model (#48, ADR 0032): what CF and Foundation share.
//
// A CF object is a guest Objective-C object (toll-free bridging is literal: a CFStringRef and
// an NSString * are the same guest address). Its class is one of the bridged host classes of
// bridged_classes.cpp, and its contents are host state of the object (maplethief/objc/host.h),
// never guest bytes, except where the guest must see them:
//
//   __NSCFString          UTF-16 text (std::u16string); immutable or mutable (a flag)
//     __NSCFConstantString   the __cfstring literals: {isa, flags, chars, length} in guest
//                            memory, decoded into the same state on first use; immortal
//   __NSCFData            bytes in one guest heap block (CFDataGetBytePtr is a guest pointer)
//   __NSCFNumber          a CFNumberType and its value; __NSCFBoolean: kCFBooleanTrue/False
//   __NSCFType            the CF-only types: run loop, timer, bundle, URL (each its own
//                         state type)
//
// The public classes above them (NSString, NSMutableString, NSData, NSMutableData, NSValue,
// NSNumber) are host classes too, so the imported _OBJC_CLASS_$_NSString etc. are real
// classes, not placeholders. Foundation (#49) adds the NS selectors to their method tables
// (bridged_classes.cpp) on top of the storage functions below; UIKit text drawing (#51) reads
// strings with string_chars.
//
// Every function here takes the guest thread's cpu and uses the current ObjC runtime
// (mt_objc_current); none sends a message unless it says so. Single guest thread: no locks.
#ifndef MAPLETHIEF_RUNTIME_CF_CF_OBJECTS_H
#define MAPLETHIEF_RUNTIME_CF_CF_OBJECTS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/cf.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"

namespace mt_cf {

using u32 = std::uint32_t;

// --- encodings (encoding.cpp) ------------------------------------------------------------------

// CFStringEncoding values the runtime converts (CFStringEncodingExt.h, CFString.h).
enum : u32 {
    k_enc_mac_roman = 0x0000u, // not supported (no game path uses it)
    k_enc_unicode = 0x0100u,   // UTF-16, the device's byte order (little-endian) unless a BOM
    k_enc_iso_latin1 = 0x0201u,
    k_enc_dos_korean = 0x0422u, // CP949 (Unified Hangul Code): EUC-KR plus 8,822 syllables
    k_enc_ascii = 0x0600u,
    k_enc_euc_kr = 0x0940u, // KS X 1001 only; what the game converts its tables with
    k_enc_utf8 = 0x08000100u,
    k_enc_utf16be = 0x10000100u,
    k_enc_utf16le = 0x14000100u,
    k_enc_invalid = 0xFFFFFFFFu, // kCFStringEncodingInvalidId
};

// Whether decode/encode handle `encoding`.
bool encoding_supported(u32 encoding) noexcept;
// Bytes in `encoding` -> UTF-16. False (out unspecified) if a byte sequence is invalid or the
// encoding is unsupported, as CFStringCreateWithBytes returns NULL.
bool decode(const std::uint8_t *bytes, std::size_t n, u32 encoding, std::u16string &out);
// UTF-16 -> bytes in `encoding`, no terminator, no BOM. False if a character has no encoding
// (no loss byte: CFStringGetCString / -cStringUsingEncoding: fail the same way).
bool encode(const char16_t *chars, std::size_t n, u32 encoding, std::string &out);
// UTF-16 -> UTF-8 for logs and host APIs; an unpaired surrogate becomes U+FFFD.
std::string to_utf8(const std::u16string &s);
// CFStringConvertEncodingToNSStringEncoding and its inverse
// (CFStringConvertNSStringEncodingToEncoding): the NS constants for the encodings Foundation
// names (NSUTF8StringEncoding 4, ...), else the CF value with bit 31 set.
u32 cf_to_ns_encoding(u32 cf) noexcept;
u32 ns_to_cf_encoding(u32 ns) noexcept;

// --- classes and lifetime (cf.cpp) -------------------------------------------------------------

// The guest class objects of the bridged classes in the current runtime (0 if absent).
struct Classes {
    u32 string, constant_string, data, number, boolean, type;
};
const Classes &classes(mt_cpu *cpu);

// Logs one line ("cf: ...") through the ObjC runtime's log sink (the boot's).
void log(mt_cpu *cpu, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// The guest heap of the guest thread (for CF's guest buffers).
mt_mem *heap(mt_cpu *cpu) noexcept;

// Immortal objects ignore -retain/-release (the CF constants, the main run loop and bundle);
// constant strings and booleans are immortal by class.
void make_immortal(mt_cpu *cpu, u32 obj);
bool is_immortal(mt_cpu *cpu, u32 obj);

// A new instance of `cls` (retain count 1, no message sent), 0 if out of memory.
u32 create(mt_cpu *cpu, u32 cls);

// --- strings (string.cpp) ----------------------------------------------------------------------

// Whether `obj` is a CF string (an __NSCFString or a constant string).
bool is_string(mt_cpu *cpu, u32 obj);
// The text of a CF string, or nullptr if `obj` is none (or a malformed literal). Valid until
// the string is mutated or deallocated.
const std::u16string *string_chars(mt_cpu *cpu, u32 obj);
// The text of a mutable CF string, for changing it in place, or nullptr if `obj` is not one.
std::u16string *string_mutable_chars(mt_cpu *cpu, u32 obj);
bool string_is_mutable(mt_cpu *cpu, u32 obj);
// A new __NSCFString (retain count 1), 0 if out of memory.
u32 string_create(mt_cpu *cpu, std::u16string chars, bool is_mutable);
// A new __NSCFString from bytes in `encoding`, 0 if they do not decode.
u32 string_create_bytes(mt_cpu *cpu, const std::uint8_t *bytes, std::size_t n, u32 encoding,
                        bool is_mutable);
// Replaces the text of an __NSCFString, mutable or not: the -init... methods of a string from
// +alloc (Foundation, #49). False if `obj` is no __NSCFString (constant strings never change).
bool string_assign(mt_cpu *cpu, u32 obj, std::u16string chars);
// A NUL-terminated guest copy of the string in `encoding`, owned by the string (freed when it
// is deallocated; still valid after a later mutation): -UTF8String, -cStringUsingEncoding:,
// CFStringGetCStringPtr. 0 if the text has no encoding in it.
u32 string_guest_cstring(mt_cpu *cpu, u32 obj, u32 encoding);
// A constant string literal {isa, 0x7C8, chars, length} for an ASCII text, in guest memory
// (the CF constants kCFRunLoopDefaultMode etc.); 0 if out of memory.
u32 string_create_constant(mt_cpu *cpu, const char *ascii);

// --- data (data.cpp) ---------------------------------------------------------------------------

// A new __NSCFData with a copy of `n` bytes at host `bytes` (may be null when n is 0).
u32 data_create(mt_cpu *cpu, const std::uint8_t *bytes, u32 n, bool is_mutable);
// The guest address and length of a CF data object's bytes; false if `obj` is not one.
bool data_bytes(mt_cpu *cpu, u32 obj, u32 *guest_bytes, u32 *length);
// Replaces the bytes of an __NSCFData, mutable or not: the -init... methods of data from +alloc
// (Foundation, #49). False if `obj` is not CF data or out of memory.
bool data_assign(mt_cpu *cpu, u32 obj, const std::uint8_t *bytes, u32 n);
// Mutable data only: appends or changes the length (new bytes zero). The byte pointer may
// move, as with CFDataGetMutableBytePtr. False if `obj` is not mutable data or out of memory.
bool data_append(mt_cpu *cpu, u32 obj, const std::uint8_t *bytes, u32 n);
bool data_set_length(mt_cpu *cpu, u32 obj, u32 length);

// --- numbers (cf.cpp) --------------------------------------------------------------------------

struct Number {
    u32 type; // CFNumberType: 4 kCFNumberSInt64Type, 6 kCFNumberFloat64Type, ...
    bool is_float;
    std::int64_t i;
    double f;
};
u32 number_create(mt_cpu *cpu, const Number &value);
// The value of an __NSCFNumber or __NSCFBoolean (type kCFNumberCharType for a boolean).
bool number_value(mt_cpu *cpu, u32 obj, Number *out);
// kCFBooleanTrue / kCFBooleanFalse.
u32 boolean(mt_cpu *cpu, bool value);

// --- the bundle (bundle.cpp) -------------------------------------------------------------------

// The bundle-relative path (exact case, '/'-separated) of a resource, searched as
// CFBundleCopyResourceURLForLocalization does: <subdir>/<name>[.<type>] in the bundle, then
// in <localization>.lproj/ (and its alias: English <-> en, Korean <-> ko); a null or empty
// localization means the preferred ones (the configured language, then the development
// region English). Empty if there is no such file or no bundle is configured.
std::string bundle_find(mt_cpu *cpu, const std::string &name, const std::string &type,
                        const std::string &subdir, const std::string &localization);
// Host path of a bundle-relative path; the guest path (NSBundle -bundlePath + "/" + rel).
std::string bundle_host_path(const std::string &rel);
std::string bundle_guest_path(const std::string &rel);
// Whether a bundle-relative path names a file of the bundle (exact case).
bool bundle_has(mt_cpu *cpu, const std::string &rel);
// The file's bytes; false if it cannot be read.
bool bundle_read(const std::string &rel, std::vector<std::uint8_t> &out);
// Every file of the bundle, bundle-relative, sorted.
std::vector<std::string> bundle_files(mt_cpu *cpu);
// The bundle-relative path of a CFURL made by the bundle functions, or false.
bool url_bundle_path(mt_cpu *cpu, u32 url, std::string *rel);
// A new CFURL (retain count 1) for a bundle-relative path.
u32 url_create(mt_cpu *cpu, const std::string &rel);

// --- internals shared by the files of runtime/src/cf/ ------------------------------------------

// The configuration of mt_cf_configure, with owned strings. `generation` changes with every
// mt_cf_configure (the bundle index is rebuilt then).
struct Config {
    std::string bundle_dir, bundle_path, language;
    u32 generation = 0;
};
const Config &config() noexcept;

// The runtime CF is bound to (mt_cf_start, or the current one on first use); rebinding
// forgets every guest object of the previous runtime.
mt_objc *runtime(mt_cpu *cpu);
// Changes on every rebinding (also to a new runtime at the same host address): Foundation and
// UIKit drop their guest objects of the previous runtime when it changes.
u32 binding() noexcept;
// Creates the objects of the data imports recorded since the last call (in this address
// space) and stores their pointers; returns how many. mt_cf_start = rebind + this.
u32 make_constants(mt_cpu *cpu);
// Per-file hooks run when CF rebinds: the run loop's and the bundle's guest objects.
void runloop_forget() noexcept;
void bundle_forget() noexcept;

// Stops the guest like the device's crash for a CF misuse (a NULL or wrong object): logs
// "cf: <function>: <what>" and traps with MT_TRAP_ABORT at the guest caller.
[[noreturn]] void misuse(mt_cpu *cpu, const char *function, const char *what, u32 obj) noexcept;

} // namespace mt_cf

#endif // MAPLETHIEF_RUNTIME_CF_CF_OBJECTS_H
