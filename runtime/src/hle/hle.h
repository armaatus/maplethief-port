// HLE implementations of imports, one section per module (ADR 0019). Each is an
// mt_import_fn: it reads its arguments from the guest registers and stack through
// maplethief/hle/abi.h and writes its result back. registry.cpp lists them by symbol name.
#ifndef MAPLETHIEF_RUNTIME_HLE_HLE_H
#define MAPLETHIEF_RUNTIME_HLE_HLE_H

#include <cstdint>

#include "maplethief/recomp/recomp.h"
#include "maplethief/sjlj.h" // mt_sjlj_hle_*: the SjLj unwinder and catch helpers (#37)

extern "C" {

// libgcc.cpp: the compiler-rt helpers libgcc_s.1.dylib exports.
void mt_hle_divsi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_modsi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_udivsi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_umodsi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_divdi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_udivdi3(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_floatdisf(mt_cpu *cpu, std::uint32_t id) noexcept;

// boot/process.c: process start-up and exit (#43, ADR 0027).
void mt_hle_cxa_atexit(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_exit(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIApplicationMain(mt_cpu *cpu, std::uint32_t id) noexcept;

// libc.cpp, libc_sys.cpp: the C library of libSystem.B.dylib (ADR 0028).
void mt_hle_malloc(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_calloc(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_realloc(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_free(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_memcpy(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_memset(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_memcmp(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_strlen(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_strcpy(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_strcat(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_strcmp(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_atoi(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_atol(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_printf(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_puts(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_vfprintf(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_vsprintf(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_snprintf_chk(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_floor(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_pow(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_trunc(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_gettimeofday(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_abort(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_assert_rtn(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_sysctl(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_sysctlbyname(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_if_nametoindex(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_gethostbyname(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_inet_addr(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_inet_ntoa(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_setsockopt(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_close(mt_cpu *cpu, std::uint32_t id) noexcept;
// Data imports: the FILE * variables, pointed at guest FILE objects for fd 0, 1 and 2.
void mt_hle_init_stdinp(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_stdoutp(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_stderrp(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;

} // extern "C"

#endif // MAPLETHIEF_RUNTIME_HLE_HLE_H
