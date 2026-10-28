// The CoreFoundation HLE functions and data-import hooks (#48, ADR 0032), listed by import
// name in runtime/src/hle/registry.cpp, and the bridged host classes listed in
// runtime/src/objc/classes.cpp.
#ifndef MAPLETHIEF_RUNTIME_CF_CF_HLE_H
#define MAPLETHIEF_RUNTIME_CF_CF_HLE_H

#include <cstdint>

#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

extern "C" {

// cf.cpp: lifetime and the data imports (their objects are created by mt_cf_start).
void mt_hle_CFRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_init_kCFAllocatorDefault(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFBooleanTrue(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFBooleanFalse(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFNumberNaN(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFNumberNegativeInfinity(mt_cpu *cpu, std::uint32_t id,
                                           std::uint32_t addr) noexcept;
void mt_hle_init_kCFNumberPositiveInfinity(mt_cpu *cpu, std::uint32_t id,
                                           std::uint32_t addr) noexcept;
void mt_hle_init_kCFRunLoopCommonModes(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFRunLoopDefaultMode(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFBundleIdentifierKey(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_kCFBundleVersionKey(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_NSLocaleCountryCode(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_NSLocaleLanguageCode(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
// Foundation's string constants are CF constants too (#49).
void mt_hle_init_NSLocalizedDescriptionKey(mt_cpu *cpu, std::uint32_t id,
                                           std::uint32_t addr) noexcept;

// string.cpp
void mt_hle_CFStringCreateWithCString(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFStringAppendCharacters(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFStringConvertEncodingToNSStringEncoding(mt_cpu *cpu, std::uint32_t id) noexcept;

// data.cpp
void mt_hle_CFDataCreate(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFDataGetLength(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFDataGetBytePtr(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFDataGetBytes(mt_cpu *cpu, std::uint32_t id) noexcept;

// bundle.cpp
void mt_hle_CFBundleGetMainBundle(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFBundleCopyResourceURLForLocalization(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFURLCreateDataAndPropertiesFromResource(mt_cpu *cpu, std::uint32_t id) noexcept;

// runloop.cpp
void mt_hle_CFAbsoluteTimeGetCurrent(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopGetMain(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopGetCurrent(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopTimerCreate(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopTimerSetNextFireDate(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopAddTimer(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CFRunLoopAddSource(mt_cpu *cpu, std::uint32_t id) noexcept;

} // extern "C"

namespace mt_cf {

// bridged_classes.cpp, in superclass order.
extern const mt_objc_host_class k_nsstring;
extern const mt_objc_host_class k_nsmutablestring;
extern const mt_objc_host_class k_nscfstring;
extern const mt_objc_host_class k_nscfconstantstring;
extern const mt_objc_host_class k_nsdata;
extern const mt_objc_host_class k_nsmutabledata;
extern const mt_objc_host_class k_nscfdata;
extern const mt_objc_host_class k_nsvalue;
extern const mt_objc_host_class k_nsnumber;
extern const mt_objc_host_class k_nscfnumber;
extern const mt_objc_host_class k_nscfboolean;
extern const mt_objc_host_class k_nscftype;

} // namespace mt_cf

#endif // MAPLETHIEF_RUNTIME_CF_CF_HLE_H
