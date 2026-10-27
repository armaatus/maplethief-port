// The blocks ABI (ADR 0029): libclosure's runtime as iOS 4 had it, over guest memory, and the
// block classes. Game code uses blocks in GameCenterManager (GameKit is emulated offline,
// #52) and Chartboost.
//
// Guest layouts (32-bit, clang's block ABI):
//
//     block       { isa, flags, reserved, invoke, descriptor } then the captured variables
//     descriptor  { reserved, size, copy_helper, dispose_helper }   helpers only if
//                                                                   BLOCK_HAS_COPY_DISPOSE
//     byref       { isa, forwarding, flags, size, keep, destroy }   keep/destroy only if
//                                                                   BLOCK_HAS_COPY_DISPOSE
//                 then the __block variable
//
// A block literal starts on the stack (isa _NSConcreteStackBlock) or, without captures, in
// __DATA (_NSConcreteGlobalBlock). _Block_copy moves a stack block to the heap (isa
// _NSConcreteMallocBlock, BLOCK_NEEDS_FREE, reference count in the low flag bits), and the
// copy helper the compiler wrote copies the captures with _Block_object_assign: objects are
// retained, blocks copied, __block variables (byrefs) moved to the heap once and shared. The
// dispose helper undoes it with _Block_object_dispose. The isa objects are host classes, so
// [block copy], -retain, -release and -autorelease work too.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "objc/objc_internal.h"

using namespace mt_objc_impl;

