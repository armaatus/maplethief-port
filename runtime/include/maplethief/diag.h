/* Diagnostics of the runtime (ADR 0030, issue #53): the HLE call trace, trace channels, the
 * unimplemented import/selector report, the guest symbol table, the function-entry
 * coverage of generated code and the guest crash reporter.
 *
 * The boot (maplethief/boot.h, ADR 0027) wires it up. mt_process_load calls
 * mt_diag_process_begin before it links the imports, so that the HLE trace sees the linked
 * table: it reads the environment, unless the host configured the diagnostics first with
 * mt_diag_configure, and routes the lines to the process's log sink. mt_process_run makes the
 * process's cpu the crash reporter's (mt_crash_set_cpu). Every end of a run (exit, stop, trap)
 * writes the report (mt_diag_finish). The runtime's mt_trap (boot/trap.c) prints the crash
 * report (mt_crash_report_trap). The host only adds mt_crash_install() for hardware faults.
 *
 * Environment (mt_diag_configure_from_env; every variable is optional):
 *
 *   MAPLETHIEF_TRACE_HLE=FILTERS  trace every HLE call whose import name matches FILTERS:
 *                                 comma-separated globs (* and ?), "!glob" excludes; the last
 *                                 matching pattern wins, e.g. "*,!_objc_msgSend*". A glob
 *                                 without a leading underscore also matches the name
 *                                 without it ("memcpy" matches "_memcpy").
 *   MAPLETHIEF_TRACE=CHANNELS     enable mt_tracef channels, comma-separated ("boot,objc";
 *                                 "all" or "*" for every channel). The channel "entry" logs
 *                                 the first entry of each guest function (MT_TRACE_ENTRY).
 *   MAPLETHIEF_REPORT=PATH        write the unimplemented report to PATH at exit, even when it
 *                                 is empty; "-" for stderr. Unset: stderr, only if not empty.
 *   MAPLETHIEF_COVERAGE=PATH      write the function-entry coverage (MT_TRACE_ENTRY builds)
 *                                 to PATH at exit.
 *   MAPLETHIEF_SYMBOLS=PATH       load the guest symbol table (python -m recomp symbols; the
 *                                 build writes <build dir>/maplethief.sym). Without one,
 *                                 addresses stay raw.
 *
 * The trace, channels and report write through the log function of the configuration
 * (default: stderr). The guest is single-threaded (ADR 0014): nothing here locks, except that
 * the crash reporter may run on any thread.
 *
 * C17 that also compiles as C++20. runtime/src/diag/diag.cpp and symbols.cpp are plain C++
 * without exceptions or a C++ runtime library (like the import code, ADR 0019), so the
 * recompiler's C test harness links them; crash.cpp is the runtime's only. */
#ifndef MAPLETHIEF_DIAG_H
#define MAPLETHIEF_DIAG_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/hle/imports.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* --- configuration -------------------------------------------------------------------- */

#define MT_TRACE_HLE_ENV "MAPLETHIEF_TRACE_HLE"
#define MT_TRACE_ENV "MAPLETHIEF_TRACE"
#define MT_REPORT_ENV "MAPLETHIEF_REPORT"
#define MT_COVERAGE_ENV "MAPLETHIEF_COVERAGE"
#define MT_SYMBOLS_ENV "MAPLETHIEF_SYMBOLS"

typedef struct mt_diag_config {
    const char *trace_hle;     /* HLE call trace filters; NULL or "" = off */
    const char *trace;         /* mt_tracef channels; NULL or "" = none */
    const char *report_path;   /* NULL: stderr if not empty; "-": stderr always; else a file */
    const char *coverage_path; /* NULL: no coverage file */
    const char *symbols_path;  /* NULL: keep the loaded symbol table (if any) */
    mt_log_fn log;             /* trace, channel and report lines; NULL = stderr */
    void *log_user;
} mt_diag_config;

/* Everything off, log to stderr. */
mt_diag_config mt_diag_config_default(void) MT_NOEXCEPT;

/* Replaces the configuration (the strings are copied). Loads symbols_path if set, and
 * registers mt_diag_finish with atexit once. Returns 0, or -1 if the symbol table could not
 * be loaded (logged; everything else still applies). Call before mt_imports_link. */
int mt_diag_configure(const mt_diag_config *cfg) MT_NOEXCEPT;

/* mt_diag_configure with the MAPLETHIEF_* variables above (unset ones stay off). */
int mt_diag_configure_from_env(void) MT_NOEXCEPT;

/* Writes the unimplemented report and the coverage file, per the configuration. Runs at exit
 * (atexit) and after a crash report; later calls do nothing until mt_diag_reset. */
void mt_diag_finish(void) MT_NOEXCEPT;

/* Called by the boot (mt_process_load, ADR 0027) for each guest process: configures the
 * diagnostics from the environment if nothing configured them yet, routes their lines to the
 * process's log (`log`, as mt_import_config.log: NULL = stderr) unless the configuration has
 * its own, and starts a new report, which the end of the run (exit, stop, trap) writes with
 * mt_diag_finish. */
