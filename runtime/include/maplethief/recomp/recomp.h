/* The one header every generated translation unit includes (work/gen/mt_gen.h includes it):
 * the contract between recompiled code and the runtime (ADR 0014).
 *
 *   compiler.h  portability macros (restrict, noreturn, guaranteed tail calls) and the
 *               host assumptions, checked at compile time
 *   cpu.h       mt_cpu, the guest register state
 *   mem.h       guest memory access (4 GiB reservation or bounds-checked window)
 *   alu.h       integer helpers (shifts, sign extension, bit operations)
 *   vfp.h       VFP register access and IEEE arithmetic with ARM NaN rules
 *   calls.h     calls, returns, indirect dispatch, imports, traps, SjLj hooks
 *   imports.h   the generated import tables: descriptors, binds, thunks (ADR 0019)
 */
#ifndef MAPLETHIEF_RECOMP_RECOMP_H
#define MAPLETHIEF_RECOMP_RECOMP_H

#include "maplethief/recomp/compiler.h"

#include "maplethief/recomp/alu.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/recomp/cpu.h"
#include "maplethief/recomp/imports.h"
#include "maplethief/recomp/mem.h"
#include "maplethief/recomp/vfp.h"

#endif /* MAPLETHIEF_RECOMP_RECOMP_H */
