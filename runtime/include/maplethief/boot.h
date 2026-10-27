/* Process start-up: what the kernel, dyld and crt1 did for the game (#43, ADR 0027).
 *
 * One guest process is one mt_process. Loading it does, in this order, what dyld did before
 * it jumped to the entry point; running it does what crt1 and the static constructors did:
 *
 *   mt_process_load
 *     1. image      map the thin armv7 Mach-O's segments (maplethief/memory.h) and read its
 *                   sections and LC_UNIXTHREAD entry point from the same file; the image
 *                   must need no rebasing (this one has no rebase opcodes)
 *     2. link       fill the import table by name: cfg->hle first, then the runtime's
 *                   registry (maplethief/hle/imports.h)
 *     3. objc       create the Objective-C runtime (maplethief/objc/objc.h), so that the
 *                   data-import init hooks of step 4 can register host classes with it
 *                   (mt_process_objc)
 *     4. place      give every import a guest address; data imports get zeroed objects on
 *                   the guest heap and their init hooks run
 *     5. bind       write the 1,268 bound pointers; tell the SjLj unwinder the personalities
 *     6. objc-image realize the image's classes and categories from its __objc_* sections
 *     7. overrides  install the host overrides (ADR 0026), then make the runtime current
 *     8. seal       make __TEXT read-only (mt_mem_seal_image)
 *     9. stack      the main thread's guest stack, with the kernel's layout at its top:
 *                   argc, argv[], 0, envp[], 0, apple[], 0, then the strings
 *    10. vars       __DATA,__program_vars: NXArgc, NXArgv, environ and __progname
 *
 *   mt_process_run
 *    11. constructors  each __mod_init_func entry, with dyld's arguments
 *                      (argc, argv, envp, apple, &program vars)
 *    12. main       if the entry is crt1's start stub, call main the way start does, then
 *                   exit(main's result) through the guest's _exit import; any other entry
 *                   runs as it is
 *
 * Every step is a milestone of the boot trace: one log line ("boot: <name>: <detail>",
 * through cfg->imports.log) and an entry of mt_process_milestones. Other modules add theirs
 * with mt_process_milestone (UIApplicationMain here; the app delegate and the first GL frame
 * in #54/#117).
 *
 * The guest ends in one of three ways, each a host longjmp back to mt_process_run:
 *   exit   _exit (or main returning): the __cxa_atexit handlers run in reverse order, then
 *          mt_process_run returns MT_PROCESS_EXITED with the status
 *   stop   mt_process_stop, e.g. UIApplicationMain with no app lifecycle hook installed:
 *          MT_PROCESS_STOPPED with the reason
 *   trap   mt_trap (the runtime's, runtime/src/boot/trap.c): MT_PROCESS_TRAPPED with the
 *          trap's kind, pc and detail
 * The process and its guest memory stay readable until mt_process_destroy.
 *
 * Usage (the program links the generated code, maplethief_gen, which includes
 * runtime/src/recomp/dispatch.c):
 *
 *     mt_program prog = MT_PROGRAM_GENERATED;
 *     mt_boot_config cfg = mt_boot_config_default();
 *     cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;   // in a unit that sees MT_MEM_CHECKED
 *     mt_import_config_from_env(&cfg.imports);
 *     cfg.app_main = my_ui_application_main;        // the app lifecycle (#54)
 *     mt_process *p = mt_process_load(&cfg, &prog, slice, slice_size);
 *     if (p != NULL) {
 *         uint32_t outcome = mt_process_run(p);
 *         ... mt_process_result(p) ...
 *         mt_process_destroy(p);
 *     }
 *
 * One process at a time (the guest is single-threaded, ADR 0014): mt_process_current is the
 * loaded one. C17 that also compiles as C++20; the implementation is runtime/src/boot/. */
#ifndef MAPLETHIEF_BOOT_H
#define MAPLETHIEF_BOOT_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- the program ------------------------------------------------------------------------ */

/* The generated tables the boot needs (maplethief/recomp/imports.h). */
typedef struct mt_program {
    const mt_import_desc *import_descs;
    uint32_t import_count;
    mt_import_fn *import_table;
    const mt_import_bind *binds;
    uint32_t bind_count;
} mt_program;

/* An initializer for the generated program's tables, in a unit that links maplethief_gen. */
#define MT_PROGRAM_GENERATED                                                                       \
    {mt_import_descs, mt_import_count, mt_import_table, mt_import_binds, mt_import_bind_count}

/* --- the image ---------------------------------------------------------------------------- */

typedef struct mt_section {
    char segname[17]; /* NUL-terminated */
    char sectname[17];
    uint32_t addr;
    uint32_t size;
    uint32_t flags; /* section type in the low byte (9: S_MOD_INIT_FUNC_POINTERS) */
} mt_section;

