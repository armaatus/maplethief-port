/* Three functions shaped like the recompiler's output, compiled with the function-entry
 * coverage hook on (MT_TRACE_ENTRY 1, ADR 0030), for tests/cpp/diag_test.cpp. f_00030001 is
 * only entered by the run coverage's watcher test (#295, ADR 0160): its first entry must come
 * after the watcher's registration. */
#define MT_TRACE_ENTRY 1
#include "maplethief/recomp/recomp.h"

void f_000225E5(mt_cpu *MT_RESTRICT cpu);
void f_00002001(mt_cpu *MT_RESTRICT cpu);
void f_00030001(mt_cpu *MT_RESTRICT cpu);

void f_000225E5(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_ENTER();
    cpu->r[0] += 1u;
}

void f_00002001(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_ENTER();
    cpu->r[0] += 1u;
}

void f_00030001(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_ENTER();
    cpu->r[0] += 1u;
}
