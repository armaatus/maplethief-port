/* Process start-up (maplethief/boot.h, ADR 0027): loading the image the way the kernel and
 * dyld did, then the static constructors and main the way dyld and crt1 did. The steps are
 * listed in boot.h; each is one function below and one milestone of the boot trace. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/process.h"
#include "maplethief/sjlj.h"

#define S_MOD_INIT_FUNC_POINTERS 0x9u
#define SECTION_TYPE 0xFFu

mt_boot_config mt_boot_config_default(void) {
    mt_boot_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.imports = mt_import_config_default();
    cfg.reservation = MT_MEM_RESERVE_AUTO;
    return cfg;
}

static int failed(const mt_process *p, const char *step, const char *why) {
    char line[256];
    snprintf(line, sizeof line, "boot: %s failed: %s", step, why);
    mt_process_log(p, line);
    return -1;
}

/* Guest word access for the loader: always checked, never traps (the guest is not running). */
static int get32(const mt_process *p, uint32_t addr, uint32_t *out) {
    if (addr < MT_GUEST_PAGE_ZERO_END || (uint64_t)addr + 4u > p->cpu.mem_size) {
        return -1;
    }
    memcpy(out, p->cpu.mem + addr, 4);
    return 0;
}

static int put32(mt_process *p, uint32_t addr, uint32_t value) {
    if (addr < MT_GUEST_PAGE_ZERO_END || (uint64_t)addr + 4u > p->cpu.mem_size) {
        return -1;
    }
    memcpy(p->cpu.mem + addr, &value, 4);
    return 0;
}

/* --- 1. image ------------------------------------------------------------------------------ */

static int step_image(mt_process *p, const uint8_t *macho, size_t size) {
    mt_mem_config mc = mt_mem_config_default();
    uint32_t nseg;
    char detail[200];
    if (mt_image_info_read(macho, size, &p->image) != 0) {
        return failed(p, "image", "not a Mach-O the boot can load (see the line above)");
    }
    if (p->image.rebase_size != 0u) {
        /* ADR 0025: the image runs at its link-time addresses; the game's has no rebases. */
        return failed(p, "image", "the image has rebase opcodes; only slide 0 is supported");
    }
    mc.reservation = p->cfg.reservation;
    p->mem = mt_mem_create(&mc);
    if (p->mem == NULL) {
        return failed(p, "image", "cannot reserve guest memory");
    }
    if (mt_mem_map_macho(p->mem, macho, size) != 0) {
        return failed(p, "image", "cannot map the segments");
    }
    mt_mem_attach(p->mem, &p->cpu);
    p->cpu.fpscr = MT_FPSCR_IOS; /* ADR 0022 */
    mt_mem_segments(p->mem, &nseg);
    if (p->image.main != 0u) {
        snprintf(detail, sizeof detail,
                 "%u segments mapped, %u sections, entry 0x%08X (crt1 start, main 0x%08X)",
                 (unsigned)nseg, (unsigned)p->image.section_count, (unsigned)p->image.entry,
                 (unsigned)p->image.main);
    } else {
        snprintf(detail, sizeof detail, "%u segments mapped, %u sections, entry 0x%08X",
                 (unsigned)nseg, (unsigned)p->image.section_count, (unsigned)p->image.entry);
    }
    mt_process_milestone("image", p->image.entry, detail);
    return 0;
}

/* --- 2. link, 4. place, 5. binds ------------------------------------------------------------ */

static int step_link(mt_process *p) {
    uint32_t n_registry, unimplemented;
    const mt_hle_entry *registry = mt_hle_registry(&n_registry);
    char detail[160];
    p->hle_count = p->cfg.hle_count + n_registry;
    p->hle = (mt_hle_entry *)malloc((size_t)(p->hle_count + 1u) * sizeof *p->hle);
    p->import_addrs = (uint32_t *)calloc((size_t)p->prog.import_count + 1u, sizeof(uint32_t));
    if (p->hle == NULL || p->import_addrs == NULL) {
        return failed(p, "link", "out of host memory");
    }
    if (p->cfg.hle_count != 0u) {
        memcpy(p->hle, p->cfg.hle, (size_t)p->cfg.hle_count * sizeof *p->hle);
    }
    memcpy(p->hle + p->cfg.hle_count, registry, (size_t)n_registry * sizeof *p->hle);
    unimplemented = mt_imports_link(p->prog.import_descs, p->prog.import_count,
                                    p->prog.import_table, p->hle, p->hle_count, &p->cfg.imports);
    snprintf(detail, sizeof detail, "%u imports, %u function imports left to the default handler",
             (unsigned)p->prog.import_count, (unsigned)unimplemented);
    mt_process_milestone("link", unimplemented, detail);
    return 0;
}

