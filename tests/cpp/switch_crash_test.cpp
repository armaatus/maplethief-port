// MapleThiefTests.nro --crash-test (#236): a forced guest fault on a console, for the crash
// reporter's hardware path on Horizon (ADR 0030's Horizon addendum). The desktop's crash_child
// "guard" mode, in-process: a guest stack with the diagnostics fixture's r7 frame chain, the
// handler installed, then an unchecked write into the guard below the stack, as Release code
// overflowing its stack does. libnx's exception handler writes the report to stderr (nxlink)
// and to sdmc:/switch/maplethief/crash.txt, then exits to the Homebrew Menu. Expected in the
// file: "=== guest crash: hardware fault ===", "write at guest 0x1f0000..: guard below guest
// stack" and the backtrace "#0 0x00002043 CGame::Update+0x42", "#1 0x00004011 main_loop+0x10".
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "diag_fixture.h"
#include "maplethief/diag.h"
#include "maplethief/memory.h"

namespace fx = mt_diag_fixture;

namespace {

std::uint32_t g_fault_addr = 0;

__attribute__((noinline)) void touch_guard(mt_cpu *cpu) {
    volatile std::uint32_t *p = reinterpret_cast<volatile std::uint32_t *>(cpu->mem + g_fault_addr);
    *p = 0x12345678u;
}

} // namespace

extern "C" int mt_test_switch_crash_test(void) {
    mt_mem_config mcfg = mt_mem_config_default();
    mt_mem *mem = mt_mem_create(&mcfg);
    if (mem == nullptr) {
        std::fprintf(stderr, "crash test: no guest memory\n");
        return 2;
    }
    mt_stack stack{};
    if (mt_stack_create(mem, MT_STACK_GRANULE, &stack) != 0) {
        return 2;
    }
    static mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    cpu.r[MT_SP] = stack.top;
    if (mt_symbols_load_text(fx::k_symbols, std::strlen(fx::k_symbols), nullptr) != 4) {
        return 3;
    }
    fx::build_frames(&cpu);
    cpu.r[0] = mt_heap_malloc(mem, 32u); // a register that names a heap block
    if (mt_crash_install() != 0) {
        return 4;
    }
    mt_crash_set_cpu(&cpu);
    g_fault_addr = stack.guard + 0x100u;
    std::fprintf(stderr,
                 "crash test: writing to guest 0x%08X, in the guard below the guest stack; the "
                 "report goes to " MT_CRASH_FILE_SWITCH "\n",
                 g_fault_addr);
    std::fflush(stderr);
    touch_guard(&cpu);
    std::fprintf(stderr, "crash test: the fault did not happen\n");
    return 6;
}