void mt_diag_process_begin(mt_log_fn log, void *log_user) MT_NOEXCEPT;

/* Back to mt_diag_config_default, with an empty report, no coverage and no symbols. For
 * tests. Wrapped import slots keep pointing at the trampoline: link again. */
void mt_diag_reset(void) MT_NOEXCEPT;

/* --- trace channels ------------------------------------------------------------------- */

/* 1 if `channel` is enabled (MAPLETHIEF_TRACE). */
int mt_trace_enabled(const char *channel) MT_NOEXCEPT;

/* Logs "<channel>: <message>" if the channel is enabled. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void mt_tracef(const char *channel, const char *fmt, ...) MT_NOEXCEPT;

/* --- the HLE call trace --------------------------------------------------------------- */

/* 1 if import name `name` passes the filter list `filters` (syntax of MAPLETHIEF_TRACE_HLE).
 * An empty or NULL list matches nothing. */
int mt_trace_filter_match(const char *filters, const char *name) MT_NOEXCEPT;

/* Called by mt_imports_link after it filled `table`: if the HLE trace is on, remembers the
 * function of every function import whose name passes the filters and puts
 * mt_trace_import_call in its slot. Returns the number of slots wrapped (0 when off). */
uint32_t mt_trace_imports_wrap(const mt_import_desc *descs, uint32_t count,
                               mt_import_fn *table) MT_NOEXCEPT;

/* The trampoline of a traced slot: logs
 *     hle: _name(r0, r1, r2, r3) [sel] from 0xCALLER Symbol+0xOFF
 * calls the import's function, then logs "hle: _name -> r0 [r1]". A call that does not return
 * (a trap, an SjLj longjmp) has only its first line. */
