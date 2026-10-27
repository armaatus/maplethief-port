// Every HLE implementation the runtime has, by import symbol (ADR 0019). mt_imports_link
// fills the import table from this list; an import that is not here gets the default
// unimplemented-import handler. M4 modules add their functions (and data-object init hooks)
// here, grouped by library.
#include "maplethief/hle/imports.h"

#include <cstdint>

#include "hle/hle.h"
#include "maplethief/gcd.h"       // mt_hle_dispatch_*: GCD (#50)
#include "maplethief/objc/objc.h" // mt_hle_objc_*, mt_hle_Block_*: the ObjC runtime (#47)

namespace {

// One entry per line, grouped by library. The core entries come first; the C test harness
// of the recompiler (tests/recomp_harness.py) builds this file with MT_HLE_CORE_ONLY, which
// leaves out every module that needs the rest of the runtime (guest memory, ObjC, ...).
// clang-format off
constexpr mt_hle_entry k_registry[] = {
    // libgcc_s.1.dylib: the SjLj unwinder (runtime/src/sjlj.c, ADR 0020)
    {"__Unwind_SjLj_Register", mt_sjlj_hle_register, nullptr},
    {"__Unwind_SjLj_Resume", mt_sjlj_hle_resume, nullptr},
    {"__Unwind_SjLj_Unregister", mt_sjlj_hle_unregister, nullptr},
    // libstdc++.6.dylib
    {"__ZSt9terminatev", mt_sjlj_hle_terminate, nullptr},
    // libobjc.A.dylib: the catch helpers of Chartboost's two @catch blocks
    {"_objc_begin_catch", mt_sjlj_hle_objc_begin_catch, nullptr},
    {"_objc_end_catch", mt_sjlj_hle_objc_end_catch, nullptr},
    {"_objc_exception_rethrow", mt_sjlj_hle_objc_exception_rethrow, nullptr},
    // libgcc_s.1.dylib
    {"___divdi3", mt_hle_divdi3, nullptr},
    {"___divsi3", mt_hle_divsi3, nullptr},
    {"___floatdisf", mt_hle_floatdisf, nullptr},
    {"___modsi3", mt_hle_modsi3, nullptr},
    {"___udivdi3", mt_hle_udivdi3, nullptr},
    {"___udivsi3", mt_hle_udivsi3, nullptr},
    {"___umodsi3", mt_hle_umodsi3, nullptr},
    // Process start-up and exit (runtime/src/boot/, #43, ADR 0027): libSystem, UIKit
    {"___cxa_atexit", mt_hle_cxa_atexit, nullptr},
    {"_UIApplicationMain", mt_hle_UIApplicationMain, nullptr},
    {"_exit", mt_hle_exit, nullptr},
#ifndef MT_HLE_CORE_ONLY
    // libobjc.A.dylib: the Objective-C runtime (runtime/src/objc/, ADR 0026, ADR 0029)
    {"_objc_enumerationMutation", mt_hle_objc_enumerationMutation, nullptr},
    {"_objc_getProperty", mt_hle_objc_getProperty, nullptr},
    {"_objc_msgSend", mt_hle_objc_msgSend, nullptr},
    {"_objc_msgSendSuper2", mt_hle_objc_msgSendSuper2, nullptr},
    {"_objc_msgSend_stret", mt_hle_objc_msgSend_stret, nullptr},
    {"_objc_setProperty", mt_hle_objc_setProperty, nullptr},
    {"_objc_sync_enter", mt_hle_objc_sync_enter, nullptr},
    {"_objc_sync_exit", mt_hle_objc_sync_exit, nullptr},
    // libSystem.B.dylib: the blocks runtime (runtime/src/objc/blocks.cpp)
    {"__Block_copy", mt_hle_Block_copy, nullptr},
    {"__Block_object_assign", mt_hle_Block_object_assign, nullptr},
    {"__Block_object_dispose", mt_hle_Block_object_dispose, nullptr},
    {"__Block_release", mt_hle_Block_release, nullptr},
    // libSystem.B.dylib: GCD (runtime/src/gcd/, ADR 0033)
    {"__dispatch_main_q", nullptr, mt_hle_init_dispatch_main_q},
    {"_dispatch_after", mt_hle_dispatch_after, nullptr},
    {"_dispatch_async", mt_hle_dispatch_async, nullptr},
    {"_dispatch_get_global_queue", mt_hle_dispatch_get_global_queue, nullptr},
    {"_dispatch_group_async", mt_hle_dispatch_group_async, nullptr},
    {"_dispatch_group_create", mt_hle_dispatch_group_create, nullptr},
    {"_dispatch_group_wait", mt_hle_dispatch_group_wait, nullptr},
    {"_dispatch_once", mt_hle_dispatch_once, nullptr},
    {"_dispatch_release", mt_hle_dispatch_release, nullptr},
    {"_dispatch_sync", mt_hle_dispatch_sync, nullptr},
    {"_dispatch_time", mt_hle_dispatch_time, nullptr},
    // libSystem.B.dylib: the C library (libc.cpp, libc_sys.cpp, ADR 0028)
    {"___assert_rtn", mt_hle_assert_rtn, nullptr},
    {"___snprintf_chk", mt_hle_snprintf_chk, nullptr},
    {"___stderrp", nullptr, mt_hle_init_stderrp},
    {"___stdinp", nullptr, mt_hle_init_stdinp},
    {"___stdoutp", nullptr, mt_hle_init_stdoutp},
    {"_abort", mt_hle_abort, nullptr},
    {"_atoi", mt_hle_atoi, nullptr},
    {"_atol", mt_hle_atol, nullptr},
    {"_calloc", mt_hle_calloc, nullptr},
    {"_close", mt_hle_close, nullptr},
    {"_floor", mt_hle_floor, nullptr},
    {"_free", mt_hle_free, nullptr},
    {"_gethostbyname", mt_hle_gethostbyname, nullptr},
    {"_gettimeofday", mt_hle_gettimeofday, nullptr},
    {"_if_nametoindex", mt_hle_if_nametoindex, nullptr},
    {"_inet_addr", mt_hle_inet_addr, nullptr},
    {"_inet_ntoa", mt_hle_inet_ntoa, nullptr},
    {"_malloc", mt_hle_malloc, nullptr},
    {"_memcmp", mt_hle_memcmp, nullptr},
    {"_memcpy", mt_hle_memcpy, nullptr},
    {"_memset", mt_hle_memset, nullptr},
    {"_pow", mt_hle_pow, nullptr},
    {"_printf", mt_hle_printf, nullptr},
    {"_puts", mt_hle_puts, nullptr},
    {"_realloc", mt_hle_realloc, nullptr},
    {"_setsockopt", mt_hle_setsockopt, nullptr},
    {"_strcat", mt_hle_strcat, nullptr},
    {"_strcmp", mt_hle_strcmp, nullptr},
    {"_strcpy", mt_hle_strcpy, nullptr},
    {"_strlen", mt_hle_strlen, nullptr},
    {"_sysctl", mt_hle_sysctl, nullptr},
    {"_sysctlbyname", mt_hle_sysctlbyname, nullptr},
    {"_trunc", mt_hle_trunc, nullptr},
    {"_vfprintf", mt_hle_vfprintf, nullptr},
    {"_vsprintf", mt_hle_vsprintf, nullptr},
#endif // MT_HLE_CORE_ONLY
};
// clang-format on

} // namespace

extern "C" const mt_hle_entry *mt_hle_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_registry / sizeof k_registry[0]);
    return k_registry;
}
