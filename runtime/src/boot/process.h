/* The process object behind maplethief/boot.h, shared by the files of runtime/src/boot/.
 *
 * process.c holds what guest code reaches while it runs (the current process, the boot
 * trace, __cxa_atexit, exit/stop/trap and the HLE functions of this module) and needs only
 * the dispatch table and the import code, so that the recompiler's C test harness can link
 * it through registry.cpp. boot.c holds loading and the start-up sequence. */
#ifndef MAPLETHIEF_RUNTIME_BOOT_PROCESS_H
#define MAPLETHIEF_RUNTIME_BOOT_PROCESS_H

#include <setjmp.h>
#include <stdint.h>

#include "maplethief/boot.h"

MT_EXTERN_C_BEGIN

enum mt_boot_phase {
    MT_PHASE_LOADING = 0,
    MT_PHASE_CONSTRUCTORS = 1,
    MT_PHASE_MAIN = 2, /* main (or the entry) is running */
    MT_PHASE_DONE = 3,
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) /* x64's jmp_buf is 16-byte aligned: the padding is expected */
#endif

struct mt_process {
    mt_boot_config cfg;
    mt_program prog;
    mt_cpu cpu; /* the main thread */
    mt_mem *mem;
    mt_objc *objc;
    mt_image_info image;
    mt_process_args args;
    uint32_t *import_addrs; /* prog.import_count */
    mt_hle_entry *hle;      /* cfg.hle, then the registry */
    char *text;             /* host strings the boot composed (the default environment) */
    uint32_t hle_count;
    uint32_t phase; /* enum mt_boot_phase */
    mt_process_result result;
    mt_milestone *milestones;
    uint32_t milestone_count, milestone_cap;
    mt_atexit_entry *handlers; /* __cxa_atexit registrations */
    uint32_t handler_count, handler_cap;
    uint32_t exiting;         /* exit is running the handlers */
    uint8_t image_sha256[32]; /* the armv7 slice's SHA-256 (save states, #202) */
    int running;              /* mt_process_run is on the host stack: `jmp` is valid */
    jmp_buf jmp;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* Sets or clears the current process (boot.c, at load and destroy). */
void mt_process_set_current(mt_process *p) MT_NOEXCEPT;

/* Replaces the __cxa_atexit registrations (a save state's restore, #202). Returns 0, or -1 if
 * out of memory (nothing changed). */
int mt_process_set_atexit_entries(mt_process *p, const mt_atexit_entry *entries,
                                  uint32_t count) MT_NOEXCEPT;

/* Logs one line through p->cfg.imports.log, or stderr. */
void mt_process_log(const mt_process *p, const char *line) MT_NOEXCEPT;

/* Calls guest function `fn` with r0-r3 = args[0..3] and, if `with_fifth`, args[4] at [sp] (a
 * 16-byte aligned area below sp); lr = MT_HLE_RETURN. Restores sp and lr afterwards. */
void mt_process_call_guest(mt_cpu *cpu, uint32_t fn, const uint32_t args[5],
                           int with_fifth) MT_NOEXCEPT;

/* HLE functions (mt_import_fn), listed in runtime/src/hle/registry.cpp. Without a current
 * process each one behaves like an unimplemented import. */
void mt_hle_cxa_atexit(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_exit(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
void mt_hle_UIApplicationMain(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

/* The Objective-C steps of the start-up (objc_boot.c), in the order boot.c calls them. Each
 * returns 0, or -1 if the boot cannot go on (logged). */
int mt_boot_objc_create(mt_process *p) MT_NOEXCEPT;    /* step 3, before place */
int mt_boot_objc_image(mt_process *p) MT_NOEXCEPT;     /* step 6, after the binds */
int mt_boot_objc_overrides(mt_process *p) MT_NOEXCEPT; /* step 7, then mt_objc_set_current */

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RUNTIME_BOOT_PROCESS_H */
