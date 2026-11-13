// Shared between the diagnostics sources (ADR 0030); not part of maplethief/diag.h.
#ifndef MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H
#define MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H

#include <cstdint>

#include "maplethief/recomp/calls.h"

extern "C" {

// Writes one line through the diagnostics' log sink (diag.h).
void mt_diag_log(const char *line) noexcept;

// The name of import `id` of the table mt_imports_link last linked, or nullptr.
const char *mt_diag_import_name(std::uint32_t id) noexcept;

// The access of a data or instruction abort from its AArch64 ESR (EC 0x24/0x25: WnR, bit 6;
// EC 0x20/0x21: execute); MT_ACCESS_UNKNOWN for any other exception class.
int mt_crash_access_from_esr(std::uint32_t esr) noexcept;

// The guest function pointer of the generated function with the highest host address at or
// below `host_pc` in `table` (mt_fn_table's layout, any order), and the PC's offset from it;
// 0 if the PC lies below every one of them or more than 1 MiB past the nearest.
std::uint32_t mt_crash_nearest_function(const mt_fn_entry *table, std::uint32_t count,
                                        std::uint64_t host_pc, std::uint64_t *offset) noexcept;

#if defined(__SWITCH__)
// crash_horizon.cpp: arms libnx's exception handler, which calls mt_crash_report_hardware.
void mt_crash_horizon_install(void) noexcept;
#endif

// The report of a hardware fault (crash.cpp): true if it was reported (a guest fault, or any
// fault when a sink is set). The platform's handler calls it.
bool mt_crash_report_hardware(const char *what, std::uint64_t host_addr, std::uint64_t host_pc,
                              int access) noexcept;

// Whether mt_crash_install has run.
bool mt_crash_installed(void) noexcept;

} // extern "C"

#endif // MAPLETHIEF_RUNTIME_DIAG_DIAG_INTERNAL_H
