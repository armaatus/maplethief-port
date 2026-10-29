/* Guest memory: the 32-bit address space, the image, the guest heap and guest stacks
 * (ADR 0025, issue #42).
 *
 * Generated code reaches guest memory through maplethief/recomp/mem.h: host address =
 * cpu->mem + guest address. This header is the runtime side that provides that memory. One
 * mt_mem is one guest address space; every guest thread's mt_cpu is attached to it.
 *
 * The layout is the same on every platform, so guest addresses (heap blocks, stacks) are the
 * same everywhere:
 *
 *   0x00000000 - 0x00000FFF  page zero (__PAGEZERO): never mapped, null pointers fault
 *   0x00001000 - 0x00FFFFFF  the image: the Mach-O's segments at their link-time addresses
 *   0x01000000 - 0x1EFFFFFF  the guest heap (mt_heap_*), grown and committed on demand
 *   0x1F000000 - 0x1FFFFFFF  guest stacks (mt_stack_*), each above a no-access guard
 *   0x20000000 -             outside guest memory: cpu->mem_size is MT_GUEST_ARENA_SIZE
 *
 * The host reservation behind it is either the full 4 GiB plus a guard area, so that code
 * compiled with MT_MEM_CHECKED 0 can never leave it (any guest address beyond the arena
 * faults in hardware), or the 512 MiB arena alone (the iOS fallback), which needs
 * MT_MEM_CHECKED 1. Pages are committed only when the image, the heap or a stack needs them.
 *
 * Usage, at start-up:
 *
 *     mt_mem_config cfg = mt_mem_config_default();
 *     cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;  // matches the generated code's model
 *     mt_mem *mem = mt_mem_create(&cfg);
 *     mt_mem_map_macho(mem, slice, slice_size);     // the thin armv7 Mach-O
 *     ... apply binds (#43) ...
 *     mt_mem_seal_image(mem);                       // __TEXT read-only
 *     mt_stack stack;
 *     mt_stack_create(mem, MT_STACK_MAIN_SIZE, &stack);
 *     mt_mem_attach(mem, &cpu);
 *     cpu.r[MT_SP] = stack.top;
 *
 * HLE code reads guest pointers with mt_g2h / mt_g2h_str (always bounds-checked, faulting
 * with the guest caller's address) and finds the address space of a thread with
 * mt_mem_from_cpu. Everything here is thread-safe unless it says otherwise.
 *
 * C17 that also compiles as C++20; the implementation is runtime/src/mem/. */
#ifndef MAPLETHIEF_MEMORY_H
#define MAPLETHIEF_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- layout ----------------------------------------------------------------------------- */

#define MT_GUEST_PAGE_ZERO_END 0x00001000u /* [0, this): page zero */
#define MT_GUEST_IMAGE_END 0x01000000u     /* [MT_GUEST_PAGE_ZERO_END, this): image segments */
#define MT_GUEST_HEAP_BASE 0x01000000u
#define MT_GUEST_HEAP_END 0x1F000000u
#define MT_GUEST_STACK_AREA 0x1F000000u
#define MT_GUEST_STACK_AREA_END 0x20000000u
#define MT_GUEST_ARENA_SIZE 0x20000000u /* every guest byte is below this; cpu->mem_size */

/* The full reservation: 4 GiB plus this much, so that an access of up to 8 bytes at guest
 * 0xFFFFFFFF stays inside it. A multiple of every host page size. */
#define MT_GUEST_RESERVE_GUARD 0x10000u

#define MT_HEAP_ALIGN 16u             /* every heap block is aligned to this */
#define MT_STACK_GRANULE 0x10000u     /* stack sizes round up to this */
#define MT_STACK_GUARD 0x10000u       /* no-access bytes below every stack */
#define MT_STACK_MAIN_SIZE 0x100000u  /* 1 MiB: the iOS main thread's stack */
#define MT_STACK_THREAD_SIZE 0x80000u /* 512 KiB: the iOS default for other threads */

/* --- the address space ------------------------------------------------------------------ */

typedef struct mt_mem mt_mem;

enum mt_mem_reservation {
    /* The full 4 GiB reservation if the host allows it, else the arena alone. Right for code
     * compiled with MT_MEM_CHECKED 1, which never touches anything beyond the arena. */
    MT_MEM_RESERVE_AUTO = 0,
    /* The full reservation or nothing: what code compiled with MT_MEM_CHECKED 0 needs. */
    MT_MEM_RESERVE_FULL = 1,
    /* The arena alone (512 MiB): the iOS model, needs MT_MEM_CHECKED 1. */
    MT_MEM_RESERVE_ARENA = 2,
};

