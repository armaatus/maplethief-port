// RTTI over guest memory (#45, ADR 0031): the type_info vtable imports and the host
// implementations of the type_info virtual functions that catch matching uses. See
// maplethief/hle/cxxabi.h.
//
// The semantics are GCC 4.2's libsupc++ (tinfo.cc, tinfo2.cc): type_info::operator== compares
// names, __class_type_info::__do_catch is equality or an upcast of the thrown object, and
// __do_upcast finds the unique public base subobject of the catch type. GCC walks the bases
// with the __vmi flags as shortcuts; this walks every path and compares where each one ends,
// which gives the same answers for every hierarchy (the flags only say which paths can be
// skipped) and needs no flags to be trusted.
//
// Plain C++ without exceptions, allocation or function-local statics (ADR 0019): the
// recompiler's C test harness compiles it into its programs (tests/test_recomp_sjlj.py).
#include "maplethief/hle/cxxabi.h"

#include <cstdint>

namespace {

using u32 = std::uint32_t;

// Where the three vtables are (0: not placed). The guest is single-threaded (ADR 0014); a new
// process places its imports again.
struct Vtables {
    u32 cls = 0;
    u32 si = 0;
    u32 vmi = 0;
};
Vtables g_vt;

constexpr int k_max_depth = 64;    // base-class nesting: more means a cycle in bad data
constexpr u32 k_max_bases = 4096u; // bases of one __vmi_class_type_info
constexpr u32 k_max_name = 4096u;  // bytes of a mangled name

// The guest PC a fault in here reports: the caller of the import (as in sjlj.c).
u32 site(const mt_cpu *cpu) { return cpu->r[MT_LR]; }
u32 ld8(mt_cpu *cpu, u32 addr) { return mt_ld8(cpu, addr, site(cpu)); }
u32 ld32(mt_cpu *cpu, u32 addr) { return mt_ld32(cpu, addr, site(cpu)); }

bool names_equal(mt_cpu *cpu, u32 a, u32 b) {
    if (a == b) {
        return true;
    }
    if (a == 0u || b == 0u || ld8(cpu, a) == '*' || ld8(cpu, b) == '*') {
        return false;
    }
    for (u32 i = 0; i < k_max_name; i++) {
        const u32 x = ld8(cpu, a + i), y = ld8(cpu, b + i);
        if (x != y) {
            return false;
        }
        if (x == 0u) {
            return true;
        }
    }
    return false;
}

void init_vtable(mt_cpu *cpu, u32 id, u32 addr, u32 Vtables::*which) {
    mt_st32(cpu, addr, 0u, 0u);      // offset-to-top
    mt_st32(cpu, addr + 4u, 0u, 0u); // the vtable's own type_info: nothing reads it
    for (u32 i = 0; i < MT_CXX_VT_SLOTS; i++) {
        mt_st32(cpu, addr + MT_CXX_VT_ADDRESS_POINT + 4u * i, MT_IMPORT_THUNK_BASE + 4u * id, 0u);
    }
    g_vt.*which = addr;
}

// Where a path from the thrown object ends: the virtual base it last went through (0: none)
// and the offset from there. Two paths reach the same subobject exactly when these are equal,
// whether or not the object's address is known.
struct Place {
    u32 vbase;
    u32 offset;
};

struct Search {
    u32 to;
    bool found = false;
    bool ambiguous = false;
    bool is_public = false;
    Place place{};
    bool has_ptr = false; // ptr is the subobject's address
    u32 ptr = 0;
};

void walk(mt_cpu *cpu, u32 type, Place place, bool has_ptr, u32 ptr, bool is_public, int depth,
          Search &s) {
    if (s.ambiguous || depth > k_max_depth) {
        return;
    }
    if (mt_cxx_same_type(cpu, type, s.to)) {
        if (!s.found) {
            s.found = true;
            s.place = place;
            s.is_public = is_public;
            s.has_ptr = has_ptr;
            s.ptr = ptr;
        } else if (s.place.vbase != place.vbase || s.place.offset != place.offset) {
            s.ambiguous = true; // two different subobjects of the catch type
        } else {
            s.is_public = s.is_public || is_public; // one subobject, reached two ways
        }
        return;
    }
    switch (mt_cxx_type_kind(cpu, type)) {
    case MT_CXX_TYPE_SI:
        walk(cpu, ld32(cpu, type + MT_CXX_SI_BASE), place, has_ptr, ptr, is_public, depth + 1, s);
        break;
    case MT_CXX_TYPE_VMI: {
        u32 n = ld32(cpu, type + MT_CXX_VMI_BASE_COUNT);
        if (n > k_max_bases) {
            n = k_max_bases;
        }
        for (u32 i = 0; i < n && !s.ambiguous; i++) {
            const u32 entry = type + MT_CXX_VMI_BASES + 8u * i;
            const u32 base = ld32(cpu, entry), flags = ld32(cpu, entry + 4u);
            // The offset is the signed upper 24 bits (an arithmetic shift, spelled portably).
            const u32 offset = (flags >> 8) | ((flags & 0x80000000u) != 0u ? 0xFF000000u : 0u);
            Place next{place.vbase, place.offset + offset};
            u32 next_ptr = ptr + offset;
            if ((flags & MT_CXX_BASE_VIRTUAL) != 0u) {
                // `offset` is where the vtable keeps the virtual base's offset (negative).
                next = Place{base, 0u};
                next_ptr = has_ptr ? ptr + ld32(cpu, ld32(cpu, ptr) + offset) : 0u;
            }
            walk(cpu, base, next, has_ptr, next_ptr,
                 is_public && (flags & MT_CXX_BASE_PUBLIC) != 0u, depth + 1, s);
        }
        break;
    }
    default:
        break; // __class_type_info (no bases) or not a class
    }
}

} // namespace