static int step_place(mt_process *p) {
    uint32_t id, data = 0;
    char detail[96];
    if (mt_imports_place(&p->cpu, p->prog.import_descs, p->prog.import_count, p->hle, p->hle_count,
                         mt_mem_guest_alloc, p->mem, p->import_addrs) != 0) {
        return failed(p, "place", "a data import does not fit in guest memory");
    }
    for (id = 0; id < p->prog.import_count; id++) {
        data += (p->prog.import_descs[id].flags & MT_IMPORT_DATA) != 0u ? 1u : 0u;
    }
    snprintf(detail, sizeof detail, "%u data imports placed on the guest heap", (unsigned)data);
    mt_process_milestone("place", data, detail);
    return 0;
}

static int step_binds(mt_process *p) {
    mt_sjlj_state *sjlj = mt_sjlj_state_of(&p->cpu);
    char detail[96];
    if (mt_imports_apply_binds(&p->cpu, p->prog.binds, p->prog.bind_count, p->import_addrs,
                               p->prog.import_count) != 0) {
        return failed(p, "binds", "a bind's slot is outside guest memory or its id is unknown");
    }
    mt_sjlj_init(sjlj);
    mt_sjlj_bind_personalities(sjlj, p->prog.import_descs, p->prog.import_count);
    snprintf(detail, sizeof detail, "%u binds applied", (unsigned)p->prog.bind_count);
    mt_process_milestone("binds", p->prog.bind_count, detail);
    return 0;
}

/* --- 8. seal, 9. stack, 10. program vars ---------------------------------------------------- */

static int step_seal(mt_process *p) {
    if (mt_mem_seal_image(p->mem) != 0) {
        return failed(p, "seal", "cannot protect the image");
    }
    mt_process_milestone("seal", 0u, "__TEXT read-only");
    return 0;
}

/* The sandbox directory: the executable's path without its last two components
 * (".../<UUID>/MapleThief.app/MapleThief" -> ".../<UUID>"). */
static size_t home_length(const char *exe) {
    size_t n = strlen(exe), cut = 0;
    while (n > 0u && cut < 2u) {
        n--;
        if (exe[n] == '/') {
            cut++;
        }
    }
    return cut == 2u ? n : 0u;
}

/* The default environment: what an iOS 4/5 app saw (PATH, TMPDIR, SHELL, HOME, USER, LOGNAME,
 * CFFIXED_USER_HOME), under the sandbox directory. Composed into p->text. */
static int default_env(mt_process *p, const char *exe, const char *env[8]) {
    static const struct {
        const char *before;
        int home; /* the sandbox directory goes between `before` and `after` */
        const char *after;
    } k_env[7] = {
        {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0, ""},
        {"TMPDIR=", 1, "/tmp/"},
        {"SHELL=/bin/sh", 0, ""},
        {"HOME=", 1, ""},
        {"USER=mobile", 0, ""},
        {"LOGNAME=mobile", 0, ""},
        {"CFFIXED_USER_HOME=", 1, ""},
    };
    const int home = (int)home_length(exe);
    size_t size = 0, at = 0;
    int i;
    for (i = 0; i < 7; i++) {
        size += strlen(k_env[i].before) + (size_t)home + strlen(k_env[i].after) + 1u;
    }
    p->text = (char *)malloc(size);
    if (p->text == NULL) {
        return -1;
    }
    for (i = 0; i < 7; i++) {
        const int n = snprintf(p->text + at, size - at, "%s%.*s%s", k_env[i].before,
                               k_env[i].home ? home : 0, exe, k_env[i].after);
        env[i] = p->text + at;
        at += (size_t)n + 1u;
    }
    env[7] = NULL;
    return 0;
}

static uint32_t count_strings(const char *const *list) {
    uint32_t n = 0;
    while (list != NULL && list[n] != NULL) {
        n++;
    }
    return n;
}

