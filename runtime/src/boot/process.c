/* The running process (maplethief/boot.h, ADR 0027): the current process, the boot trace,
 * __cxa_atexit, exit / stop / trap, and the HLE functions of start-up. See process.h for why
 * this file is separate from boot.c. */
#include "boot/process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maplethief/hle/abi.h"

/* Set by boot.c between mt_process_load and mt_process_destroy; one guest thread (ADR 0014). */
static mt_process *g_current;

void mt_process_set_current(mt_process *p) { g_current = p; }

mt_process *mt_process_current(void) { return g_current; }

void mt_process_log(const mt_process *p, const char *line) {
    if (p != NULL && p->cfg.imports.log != NULL) {
        p->cfg.imports.log(p->cfg.imports.log_user, line);
        return;
    }
    fputs(line, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

/* A malloc'ed array with room for one more element: `items` itself if it has room, else a
 * bigger copy (and *cap grows), or NULL if the host is out of memory (`items` unchanged). */
static void *with_room(void *items, uint32_t *cap, uint32_t count, size_t size) {
    uint32_t n;
    void *bigger;
    if (count < *cap) {
        return items;
    }
    n = *cap != 0u ? *cap * 2u : 16u;
    bigger = realloc(items, (size_t)n * size);
    if (bigger != NULL) {
        *cap = n;
    }
    return bigger;
}

void mt_process_milestone(const char *name, uint32_t value, const char *detail) {
    mt_process *p = g_current;
    mt_milestone *m;
    char line[320];
    if (p == NULL) {
        return;
    }
    m = (mt_milestone *)with_room(p->milestones, &p->milestone_cap, p->milestone_count, sizeof *m);
    if (m != NULL) {
        p->milestones = m;
        p->milestones[p->milestone_count].name = name;
        p->milestones[p->milestone_count].value = value;
        p->milestone_count++;
    }
    if (detail != NULL) {
        snprintf(line, sizeof line, "boot: %s: %s", name, detail);
    } else {
        snprintf(line, sizeof line, "boot: %s: 0x%08X", name, (unsigned)value);
    }
    mt_process_log(p, line);
}

const mt_milestone *mt_process_milestones(const mt_process *p, uint32_t *count) {
    *count = p != NULL ? p->milestone_count : 0u;
    return p != NULL ? p->milestones : NULL;
}

const mt_atexit_entry *mt_process_atexit_entries(const mt_process *p, uint32_t *count) {
    *count = p != NULL ? p->handler_count : 0u;
    return p != NULL ? p->handlers : NULL;
}

void mt_process_call_guest(mt_cpu *cpu, uint32_t fn, const uint32_t args[5], int with_fifth) {
    const uint32_t sp = cpu->r[MT_SP], lr = cpu->r[MT_LR];
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        cpu->r[i] = args[i];
    }
    if (with_fifth) {
        /* The fifth argument at [sp], in a 16-byte aligned slot below the caller's frame. */
        const uint32_t at = (sp - 16u) & ~15u;
        memcpy(mt_g2h(cpu, at, 4u), &args[4], 4u);
        cpu->r[MT_SP] = at;
    }
    cpu->r[MT_LR] = MT_HLE_RETURN;
    mt_dispatch(cpu, fn, 0u)(cpu);
    cpu->r[MT_SP] = sp;
    cpu->r[MT_LR] = lr;
}

/* --- exit, stop, trap --------------------------------------------------------------------- */

/* Back to mt_process_run's setjmp (boot.c) with the result already recorded. */
MT_NORETURN static void end_run(mt_process *p) {
    p->phase = MT_PHASE_DONE;
    MT_HOST_LONGJMP(p->jmp, 1);
}

static mt_process *running_on(const mt_cpu *cpu) {
    mt_process *p = g_current;
    return p != NULL && p->running && &p->cpu == cpu ? p : NULL;
}

MT_NORETURN static void no_process(const char *what) {
    char line[160];
    snprintf(line, sizeof line, "%s with no running guest process: aborting", what);
    mt_process_log(g_current, line);
    abort();
}

void mt_process_finalize(mt_cpu *cpu, uint32_t dso) {
    mt_process *p = g_current;
    uint32_t i;
    if (p == NULL) {
        return;
    }
    /* Last registered first. A handler may register more; those run first, as with
     * __cxa_finalize, because the loop restarts from the end after every handler. */
    for (i = p->handler_count; i > 0u;) {
        mt_atexit_entry *e = &p->handlers[i - 1u];
        uint32_t args[5] = {0};
        if (e->done || (dso != 0u && e->dso != dso)) {
            i--;
            continue;
        }
        e->done = 1u;
        args[0] = e->arg;
        mt_process_call_guest(cpu, e->func, args, 0);
        i = p->handler_count;
    }
}

void mt_process_exit(mt_cpu *cpu, int32_t status) {
    mt_process *p = running_on(cpu);
    char detail[64];
    if (p == NULL) {
        no_process("exit");
    }
    if (!p->exiting) {
        p->exiting = 1u;
        snprintf(detail, sizeof detail, "exit(%d)", (int)status);
        mt_process_milestone("exit", (uint32_t)status, detail);
        mt_process_finalize(cpu, 0u);
    }
    p->result.outcome = MT_PROCESS_EXITED;
    p->result.exit_status = status;
    end_run(p);
}

void mt_process_stop(mt_cpu *cpu, const char *reason) {
    mt_process *p = running_on(cpu);
    if (p == NULL) {
        no_process("stop");
    }
    mt_process_milestone("stop", 0u, reason);
    p->result.outcome = MT_PROCESS_STOPPED;
    p->result.stop_reason = reason;
    end_run(p);
}

static const char *trap_name(uint32_t kind) {
    static const char *const names[] = {
        "?",
        "unsupported instruction",
        "bad return",
        "fell off a function",
        "unreachable",
        "unlifted function",
        "udf/bkpt/svc",
        "unimplemented import",
        "no function",
        "SjLj misuse",
        "uncaught exception",
        "std::terminate",
        "guest memory fault",
        "undeliverable message",
    };
    return kind < sizeof names / sizeof names[0] ? names[kind] : "?";
}

int mt_process_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    mt_process *p = running_on(cpu);
    char text[200];
    if (p == NULL) {
        return 0;
    }
    snprintf(text, sizeof text, "%s (kind %u) at pc 0x%08X, detail 0x%08X, lr 0x%08X",
             trap_name(kind), (unsigned)kind, (unsigned)pc, (unsigned)detail,
             (unsigned)cpu->r[MT_LR]);
    mt_process_milestone("trap", kind, text);
    p->result.outcome = MT_PROCESS_TRAPPED;
    p->result.trap_kind = kind;
    p->result.trap_pc = pc;
    p->result.trap_detail = detail;
    end_run(p);
}

