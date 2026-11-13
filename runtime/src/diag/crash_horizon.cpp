// The crash reporter's hardware fault handler on the Nintendo Switch (ADR 0030's Horizon
// addendum, #236). Compiled only there (runtime/CMakeLists.txt).
//
// Horizon has no signals. When a thread faults, the kernel enters the process at its entry
// point with the exception's frame; libnx (exception.o) copies it into a ThreadExceptionDump,
// switches the thread to __nx_exception_stack, returns from the exception and calls
// __libnx_exception_handler there, in ordinary thread context. A handler that returns makes
// libnx call svcBreak, which ends the process as if there were no handler. This one writes
// ADR 0030's report (crash.cpp: stderr, which is nxlink when a computer sent the NRO, the
// crash file sdmc:/switch/maplethief/crash.txt, then the frontend's sink) and exits with
// MT_CRASH_EXIT_STATUS, which runs the atexit handlers and returns to the Homebrew Menu.
//
// The fault's address is FAR for an instruction or data abort (from the ESR's exception
// class), and its access the ESR's; any other exception (a misaligned PC or SP, an undefined
// instruction, a bad svc) is reported without an address, as a host crash unless it touched
// guest memory.
#include <switch.h>

#include <atomic>
#include <cstdlib>

#include "diag_internal.h"
#include "maplethief/diag.h"

extern "C" {

// libnx's weak definitions are empty: these give it a stack to run the handler on.
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx);

} // extern "C"

namespace {

std::atomic<bool> g_armed{false};

const char *describe(u32 error_desc, u32 esr) {
    const u32 ec = esr >> 26;
    if (ec == 0x24u || ec == 0x25u) {
        return "data abort";
    }
    switch (error_desc) {
    case ThreadExceptionDesc_InstructionAbort:
        return "instruction abort";
    case ThreadExceptionDesc_MisalignedPC:
        return "misaligned pc";
    case ThreadExceptionDesc_MisalignedSP:
        return "misaligned sp";
    case ThreadExceptionDesc_SError:
        return "SError";
    case ThreadExceptionDesc_BadSVC:
        return "bad svc";
    case ThreadExceptionDesc_Trap:
        return "trap (undefined instruction)";
    default:
        return "CPU exception";
    }
}

} // namespace

extern "C" void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    if (!g_armed.load()) {
        return; // not installed: libnx's svcBreak, as without a handler
    }
    const u32 esr = ctx->esr;
    const int access = mt_crash_access_from_esr(esr);
    const std::uint64_t addr = access != MT_ACCESS_UNKNOWN ? ctx->far.x : 0u;
    mt_crash_report_hardware(describe(ctx->error_desc, esr), addr, ctx->pc.x, access);
    std::exit(MT_CRASH_EXIT_STATUS);
}

extern "C" void mt_crash_horizon_install(void) noexcept { g_armed.store(true); }
