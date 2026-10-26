/* Indirect dispatch over the generated table (maplethief/recomp/dispatch.h, ADR 0014 #35).
 *
 * C17 that MSVC also compiles. It reads only the generated data and guest memory (for the
 * backtrace), and calls mt_trap on a miss. It needs the generated table to link, so it is
 * not in the runtime's CMake target yet: the build of work/gen/ (#38's gen.cmake) and the
 * M4 runtime add it. Until then tests/recomp/test_runtime.c compiles it in, so every
 * snippet and recompiled-code test runs this lookup. */
#include "maplethief/recomp/dispatch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* An import (#36): its stub or MT_IMPORT_THUNK_BASE + 4 * id, in mt_import_thunks. */
static mt_guest_fn import_thunk(uint32_t target) {
    uint32_t lo = 0, hi = mt_import_thunk_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (mt_import_thunks[mid].addr < target) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < mt_import_thunk_count && mt_import_thunks[lo].addr == target
               ? mt_import_thunks[lo].fn
               : NULL;
}

mt_guest_fn mt_fn_lookup(uint32_t target) {
    const mt_fn_index *ix = &mt_fn_idx;
    /* Below base the subtraction wraps to a huge offset, so one bound check covers both
     * sides. The Thumb bit is cleared for the bucket and kept for the comparison. */
    uint32_t bucket = ((target & ~1u) - ix->base) >> ix->shift;
    uint32_t i, end;
    if (MT_LIKELY(bucket < ix->buckets)) {
        end = ix->first[bucket + 1u];
        for (i = ix->first[bucket]; i < end; i++) {
            if (mt_fn_table[i].addr == target) {
                return mt_fn_table[i].fn;
            }
        }
    }
    return import_thunk(target);
}

mt_guest_fn mt_dispatch(mt_cpu *cpu, uint32_t target, uint32_t site) {
    mt_guest_fn fn = mt_fn_lookup(target);
    if (MT_LIKELY(fn != NULL)) {
        return fn;
    }
    cpu->r[MT_PC] = site;
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

const mt_fn_entry *mt_fn_containing(uint32_t addr) {
    uint32_t lo = 0, hi = mt_fn_count, key = addr & ~1u;
    if (key >= mt_fn_idx.end) {
        return NULL;
    }
    while (lo < hi) { /* first entry above key */
        uint32_t mid = lo + (hi - lo) / 2u;
        if ((mt_fn_table[mid].addr & ~1u) <= key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo == 0u ? NULL : &mt_fn_table[lo - 1u];
}

uint32_t mt_guest_backtrace(const mt_cpu *cpu, uint32_t *out, uint32_t max) {
    uint32_t fp = cpu->r[7], n = 0;
    while (n < max && fp != 0u && (fp & 3u) == 0u && (uint64_t)fp + 8u <= cpu->mem_size) {
        uint32_t next, lr;
        memcpy(&next, cpu->mem + fp, 4); /* the host is little-endian (compiler.h) */
        memcpy(&lr, cpu->mem + fp + 4u, 4);
        if (lr == 0u) {
            break;
        }
        out[n++] = lr;
        if (next <= fp) { /* a caller's frame is always higher up the stack */
            break;
        }
        fp = next;
    }
    return n;
}

typedef struct {
    char *buf;
    size_t size, len;
} mt_text;

static void put(mt_text *t, const char *fmt, ...) {
    va_list ap;
    int n;
    size_t room = t->len < t->size ? t->size - t->len : 0u;
    va_start(ap, fmt);
    n = vsnprintf(room ? t->buf + t->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        t->len += (size_t)n;
    }
}

static void put_where(mt_text *t, uint32_t addr) {
    const mt_fn_entry *f = mt_fn_containing(addr);
    if (f != NULL) {
        put(t, " (f_%08X+0x%X)", f->addr, (addr & ~1u) - (f->addr & ~1u));
    }
}

size_t mt_dispatch_describe(const mt_cpu *cpu, uint32_t target, uint32_t site, char *buf,
                            size_t size) {
    mt_text t;
    const mt_fn_entry *near;
    uint32_t frames[16], n, i;
    t.buf = buf;
    t.size = size;
    t.len = 0;
    if (size > 0u) {
        buf[0] = '\0';
    }
    put(&t, "no guest function at 0x%08X: indirect call or jump at 0x%08X", target, site);
    put_where(&t, site);
    put(&t, ", lr 0x%08X", cpu->r[MT_LR]);
    put_where(&t, cpu->r[MT_LR]);
    near = mt_fn_containing(target);
    if (target == 0u) {
        put(&t, "; a null function pointer");
    } else if (mt_fn_lookup(target ^ 1u) != NULL) {
        if (target & 1u) {
            put(&t, "; f_%08X is ARM code, but bit 0 of the target selects Thumb", target ^ 1u);
        } else {
            put(&t, "; f_%08X is Thumb code, but bit 0 of the target is clear (ARM)", target | 1u);
        }
    } else if ((target & 3u) == 2u) {
        put(&t, "; an ARM target must be 4-byte aligned (bit 1 set, bit 0 clear)");
    } else if (near == NULL) {
        put(&t, (target & ~1u) < mt_fn_idx.base ? "; below the first function"
                                                : "; past the last function");
    } else if ((near->addr & ~1u) != (target & ~1u)) {
        put(&t, "; 0x%X bytes after the start of f_%08X", (target & ~1u) - (near->addr & ~1u),
            near->addr);
    }
    n = mt_guest_backtrace(cpu, frames, (uint32_t)(sizeof frames / sizeof frames[0]));
    put(&t, "; backtrace (r7 chain):");
    if (n == 0u) {
        put(&t, " none");
    }
    for (i = 0; i < n; i++) {
        put(&t, " 0x%08X", frames[i]);
        put_where(&t, frames[i]);
    }
    if (size > 0u && t.len >= size) {
        return size - 1u;
    }
    return t.len;
}
