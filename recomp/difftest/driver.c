/* Host side of the differential test (#41, ADR 0017): runs recompiled leaf functions on the
 * cases of an input file and writes their end states. recomp/difftest/host.py generates the
 * function table (mt_dt_fns), builds this file with the lifted C and documents the formats.
 *
 *   driver PRISTINE CASES OUT
 *
 * PRISTINE is the guest window (image at its link-time addresses, the rest zero). For each
 * case the driver loads a heap image and the case's patches, runs the production lift
 * (variant 0) and the flags-in/flags-out lift (variant 1) from the case's registers, and
 * writes the end state plus every byte of guest memory that differs from the case's start.
 * A memory fault, trap, dispatch miss or SjLj hook stops the run (longjmp) and is recorded
 * with its details; nothing exits the process. Guest memory is one bounds-checked window
 * (MT_MEM_CHECKED 1), as in the tests, so the emulator maps exactly the same bytes. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maplethief/recomp/recomp.h"

typedef struct mt_dt_fn {
    uint32_t ptr;
    mt_guest_fn fn;       /* production lift */
    mt_guest_fn flags_fn; /* flags_in + flags_out lift */
} mt_dt_fn;

/* Generated next to the lifted code (fns.c). */
extern const mt_dt_fn mt_dt_fns[];
extern const uint32_t mt_dt_fn_count;

/* Leaves call no imports; the table exists because calls.h declares it. */
mt_import_fn mt_import_table[1];
const char *const mt_import_names[1] = {""};

enum { MAGIC_IN = 0x3144544Du, MAGIC_OUT = 0x3244544Du, PAGE = 4096u };
enum { ST_RETURN = 0, ST_FAULT = 1, ST_TRAP = 2, ST_DISPATCH = 3, ST_SJLJ = 4 };

static jmp_buf stop;
static uint32_t stop_status, stop_detail[3];

static MT_NORETURN void stop_run(uint32_t status, uint32_t a, uint32_t b, uint32_t c) {
    stop_status = status;
    stop_detail[0] = a;
    stop_detail[1] = b;
    stop_detail[2] = c;
    longjmp(stop, 1);
}

void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    (void)cpu;
    stop_run(ST_TRAP, kind, pc, detail);
}

void mt_mem_fault(mt_cpu *cpu, uint32_t addr, uint32_t size, uint32_t pc) {
    (void)cpu;
    stop_run(ST_FAULT, addr, size, pc);
}

mt_guest_fn mt_dispatch(mt_cpu *cpu, uint32_t target, uint32_t site) {
    (void)cpu;
    stop_run(ST_DISPATCH, target, site, 0u);
}

jmp_buf *mt_sjlj_jmpbuf(mt_cpu *cpu, uint32_t guest_jbuf) {
    (void)cpu;
    stop_run(ST_SJLJ, guest_jbuf, 0u, 0u);
}