/* The reservation the including translation unit's MT_MEM_CHECKED needs. */
#if MT_MEM_CHECKED
#define MT_MEM_RESERVE_FOR_BUILD MT_MEM_RESERVE_AUTO
#else
#define MT_MEM_RESERVE_FOR_BUILD MT_MEM_RESERVE_FULL
#endif

typedef struct mt_mem_config {
    uint32_t reservation; /* enum mt_mem_reservation */
} mt_mem_config;

/* MT_MEM_RESERVE_AUTO. */
mt_mem_config mt_mem_config_default(void) MT_NOEXCEPT;

/* Reserves a new, empty guest address space (nothing mapped, empty heap, no stacks). Returns
 * NULL, after logging why to stderr, if the reservation fails. */
mt_mem *mt_mem_create(const mt_mem_config *cfg) MT_NOEXCEPT;

/* Releases the address space. No mt_cpu may use it any more. NULL is ignored. */
void mt_mem_destroy(mt_mem *mem) MT_NOEXCEPT;

/* Host address of guest address 0. */
uint8_t *mt_mem_base(const mt_mem *mem) MT_NOEXCEPT;

/* 1 if the host reservation is the full 4 GiB plus guard (MT_MEM_CHECKED 0 is safe), 0 if it
 * is the arena alone. */
int mt_mem_is_full_reservation(const mt_mem *mem) MT_NOEXCEPT;

/* Points cpu->mem and cpu->mem_size at this address space. */
void mt_mem_attach(mt_mem *mem, mt_cpu *cpu) MT_NOEXCEPT;

/* The address space a cpu is attached to (looked up by cpu->mem), or NULL if it is not one of
 * the live mt_mem objects (e.g. a test's plain buffer). */
mt_mem *mt_mem_from_cpu(const mt_cpu *cpu) MT_NOEXCEPT;

/* The host's page size, which is the granularity of protection: 4 KiB on x86-64, Windows and
 * most Linux; 16 KiB on Apple arm64. */
uint32_t mt_mem_host_page_size(void) MT_NOEXCEPT;

/* --- the image -------------------------------------------------------------------------- */

/* Mach-O VM_PROT_* bits, as in a segment's initprot. */
#define MT_PROT_READ 1u
#define MT_PROT_WRITE 2u
#define MT_PROT_EXECUTE 4u

/* One LC_SEGMENT of a 32-bit Mach-O. */
typedef struct mt_segment {
    char name[17]; /* NUL-terminated */
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t initprot; /* MT_PROT_* */
} mt_segment;

#define MT_MAX_SEGMENTS 16u

/* Reads the LC_SEGMENT commands of a thin 32-bit little-endian ARM Mach-O (the untrusted
 * file is checked: header, command sizes, file ranges, filesize <= vmsize, no address
 * overflow). Writes up to `max` segments to `out` and their number to *count. Returns 0, or
 * -1 if the file is not such a Mach-O or a check fails (logged to stderr). */
int mt_macho_segments(const uint8_t *file, size_t size, mt_segment *out, uint32_t max,
                      uint32_t *count) MT_NOEXCEPT;

/* Maps one segment at its vmaddr (slide 0): commits its pages read-write, copies its file
 * bytes and leaves the rest of vmsize zero. A segment with initprot 0 at address 0
 * (__PAGEZERO) or with vmsize 0 is skipped. Returns 0, or -1 if the segment lies outside
 * [MT_GUEST_PAGE_ZERO_END, MT_GUEST_IMAGE_END), overlaps a mapped one, or its file range is
 * not inside `file`. Not thread-safe with other image calls. */
int mt_mem_map_segment(mt_mem *mem, const mt_segment *seg, const uint8_t *file,
                       size_t size) MT_NOEXCEPT;

/* mt_macho_segments, then mt_mem_map_segment for each. Returns 0 or -1. */
int mt_mem_map_macho(mt_mem *mem, const uint8_t *file, size_t size) MT_NOEXCEPT;

/* Gives every mapped host page the protection of the segments in it: read-only unless a
 * segment in the page is writable. On a 16 KiB-page host, a page shared by __TEXT and __DATA
 * stays writable, and page zero shares __TEXT's first page (readable, not writable). Call
 * once after the loader has written its fixups. Returns 0 or -1. */
