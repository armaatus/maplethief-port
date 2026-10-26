/* Guest CPU state shared by recompiled code and the runtime (ADR 0014).
 *
 * One mt_cpu per guest thread. Every generated function is `void f_XXXXXXXX(mt_cpu *cpu)`
 * and reads and writes the guest registers here directly (cpu->r[n], cpu->d[n]), so the
 * state is exact at every call, return, trap and SjLj resume. The APSR flags are the
 * exception: generated code keeps N, Z, C and V in C locals for the duration of a function
 * (no flag is live into a function or across a call, which the recompiler checks), and
 * `cpsr` only holds them where something outside the function needs them (traps, the
 * snippet harness). */
#ifndef MAPLETHIEF_RECOMP_CPU_H
#define MAPLETHIEF_RECOMP_CPU_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/compiler.h"

MT_EXTERN_C_BEGIN

enum {
    MT_SP = 13,
    MT_LR = 14,
    MT_PC = 15,
};

/* cpsr bits: N, Z, C, V in 31..28 as on the hardware; T (Thumb) in bit 5. */
#define MT_CPSR_N 0x80000000u
#define MT_CPSR_Z 0x40000000u
#define MT_CPSR_C 0x20000000u
#define MT_CPSR_V 0x10000000u
#define MT_CPSR_T 0x00000020u

typedef struct mt_cpu {
    /* r0-r12, sp (r13), lr (r14), pc (r15). Generated code never reads r[15]: a PC read is a
     * constant. It is written only before a trap, so diagnostics see the guest PC. */
    uint32_t r[16];
    uint32_t cpsr;
    /* VFP status and control. Only the NZCV bits (written by vcmp/vcmpe, read by vmrs) change
     * at run time: the binary never writes FPSCR (no vmsr), so the rounding mode stays
     * round-to-nearest-even and flush-to-zero / default-NaN stay off (ADR 0014). */
    uint32_t fpscr;
    /* VFPv3-D32: d0-d31 as raw bit patterns. s0-s31 alias the halves of d0-d15
     * (s[2n] = low 32 bits of d[n], s[2n+1] = high 32 bits); q0-q15 are pairs of d.
     * Access singles through mt_s_get / mt_s_set (vfp.h). */
    uint64_t d[32];
    /* Guest memory: host address of guest address 0, and the number of bytes from there that
     * the host may access (2^32 plus a guard area for a full reservation, or a smaller
     * bounds-checked window), see mem.h. Set by the runtime, constant while code runs. */
    uint8_t *mem;
    uint64_t mem_size;
    /* Runtime-owned context for this thread (opaque to generated code). */
    void *host;
} mt_cpu;

MT_STATIC_ASSERT(offsetof(mt_cpu, cpsr) == 64, "mt_cpu layout");
MT_STATIC_ASSERT(offsetof(mt_cpu, d) == 72, "mt_cpu layout");
MT_STATIC_ASSERT(offsetof(mt_cpu, mem) == 72 + 32 * 8, "mt_cpu layout");

/* A recompiled guest function (and an import thunk entered through the dispatch table).
 * The parameter is restrict-qualified exactly like the generated definitions: MSVC treats
 * the two function types as different otherwise (C4113). */
typedef void (*mt_guest_fn)(mt_cpu *MT_RESTRICT cpu);

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_CPU_H */
