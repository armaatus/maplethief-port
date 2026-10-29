/* Two functions shaped like the recompiler's output, compiled with the function-entry
 * coverage hook on (MT_TRACE_ENTRY 1, ADR 0030), for tests/cpp/diag_test.cpp. */
#define MT_TRACE_ENTRY 1
#include "maplethief/recomp/recomp.h"

void f_000225E5(mt_cpu *MT_RESTRICT cpu);
void f_00002001(mt_cpu *MT_RESTRICT cpu);

void f_000225E5(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_ENTER();
    cpu->r[0] += 1u;
}

void f_00002001(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_ENTER();
    cpu->r[0] += 1u;
}