extern "C" void mt_cxx_set_type_info_vtables(u32 class_vt, u32 si_vt, u32 vmi_vt) noexcept {
    g_vt.cls = class_vt;
    g_vt.si = si_vt;
    g_vt.vmi = vmi_vt;
}

extern "C" u32 mt_cxx_type_kind(mt_cpu *cpu, u32 type) noexcept {
    if (type == 0u) {
        return MT_CXX_TYPE_OTHER;
    }
    const u32 vptr = ld32(cpu, type + MT_CXX_TI_VPTR);
    if (g_vt.cls != 0u && vptr == g_vt.cls + MT_CXX_VT_ADDRESS_POINT) {
        return MT_CXX_TYPE_CLASS;
    }
    if (g_vt.si != 0u && vptr == g_vt.si + MT_CXX_VT_ADDRESS_POINT) {
        return MT_CXX_TYPE_SI;
    }
    if (g_vt.vmi != 0u && vptr == g_vt.vmi + MT_CXX_VT_ADDRESS_POINT) {
        return MT_CXX_TYPE_VMI;
    }
    return MT_CXX_TYPE_OTHER;
}

extern "C" void mt_cxx_init_class_type_info_vtable(mt_cpu *cpu, u32 id, u32 addr) noexcept {
    init_vtable(cpu, id, addr, &Vtables::cls);
}

extern "C" void mt_cxx_init_si_class_type_info_vtable(mt_cpu *cpu, u32 id, u32 addr) noexcept {
    init_vtable(cpu, id, addr, &Vtables::si);
}

extern "C" void mt_cxx_init_vmi_class_type_info_vtable(mt_cpu *cpu, u32 id, u32 addr) noexcept {
    init_vtable(cpu, id, addr, &Vtables::vmi);
}

extern "C" int mt_cxx_same_type(mt_cpu *cpu, u32 a, u32 b) noexcept {
    if (a == b) {
        return 1;
    }
    if (mt_cxx_type_kind(cpu, a) == MT_CXX_TYPE_OTHER ||
        mt_cxx_type_kind(cpu, b) == MT_CXX_TYPE_OTHER) {
        return 0;
    }
    return names_equal(cpu, ld32(cpu, a + MT_CXX_TI_NAME), ld32(cpu, b + MT_CXX_TI_NAME)) ? 1 : 0;
}

extern "C" int mt_cxx_upcast(mt_cpu *cpu, u32 from, u32 to, u32 *object) noexcept {
    if (mt_cxx_type_kind(cpu, from) == MT_CXX_TYPE_OTHER ||
        mt_cxx_type_kind(cpu, to) == MT_CXX_TYPE_OTHER) {
        return 0;
    }
    Search s;
    s.to = to;
    walk(cpu, from, Place{0u, 0u}, object != nullptr, object != nullptr ? *object : 0u, true, 0, s);
    if (!s.found || s.ambiguous || !s.is_public) {
        return 0;
    }
    if (object != nullptr && s.has_ptr) {
        *object = s.ptr;
    }
    return 1;
}

extern "C" int mt_cxx_can_catch(mt_cpu *cpu, u32 catch_type, u32 thrown_type,
                                u32 *object) noexcept {
    if (thrown_type == 0u || catch_type == 0u) {
        return 0; // a catch-all never gets here (sjlj.c); a thrown type 0 only matches it
    }
    if (mt_cxx_same_type(cpu, catch_type, thrown_type)) {
        return 1;
    }
    return mt_cxx_upcast(cpu, thrown_type, catch_type, object);
}