int mt_mem_seal_image(mt_mem *mem) MT_NOEXCEPT;

/* The mapped segments, in mapping order; *count gets their number. */
const mt_segment *mt_mem_segments(const mt_mem *mem, uint32_t *count) MT_NOEXCEPT;

/* --- the guest heap --------------------------------------------------------------------- */
/* Darwin malloc semantics where the guest can see them: every block is aligned to
 * MT_HEAP_ALIGN, malloc(0) returns a unique minimum-sized block, realloc(p, 0) frees p and
 * returns a new minimum-sized block. Guest address 0 means failure (out of heap). Fresh heap
 * memory is zero; reused blocks keep their old bytes, as on iOS. The allocator's bookkeeping
 * is host-side, so a guest overflow cannot corrupt it, and a free of an address that is not
 * a live block is detected. Addresses are deterministic: the same calls give the same
 * addresses on every host. */

uint32_t mt_heap_malloc(mt_mem *mem, uint32_t size) MT_NOEXCEPT;
uint32_t mt_heap_calloc(mt_mem *mem, uint32_t count, uint32_t size) MT_NOEXCEPT;

/* addr 0 is malloc. If addr is not a live block it returns 0 and changes nothing (check
 * with mt_heap_usable_size to tell that from out-of-heap). */
uint32_t mt_heap_realloc(mt_mem *mem, uint32_t addr, uint32_t size) MT_NOEXCEPT;

/* Returns 0, or -1 (and changes nothing) if addr is not a live block: a double or invalid
 * free, which the HLE caller reports with the guest PC. free(0) is a no-op returning 0. */
int mt_heap_free(mt_mem *mem, uint32_t addr) MT_NOEXCEPT;

/* A block aligned to `align` (a power of two; values below MT_HEAP_ALIGN mean
 * MT_HEAP_ALIGN). Returns 0 if align is not a power of two or the heap is full. */
uint32_t mt_heap_memalign(mt_mem *mem, uint32_t align, uint32_t size) MT_NOEXCEPT;

/* Usable size of the live block at addr (>= the size asked for), or 0 if addr is not the
 * start of a live block (malloc_size semantics). */
uint32_t mt_heap_usable_size(mt_mem *mem, uint32_t addr) MT_NOEXCEPT;

typedef struct mt_heap_stats {
    uint32_t live_blocks;
    uint32_t live_bytes; /* usable sizes of the live blocks */
    uint32_t top;        /* end of the heap space handed out so far */
    uint32_t committed;  /* end of the committed heap pages */
} mt_heap_stats;

void mt_heap_get_stats(mt_mem *mem, mt_heap_stats *out) MT_NOEXCEPT;

/* An mt_guest_alloc_fn (maplethief/hle/imports.h) over the heap: `user` is the mt_mem; the
 * block is zeroed. For load-time objects such as data imports. */
uint32_t mt_mem_guest_alloc(void *user, uint32_t size, uint32_t align) MT_NOEXCEPT;

/* The matching free (an mt_guest_free_fn, maplethief/objc/objc.h): mt_heap_free on the
 * mt_mem `user`. Returns 0, or -1 if addr is not a live block. The ObjC runtime frees
 * deallocated objects and released blocks with it (ADR 0029). */
int mt_mem_guest_free(void *user, uint32_t addr) MT_NOEXCEPT;

/* --- guest stacks ----------------------------------------------------------------------- */

/* A guest stack: [base, top) is read-write and starts zeroed, [guard, base) is no-access
 * (MT_STACK_GUARD bytes), so an overflow faults in hardware. The initial sp is `top`
 * (16-byte aligned). */
typedef struct mt_stack {
    uint32_t guard;
    uint32_t base;
    uint32_t top;
} mt_stack;

/* Creates a stack of at least `size` bytes (rounded up to MT_STACK_GRANULE; 0 means
 * MT_STACK_THREAD_SIZE) in the stack area. Returns 0, or -1 if the area is full. */
int mt_stack_create(mt_mem *mem, uint32_t size, mt_stack *out) MT_NOEXCEPT;

/* Frees a stack: its pages are decommitted (no-access again, zero when reused). Returns 0, or
 * -1 if it is not a live stack. */
int mt_stack_destroy(mt_mem *mem, const mt_stack *stack) MT_NOEXCEPT;

/* --- diagnostics ------------------------------------------------------------------------ */

