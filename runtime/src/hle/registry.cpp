// Every HLE implementation the runtime has, by import symbol (ADR 0019). mt_imports_link
// fills the import table from this list; an import that is not here gets the default
// unimplemented-import handler. M4 modules add their functions (and data-object init hooks)
// here, grouped by library.
#include "maplethief/hle/imports.h"

#include <cstdint>

#include "hle/hle.h"

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
    // libgcc_s.1.dylib
    {"___divdi3", mt_hle_divdi3, nullptr},
    {"___divsi3", mt_hle_divsi3, nullptr},
    {"___floatdisf", mt_hle_floatdisf, nullptr},
    {"___modsi3", mt_hle_modsi3, nullptr},
    {"___udivdi3", mt_hle_udivdi3, nullptr},
    {"___udivsi3", mt_hle_udivsi3, nullptr},
    {"___umodsi3", mt_hle_umodsi3, nullptr},
};
// clang-format on

} // namespace

extern "C" const mt_hle_entry *mt_hle_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_registry / sizeof k_registry[0]);
    return k_registry;
}