#define MT_MAX_SECTIONS 64u

/* What the boot reads from the Mach-O besides its segments. */
typedef struct mt_image_info {
    uint32_t entry;       /* LC_UNIXTHREAD pc (ARM: bit 0 clear) */
    uint32_t entry_cpsr;  /* LC_UNIXTHREAD cpsr (T bit 0x20 for a Thumb entry) */
    uint32_t mach_header; /* guest address of the Mach header: the segment at file offset 0 */
    uint32_t rebase_size; /* bytes of rebase opcodes (LC_DYLD_INFO); the boot needs 0 */
    /* main, if the entry is crt1's start stub (Thumb bit set), else 0 */
    uint32_t main;
    mt_section sections[MT_MAX_SECTIONS];
    uint32_t section_count;
} mt_image_info;

/* Reads the sections, the LC_UNIXTHREAD entry and the rebase size of a thin 32-bit ARM
 * Mach-O, and recognises crt1's start stub at the entry (from the file's bytes). The file is
 * untrusted: every size and offset is checked. Returns 0, or -1 (logged to stderr) if it is
 * not such a Mach-O, has no LC_UNIXTHREAD or a check fails. */
int mt_image_info_read(const uint8_t *file, size_t size, mt_image_info *out) MT_NOEXCEPT;

/* The section named segname,sectname, or NULL. */
const mt_section *mt_image_section(const mt_image_info *info, const char *segname,
                                   const char *sectname) MT_NOEXCEPT;

/* --- configuration ------------------------------------------------------------------------ */

typedef struct mt_process mt_process;

/* What the guest passed to UIApplicationMain(argc, argv, principalClassName,
 * delegateClassName). The two names are guest NSString pointers (nil in this game). */
typedef struct mt_app_main_args {
    int32_t argc;
    uint32_t argv;
    uint32_t principal_class_name;
    uint32_t delegate_class_name;
    uint32_t caller; /* the guest return address (lr) */
} mt_app_main_args;

/* The app lifecycle: UIApplicationMain hands the guest to it. It runs the application (#54:
 * UIApplication, MainWindow.nib, the delegate, the run loop) and normally never returns: it
 * ends the guest with mt_process_exit or mt_process_stop. If it returns, the value is
 * UIApplicationMain's result in r0. */
typedef int32_t (*mt_app_main_fn)(mt_process *p, mt_cpu *cpu, const mt_app_main_args *args,
                                  void *user);

/* The guest path of the executable by default (argv[0] and apple[0]): the app's sandbox on
 * the device; the UUID directory is a fixed placeholder. */
#define MT_BOOT_DEFAULT_EXECUTABLE                                                                 \
    "/var/mobile/Applications/00000000-0000-0000-0000-000000000000/MapleThief.app/MapleThief"

typedef struct mt_boot_config {
    /* The unimplemented-import policy and the log sink, for the import table and the boot
     * trace alike (mt_import_config_default: stop, stderr). */
    mt_import_config imports;
    uint32_t reservation; /* enum mt_mem_reservation (maplethief/memory.h) */
    /* argv[0] and apple[0]; NULL: MT_BOOT_DEFAULT_EXECUTABLE */
    const char *executable_path;
    /* argv[1..], NULL-terminated; NULL: none */
    const char *const *args;
    /* envp, NULL-terminated; NULL: a small default environment (HOME, TMPDIR, PATH, ...)
     * under the executable's sandbox directory */
    const char *const *env;
    /* Implementations that take precedence over the runtime's registry, matched by name like
     * it (e.g. a test's stand-ins); NULL/0: none. Must outlive the process. */
    const mt_hle_entry *hle;
    uint32_t hle_count;
    /* UIApplicationMain's hook; NULL: UIApplicationMain stops the guest (MT_PROCESS_STOPPED) */
    mt_app_main_fn app_main;
    void *app_user;
} mt_boot_config;

/* Policy stop, stderr, MT_MEM_RESERVE_AUTO, the default executable path and environment, no
 * extra implementations, no app hook. */
mt_boot_config mt_boot_config_default(void) MT_NOEXCEPT;

/* --- the process ---------------------------------------------------------------------------- */

/* Steps 1-10 above on a copy of `cfg` (the strings it points at must outlive the process).
 * The file is the thin armv7 slice; it is only read during the call. Returns NULL, after
 * logging which step failed, if one does; there is then no current process. Fails if a
 * process is already loaded. */
mt_process *mt_process_load(const mt_boot_config *cfg, const mt_program *prog, const uint8_t *macho,
                            size_t size) MT_NOEXCEPT;