/* Copies `s` (with its NUL) to guest address *at and advances it; returns the string's guest
 * address. */
static uint32_t put_string(mt_process *p, uint32_t *at, const char *s) {
    const uint32_t addr = *at, len = (uint32_t)strlen(s) + 1u;
    memcpy(p->cpu.mem + addr, s, len);
    *at += len;
    return addr;
}

static int step_stack(mt_process *p) {
    const char *exe =
        p->cfg.executable_path != NULL ? p->cfg.executable_path : MT_BOOT_DEFAULT_EXECUTABLE;
    const char *defaults[8];
    const char *const *env = p->cfg.env;
    uint32_t nargs = count_strings(p->cfg.args), nenv, words, i, strings, at, sp, argv0 = 0;
    uint64_t text = strlen(exe) * 2u + 2u;
    mt_process_args *a = &p->args;
    char detail[200];
    if (env == NULL) {
        if (default_env(p, exe, defaults) != 0) {
            return failed(p, "stack", "out of host memory");
        }
        env = defaults;
    }
    nenv = count_strings(env);
    for (i = 0; i < nargs; i++) {
        text += strlen(p->cfg.args[i]) + 1u;
    }
    for (i = 0; i < nenv; i++) {
        text += strlen(env[i]) + 1u;
    }
    if (mt_stack_create(p->mem, MT_STACK_MAIN_SIZE, &a->stack) != 0) {
        return failed(p, "stack", "no room for the main stack");
    }
    /* argc, argv[0..argc), 0, envp[0..nenv), 0, apple[0], 0 */
    words = 1u + (1u + nargs) + 1u + nenv + 1u + 1u + 1u;
    if (text + 4u * (uint64_t)words + 64u > MT_STACK_MAIN_SIZE / 4u) {
        return failed(p, "stack", "the arguments and environment are too large");
    }
    strings = (a->stack.top - (uint32_t)text) & ~15u;
    sp = (strings - 4u * words) & ~15u;
    a->sp = sp;
    a->argc = (int32_t)(1u + nargs);
    a->argv = sp + 4u;
    a->envp = a->argv + 4u * (1u + nargs + 1u);
    a->apple = a->envp + 4u * (nenv + 1u);
    at = strings;
    put32(p, sp, (uint32_t)a->argc);
    argv0 = put_string(p, &at, exe);
    put32(p, a->argv, argv0);
    for (i = 0; i < nargs; i++) {
        put32(p, a->argv + 4u * (1u + i), put_string(p, &at, p->cfg.args[i]));
    }
    put32(p, a->argv + 4u * (1u + nargs), 0u);
    for (i = 0; i < nenv; i++) {
        put32(p, a->envp + 4u * i, put_string(p, &at, env[i]));
    }
    put32(p, a->envp + 4u * nenv, 0u);
    put32(p, a->apple, put_string(p, &at, exe)); /* apple[0]: the executable path */
    put32(p, a->apple + 4u, 0u);
    p->cpu.r[MT_SP] = sp;
    snprintf(detail, sizeof detail,
             "main stack 0x%08X-0x%08X, sp 0x%08X: argc %d, argv 0x%08X, envp 0x%08X (%u), "
             "apple 0x%08X",
             (unsigned)a->stack.base, (unsigned)a->stack.top, (unsigned)sp, (int)a->argc,
             (unsigned)a->argv, (unsigned)a->envp, (unsigned)nenv, (unsigned)a->apple);
    mt_process_milestone("stack", sp, detail);
    return 0;
}

