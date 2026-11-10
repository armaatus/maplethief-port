// Whether touching a host address faults in hardware, without crashing the test: on POSIX a
// forked child touches it (a SIGSEGV/SIGBUS handler exits with a marker status, so no crash
// report is written); on Windows VirtualQuery reads the page's protection. The Nintendo Switch
// has neither child processes nor signals: a stub that says so (maplethief/horizon.h, #236).
#ifndef MAPLETHIEF_TESTS_CPP_HOST_FAULT_H
#define MAPLETHIEF_TESTS_CPP_HOST_FAULT_H

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__SWITCH__)
#include "maplethief/horizon.h"
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mt_test {

enum class Access { read, write };

inline bool faults(const std::uint8_t *p, Access access) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof info) == 0 || info.State != MEM_COMMIT) {
        return true;
    }
    const DWORD prot = info.Protect & 0xFFu;
    if (prot == PAGE_NOACCESS || (info.Protect & PAGE_GUARD) != 0u) {
        return true;
    }
    if (access == Access::read) {
        return false;
    }
    return prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_WRITECOPY &&
           prot != PAGE_EXECUTE_WRITECOPY;
#elif defined(__SWITCH__)
    (void)p;
    (void)access;
    MT_HORIZON_MISSING("probing a host page for a fault (mt_test::faults)");
    return false;
#else
    constexpr int k_faulted = 99;
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa{};
        sa.sa_handler = [](int) { _exit(k_faulted); };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
        volatile std::uint8_t *v = const_cast<volatile std::uint8_t *>(p);
        if (access == Access::write) {
            *v = 0x5A;
        } else {
            (void)*v;
        }
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == k_faulted;
#endif
}

} // namespace mt_test

#endif // MAPLETHIEF_TESTS_CPP_HOST_FAULT_H
