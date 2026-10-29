// A guest that crashes on purpose, run as a child process by diag_test.cpp (ADR 0030): the
// crash reporter has to report a fault that kills the process, which a test cannot do in its
// own process. Modes (argv[1]):
//
//   exit   calls the unimplemented import, then returns from main: the report is written at
//          exit
//   null   an unchecked guest write to page zero (what Release code does), from a function
//          named like generated code: a hardware fault
//   guard  an unchecked guest write into the guard below the guest stack: a stack overflow
//   trap   a checked HLE access to page zero: mt_mem_fault -> mt_trap -> the trap report
//
// Every mode first calls the unimplemented import once (continue policy), so the
// unimplemented report follows the crash report. Output goes to stderr.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diag_fixture.h"
#include "maplethief/diag.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"

#if defined(_MSC_VER)
#define MT_TEST_NOINLINE __declspec(noinline)
#else
#define MT_TEST_NOINLINE __attribute__((noinline))
#endif

namespace fx = mt_diag_fixture;

namespace {
std::uint32_t g_fault_addr = 0;
}

// The runtime's mt_trap for this program: report and end the process (as the runtime's own
// will, #43).
extern "C" void mt_trap(mt_cpu *cpu, std::uint32_t kind, std::uint32_t pc,
                        std::uint32_t detail) noexcept {
    mt_crash_report_trap(cpu, kind, pc, detail);
    std::exit(70);
}

// Named like a generated function (f_ + its guest function pointer), so the crash reporter
// finds the guest function from the host PC. Exported (ENABLE_EXPORTS) for dladdr.
extern "C" MT_TEST_NOINLINE void f_00003001(mt_cpu *cpu) {
    volatile std::uint32_t *p = reinterpret_cast<volatile std::uint32_t *>(cpu->mem + g_fault_addr);
    *p = 0x12345678u;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    mt_mem_config mcfg = mt_mem_config_default();
    mt_mem *mem = mt_mem_create(&mcfg);
    if (mem == nullptr) {
        return 2;
    }
    mt_stack stack{};
    if (mt_stack_create(mem, MT_STACK_GRANULE, &stack) != 0) {
        return 2;
    }
    static mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    cpu.r[MT_SP] = stack.top;
    const std::uint32_t block = mt_heap_malloc(mem, 32u);

    if (mt_symbols_load_text(fx::k_symbols, std::strlen(fx::k_symbols), nullptr) != 4) {
        return 3;
    }
    mt_diag_config dcfg = mt_diag_config_default();
    if (mt_diag_configure(&dcfg) != 0) {
        return 3;
    }
    mt_import_config icfg = mt_import_config_default();
    icfg.unimplemented = MT_UNIMPLEMENTED_CONTINUE;
    static mt_import_fn table[fx::k_import_count];
    mt_imports_link(fx::k_descs, fx::k_import_count, table, fx::k_hle, 1u, &icfg);

    fx::build_frames(&cpu);
    table[1](&cpu, 1u); // _missing, from lr = CFoo::bar+0x20
    cpu.r[0] = block;   // a register that names a heap block

    if (std::strcmp(mode, "exit") == 0) {
        return 0;
    }
    if (mt_crash_install() != 0) {
        return 4;
    }
    mt_crash_set_cpu(&cpu);
    if (std::strcmp(mode, "null") == 0) {
        g_fault_addr = 8u;
    } else if (std::strcmp(mode, "guard") == 0) {
        g_fault_addr = stack.guard + 0x100u;
    } else if (std::strcmp(mode, "trap") == 0) {
        cpu.r[MT_LR] = fx::k_lr_bar;
        (void)mt_g2h(&cpu, 4u, 4u); // stops through mt_trap
        return 5;
    } else {
        std::fprintf(stderr, "crash_child: unknown mode '%s'\n", mode);
        return 2;
    }
    f_00003001(&cpu);
    std::fprintf(stderr, "crash_child: the fault did not happen\n");
    return 6;
}