/* dyld's ProgramVars: { mh, &NXArgc, &NXArgv, &environ, &__progname } in __program_vars. */
static int step_program_vars(mt_process *p) {
    const mt_section *s = mt_image_section(&p->image, "__DATA", "__program_vars");
    mt_process_args *a = &p->args;
    uint32_t vars[5], i, argv0 = 0, progname;
    char detail[240];
    if (s == NULL) {
        mt_process_milestone("program-vars", 0u, "the image has no __program_vars");
        return 0;
    }
    if (s->size < 20u) {
        return failed(p, "program-vars", "__program_vars is smaller than 5 words");
    }
    for (i = 0; i < 5u; i++) {
        if (get32(p, s->addr + 4u * i, &vars[i]) != 0) {
            return failed(p, "program-vars", "__program_vars is not mapped");
        }
    }
    if (vars[0] != p->image.mach_header) {
        return failed(p, "program-vars", "its mach header pointer is not the image's");
    }
    if (get32(p, a->argv, &argv0) != 0) {
        return failed(p, "program-vars", "argv is not in guest memory");
    }
    progname = argv0;
    for (i = argv0; p->cpu.mem[i] != 0; i++) {
        if (p->cpu.mem[i] == '/') {
            progname = i + 1u;
        }
    }
    if (put32(p, vars[1], (uint32_t)a->argc) != 0 || put32(p, vars[2], a->argv) != 0 ||
        put32(p, vars[3], a->envp) != 0 || put32(p, vars[4], progname) != 0) {
        return failed(p, "program-vars", "a variable is outside guest memory");
    }
    a->program_vars = s->addr;
    snprintf(detail, sizeof detail,
             "NXArgc 0x%08X = %d, NXArgv 0x%08X = 0x%08X, environ 0x%08X = 0x%08X, "
             "__progname 0x%08X = \"%s\"",
             (unsigned)vars[1], (int)a->argc, (unsigned)vars[2], (unsigned)a->argv,
             (unsigned)vars[3], (unsigned)a->envp, (unsigned)vars[4],
             (const char *)p->cpu.mem + progname);
    mt_process_milestone("program-vars", s->addr, detail);
    return 0;
}

/* --- the process ----------------------------------------------------------------------------- */