enum mt_process_outcome {
    MT_PROCESS_LOADED = 0,  /* loaded, not run yet */
    MT_PROCESS_EXITED = 1,  /* _exit / main returned: status in exit_status */
    MT_PROCESS_STOPPED = 2, /* mt_process_stop: stop_reason */
    MT_PROCESS_TRAPPED = 3, /* mt_trap: trap_kind, trap_pc, trap_detail */
};

typedef struct mt_process_result {
    uint32_t outcome; /* enum mt_process_outcome */
    int32_t exit_status;
    const char *stop_reason; /* static string */
    uint32_t trap_kind;      /* enum mt_trap_kind (maplethief/recomp/calls.h) */
    uint32_t trap_pc;
    uint32_t trap_detail;
} mt_process_result;

/* Steps 11-12: runs the guest until it exits, stops or traps, and returns the outcome (enum
 * mt_process_outcome). A process runs once; a second call returns its outcome unchanged. */
uint32_t mt_process_run(mt_process *p) MT_NOEXCEPT;

/* Frees the process, its guest memory and its ObjC runtime. NULL is ignored. */
void mt_process_destroy(mt_process *p) MT_NOEXCEPT;

/* The loaded process (between mt_process_load and mt_process_destroy), or NULL. */
mt_process *mt_process_current(void) MT_NOEXCEPT;

const mt_process_result *mt_process_result_of(const mt_process *p) MT_NOEXCEPT;
mt_cpu *mt_process_cpu(mt_process *p) MT_NOEXCEPT;
mt_mem *mt_process_mem(mt_process *p) MT_NOEXCEPT;
mt_objc *mt_process_objc(mt_process *p) MT_NOEXCEPT;
const mt_image_info *mt_process_image(const mt_process *p) MT_NOEXCEPT;
/* Guest address of import `id` (after step 4), or 0. */
uint32_t mt_process_import_addr(const mt_process *p, uint32_t id) MT_NOEXCEPT;

/* The guest's view of its start-up: where the kernel put argc/argv/envp/apple and their
 * values (guest addresses), and the main stack. */
typedef struct mt_process_args {
    uint32_t sp; /* initial sp: points at argc */
    int32_t argc;
    uint32_t argv, envp, apple;
    uint32_t program_vars; /* __DATA,__program_vars, 0 if the image has none */
    mt_stack stack;
} mt_process_args;

const mt_process_args *mt_process_args_of(const mt_process *p) MT_NOEXCEPT;

/* --- the boot trace ------------------------------------------------------------------------- */

typedef struct mt_milestone {
    const char *name; /* static string: "binds", "constructor", "main", "UIApplicationMain" */
    uint32_t value;   /* what it is about: a count, a guest address, a status */
} mt_milestone;

/* Records a milestone and logs "boot: <name>: <detail>" (detail may be NULL). `name` must be
 * a static string. Does nothing without a current process. */
void mt_process_milestone(const char *name, uint32_t value, const char *detail) MT_NOEXCEPT;

/* The milestones so far, in order. */
const mt_milestone *mt_process_milestones(const mt_process *p, uint32_t *count) MT_NOEXCEPT;

/* --- exit, stop, __cxa_atexit ----------------------------------------------------------- */

/* exit(status): runs the __cxa_atexit handlers that have not run (last registered first),
 * then ends the run with MT_PROCESS_EXITED. A second exit from inside a handler ends the run
 * at once. Without a running process it logs and aborts the host. */
MT_NORETURN void mt_process_exit(mt_cpu *cpu, int32_t status) MT_NOEXCEPT;

/* Ends the run with MT_PROCESS_STOPPED and `reason` (a static string); no handler runs.
 * Without a running process it logs and aborts the host. */
MT_NORETURN void mt_process_stop(mt_cpu *cpu, const char *reason) MT_NOEXCEPT;

/* Ends the run with MT_PROCESS_TRAPPED (logged). Returns 0 only if no process is running on
 * `cpu` (then the caller handles the trap itself). The runtime's mt_trap calls it. */
int mt_process_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) MT_NOEXCEPT;

/* One __cxa_atexit registration: func(arg) for the image `dso`. */
typedef struct mt_atexit_entry {
    uint32_t func;
    uint32_t arg;
    uint32_t dso;
    uint32_t done; /* 1 once it has run */
} mt_atexit_entry;

/* __cxa_finalize(dso): runs the handlers registered for `dso` (0: all) that have not run,
 * last registered first, each through the dispatch table. */
void mt_process_finalize(mt_cpu *cpu, uint32_t dso) MT_NOEXCEPT;

/* The registrations so far, in order. */
const mt_atexit_entry *mt_process_atexit_entries(const mt_process *p, uint32_t *count) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_BOOT_H */
