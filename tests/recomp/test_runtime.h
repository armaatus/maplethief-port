/* A minimal runtime for running recompiled code in tests (ADR 0014; the snippet suite #39
 * builds on it). It implements what maplethief/recomp/recomp.h declares: traps, memory
 * faults and the dispatch print a line starting with "MT-" and exit with a non-zero status,
 * so a test sees what went wrong. Guest memory is one bounds-checked window. */
#ifndef MT_TEST_RUNTIME_H
#define MT_TEST_RUNTIME_H

#include <setjmp.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

/* The return address the tests put in lr before calling a guest function. */
#define MT_TEST_RETURN 0xFFFFFFF1u

/* Allocate a zeroed guest window of `size` bytes, point sp 64 bytes below its end, set lr to
 * MT_TEST_RETURN and fill every import slot (if `import_count` > 0) with a handler that
 * reports the import as unimplemented, except the compiler-rt helpers the test runtime
 * implements (___divsi3, ___modsi3, ___udivsi3, ___umodsi3). */
void mt_test_init(mt_cpu *cpu, uint64_t size, uint32_t import_count);

/* Copy `path` (a raw file) into guest memory at `addr`; returns 0 on success. */
int mt_test_load(mt_cpu *cpu, const char *path, uint32_t addr);

/* If set, mt_trap returns to this jump buffer after printing the trap (a test that expects
 * the trap then prints the state as usual); otherwise it exits with status 70. */
extern jmp_buf *mt_test_trap_jmp;

/* Print r0-r15, cpsr, fpscr and d0-d31 as "name=0x..." lines. */
void mt_test_print_state(const mt_cpu *cpu);

#endif /* MT_TEST_RUNTIME_H */