namespace {

// Block and byref flags (libclosure Block_private.h, 2010).
constexpr u32 k_refcount_mask = 0xFFFFu;
constexpr u32 k_needs_free = 1u << 24;
constexpr u32 k_has_copy_dispose = 1u << 25;
constexpr u32 k_is_gc = 1u << 27;
constexpr u32 k_is_global = 1u << 28;

// _Block_object_assign / _Block_object_dispose field flags.
constexpr u32 k_field_is_object = 3;
constexpr u32 k_field_is_block = 7;
constexpr u32 k_field_is_byref = 8;
constexpr u32 k_field_is_weak = 16;
constexpr u32 k_byref_caller = 128;

constexpr u32 k_byref_header = 16; // isa, forwarding, flags, size

u32 sel1(mt_objc *rt, const char *name) {
    const long id = sel_id_of(rt, name);
    return id >= 0 ? static_cast<u32>(id) + 1 : 0;
}

void complain(mt_objc *rt, const char *what, u32 addr, u32 caller) {
    char line[200];
    std::snprintf(line, sizeof line, "objc: %s 0x%08X (called from 0x%08X)", what, addr, caller);
    if (rt != nullptr) {
        log_line(rt, line);
    } else {
        std::fputs(line, stderr);
        std::fputc('\n', stderr);
    }
}

u32 guest_alloc(mt_objc *rt, u32 size) {
    if (rt == nullptr || rt->alloc == nullptr || size == 0) {
        return 0;
    }
    const u32 at = rt->alloc(rt->alloc_user, size, 16);
    return at != 0 && static_cast<std::uint64_t>(at) + size <= rt->cpu->mem_size ? at : 0;
}

void guest_free(mt_objc *rt, u32 addr, u32 caller) {
    if (rt->free_fn != nullptr && rt->free_fn(rt->alloc_user, addr) != 0) {
        complain(rt, "freeing a block or __block variable that is no heap block:", addr, caller);
    }
}

// Adds one to a reference count in the low flag bits, latching at the maximum.
u32 incr(u32 flags) { return (flags & k_refcount_mask) == k_refcount_mask ? flags : flags + 1; }

u32 malloc_block_class(mt_objc *rt) {
    if (rt->malloc_block_class == 0) {
        rt->malloc_block_class = mt_objc_class_named(rt, "__NSMallocBlock__");
    }
    return rt->malloc_block_class;
}

u32 block_copy(mt_cpu *cpu, mt_objc *rt, u32 block, u32 caller) {
    if (block == 0) {
        return 0;
    }
    const u32 flags = mt_ld32(cpu, block + 4, caller);
    if ((flags & k_needs_free) != 0) {
        mt_st32(cpu, block + 4, incr(flags), caller);
        return block;
    }
    if ((flags & (k_is_global | k_is_gc)) != 0) {
        return block;
    }
    const u32 descriptor = mt_ld32(cpu, block + 16, caller);
    const u32 size = mt_ld32(cpu, descriptor + 4, caller);
    const u32 copy = size >= 20 && size < 0x100000 ? guest_alloc(rt, size) : 0;
    if (copy == 0) {
        complain(rt, "cannot copy block", block, caller);
        return 0;
    }
    std::memmove(cpu->mem + copy, cpu->mem + block, size);
    mt_st32(cpu, copy, malloc_block_class(rt), caller);
    mt_st32(cpu, copy + 4, (flags & ~k_refcount_mask) | k_needs_free | 1u, caller);
    if ((flags & k_has_copy_dispose) != 0) {
        const u32 args[2] = {copy, block};
        call_guest(cpu, rt, mt_ld32(cpu, descriptor + 8, caller), args, 2);
    }
    return copy;
}

void block_release(mt_cpu *cpu, mt_objc *rt, u32 block, u32 caller) {
    if (block == 0) {
        return;
    }
    const u32 flags = mt_ld32(cpu, block + 4, caller);
    if ((flags & k_needs_free) == 0) {
        return; // a stack or global block: nothing to release
    }
    const u32 count = flags & k_refcount_mask;
    if (count == 0) {
        complain(rt, "block over-released:", block, caller);
        return;
    }
    if (count == k_refcount_mask) {
        return; // latched: never freed
    }
    mt_st32(cpu, block + 4, flags - 1, caller);
    if (count != 1) {
        return;
    }
    if ((flags & k_has_copy_dispose) != 0) {
        const u32 descriptor = mt_ld32(cpu, block + 16, caller);
        call_guest(cpu, rt, mt_ld32(cpu, descriptor + 12, caller), &block, 1);
    }
    guest_free(rt, block, caller);
}

// _Block_byref_assign_copy: the heap copy of a __block variable, made on its first copy.
u32 byref_copy(mt_cpu *cpu, mt_objc *rt, u32 arg, u32 caller) {
    const u32 src = mt_ld32(cpu, arg + 4, caller); // forwarding
    const u32 flags = mt_ld32(cpu, src + 8, caller);
    if ((flags & k_is_gc) != 0) {
        return src;
    }
    if ((flags & k_refcount_mask) == 0) { // still on the stack
        const u32 size = mt_ld32(cpu, src + 12, caller);
        const u32 copy = size >= k_byref_header && size < 0x100000 ? guest_alloc(rt, size) : 0;
        if (copy == 0) {
            complain(rt, "cannot copy __block variable", src, caller);
            return src;
        }
        // One reference for the stack frame and one for the copying block.
        mt_st32(cpu, copy + 8, flags | k_needs_free | 2u, caller);
        mt_st32(cpu, copy + 4, copy, caller);
        mt_st32(cpu, src + 4, copy, caller);
        mt_st32(cpu, copy + 12, size, caller);
        if ((flags & k_has_copy_dispose) != 0) {
            const u32 keep = mt_ld32(cpu, src + 16, caller);
            mt_st32(cpu, copy + 16, keep, caller);
            mt_st32(cpu, copy + 20, mt_ld32(cpu, src + 20, caller), caller);
            const u32 args[2] = {copy, src};
            call_guest(cpu, rt, keep, args, 2);
        } else {
            std::memmove(cpu->mem + copy + k_byref_header, cpu->mem + src + k_byref_header,
                         size - k_byref_header);
        }
        return copy;
    }
    if ((flags & k_needs_free) != 0) {
        mt_st32(cpu, src + 8, incr(flags), caller);
    }
    return src;
}

void byref_release(mt_cpu *cpu, mt_objc *rt, u32 arg, u32 caller) {
    const u32 shared = mt_ld32(cpu, arg + 4, caller); // forwarding
    const u32 flags = mt_ld32(cpu, shared + 8, caller);
    if ((flags & k_needs_free) == 0) {
        return;
    }
    const u32 count = flags & k_refcount_mask;
    if (count == 0) {
        complain(rt, "__block variable over-released:", shared, caller);
        return;
    }
    if (count == k_refcount_mask) {
        return;
    }
    mt_st32(cpu, shared + 8, flags - 1, caller);
    if (count != 1) {
        return;
    }
    if ((flags & k_has_copy_dispose) != 0) {
        call_guest(cpu, rt, mt_ld32(cpu, shared + 20, caller), &shared, 1);
    }
    guest_free(rt, shared, caller);
}

// --- the block classes -----------------------------------------------------------------------

// -copy, -copyWithZone:
void block_m_copy(mt_cpu *cpu) {
    cpu->r[0] = block_copy(cpu, mt_objc_current(), cpu->r[0], cpu->r[MT_LR]);
}

// -retain: one more reference of a heap block; a stack or global block returns itself (as
// libSystem's do: retaining a stack block does not keep it alive, copying does).
void block_m_retain(mt_cpu *cpu) {
    const u32 flags = mt_ld32(cpu, cpu->r[0] + 4, cpu->r[MT_LR]);
    if ((flags & k_needs_free) != 0) {
        mt_st32(cpu, cpu->r[0] + 4, incr(flags), cpu->r[MT_LR]);
    }
}

void block_m_release(mt_cpu *cpu) {
    block_release(cpu, mt_objc_current(), cpu->r[0], cpu->r[MT_LR]);
}

void block_m_retain_count(mt_cpu *cpu) {
    const u32 flags = mt_ld32(cpu, cpu->r[0] + 4, cpu->r[MT_LR]);
    cpu->r[0] = (flags & k_needs_free) != 0 ? flags & k_refcount_mask : 1u;
}

const mt_objc_host_method k_block_methods[] = {
    {"copy", block_m_copy},       {"copyWithZone:", block_m_copy},       {"retain", block_m_retain},
    {"release", block_m_release}, {"retainCount", block_m_retain_count},
};

} // namespace

