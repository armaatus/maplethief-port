/* The runtime's mt_trap (maplethief/recomp/calls.h): a guest trap ends the running process
 * (mt_process_trap: logged, milestone "trap", mt_process_run returns MT_PROCESS_TRAPPED).
 * With no process running there is nothing to return to, so it logs and aborts the host.
 *
 * Alone in its file on purpose: a test executable that defines its own mt_trap links the
 * runtime library without pulling this object in. */
#include <stdio.h>
#include <stdlib.h>

#include "maplethief/boot.h"
#include "maplethief/diag.h"

void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    /* The crash report (registers, symbolized guest backtrace; #53, ADR 0030) has to come
     * first: mt_process_trap does not return while a process runs. */
    mt_crash_report_trap(cpu, kind, pc, detail);
    mt_process_trap(cpu, kind, pc, detail); /* does not return while a process runs */
    fprintf(stderr,
            "guest trap kind %u at pc 0x%08X (detail 0x%08X, lr 0x%08X) with no running guest "
            "process: aborting\n",
            (unsigned)kind, (unsigned)pc, (unsigned)detail, (unsigned)cpu->r[MT_LR]);
    fflush(stderr);
    abort();
}
