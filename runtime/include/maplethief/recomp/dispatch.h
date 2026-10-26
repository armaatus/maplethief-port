/* Indirect dispatch: guest code address -> host function (ADR 0014, #35).
 *
 * Generated code reaches this only through mt_dispatch (calls.h: MT_CALL_INDIRECT,
 * MT_JUMP_INDIRECT). The data is generated into work/gen/dispatch.c by recomp/dispatch.py;
 * the code that reads it is runtime/src/recomp/dispatch.c, which the runtime and the test
 * runtime link with the generated code.
 *
 *   mt_fn_table   every guest function by its function pointer, sorted: Thumb functions with
 *                 bit 0 set (f_000225E5), ARM code with it clear (the entry stub
 *                 f_000027E0). The Thumb bit is part of the key, so a Thumb function reached
 *                 in ARM state, or an ARM one in Thumb state, is not found.
 *   mt_fn_idx     a bucket index over the table's address range: bucket b covers the
 *                 2^shift bytes from base + (b << shift), and first[b] .. first[b + 1] are
 *                 the table entries whose address (bit 0 cleared) lies in it. A lookup is
 *                 one index read and a scan of at most a few entries (ADR 0014 addendum).
 *   mt_import_thunks the imports' guest code addresses (their stubs, and
 *                 MT_IMPORT_THUNK_BASE + 4 * id for the stub-less ones), generated into
 *                 work/gen/imports.c (#36, maplethief/recomp/imports.h). A target that is no
 *                 function is looked up there (binary search), so a pointer to an import is
 *                 callable too.
 *
 * On a miss mt_dispatch stops the guest with MT_TRAP_NO_FUNCTION (pc = the guest address of
 * the call or jump, detail = the target). The runtime's mt_trap reports it with
 * mt_dispatch_describe. */
#ifndef MAPLETHIEF_RECOMP_DISPATCH_H
#define MAPLETHIEF_RECOMP_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/calls.h"
#include "maplethief/recomp/compiler.h"
#include "maplethief/recomp/cpu.h"
#include "maplethief/recomp/imports.h"

MT_EXTERN_C_BEGIN

typedef struct mt_fn_index {
    uint32_t base;         /* lowest table address with bit 0 cleared */
    uint32_t shift;        /* log2 of the bucket size in bytes */
    uint32_t buckets;      /* number of buckets */
    uint32_t end;          /* one past the last guest code address of the table's functions */
    const uint16_t *first; /* buckets + 1 table indices */
} mt_fn_index;

/* Generated (work/gen/dispatch.c). */
extern const mt_fn_entry mt_fn_table[];
extern const uint32_t mt_fn_count;
extern const mt_fn_index mt_fn_idx;

/* The host function whose guest function pointer is exactly `target` (a function, or an
 * import's thunk), or NULL. */
mt_guest_fn mt_fn_lookup(uint32_t target) MT_NOEXCEPT;

/* The table entry with the highest address (bit 0 cleared) at or below `addr` (bit 0
 * cleared): the function a code address belongs to, since functions tile __text and the
 * stubs. NULL if `addr` is below the first function or at or above mt_fn_idx.end. */
const mt_fn_entry *mt_fn_containing(uint32_t addr) MT_NOEXCEPT;

/* Up to `max` return addresses from the guest's r7 frame chain (Apple's ARM ABI: r7 points
 * at the saved r7, with the saved lr above it), innermost first. Reads guest memory only
 * inside cpu->mem_size and stops when the chain does not move up the stack. */
uint32_t mt_guest_backtrace(const mt_cpu *cpu, uint32_t *out, uint32_t max) MT_NOEXCEPT;

/* One line (no newline) explaining an indirect call or jump at guest `site` to `target`
 * that has no function: the site and the function it is in, lr, why the target is not a
 * function (null, wrong instruction set, misaligned ARM address, inside a function) and the
 * guest backtrace. Writes at most `size` bytes including the terminating NUL; returns the
 * length of the text (truncated if it did not fit). */
size_t mt_dispatch_describe(const mt_cpu *cpu, uint32_t target, uint32_t site, char *buf,
                            size_t size) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_DISPATCH_H */
