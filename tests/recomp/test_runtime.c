/* Minimal test runtime for recompiled code; see test_runtime.h. */
#include "test_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maplethief/recomp/dispatch.h"

/* The runtime's own dispatch (mt_dispatch, mt_fn_lookup, the miss description), compiled
 * into this translation unit so that every build of the test runtime uses it. The table
 * (mt_fn_table, mt_fn_count, mt_fn_idx) comes from the generated dispatch.c or the test's
 * driver. */
#include "../../runtime/src/recomp/dispatch.c"

static uint32_t mt_test_import_count;

jmp_buf *mt_test_trap_jmp;

void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    if (kind == MT_TRAP_NO_FUNCTION) {
        char text[1024];
        uint32_t frames[16], n, i;
        const mt_fn_entry *caller = mt_fn_containing(pc);
        mt_dispatch_describe(cpu, detail, pc, text, sizeof text);
        printf("MT-DISPATCH-MISS %s\n", text);
        printf("miss_caller=0x%08X\n", caller != NULL ? caller->addr : 0u);
        n = mt_guest_backtrace(cpu, frames, 16u);
        printf("miss_frames=0x%08X\n", n);
        for (i = 0; i < n; i++) {
            printf("miss_frame%u=0x%08X\n", i, frames[i]);
        }
    }
    printf("MT-TRAP kind=%u pc=0x%08X detail=0x%08X lr=0x%08X\n", kind, pc, detail, cpu->r[MT_LR]);
    printf("trap_kind=0x%08X\ntrap_pc=0x%08X\ntrap_detail=0x%08X\n", kind, pc, detail);
    fflush(stdout);
    if (mt_test_trap_jmp != NULL) {
        longjmp(*mt_test_trap_jmp, 1);
    }
    exit(70);
}

void mt_mem_fault(mt_cpu *cpu, uint32_t addr, uint32_t size, uint32_t pc) {
    printf("MT-MEMFAULT addr=0x%08X size=%u pc=0x%08X lr=0x%08X\n", addr, size, pc, cpu->r[MT_LR]);
    fflush(stdout);
    exit(71);
}

static void unimplemented_import(mt_cpu *cpu, uint32_t id) {
    printf("MT-IMPORT-UNIMPLEMENTED id=%u name=%s caller=0x%08X\n", id,
           id < mt_test_import_count ? mt_import_names[id] : "?", cpu->r[MT_LR]);
    fflush(stdout);
    exit(74);
}

/* compiler-rt helpers: AAPCS, arguments in r0/r1, result in r0. ARM divides by zero to 0. */
static void imp_divsi3(mt_cpu *cpu, uint32_t id) {
    int32_t a = (int32_t)cpu->r[0], b = (int32_t)cpu->r[1];
    (void)id;
    cpu->r[0] = b == 0 ? 0u : (a == INT32_MIN && b == -1) ? (uint32_t)a : (uint32_t)(a / b);
}
static void imp_modsi3(mt_cpu *cpu, uint32_t id) {
    int32_t a = (int32_t)cpu->r[0], b = (int32_t)cpu->r[1];
    (void)id;
    cpu->r[0] = b == 0 ? 0u : (b == -1) ? 0u : (uint32_t)(a % b);
}
static void imp_udivsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    cpu->r[0] = cpu->r[1] == 0u ? 0u : cpu->r[0] / cpu->r[1];
}
static void imp_umodsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    cpu->r[0] = cpu->r[1] == 0u ? 0u : cpu->r[0] % cpu->r[1];
}

void mt_test_init(mt_cpu *cpu, uint64_t size, uint32_t import_count) {
    uint32_t i;
    memset(cpu, 0, sizeof *cpu);
    cpu->mem = (uint8_t *)calloc(1, (size_t)size);
    if (cpu->mem == NULL) {
        printf("MT-OOM\n");
        exit(75);
    }
    cpu->mem_size = size;
    cpu->fpscr = MT_FPSCR_IOS;
    cpu->r[MT_SP] = (uint32_t)(size - 64u);
    cpu->r[MT_LR] = MT_TEST_RETURN;
    mt_test_import_count = import_count;
    for (i = 0; i < import_count; i++) {
        const char *name = mt_import_names[i];
        mt_import_fn fn = unimplemented_import;
        if (strcmp(name, "___divsi3") == 0) {
            fn = imp_divsi3;
        } else if (strcmp(name, "___modsi3") == 0) {
            fn = imp_modsi3;
        } else if (strcmp(name, "___udivsi3") == 0) {
            fn = imp_udivsi3;
        } else if (strcmp(name, "___umodsi3") == 0) {
            fn = imp_umodsi3;
        }
        mt_import_table[i] = fn;
    }
}

int mt_test_load(mt_cpu *cpu, const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) {
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (uint64_t)addr + (uint64_t)n > cpu->mem_size ||
        fread(cpu->mem + addr, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

void mt_test_print_state(const mt_cpu *cpu) {
    int i;
    for (i = 0; i < 16; i++) {
        printf("r%d=0x%08X\n", i, cpu->r[i]);
    }
    printf("cpsr=0x%08X\nfpscr=0x%08X\n", cpu->cpsr, cpu->fpscr);
    for (i = 0; i < 32; i++) {
        printf("d%d=0x%08X%08X\n", i, (uint32_t)(cpu->d[i] >> 32), (uint32_t)cpu->d[i]);
    }
}
