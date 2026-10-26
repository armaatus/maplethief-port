/* Guest memory access for recompiled code (ADR 0014; the address space itself is #42).
 *
 * The guest has a 32-bit, little-endian address space. Host address = cpu->mem + guest
 * address. The image is mapped at its link-time addresses (slide 0), so addresses the
 * recompiler folded into constants are valid as they are. Two models share this one API:
 *
 *   MT_MEM_CHECKED 0  cpu->mem is a reservation of 4 GiB plus a guard area, so every guest
 *                     address is inside the host mapping and an access is base + offset with
 *                     no check; unmapped pages fault in hardware (the runtime's handler
 *                     reports the guest address).
 *   MT_MEM_CHECKED 1  (default) cpu->mem is a window of cpu->mem_size bytes; an access that
 *                     does not fit traps through mt_mem_fault with the guest PC. For debug
 *                     builds, the tests, and platforms where a 4 GiB reservation is
 *                     impractical (iOS).
 *
 * Accesses go through memcpy, so they are correct for unaligned addresses (ARMv7 allows
 * unaligned LDR/STR/LDRH/STRH) and free of strict-aliasing problems; compilers turn them
 * into single loads and stores. Every macro takes the guest PC of the accessing instruction
 * as a constant; it only reaches the fault path. They expect `cpu` in scope. */
#ifndef MAPLETHIEF_RECOMP_MEM_H
#define MAPLETHIEF_RECOMP_MEM_H

#include <stdint.h>
#include <string.h>

#include "maplethief/recomp/compiler.h"
#include "maplethief/recomp/cpu.h"

MT_EXTERN_C_BEGIN

#ifndef MT_MEM_CHECKED
#define MT_MEM_CHECKED 1
#endif

/* Reports an access of `size` bytes at guest `addr` by the instruction at guest `pc` that is
 * outside guest memory, then stops the guest (implemented by the runtime). */
MT_NORETURN void mt_mem_fault(mt_cpu *cpu, uint32_t addr, uint32_t size, uint32_t pc) MT_NOEXCEPT;

MT_INLINE uint8_t *mt_host(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t size, uint32_t pc) {
#if MT_MEM_CHECKED
    if (MT_UNLIKELY((uint64_t)addr + size > cpu->mem_size)) {
        mt_mem_fault(cpu, addr, size, pc);
    }
#else
    (void)size;
    (void)pc;
#endif
    return cpu->mem + addr;
}

MT_INLINE uint32_t mt_ld8(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t pc) {
    return *mt_host(cpu, addr, 1, pc);
}
MT_INLINE uint32_t mt_ld16(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t pc) {
    uint16_t v;
    memcpy(&v, mt_host(cpu, addr, 2, pc), 2);
    return v;
}
MT_INLINE uint32_t mt_ld32(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t pc) {
    uint32_t v;
    memcpy(&v, mt_host(cpu, addr, 4, pc), 4);
    return v;
}
MT_INLINE uint64_t mt_ld64(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t pc) {
    uint64_t v;
    memcpy(&v, mt_host(cpu, addr, 8, pc), 8);
    return v;
}
MT_INLINE void mt_st8(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t value, uint32_t pc) {
    *mt_host(cpu, addr, 1, pc) = (uint8_t)value;
}
MT_INLINE void mt_st16(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t value, uint32_t pc) {
    uint16_t v = (uint16_t)value;
    memcpy(mt_host(cpu, addr, 2, pc), &v, 2);
}
MT_INLINE void mt_st32(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint32_t value, uint32_t pc) {
    memcpy(mt_host(cpu, addr, 4, pc), &value, 4);
}
MT_INLINE void mt_st64(mt_cpu *MT_RESTRICT cpu, uint32_t addr, uint64_t value, uint32_t pc) {
    memcpy(mt_host(cpu, addr, 8, pc), &value, 8);
}

/* What generated code writes. Loads return uint32_t (zero-extended; sign extension is
 * explicit, see mt_sx8 / mt_sx16 in alu.h), except MT_LD64. */
#define MT_LD8(addr, pc) mt_ld8(cpu, (addr), (pc))
#define MT_LD16(addr, pc) mt_ld16(cpu, (addr), (pc))
#define MT_LD32(addr, pc) mt_ld32(cpu, (addr), (pc))
#define MT_LD64(addr, pc) mt_ld64(cpu, (addr), (pc))
#define MT_ST8(addr, value, pc) mt_st8(cpu, (addr), (value), (pc))
#define MT_ST16(addr, value, pc) mt_st16(cpu, (addr), (value), (pc))
#define MT_ST32(addr, value, pc) mt_st32(cpu, (addr), (value), (pc))
#define MT_ST64(addr, value, pc) mt_st64(cpu, (addr), (value), (pc))

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_MEM_H */