static MT_NORETURN void die(const char *what) {
    fprintf(stderr, "difftest driver: %s\n", what);
    exit(2);
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        die("cannot read an input file");
    }
    buf = (uint8_t *)malloc((size_t)n + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        die("cannot read an input file");
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* A cursor over the input file; every read is bounds-checked. */
typedef struct reader {
    const uint8_t *p, *end;
} reader;

static uint32_t u32(reader *r) {
    uint32_t v;
    if ((size_t)(r->end - r->p) < 4u) {
        die("truncated input");
    }
    memcpy(&v, r->p, 4);
    r->p += 4;
    return v;
}

static const uint8_t *bytes(reader *r, uint32_t n) {
    const uint8_t *at = r->p;
    uint32_t padded = (n + 3u) & ~3u;
    if ((size_t)(r->end - r->p) < padded) {
        die("truncated input");
    }
    r->p += padded;
    return at;
}

static FILE *out;
static mt_cpu cpu; /* static: unchanged by the longjmp of a stopped run */

/* Run one function; a fault, trap or dispatch miss longjmps back here. Kept apart from main
 * so that no local of main lives across setjmp. */
static void run(mt_guest_fn fn) {
    stop_status = ST_RETURN;
    memset(stop_detail, 0, sizeof stop_detail);
    if (setjmp(stop) == 0) {
        fn(&cpu);
    }
}

static void put32(uint32_t v) {
    if (fwrite(&v, 4, 1, out) != 1) {
        die("cannot write the output");
    }
}

static void put_bytes(const uint8_t *p, uint32_t n) {
    static const uint8_t zero[4] = {0};
    if (fwrite(p, 1, n, out) != n ||
        fwrite(zero, 1, (4u - (n & 3u)) & 3u, out) != ((4u - (n & 3u)) & 3u)) {
        die("cannot write the output");
    }
}

/* Every run of bytes where mem differs from ref, as (addr, len, bytes); then mem = ref. */
static void diff_and_restore(uint8_t *mem, const uint8_t *ref, uint32_t window) {
    uint32_t page, i, runs = 0;
    long count_at = ftell(out);
    put32(0u);
    for (page = 0; page < window; page += PAGE) {
        uint32_t n = window - page < PAGE ? window - page : PAGE;
        if (memcmp(mem + page, ref + page, n) == 0) {
            continue;
        }
        for (i = 0; i < n;) {
            uint32_t j;
            if (mem[page + i] == ref[page + i]) {
                i++;
                continue;
            }
            for (j = i; j < n && mem[page + j] != ref[page + j]; j++) {
            }
            put32(page + i);
            put32(j - i);
            put_bytes(mem + page + i, j - i);
            runs++;
            i = j;
        }
        memcpy(mem + page, ref + page, n);
    }
    if (fseek(out, count_at, SEEK_SET) != 0) {
        die("cannot write the output");
    }
    put32(runs);
    if (fseek(out, 0, SEEK_END) != 0) {
        die("cannot write the output");
    }
}

int main(int argc, char **argv) {
    static uint32_t start_r[16], start_cpsr, start_fpscr;
    static uint64_t start_d[32];
    size_t pristine_size, in_size;
    uint8_t *pristine, *ref, *in;
    reader r;
    uint32_t fn_index, window, heap_addr, heap_size, n_pool, n_cases, c, i;
    const uint8_t *pool;
    const mt_dt_fn *fn;

    if (argc != 4) {
        die("usage: driver PRISTINE CASES OUT");
    }
    pristine = read_file(argv[1], &pristine_size);
    in = read_file(argv[2], &in_size);
    r.p = in;
    r.end = in + in_size;
    if (u32(&r) != MAGIC_IN) {
        die("not a case file");
    }
    fn_index = u32(&r);
    window = u32(&r);
    heap_addr = u32(&r);
    heap_size = u32(&r);
    n_pool = u32(&r);
    n_cases = u32(&r);
    if (fn_index >= mt_dt_fn_count || window != pristine_size || heap_addr > window ||
        heap_size > window - heap_addr) {
        die("case file does not match the build");
    }
    pool = bytes(&r, n_pool * heap_size);
    fn = &mt_dt_fns[fn_index];
    cpu.mem = (uint8_t *)malloc(window);
    ref = (uint8_t *)malloc(window);
    if (cpu.mem == NULL || ref == NULL) {
        die("out of memory");
    }
    cpu.mem_size = window;
    memcpy(cpu.mem, pristine, window);
    memcpy(ref, pristine, window);
    out = fopen(argv[3], "wb");
    if (out == NULL) {
        die("cannot write the output");
    }
    put32(MAGIC_OUT);
    put32(n_cases * 2u);
    for (c = 0; c < n_cases; c++) {
        uint32_t case_id = u32(&r), pool_index = u32(&r), n_patches, variant;
        const uint8_t *patch_at;
        for (i = 0; i < 16; i++) {
            start_r[i] = u32(&r);
        }
        start_cpsr = u32(&r);
        start_fpscr = u32(&r);
        for (i = 0; i < 32; i++) {
            uint64_t lo = u32(&r);
            start_d[i] = lo | (uint64_t)u32(&r) << 32;
        }
        if (pool_index >= n_pool) {
            die("bad pool index");
        }
        memcpy(cpu.mem + heap_addr, pool + (size_t)pool_index * heap_size, heap_size);
        memcpy(ref + heap_addr, pool + (size_t)pool_index * heap_size, heap_size);
        n_patches = u32(&r);
        patch_at = r.p;
        for (i = 0; i < n_patches; i++) {
            uint32_t addr = u32(&r), len = u32(&r);
            const uint8_t *data = bytes(&r, len);
            if (addr > window || len > window - addr) {
                die("patch outside the window");
            }
            memcpy(cpu.mem + addr, data, len);
            memcpy(ref + addr, data, len);
        }
        for (variant = 0; variant < 2u; variant++) {
            memcpy(cpu.r, start_r, sizeof cpu.r);
            memcpy(cpu.d, start_d, sizeof cpu.d);
            cpu.cpsr = start_cpsr;
            cpu.fpscr = start_fpscr;
            run(variant ? fn->flags_fn : fn->fn);
            put32(case_id);
            put32(variant);
            put32(stop_status);
            for (i = 0; i < 3; i++) {
                put32(stop_detail[i]);
            }
            for (i = 0; i < 16; i++) {
                put32(cpu.r[i]);
            }
            put32(cpu.cpsr);
            put32(cpu.fpscr);
            for (i = 0; i < 32; i++) {
                put32((uint32_t)cpu.d[i]);
                put32((uint32_t)(cpu.d[i] >> 32));
            }
            diff_and_restore(cpu.mem, ref, window);
        }
        /* Back to the pristine window for the next case (the heap is reloaded anyway). */
        r.p = patch_at;
        for (i = 0; i < n_patches; i++) {
            uint32_t addr = u32(&r), len = u32(&r);
            (void)bytes(&r, len);
            memcpy(cpu.mem + addr, pristine + addr, len);
            memcpy(ref + addr, pristine + addr, len);
        }
    }
    if (fclose(out) != 0) {
        die("cannot write the output");
    }
    free(cpu.mem);
    free(ref);
    free(pristine);
    free(in);
    return 0;
}