void mt_trace_import_call(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;

/* --- the unimplemented report --------------------------------------------------------- */

/* Records a call of an import without an implementation (mt_import_unimplemented calls it):
 * unique by name; counts calls and keeps the first caller. */
void mt_report_unimplemented_import(const char *name, const char *library,
                                    uint32_t caller) MT_NOEXCEPT;

/* Records a message the ObjC runtime could not deliver: "-[Class sel]" (is_meta 0) or
 * "+[Class sel]". class_name NULL: a receiver of no known class. */
void mt_report_unimplemented_selector(const char *class_name, int is_meta, const char *selector,
                                      uint32_t caller) MT_NOEXCEPT;

typedef struct mt_report_stats {
    uint32_t imports;        /* unique imports */
    uint32_t import_calls;   /* their calls */
    uint32_t selectors;      /* unique (class, selector) pairs */
    uint32_t selector_calls; /* their messages */
} mt_report_stats;

mt_report_stats mt_report_get_stats(void) MT_NOEXCEPT;

/* The report as text, entries in first-seen order:
 *
 *     unimplemented report: 2 imports (5 calls), 1 selector (1 message)
 *     import _foo (libSystem.B.dylib): 3 calls, first from 0x0000A123 CFoo::bar+0x12
 *     selector -[UIView fooBar:]: 1 message, first from 0x00002A41 -[View init]+0x10
 *
 * or "unimplemented report: none". Writes at most `size` bytes including the NUL; returns the
 * full length (like snprintf). */
size_t mt_report_format(char *buf, size_t size) MT_NOEXCEPT;

/* --- function-entry coverage (generated code built with MT_TRACE_ENTRY 1, calls.h) ----- */

/* Called by MT_FN_ENTER on the first entry of a generated function: `fn_name` is its
 * __func__ ("f_000225E5"), `calls` its entry counter. */
void mt_trace_fn_first(mt_cpu *cpu, const char *fn_name, const uint64_t *calls) MT_NOEXCEPT;

/* Functions entered so far. */
uint32_t mt_coverage_count(void) MT_NOEXCEPT;

/* The coverage as text, by address: one line "0x000225E5 <calls> <symbol>" per function
 * entered, after a header line. Returns the full length (like snprintf). */
size_t mt_coverage_format(char *buf, size_t size) MT_NOEXCEPT;

/* --- guest symbols -------------------------------------------------------------------- */

/* Loads a symbol table (the text written by python -m recomp symbols), replacing the current
 * one:
 *
 *     # maplethief symbols 1
 *     000225E5 0000012C CStateGame::Update
 *
 * one line per symbol: function pointer (Thumb bit set for Thumb code), size in bytes, name;
 * "#" lines are comments. Malformed lines are skipped and counted. Returns the number of
 * symbols, or -1 (and keeps the old table) if the file cannot be read. */
int mt_symbols_load(const char *path) MT_NOEXCEPT;

/* The same from text in memory; *bad (may be NULL) gets the number of skipped lines. */
int mt_symbols_load_text(const char *text, size_t len, uint32_t *bad) MT_NOEXCEPT;

/* Drops the symbol table. */
void mt_symbols_clear(void) MT_NOEXCEPT;

uint32_t mt_symbols_count(void) MT_NOEXCEPT;

/* The symbol whose code range [ptr & ~1, +size) holds `addr` (bit 0 ignored): its name, and
 * the offset and function pointer through the out parameters (either may be NULL). NULL if
 * none. Read-only after loading, so the crash reporter may call it. */
const char *mt_symbols_lookup(uint32_t addr, uint32_t *offset, uint32_t *ptr) MT_NOEXCEPT;

/* "Name+0xOFF" (or "Name" at offset 0) for `addr`, or "" if it is in no symbol. Returns the
 * length written (truncated to size - 1). */
size_t mt_symbolize(uint32_t addr, char *buf, size_t size) MT_NOEXCEPT;

/* --- the crash reporter (runtime/src/diag/crash.cpp) ---------------------------------- */

/* Name of an enum mt_trap_kind ("memory fault", "unimplemented import", ...). */
const char *mt_trap_kind_name(uint32_t kind) MT_NOEXCEPT;

/* Up to `max` return addresses of the guest's r7 frame chain, innermost first ({r7, lr}
 * pairs, Apple's ARM ABI). Bounded and validated: each frame record must be 4-byte aligned,
 * inside a live guest stack (mt_mem_describe) or, for a cpu outside any mt_mem, inside
 * cpu->mem_size, and each next frame higher up the stack; a zero lr ends the chain. Never
 * touches memory that could fault. */
uint32_t mt_crash_backtrace(const mt_cpu *cpu, uint32_t *out, uint32_t max) MT_NOEXCEPT;

/* What stopped the guest. */
typedef struct mt_crash_info {
    uint32_t kind;       /* enum mt_trap_kind, or MT_CRASH_HARDWARE */
    uint32_t pc;         /* guest PC (0 if unknown: a hardware fault in generated code) */
    uint32_t detail;     /* the trap's detail */
    uint32_t has_fault;  /* 1: fault_addr / fault_size are known */
    uint32_t fault_addr; /* guest address of the faulting access */
    uint32_t fault_size; /* bytes (0 if unknown) */
    int access;          /* MT_ACCESS_* */
    const char *what;    /* hardware: signal or exception name; NULL otherwise */
    uint64_t host_pc;    /* hardware: host PC; 0 otherwise */
    uint64_t host_addr;  /* hardware: host fault address; 0 otherwise */
} mt_crash_info;

#define MT_CRASH_HARDWARE 0x100u /* a hardware fault, not an mt_trap */

enum mt_access {
    MT_ACCESS_UNKNOWN = 0,
    MT_ACCESS_READ = 1,
    MT_ACCESS_WRITE = 2,
    MT_ACCESS_EXECUTE = 3,
};

/* The guest crash report:
 *
 *     === guest crash: memory fault ===
 *     pc 0x0000A0D2 CFoo::bar+0x22; detail 0x00000000
 *     fault: 4-byte write to 0x00000008: page zero (a null pointer)
 *     registers (as last stored to mt_cpu):
 *       r0  0x00000000  r1  0x01000040  ...
 *       sp  0x1F10FFC0 [guest stack ...]  lr 0x0000B011 CGame::Update+0x10
 *     backtrace (r7 frame chain):
 *       #0 0x0000A0D2 CFoo::bar+0x22
 *       #1 0x0000B011 CGame::Update+0x10
 *     === end of guest crash ===
 *
 * Returns the full length (like snprintf). */
size_t mt_crash_format(const mt_cpu *cpu, const mt_crash_info *info, char *buf,
                       size_t size) MT_NOEXCEPT;

/* Formats the report of an mt_trap (for MT_TRAP_MEMORY with mt_mem_last_fault's size and
 * region) and writes it line by line through the diagnostics' log sink (the process's log
 * during a run; stderr by default). The runtime's mt_trap calls it before it ends the run. */
void mt_crash_report_trap(const mt_cpu *cpu, uint32_t kind, uint32_t pc,
                          uint32_t detail) MT_NOEXCEPT;

/* Installs the hardware fault handlers, once per process: SIGSEGV, SIGBUS, SIGILL, SIGFPE on
 * an alternate signal stack (POSIX), a vectored exception handler (Windows). A fault inside
 * the guest reservation of the current thread's cpu is reported as a guest crash (guest
 * address, region, registers, backtrace; the guest function from the host PC where the
 * platform can name it), followed by mt_diag_finish; then the previous handler (or the
 * default action) takes over, so the process still dies of the fault. Other faults go to the
 * previous handler untouched. Returns 0, or -1 if the handlers could not be installed. */
int mt_crash_install(void) MT_NOEXCEPT;

/* The guest cpu running on this host thread (NULL: none). The fault handler uses it to tell a
 * guest fault from a host one. */
void mt_crash_set_cpu(mt_cpu *cpu) MT_NOEXCEPT;
mt_cpu *mt_crash_get_cpu(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_DIAG_H */