enum mt_mem_region {
    MT_REGION_OUTSIDE = 0,      /* at or beyond MT_GUEST_ARENA_SIZE: outside guest memory */
    MT_REGION_PAGE_ZERO = 1,    /* page zero: a null pointer */
    MT_REGION_IMAGE = 2,        /* inside a mapped segment */
    MT_REGION_UNMAPPED = 3,     /* image area, no segment */
    MT_REGION_HEAP = 4,         /* heap below its top (a live or a free block) */
    MT_REGION_HEAP_UNUSED = 5,  /* heap area above its top */
    MT_REGION_STACK = 6,        /* inside a guest stack */
    MT_REGION_STACK_GUARD = 7,  /* the guard below a guest stack: a stack overflow */
    MT_REGION_STACK_UNUSED = 8, /* stack area, no stack */
};

/* Classifies guest address `addr` and writes a one-line description to buf (if buf_size >
 * 0), e.g. "__TEXT+0x17e0", "heap block 0x01000040+0x8 (32 bytes)" or "guard below guest
 * stack 0x1f010000-0x1f110000". Returns the enum mt_mem_region. mem may be NULL (then only
 * the layout is known). Takes no lock it could wait for, but is not async-signal-safe. */
uint32_t mt_mem_describe(mt_mem *mem, uint32_t addr, char *buf, size_t buf_size) MT_NOEXCEPT;

/* What the last mt_mem_fault on this host thread recorded. */
typedef struct mt_mem_fault_info {
    uint32_t addr;
    uint32_t size;
    uint32_t pc;     /* guest PC of the access (for an HLE access, the guest caller) */
    uint32_t region; /* enum mt_mem_region */
} mt_mem_fault_info;

/* Copies the last fault of this host thread to *out; returns 0, or -1 if there was none. */
int mt_mem_last_fault(mt_mem_fault_info *out) MT_NOEXCEPT;

/* mt_mem_fault (maplethief/recomp/mem.h), implemented by the runtime: records the fault,
 * logs "guest memory fault: <size>-byte access to 0x<addr> at pc 0x<pc>: <region>" to
 * stderr and stops the guest with mt_trap(cpu, MT_TRAP_MEMORY, pc, addr). */

/* --- host <-> guest pointers for HLE code ------------------------------------------------ */

/* Host pointer to `size` bytes at guest `addr`. Always checked, whatever MT_MEM_CHECKED: an
 * address in page zero or a range beyond guest memory stops the guest through mt_mem_fault,
 * with the guest caller (lr) as the PC. */
MT_INLINE uint8_t *mt_g2h(mt_cpu *cpu, uint32_t addr, uint32_t size) {
    if (MT_UNLIKELY(addr < MT_GUEST_PAGE_ZERO_END || (uint64_t)addr + size > cpu->mem_size)) {
        mt_mem_fault(cpu, addr, size, cpu->r[MT_LR]);
    }
    return cpu->mem + addr;
}

/* A guest C string as a host pointer, checked like mt_g2h: the string and its NUL must lie
 * in guest memory. */
MT_INLINE const char *mt_g2h_str(mt_cpu *cpu, uint32_t addr) {
    const uint8_t *s = mt_g2h(cpu, addr, 1u);
    if (MT_UNLIKELY(memchr(s, 0, (size_t)(cpu->mem_size - addr)) == NULL)) {
        mt_mem_fault(cpu, addr, (uint32_t)(cpu->mem_size - addr), cpu->r[MT_LR]);
    }
    return (const char *)s;
}

/* Reports a host pointer that is not inside guest memory (a runtime bug) and aborts. */
MT_NORETURN void mt_mem_bad_host_pointer(const mt_cpu *cpu, const void *host) MT_NOEXCEPT;

/* Guest address of a host pointer into guest memory; NULL gives 0. Any other pointer outside
 * guest memory aborts through mt_mem_bad_host_pointer. */
MT_INLINE uint32_t mt_h2g(const mt_cpu *cpu, const void *host) {
    const uint8_t *p = (const uint8_t *)host;
    if (p == NULL) {
        return 0u;
    }
    if (MT_UNLIKELY(p < cpu->mem || (uint64_t)(p - cpu->mem) >= cpu->mem_size)) {
        mt_mem_bad_host_pointer(cpu, host);
    }
    return (uint32_t)(p - cpu->mem);
}

MT_EXTERN_C_END

#endif /* MAPLETHIEF_MEMORY_H */
