// Foundation (#49, ADR 0035): the Foundation classes and functions the game reaches, as host
// classes of the ObjC runtime (ADR 0026, maplethief/objc/host.h) on CoreFoundation's object
// model (ADR 0032, runtime/src/cf/cf_objects.h).
//
//   ns_string.cpp      NSString / NSMutableString selectors on CF's __NSCFString (host categories),
//                      +stringWithFormat:'s %@, -[NSObject description], NSClassFromString
//   ns_data.cpp        NSData selectors on CF's __NSCFData; NSNumber's on __NSCFNumber
//   collections.cpp    NSArray, NSMutableArray, NSDictionary, NSMutableDictionary, NSSet, NSNull
//   ns_bundle.cpp      NSBundle (paths, Info.plist, localized strings), NSLocale
//   plist.cpp          the binary property list reader (Info.plist, *.strings)
//   files.cpp          NSFileManager, NSFileHandle, NSHomeDirectory,
//                      NSSearchPathForDirectoriesInDomains: the save files
//   misc.cpp           NSURL, NSError, NSThread
//
// Which selectors: exactly the game-reachable ones of docs/re/host-surface.md (generated from
// #11's analysis, checked by objc.host_surface) plus what the runtime's own host code sends
// (#48's CFURL properties, #52's stubs). Any other selector is an unrecognized selector.
//
// The functions below are the host API of these objects for the other framework areas: UIKit
// (#54) builds the touch sets and reads strings, AVFoundation reads file URLs. Like CF's, every
// function takes the guest thread's cpu and uses the current runtime; none sends a message
// unless it says so. Created objects have retain count 1 (not autoreleased) unless noted.
#ifndef MAPLETHIEF_RUNTIME_FOUNDATION_FOUNDATION_H
#define MAPLETHIEF_RUNTIME_FOUNDATION_FOUNDATION_H

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

extern "C" {

// HLE functions (runtime/src/hle/registry.cpp).
void mt_hle_NSClassFromString(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_NSHomeDirectory(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_NSSearchPathForDirectoriesInDomains(mt_cpu *cpu, std::uint32_t id) noexcept;

} // extern "C"

namespace mt_ns {

using u32 = std::uint32_t;

// --- strings (ns_string.cpp) -------------------------------------------------------------------

// The UTF-8 text of an NSString (a CF string), "" for nil or a non-string.
std::string string_utf8(mt_cpu *cpu, u32 str);
// A new immutable NSString from UTF-8 text (invalid bytes read as Latin-1); 0 if out of memory.
u32 string_from_utf8(mt_cpu *cpu, const std::string &text);
// [obj description] as UTF-8: the text itself for a string, "(null)" for nil. Sends
// -description to other objects.
std::string describe(mt_cpu *cpu, u32 obj);
// Whether two objects are equal as dictionary keys: the same object, two strings with the same
// text, two numbers with the same value, or else [a isEqual:b] (sent).
bool objects_equal(mt_cpu *cpu, u32 a, u32 b);

// --- collections (collections.cpp) -------------------------------------------------------------

// A new NSArray (mutable: NSMutableArray) holding `items` (each retained).
u32 array_create(mt_cpu *cpu, const u32 *items, u32 count, bool is_mutable);
// The items of an NSArray, or nullptr if `obj` is none. Valid until the array changes.
const std::vector<u32> *array_items(mt_cpu *cpu, u32 obj);
// A new NSSet holding the distinct `items` (each retained once): -[UIEvent allTouches], the
// touches of touchesBegan:withEvent: (#54).
u32 set_create(mt_cpu *cpu, const u32 *items, u32 count);
// The members of an NSSet, or nullptr if `obj` is none.
const std::vector<u32> *set_items(mt_cpu *cpu, u32 obj);
// A new NSDictionary (mutable: NSMutableDictionary), empty.
u32 dictionary_create(mt_cpu *cpu, bool is_mutable);
// [dict objectForKey:key] without a message to the dictionary (keys compare with
// objects_equal); 0 if absent or `dict` is no dictionary.
u32 dictionary_get(mt_cpu *cpu, u32 dict, u32 key);
// [dict setObject:value forKey:key] on any dictionary (the key is copied if it is a mutable
// string, the value retained). False if `dict` is no dictionary.
bool dictionary_set(mt_cpu *cpu, u32 dict, u32 key, u32 value);
// The NSNull singleton.
u32 null_object(mt_cpu *cpu);

// --- bundle (ns_bundle.cpp) --------------------------------------------------------------------

// The guest path of the app's home directory: the directory that holds the bundle
// (/var/mobile/Applications/<UUID>), NSHomeDirectory().
std::string home_guest_path();

// --- URLs (misc.cpp) ---------------------------------------------------------------------------

// The string of an NSURL (for a file URL: its path, not "file://..."), false if `url` is none.
bool url_string(mt_cpu *cpu, u32 url, std::string *out, bool *is_file);

// --- files (files.cpp) -------------------------------------------------------------------------

// The host path of a guest path, for reading (the bundle and the home directory) or for
// writing (the home directory only). Empty if the guest may not reach it: outside both, or
// with a "." or ".." component. <home>/X maps to <mt_stubs_data_dir()>/X.
std::string host_path(const std::string &guest, bool for_writing);

// --- classes and host categories (registered in runtime/src/objc/classes.cpp)
// ----------------------

extern const mt_objc_host_class k_nsarray;
extern const mt_objc_host_class k_nsmutablearray;
extern const mt_objc_host_class k_nsdictionary;
extern const mt_objc_host_class k_nsmutabledictionary;
extern const mt_objc_host_class k_nsset;
extern const mt_objc_host_class k_nsnull;
extern const mt_objc_host_class k_nsbundle;
extern const mt_objc_host_class k_nslocale;
extern const mt_objc_host_class k_nsfilemanager;
extern const mt_objc_host_class k_nsfilehandle;
extern const mt_objc_host_class k_nsurl;
extern const mt_objc_host_class k_nserror;
extern const mt_objc_host_class k_nsthread;

extern const mt_objc_host_category k_nsobject_methods;
extern const mt_objc_host_category k_nsstring_methods;
extern const mt_objc_host_category k_nsmutablestring_methods;
extern const mt_objc_host_category k_nsdata_methods;
extern const mt_objc_host_category k_nsnumber_methods;

// --- internals shared by these files -----------------------------------------------------------

// The current runtime (CF's binding).
mt_objc *runtime(mt_cpu *cpu);
// Logs "foundation: ..." through the runtime's log sink.
void log(mt_cpu *cpu, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
// Stops the guest like the device's uncaught NSException for a misuse (nil where Foundation
// raises NSInvalidArgumentException, an index out of range): logs and traps (MT_TRAP_ABORT) at
// the guest caller.
[[noreturn]] void raise(mt_cpu *cpu, const char *method, const char *what, u32 obj) noexcept;
// Whether class or metaclass `cls` is `ancestor` or inherits from it.
bool inherits(mt_cpu *cpu, u32 cls, u32 ancestor);
// The class object named `name` (0 if absent).
u32 class_named(mt_cpu *cpu, const char *name);
// A singleton instance of `cls` that ignores -retain/-release (NSNull, the main bundle, the
// current locale, the default file manager): created on first use, per runtime.
u32 singleton(mt_cpu *cpu, const char *class_name);
// -retain, -release, -retainCount of the singletons.
void singleton_retain(mt_cpu *cpu);
void singleton_release(mt_cpu *cpu);
void singleton_retain_count(mt_cpu *cpu);

} // namespace mt_ns

#endif // MAPLETHIEF_RUNTIME_FOUNDATION_FOUNDATION_H
