// A synthetic guest for the diagnostics tests (ADR 0030): a symbol table, a guest stack with
// an r7 frame chain through those symbols, and an import table with one implemented and one
// unimplemented import. Shared by diag_test.cpp and the crash_child program.
#ifndef MAPLETHIEF_TESTS_CPP_DIAG_FIXTURE_H
#define MAPLETHIEF_TESTS_CPP_DIAG_FIXTURE_H

#include <cstdint>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"

namespace mt_diag_fixture {

// Thumb functions (pointer with bit 0), sizes and names.
inline constexpr char k_symbols[] = "# maplethief symbols 1\n"
                                    "00002001 00000100 CGame::Update\n"
                                    "00003001 00000080 CFoo::bar\n"
                                    "00004001 00000040 main_loop\n"
                                    "000F0C90 00000004 _missing (stub)\n";

// The return addresses of the frame chain, innermost first: inside CGame::Update, called
// from main_loop.
inline constexpr std::uint32_t k_ret_update = 0x00002043u; // CGame::Update+0x42
inline constexpr std::uint32_t k_ret_loop = 0x00004011u;   // main_loop+0x10
inline constexpr std::uint32_t k_lr_bar = 0x00003021u;     // CFoo::bar+0x20

inline void put32(mt_cpu *cpu, std::uint32_t addr, std::uint32_t v) {
    std::memcpy(cpu->mem + addr, &v, 4);
}

// Builds two frame records {r7, lr} below sp: r7 -> {next, CGame::Update+0x42} ->
// {0, main_loop+0x10}, as Apple's ARM prologue (push {r7, lr}; mov r7, sp) leaves them.
inline void build_frames(mt_cpu *cpu) {
    const std::uint32_t outer = cpu->r[MT_SP] - 0x40u;
    const std::uint32_t inner = cpu->r[MT_SP] - 0x80u;
    put32(cpu, outer, 0u);
    put32(cpu, outer + 4u, k_ret_loop);
    put32(cpu, inner, outer);
    put32(cpu, inner + 4u, k_ret_update);
    cpu->r[7] = inner;
    cpu->r[MT_SP] = inner - 0x10u;
    cpu->r[MT_LR] = k_lr_bar;
}

// Import 0 "_answer" (implemented: returns 42), import 1 "_missing" (unimplemented), import 2
// a data import.
inline void hle_answer(mt_cpu *cpu, std::uint32_t) { cpu->r[0] = 42u; }

inline constexpr mt_import_desc k_descs[] = {
    {"_answer", "/usr/lib/libSystem.B.dylib", MT_IMPORT_FUNCTION, 0xF0C8Cu, 0u, 0u},
    {"_missing", "/usr/lib/libFoo.dylib", MT_IMPORT_FUNCTION, 0xF0C90u, 0u, 0u},
    {"_kSomething", "/usr/lib/libFoo.dylib", MT_IMPORT_DATA, 0u, 4u, 4u},
};
inline constexpr std::uint32_t k_import_count = 3u;
inline constexpr mt_hle_entry k_hle[] = {{"_answer", hle_answer, nullptr}};

} // namespace mt_diag_fixture

#endif // MAPLETHIEF_TESTS_CPP_DIAG_FIXTURE_H