/* --- HLE: libSystem and UIKit ----------------------------------------------------------------- */

/* int __cxa_atexit(void (*func)(void *), void *arg, void *dso) */
void mt_hle_cxa_atexit(mt_cpu *cpu, uint32_t id) {
    mt_process *p = running_on(cpu);
    mt_args a = mt_args_begin(cpu);
    mt_atexit_entry e, *list;
    if (p == NULL) {
        mt_import_unimplemented(cpu, id);
        return;
    }
    e.func = mt_arg_u32(&a);
    e.arg = mt_arg_u32(&a);
    e.dso = mt_arg_u32(&a);
    e.done = 0u;
    list = (mt_atexit_entry *)with_room(p->handlers, &p->handler_cap, p->handler_count, sizeof e);
    if (list == NULL) {
        mt_ret_u32(cpu, (uint32_t)-1);
        return;
    }
    p->handlers = list;
    p->handlers[p->handler_count++] = e;
    if (p->phase == MT_PHASE_CONSTRUCTORS) {
        char detail[96];
        snprintf(detail, sizeof detail, "__cxa_atexit(0x%08X, 0x%08X, 0x%08X)", (unsigned)e.func,
                 (unsigned)e.arg, (unsigned)e.dso);
        mt_process_milestone("atexit", e.func, detail);
    }
    mt_ret_u32(cpu, 0u);
}

/* void exit(int status) */
void mt_hle_exit(mt_cpu *cpu, uint32_t id) {
    if (running_on(cpu) == NULL) {
        mt_import_unimplemented(cpu, id);
        return;
    }
    mt_process_exit(cpu, (int32_t)cpu->r[0]);
}

/* int UIApplicationMain(int argc, char *argv[], NSString *principalClassName,
 *                       NSString *delegateClassName) */
void mt_hle_UIApplicationMain(mt_cpu *cpu, uint32_t id) {
    mt_process *p = running_on(cpu);
    mt_args a = mt_args_begin(cpu);
    mt_app_main_args args;
    char detail[160];
    if (p == NULL) {
        mt_import_unimplemented(cpu, id);
        return;
    }
    args.argc = mt_arg_i32(&a);
    args.argv = mt_arg_u32(&a);
    args.principal_class_name = mt_arg_u32(&a);
    args.delegate_class_name = mt_arg_u32(&a);
    args.caller = cpu->r[MT_LR];
    snprintf(detail, sizeof detail, "UIApplicationMain(%d, 0x%08X, 0x%08X, 0x%08X) from 0x%08X",
             (int)args.argc, (unsigned)args.argv, (unsigned)args.principal_class_name,
             (unsigned)args.delegate_class_name, (unsigned)args.caller);
    mt_process_milestone("UIApplicationMain", args.caller, detail);
    if (p->cfg.app_main == NULL) {
        mt_process_stop(cpu, "UIApplicationMain reached; no app lifecycle hook installed");
    }
    mt_ret_u32(cpu, (uint32_t)p->cfg.app_main(p, cpu, &args, p->cfg.app_user));
}
