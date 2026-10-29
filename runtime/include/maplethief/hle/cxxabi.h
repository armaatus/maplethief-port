/* The C++ ABI part of the HLE layer (#45, ADR 0031): RTTI over guest memory.
 *
 * The game's C++ code (LLVM-GCC, libstdc++.6.dylib, GCC 4.2's libsupc++) keeps its type_info
 * objects in the image. Each one starts with a vptr that dyld binds to one of three imported
 * vtables plus 8 (ADR 0019): __cxxabiv1::__class_type_info (a class without bases),
 * __si_class_type_info (one public, non-virtual base at offset 0) or __vmi_class_type_info
 * (anything else). The runtime places those vtables as data imports; their init hooks
 * (runtime/src/hle/rtti.cpp) fill them in and tell the runtime where each one is, so the kind
 * of a type_info is known from its vptr. The virtual functions catch matching needs
 * (__do_catch, __do_upcast) are host code over guest memory, picked by that kind:
 * mt_cxx_can_catch is the SjLj personality's type matcher (maplethief/sjlj.h), which the boot
 * installs.
 *
 * No game code reaches a type_info: no code site loads one or reads a vtable's type_info or
 * offset-to-top slot, and __dynamic_cast is not imported (ANALYSIS.md, "RTTI and vtables"). The
 * game has no typed catch either, so in normal play none of this runs; it is what makes an
 * exception thrown by HLE code find the right handler.
 *
 * operator new/delete and __cxa_pure_virtual are registry entries (runtime/src/hle/cxxabi.cpp).
 *
 * C17 that also compiles as C++20; the implementation is C++ without the C++ runtime, so the
 * recompiler's C test harness links it (tests/test_recomp_sjlj.py). */
#ifndef MAPLETHIEF_HLE_CXXABI_H
#define MAPLETHIEF_HLE_CXXABI_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- guest layout (Itanium C++ ABI 2.9.5, 32-bit) ----------------------------------------- */

/* std::type_info: vptr, then the mangled name (a guest C string, "4Base"). */
#define MT_CXX_TI_VPTR 0x00u
#define MT_CXX_TI_NAME 0x04u
/* __si_class_type_info: the base's type_info. */
#define MT_CXX_SI_BASE 0x08u
/* __vmi_class_type_info: flags, base count, then {base type_info, offset_flags} per base. */
#define MT_CXX_VMI_FLAGS 0x08u
#define MT_CXX_VMI_BASE_COUNT 0x0Cu
#define MT_CXX_VMI_BASES 0x10u
#define MT_CXX_BASE_VIRTUAL 0x1u /* offset_flags: the base is virtual ... */
#define MT_CXX_BASE_PUBLIC 0x2u  /* ... public; the signed offset is offset_flags >> 8 */

/* A type_info vtable: offset-to-top, the vtable's own type_info, then the virtual functions
 * (GCC 4.2: the two destructors, __is_pointer_p, __is_function_p, __do_catch, the two
 * __do_upcast, __do_dyncast, __do_find_public_src). A type_info's vptr is the vtable + 8.
 * The imports are 64-byte objects (recomp/imports.py). */
#define MT_CXX_VT_ADDRESS_POINT 0x08u
#define MT_CXX_VT_SLOTS 9u

enum mt_cxx_type_kind {
    MT_CXX_TYPE_OTHER = 0, /* not a class type_info this runtime knows (or not one at all) */
    MT_CXX_TYPE_CLASS = 1, /* __class_type_info */
    MT_CXX_TYPE_SI = 2,    /* __si_class_type_info */
    MT_CXX_TYPE_VMI = 3,   /* __vmi_class_type_info */
};

/* --- the vtables --------------------------------------------------------------------------- */

/* Tells the runtime where the three vtables are (guest addresses; 0: not placed). The init
 * hooks of the vtable imports call this for their own vtable; tests call it directly. */
void mt_cxx_set_type_info_vtables(uint32_t class_vt, uint32_t si_vt, uint32_t vmi_vt) MT_NOEXCEPT;

/* The kind of the type_info at `type`, from its vptr (MT_CXX_TYPE_OTHER for 0). */
uint32_t mt_cxx_type_kind(mt_cpu *cpu, uint32_t type) MT_NOEXCEPT;

/* Init hooks of the data imports __ZTVN10__cxxabiv117__class_type_infoE,
 * __ZTVN10__cxxabiv120__si_class_type_infoE and __ZTVN10__cxxabiv121__vmi_class_type_infoE
 * (mt_data_init_fn, maplethief/hle/imports.h): offset-to-top 0, no type_info of their own,
 * and each virtual slot the vtable import's thunk address (MT_IMPORT_THUNK_BASE + 4 * id),
 * which no thunk has: a guest call through it stops with MT_TRAP_NO_FUNCTION naming that
 * address. The guest never calls them; the host runs the virtual functions. */
void mt_cxx_init_class_type_info_vtable(mt_cpu *cpu, uint32_t id, uint32_t addr) MT_NOEXCEPT;
void mt_cxx_init_si_class_type_info_vtable(mt_cpu *cpu, uint32_t id, uint32_t addr) MT_NOEXCEPT;
void mt_cxx_init_vmi_class_type_info_vtable(mt_cpu *cpu, uint32_t id, uint32_t addr) MT_NOEXCEPT;

/* --- the virtual functions, on the host ----------------------------------------------------- */

/* type_info::operator== of GCC 4.2: the same object, the same name pointer, or equal names
 * (a name starting with '*' is local to its object file and only equal by pointer). Two
 * addresses that are not both class type_infos are only equal as addresses. */
int mt_cxx_same_type(mt_cpu *cpu, uint32_t a, uint32_t b) MT_NOEXCEPT;

/* __class_type_info::__do_upcast: whether an object of class `from` (whose address is
 * *object) has a unique, publicly reachable base subobject of class `to`. If so, *object
 * becomes that subobject's address. Walks __si_ and __vmi_ bases (offsets, virtual bases
 * through the object's vtable, non-public bases); two different subobjects of `to` are
 * ambiguous and do not match. `object` may be NULL when the address does not matter (then no
 * virtual base can be reached: their offsets are in the object). */
int mt_cxx_upcast(mt_cpu *cpu, uint32_t from, uint32_t to, uint32_t *object) MT_NOEXCEPT;

/* The personality's type matcher (mt_sjlj_can_catch_fn, maplethief/sjlj.h): __do_catch of
 * the catch clause's type for an exception of `thrown_type`. Equal types match; a class type
 * also matches a thrown class that has it as a unique public base, and *object (if not NULL)
 * is adjusted to that base. Anything that is not a class type_info (an ObjC class, a test's
 * plain address) matches only itself. thrown_type 0 never matches. */
int mt_cxx_can_catch(mt_cpu *cpu, uint32_t catch_type, uint32_t thrown_type,
                     uint32_t *object) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_HLE_CXXABI_H */
