/* The import tables of the generated work/gen/imports.c (ADR 0014, ADR 0019).
 *
 * Every import of the guest binary has an id, its index in the Mach-O nlist order. A call to
 * an import stub is `cpu->r[14] = ret; MT_CALL_IMPORT(id);` (calls.h), which calls
 * mt_import_table[id]. recomp/imports.py generates, besides mt_import_names[] and the table
 * storage:
 *
 *   mt_import_descs[id]  what the import is: a function (with a guest code address) or a
 *                        data object (with the size and alignment of its guest object)
 *   mt_import_binds[]    every pointer dyld would bind: slot address, import id, addend
 *   mt_import_thunks[]   guest code address -> host function that calls the import, so a
 *                        guest pointer to an import (a vtable slot, a callback) is callable
 *                        through mt_dispatch
 *
 * The runtime side (filling the table, the default handler, placing data objects and
 * applying the binds) is maplethief/hle/imports.h. */
#ifndef MAPLETHIEF_RECOMP_IMPORTS_H
#define MAPLETHIEF_RECOMP_IMPORTS_H

#include <stdint.h>

#include "maplethief/recomp/calls.h"
#include "maplethief/recomp/compiler.h"
#include "maplethief/recomp/cpu.h"

MT_EXTERN_C_BEGIN

/* mt_import_desc.flags */
#define MT_IMPORT_FUNCTION 0x1u /* code: called through its stub or a pointer */
#define MT_IMPORT_DATA 0x2u     /* an object in guest memory (variable, constant, class, vtable) */
#define MT_IMPORT_WEAK 0x4u     /* weak import: the game copes with it being absent */
#define MT_IMPORT_STRET 0x8u    /* returns a structure through the pointer in r0 */

/* Guest code address of a function import that has no stub (only its address is taken,
 * e.g. ___cxa_pure_virtual in vtables): MT_IMPORT_THUNK_BASE + 4 * id. Nothing is mapped
 * there and guest code never reads it; it only keys mt_import_thunks. The memory model
 * (#42) keeps [MT_IMPORT_THUNK_BASE, +4 * mt_import_count) out of guest memory. */
#define MT_IMPORT_THUNK_BASE 0xFFFE0000u

typedef struct mt_import_desc {
    const char *name;    /* the symbol with its leading underscore, e.g. "_memcpy" */
    const char *library; /* install name of the dylib that exports it */
    uint32_t flags;      /* MT_IMPORT_* */
    uint32_t addr;       /* function: guest code address (its first stub, or the thunk
                            address); data: 0 (the runtime places the object at load time) */
    uint32_t size;       /* data: bytes of the guest object; function: 0 */
    uint32_t align;      /* data: alignment of the guest object (a power of two); function: 0 */
} mt_import_desc;

/* A pointer the loader writes: the 32-bit little-endian word at guest address `slot`
 * becomes (guest address of import `id`) + addend. */
typedef struct mt_import_bind {
    uint32_t slot;
    uint32_t id;
    int32_t addend;
} mt_import_bind;

extern const uint32_t mt_import_count; /* MT_IMPORT_COUNT */
extern const mt_import_desc mt_import_descs[];
extern const mt_import_bind mt_import_binds[]; /* ascending by slot */
extern const uint32_t mt_import_bind_count;
extern const mt_fn_entry mt_import_thunks[]; /* ascending by address */
extern const uint32_t mt_import_thunk_count;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_IMPORTS_H */