mt_process *mt_process_load(const mt_boot_config *cfg, const mt_program *prog, const uint8_t *macho,
                            size_t size) {
    mt_process *p;
    if (mt_process_current() != NULL) {
        mt_process_log(NULL, "boot: a guest process is already loaded");
        return NULL;
    }
    p = (mt_process *)calloc(1, sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->cfg = *cfg;
    p->prog = *prog;
    p->result.outcome = MT_PROCESS_LOADED;
    mt_process_set_current(p);
    if (step_image(p, macho, size) != 0 || step_link(p) != 0 || mt_boot_objc_create(p) != 0 ||
        step_place(p) != 0 || step_binds(p) != 0 || mt_boot_objc_image(p) != 0 ||
        mt_boot_objc_overrides(p) != 0 || step_seal(p) != 0 || step_stack(p) != 0 ||
        step_program_vars(p) != 0) {
        mt_process_destroy(p);
        return NULL;
    }
    return p;
}

/* --- 11. constructors, 12. main ----------------------------------------------------------- */

static void run_constructors(mt_process *p) {
    const mt_process_args *a = &p->args;
    uint32_t i, k, total = 0, n = 0;
    char detail[96];
    p->phase = MT_PHASE_CONSTRUCTORS;
    for (i = 0; i < p->image.section_count; i++) {
        if ((p->image.sections[i].flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS) {
            total += p->image.sections[i].size / 4u;
        }
    }
    for (i = 0; i < p->image.section_count; i++) {
        const mt_section *s = &p->image.sections[i];
        if ((s->flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS) {
            continue;
        }
        for (k = 0; k < s->size / 4u; k++) {
            /* dyld calls each with (argc, argv, envp, apple, &programVars) */
            uint32_t args[5], fn = 0;
            args[0] = (uint32_t)a->argc;
            args[1] = a->argv;
            args[2] = a->envp;
            args[3] = a->apple;
            args[4] = a->program_vars;
            get32(p, s->addr + 4u * k, &fn);
            snprintf(detail, sizeof detail, "0x%08X (%u of %u)", (unsigned)fn, (unsigned)++n,
                     (unsigned)total);
            mt_process_milestone("constructor", fn, detail);
            p->cpu.r[MT_SP] = a->sp;
            mt_process_call_guest(&p->cpu, fn, args, 1);
        }
    }
}

/* The id of the function import named `name`, or prog.import_count. */
static uint32_t import_named(const mt_process *p, const char *name) {
    uint32_t id;
    for (id = 0; id < p->prog.import_count; id++) {
        if ((p->prog.import_descs[id].flags & MT_IMPORT_FUNCTION) != 0u &&
            strcmp(p->prog.import_descs[id].name, name) == 0) {
            return id;
        }
    }
    return p->prog.import_count;
}

MT_NORETURN static void run_main(mt_process *p) {
    mt_cpu *cpu = &p->cpu;
    const mt_process_args *a = &p->args;
    const mt_image_info *im = &p->image;
    uint32_t i, exit_id;
    char detail[200];
    p->phase = MT_PHASE_MAIN;
    for (i = 0; i < 13u; i++) {
        cpu->r[i] = 0u;
    }
    cpu->r[MT_LR] = 0u;
    cpu->r[MT_SP] = a->sp;
    if (im->main == 0u) {
        /* Not crt1's stub: run the entry as the kernel would, with sp at argc. */
        const uint32_t entry = im->entry | ((im->entry_cpsr & MT_CPSR_T) != 0u ? 1u : 0u);
        snprintf(detail, sizeof detail, "0x%08X (sp 0x%08X)", (unsigned)entry, (unsigned)a->sp);
        mt_process_milestone("entry", entry, detail);
        mt_dispatch(cpu, entry, entry & ~1u)(cpu);
        mt_process_exit(cpu, (int32_t)cpu->r[0]);
    }
    /* What crt1's start does before its blx: argc, argv, envp, apple in r0-r3, sp aligned to
     * 8; the return address is its `b exit` right after the blx. */
    cpu->r[0] = (uint32_t)a->argc;
    cpu->r[1] = a->argv;
    cpu->r[2] = a->envp;
    cpu->r[3] = a->apple;
    cpu->r[MT_SP] = a->sp & ~7u;
    cpu->r[MT_LR] = im->entry + 40u;
    snprintf(detail, sizeof detail,
             "0x%08X (argc %d, argv 0x%08X, envp 0x%08X, apple 0x%08X), called as by crt1 "
             "start 0x%08X",
             (unsigned)im->main, (int)a->argc, (unsigned)a->argv, (unsigned)a->envp,
             (unsigned)a->apple, (unsigned)im->entry);
    mt_process_milestone("main", im->main, detail);
    mt_dispatch(cpu, im->main, im->entry + 36u)(cpu);
    /* start's `b exit`: exit(main's result) through the guest's own import */
    snprintf(detail, sizeof detail, "main returned %d", (int)cpu->r[0]);
    mt_process_milestone("main-returned", cpu->r[0], detail);
    exit_id = import_named(p, "_exit");
    if (exit_id < p->prog.import_count) {
        cpu->r[MT_LR] = 0u;
        p->prog.import_table[exit_id](cpu, exit_id);
    }
    /* exit is unimplemented and the policy let it return */
    mt_process_exit(cpu, (int32_t)cpu->r[0]);
}

uint32_t mt_process_run(mt_process *p) {
    if (p->phase != MT_PHASE_LOADING) {
        return p->result.outcome;
    }
    p->running = 1;
    if (MT_HOST_SETJMP(p->jmp) == 0) {
        run_constructors(p);
        run_main(p);
    }
    p->running = 0;
    return p->result.outcome;
}

void mt_process_destroy(mt_process *p) {
    if (p == NULL) {
        return;
    }
    if (mt_process_current() == p) {
        mt_process_set_current(NULL);
    }
    if (p->objc != NULL) {
        if (mt_objc_current() == p->objc) {
            mt_objc_set_current(NULL);
        }
        mt_objc_destroy(p->objc);
    }
    mt_mem_destroy(p->mem);
    free(p->import_addrs);
    free(p->hle);
    free(p->text);
    free(p->milestones);
    free(p->handlers);
    free(p);
}

const mt_process_result *mt_process_result_of(const mt_process *p) { return &p->result; }
mt_cpu *mt_process_cpu(mt_process *p) { return &p->cpu; }
mt_mem *mt_process_mem(mt_process *p) { return p->mem; }
mt_objc *mt_process_objc(mt_process *p) { return p != NULL ? p->objc : NULL; }
const mt_image_info *mt_process_image(const mt_process *p) { return &p->image; }
const mt_process_args *mt_process_args_of(const mt_process *p) { return &p->args; }

uint32_t mt_process_import_addr(const mt_process *p, uint32_t id) {
    return p->import_addrs != NULL && id < p->prog.import_count ? p->import_addrs[id] : 0u;
}
