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

} // extern "C"

#endif // MAPLETHIEF_RUNTIME_HLE_HLE_H
