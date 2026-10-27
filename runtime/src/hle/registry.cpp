// Every HLE implementation the runtime has, by import symbol (ADR 0019). mt_imports_link
// fills the import table from this list; an import that is not here gets the default
// unimplemented-import handler. M4 modules add their functions (and data-object init hooks)
// here, grouped by library.
#include "maplethief/hle/imports.h"

#include <cstdint>

#include "hle/hle.h"
#include "maplethief/objc/objc.h" // mt_hle_objc_*, mt_hle_Block_*: the ObjC runtime (#47)

namespace {

// One entry per line, grouped by library.
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
};
// clang-format on

} // namespace

extern "C" const mt_hle_entry *mt_hle_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_registry / sizeof k_registry[0]);
    return k_registry;
}
