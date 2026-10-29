// The test executable's mt_trap; see guest_trap.h.
#include "guest_trap.h"

#include <csetjmp>
#include <cstdlib>

#include "maplethief/recomp/calls.h"

#if defined(_MSC_VER)
// setjmp in C++ (C4611): no object with a destructor lives across it here.
#pragma warning(disable : 4611)
#endif

namespace mt_test {

Trap g_trap{};

namespace {
std::jmp_buf *g_trap_jmp = nullptr;
} // namespace

bool stops(void (*fn)(void *), void *arg) {
    std::jmp_buf jmp;
    g_trap_jmp = &jmp;
    if (setjmp(jmp) != 0) {
        g_trap_jmp = nullptr;
        return true;
    }
    fn(arg);
    g_trap_jmp = nullptr;
    return false;
}

} // namespace mt_test

extern "C" void mt_trap(mt_cpu *, std::uint32_t kind, std::uint32_t pc,
                        std::uint32_t detail) noexcept {
    mt_test::g_trap = mt_test::Trap{kind, pc, detail};
    if (mt_test::g_trap_jmp != nullptr) {
        std::longjmp(*mt_test::g_trap_jmp, 1);
    }
    std::abort();
}