namespace mt_objc_impl {

const mt_objc_host_class k_nsblock = {
    "NSBlock", "NSObject", 20, k_block_methods, sizeof k_block_methods / sizeof k_block_methods[0],
    nullptr,   0,
};
const mt_objc_host_class k_stack_block = {
    "__NSStackBlock__", "NSBlock", 20, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_malloc_block = {
    "__NSMallocBlock__", "NSBlock", 20, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_global_block = {
    "__NSGlobalBlock__", "NSBlock", 20, nullptr, 0, nullptr, 0};

} // namespace mt_objc_impl

// --- the blocks runtime ---------------------------------------------------------------------

extern "C" u32 mt_block_copy(mt_cpu *cpu, u32 block) noexcept {
    return block_copy(cpu, mt_objc_current(), block, cpu->r[MT_LR]);
}

extern "C" void mt_block_release(mt_cpu *cpu, u32 block) noexcept {
    block_release(cpu, mt_objc_current(), block, cpu->r[MT_LR]);
}

extern "C" u32 mt_block_invoke(mt_cpu *cpu, u32 block, const u32 *args, u32 argc) noexcept {
    if (block == 0) {
        return 0;
    }
    u32 words[4] = {block, 0, 0, 0};
    for (u32 i = 0; i < argc && i < 3; i++) {
        words[i + 1] = args[i];
    }
    const u32 invoke = mt_ld32(cpu, block + 12, cpu->r[MT_LR]);
    return call_guest(cpu, mt_objc_current(), invoke, words, argc < 3 ? argc + 1 : 4);
}

extern "C" void mt_hle_Block_copy(mt_cpu *cpu, u32) noexcept {
    cpu->r[0] = block_copy(cpu, mt_objc_current(), cpu->r[0], cpu->r[MT_LR]);
}

extern "C" void mt_hle_Block_release(mt_cpu *cpu, u32) noexcept {
    block_release(cpu, mt_objc_current(), cpu->r[0], cpu->r[MT_LR]);
}

extern "C" void mt_hle_Block_object_assign(mt_cpu *cpu, u32) noexcept {
    const u32 dest = cpu->r[0], object = cpu->r[1], flags = cpu->r[2];
    const u32 caller = cpu->r[MT_LR];
    mt_objc *rt = mt_objc_current();
    u32 value = object;
    if ((flags & k_byref_caller) != 0) {
        // A byref's keep helper copying a captured object or block: a plain assignment.
    } else if ((flags & k_field_is_byref) != 0) {
        value = object != 0 ? byref_copy(cpu, rt, object, caller) : 0;
    } else if ((flags & k_field_is_block) == k_field_is_block) {
        value = block_copy(cpu, rt, object, caller);
    } else if ((flags & k_field_is_object) == k_field_is_object && rt != nullptr) {
        host_send(cpu, rt, object, -1, sel1(rt, "retain"), nullptr, 0);
    }
    mt_st32(cpu, dest, value, caller);
}

extern "C" void mt_hle_Block_object_dispose(mt_cpu *cpu, u32) noexcept {
    const u32 object = cpu->r[0], flags = cpu->r[1];
    const u32 caller = cpu->r[MT_LR];
    mt_objc *rt = mt_objc_current();
    if (object == 0 || (flags & k_byref_caller) != 0) {
        return;
    }
    if ((flags & k_field_is_byref) != 0) {
        byref_release(cpu, rt, object, caller);
    } else if ((flags & k_field_is_block) == k_field_is_block) {
        block_release(cpu, rt, object, caller);
    } else if ((flags & (k_field_is_weak | k_field_is_object)) == k_field_is_object &&
               rt != nullptr) {
        host_send(cpu, rt, object, -1, sel1(rt, "release"), nullptr, 0);
    }
}
