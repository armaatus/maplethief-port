// libstdc++.6.dylib, the C++ ABI functions the game calls (#45, ADR 0031): operator new and
// delete (and their [] forms) on the guest heap, and __cxa_pure_virtual. Also the four a throw
// and a typed catch need, which the game never calls (ANALYSIS.md, "Exceptions") but the guest
// program does (#385, ADR 0213): __cxa_allocate_exception, __cxa_throw, __cxa_begin_catch and
// __cxa_end_catch, over the SjLj unwinder's raise and catch bookkeeping. The RTTI side (the
// type_info vtables and catch matching) is rtti.cpp; std::terminate is the SjLj unwinder's
// (sjlj.c, ADR 0020) and __cxa_atexit process start-up's (boot/process.c, ADR 0027).
//
// Plain C++ without exceptions or the C++ runtime, like the rest of the HLE layer (ADR 0019).
#include <cstdint>

#include "hle/hle.h"
#include "hle/libc_internal.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/sjlj.h"

namespace {

using u32 = std::uint32_t;

// GCC 4.2's operator new: a request of 0 bytes gets a unique block; when malloc fails it calls
// the new_handler (the game installs none) and throws std::bad_alloc. The game has no typed
// catch (ANALYSIS.md, "Exceptions"); apart from the catch-alls of Chartboost's two @catch
// blocks nothing would handle it, and the device would end in std::terminate and abort. Here
// it stops the guest at once (MT_TRAP_ABORT, detail = the size asked for).
void new_impl(mt_cpu *cpu, const char *what) {
    mt_args a = mt_args_begin(cpu);
    const u32 size = mt_arg_u32(&a);
    mt_mem *mem = mt_mem_from_cpu(cpu);
    const u32 p = mem != nullptr ? mt_heap_malloc(mem, size != 0u ? size : 1u) : 0u;
    if (p == 0u) {
        mt_libc_log("%s(%u) called from 0x%08X: the guest heap is exhausted "
                    "(std::bad_alloc): aborting",
                    what, static_cast<unsigned>(size), static_cast<unsigned>(cpu->r[MT_LR]));
        mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], size);
    }
    mt_ret_u32(cpu, p);
}

// operator delete is free(): NULL does nothing, and anything that is not a live block is the
// malloc error iOS reports before it aborts (as for free, libc.cpp).
void delete_impl(mt_cpu *cpu, const char *what) {
    mt_args a = mt_args_begin(cpu);
    const u32 ptr = mt_arg_u32(&a);
    if (ptr == 0u) {
        return;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr || mt_heap_free(mem, ptr) != 0) {
        mt_libc_log("malloc: *** error for object 0x%08X: pointer being freed was not "
                    "allocated (%s called from 0x%08X)",
                    static_cast<unsigned>(ptr), what, static_cast<unsigned>(cpu->r[MT_LR]));
        mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], ptr);
    }
}

// A thrown object's guest block: this header, then the object. The unwinder knows an exception
// by the address of its _Unwind_Exception (the landing pad's data[0], what __cxa_begin_catch
// gets), here the header's start; the guest never reads the header itself. Words: the
// type_info and the destructor __cxa_throw was given, then padding to 8-byte alignment.
constexpr u32 k_exception_header = 32u;
constexpr u32 k_header_type = 0u;
constexpr u32 k_header_destructor = 4u;

} // namespace

extern "C" {

// void *operator new(size_t)
void mt_hle_Znwm(mt_cpu *cpu, std::uint32_t) noexcept { new_impl(cpu, "operator new"); }

// void *operator new[](size_t)
void mt_hle_Znam(mt_cpu *cpu, std::uint32_t) noexcept { new_impl(cpu, "operator new[]"); }

// void operator delete(void *)
void mt_hle_ZdlPv(mt_cpu *cpu, std::uint32_t) noexcept { delete_impl(cpu, "operator delete"); }

// void operator delete[](void *)
void mt_hle_ZdaPv(mt_cpu *cpu, std::uint32_t) noexcept { delete_impl(cpu, "operator delete[]"); }

// void __cxa_pure_virtual(void): reached through one of the 31 pure vtable slots (only when
// a constructor or destructor calls a pure virtual function). libstdc++ writes this line to
// stderr and calls std::terminate; the trap's crash report gives the caller and the guest
// backtrace.
void mt_hle_cxa_pure_virtual(mt_cpu *cpu, std::uint32_t) noexcept {
    static constexpr char k_message[] = "pure virtual method called\n";
    mt_libc_write(2, k_message, sizeof k_message - 1u);
    mt_trap(cpu, MT_TRAP_PURE_VIRT, cpu->r[MT_LR], 0u);
}

// void *__cxa_allocate_exception(size_t size): the object's storage, zeroed, after its header.
// The block is never freed: the unwinder forgets a handled exception, its guest storage stays
// (a few bytes per throw; only the guest program throws).
void mt_hle_cxa_allocate_exception(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 size = mt_arg_u32(&a);
    mt_mem *mem = mt_mem_from_cpu(cpu);
    const u32 block = mem != nullptr && size <= 0x100000u
                          ? mt_heap_malloc(mem, k_exception_header + size)
                          : 0u;
    if (block == 0u) {
        mt_libc_log("__cxa_allocate_exception(%u) called from 0x%08X: no guest memory "
                    "(std::terminate)",
                    static_cast<unsigned>(size), static_cast<unsigned>(cpu->r[MT_LR]));
        mt_trap(cpu, MT_TRAP_TERMINATE, cpu->r[MT_LR], size);
    }
    for (u32 i = 0; i < k_exception_header + size; i += 4u) {
        mt_st32(cpu, block + i, 0u, cpu->r[MT_LR]);
    }
    mt_ret_u32(cpu, block + k_exception_header);
}

// void __cxa_throw(void *object, std::type_info *type, void (*destructor)(void *)): raises the
// object; the SjLj personality matches each catch clause against `type` (rtti.cpp). Never
// returns: the unwinder longjmps to the handler's landing pad, or traps when none catches.
void mt_hle_cxa_throw(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 object = mt_arg_u32(&a);
    const u32 type = mt_arg_u32(&a);
    const u32 destructor = mt_arg_u32(&a);
    const u32 header = object - k_exception_header;
    mt_st32(cpu, header + k_header_type, type, cpu->r[MT_LR]);
    mt_st32(cpu, header + k_header_destructor, destructor, cpu->r[MT_LR]);
    mt_sjlj_throw(cpu, header, type, object);
}

// void *__cxa_begin_catch(void *exception): the object the catch clause receives (moved to the
// base subobject the clause named, as matching found it).
void mt_hle_cxa_begin_catch(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_u32(cpu, mt_sjlj_begin_catch(cpu, mt_arg_u32(&a)));
}

// void __cxa_end_catch(void): the innermost catch clause ends; the exception is handled when
// the last one does (unless it was rethrown). Its object's destructor is not run.
void mt_hle_cxa_end_catch(mt_cpu *cpu, std::uint32_t) noexcept { mt_sjlj_end_catch(cpu); }

} // extern "C"
