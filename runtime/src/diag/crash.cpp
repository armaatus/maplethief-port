// The guest crash reporter (ADR 0030): the report of an mt_trap, and the hardware fault
// handlers that turn a fault inside guest memory into the same report. See maplethief/diag.h.
//
// A hardware fault happens where checks are compiled out (ADR 0025): Release generated code
// (MT_MEM_CHECKED 0) accessing page zero, a stack guard, unused heap or stack space or
// anything beyond the arena, and writes to sealed __TEXT in any build. The host fault address
// minus cpu->mem is the guest address; the guest PC is not known there (generated code does
// not store it), so the report names the guest function from the host PC where the platform
// can (dladdr on the generated f_XXXXXXXX symbols), and gives the registers as generated code
// last stored them to mt_cpu.
//
// The signal handler formats with snprintf and mt_mem_describe (which only tries locks): not
// async-signal-safe in the strict sense, but it runs once, on an alternate stack, right
// before the process dies of the fault. A fault outside the guest is reported the same way
// (MT_CRASH_HOST) only when a sink wants it (mt_crash_set_sink); the sink itself keeps to
// async-signal-safe calls (platform/src/crash_report.cpp).
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "diag_internal.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h" // MT_HLE_RETURN
#include "maplethief/memory.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <signal.h>
#include <windows.h>
#elif defined(__SWITCH__)
// Horizon has no POSIX signals and no dladdr: libnx's exception handler (crash_horizon.cpp)
// and the generated code's own table (mt_crash_set_host_functions) instead.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h>
#endif
#endif

namespace {

using u32 = std::uint32_t;

thread_local mt_cpu *t_cpu = nullptr;
std::atomic<int> g_reporting{0};
std::atomic<int> g_installed{0};
mt_crash_sink_fn g_sink = nullptr;
void *g_sink_user = nullptr;
const mt_fn_entry *g_fns = nullptr; // mt_crash_set_host_functions
u32 g_fn_count = 0;

constexpr std::size_t k_file_max = 1024;
#if defined(__SWITCH__)
char g_file[k_file_max] = MT_CRASH_FILE_SWITCH; // mt_crash_set_file
#else
char g_file[k_file_max] = "";
#endif

constexpr u32 k_max_frames = 64;

struct Text {
    char *buf;
    std::size_t size;
    std::size_t len;
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void put(Text &t, const char *fmt, ...) {
    const std::size_t room = t.len < t.size ? t.size - t.len : 0u;
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(room != 0u ? t.buf + t.len : nullptr, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        t.len += static_cast<std::size_t>(n);
    }
}

// The address space of a cpu, or nullptr for a plain buffer (tests).
mt_mem *mem_of(const mt_cpu *cpu) { return cpu->mem != nullptr ? mt_mem_from_cpu(cpu) : nullptr; }

// 1 if the 8 bytes of a frame record at `fp` can be read without faulting.
bool frame_readable(const mt_cpu *cpu, mt_mem *mem, u32 fp) {
    if (cpu->mem == nullptr || static_cast<std::uint64_t>(fp) + 8u > cpu->mem_size) {
        return false;
    }
    if (mem == nullptr) {
        return true; // a test's plain buffer: all of it is readable
    }
    return mt_mem_describe(mem, fp, nullptr, 0) == MT_REGION_STACK &&
           mt_mem_describe(mem, fp + 7u, nullptr, 0) == MT_REGION_STACK;
}

// " Name+0xOFF" for code, or " [region]" for a guest data address, or nothing.
void annotate(Text &t, mt_mem *mem, u32 v, bool code_only) {
    char buf[160];
    if (mt_symbolize(v, buf, sizeof buf) != 0u) {
        put(t, " %s", buf);
        return;
    }
    if (code_only || v < MT_GUEST_PAGE_ZERO_END || v >= MT_GUEST_ARENA_SIZE || mem == nullptr) {
        return;
    }
    const u32 region = mt_mem_describe(mem, v, buf, sizeof buf);
    if (region == MT_REGION_IMAGE || region == MT_REGION_HEAP || region == MT_REGION_STACK ||
        region == MT_REGION_STACK_GUARD) {
        put(t, " [%s]", buf);
    }
}

const char *access_name(int access) {
    switch (access) {
    case MT_ACCESS_READ:
        return "read";
    case MT_ACCESS_WRITE:
        return "write";
    case MT_ACCESS_EXECUTE:
        return "execute";
    default:
        return "access";
    }
}

// A guest C string (a selector) or nullptr.
const char *guest_string(const mt_cpu *cpu, u32 addr) {
    if (cpu->mem == nullptr || addr < MT_GUEST_PAGE_ZERO_END ||
        static_cast<std::uint64_t>(addr) >= cpu->mem_size) {
        return nullptr;
    }
    mt_mem *mem = mem_of(cpu);
    if (mem != nullptr) {
        const u32 r = mt_mem_describe(mem, addr, nullptr, 0);
        if (r != MT_REGION_IMAGE && r != MT_REGION_HEAP) {
            return nullptr; // could be uncommitted
        }
    }
    const auto *s = reinterpret_cast<const char *>(cpu->mem + addr);
    const std::uint64_t room = cpu->mem_size - addr;
    return std::memchr(s, 0, room < 128u ? static_cast<std::size_t>(room) : 128u) != nullptr
               ? s
               : nullptr;
}

// "f_0000A0B1" -> 0xA0B1, else 0. (Unused on the Switch, which has no dladdr.)
[[maybe_unused]] u32 generated_fn(const char *name) {
    if (name == nullptr) {
        return 0;
    }
    if (name[0] == '_') { // a platform that keeps the C prefix
        name++;
    }
    if (name[0] != 'f' || name[1] != '_') {
        return 0;
    }
    char *end = nullptr;
    const unsigned long v = std::strtoul(name + 2, &end, 16);
    return end == name + 10 ? static_cast<u32>(v) : 0u;
}

// The host symbol at a host PC ("" if none) and the guest function it is, if generated code.
void host_symbol(std::uint64_t host_pc, char *name, std::size_t size, u32 *guest_fn) {
    name[0] = '\0';
    *guest_fn = 0;
#if !defined(_WIN32) && !defined(__SWITCH__)
    Dl_info info;
    if (host_pc != 0u &&
        dladdr(reinterpret_cast<void *>(static_cast<std::uintptr_t>(host_pc)), &info) != 0 &&
        info.dli_sname != nullptr) {
        const auto off = static_cast<unsigned long long>(
            host_pc - reinterpret_cast<std::uintptr_t>(info.dli_saddr));
        std::snprintf(name, size, "%s+0x%llx", info.dli_sname, off);
        *guest_fn = generated_fn(info.dli_sname);
    }
#else
    // No dladdr: the nearest generated function in the table, when the frontend set one.
    std::uint64_t off = 0;
    const u32 fn = host_pc != 0u ? mt_crash_nearest_function(g_fns, g_fn_count, host_pc, &off) : 0u;
    if (fn != 0u) {
        std::snprintf(name, size, "f_%08X+0x%llx (nearest)", fn,
                      static_cast<unsigned long long>(off));
        *guest_fn = fn;
    }
#endif
}

// How many bytes of host address space behind cpu->mem belong to the guest.
std::uint64_t reservation(const mt_cpu *cpu) {
    mt_mem *mem = mem_of(cpu);
    if (mem != nullptr && mt_mem_is_full_reservation(mem) != 0) {
        return (std::uint64_t{1} << 32) + MT_GUEST_RESERVE_GUARD;
    }
    return cpu->mem_size;
}

void write_stderr(const char *s, std::size_t n) {
#if defined(_WIN32)
    std::fwrite(s, 1, n, stderr);
    std::fflush(stderr);
#else
    while (n > 0u) {
        const ssize_t w = ::write(2, s, n);
        if (w <= 0) {
            break;
        }
        s += w;
        n -= static_cast<std::size_t>(w);
    }
#endif
}

// Writes a finished report to g_file (mt_crash_set_file), with open/write/close only: it may
// run in a signal handler. Its directory is created (one level).
void write_crash_file(const char *text, std::size_t n) {
    if (g_file[0] == '\0') {
        return;
    }
#if defined(_WIN32)
    wchar_t w[k_file_max];
    if (MultiByteToWideChar(CP_UTF8, 0, g_file, -1, w, static_cast<int>(k_file_max)) <= 0) {
        return;
    }
    wchar_t dir[k_file_max];
    std::memcpy(dir, w, sizeof dir);
    wchar_t *slash = std::wcsrchr(dir, L'\\');
    wchar_t *fwd = std::wcsrchr(dir, L'/');
    if (fwd != nullptr && (slash == nullptr || fwd > slash)) {
        slash = fwd;
    }
    if (slash != nullptr) {
        *slash = L'\0';
        CreateDirectoryW(dir, nullptr);
    }
    const HANDLE h =
        CreateFileW(w, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
#else
    char dir[k_file_max];
    std::memcpy(dir, g_file, sizeof dir);
    char *slash = std::strrchr(dir, '/');
    if (slash != nullptr && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    const int fd = ::open(g_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    while (n > 0u) {
        const ssize_t w = ::write(fd, text, n);
        if (w <= 0) {
            break;
        }
        text += w;
        n -= static_cast<std::size_t>(w);
    }
    ::close(fd);
#endif
}

// The report of a hardware fault at host address `host_addr`, if it is a guest one. Returns
// true if it was reported.
char g_crash_buf[32768];

bool report_hardware(const char *what, std::uint64_t host_addr, std::uint64_t host_pc, int access) {
    mt_cpu *cpu = t_cpu;
    const auto base = reinterpret_cast<std::uintptr_t>(cpu != nullptr ? cpu->mem : nullptr);
    const bool guest = cpu != nullptr && cpu->mem != nullptr && host_addr >= base &&
                       host_addr - base < reservation(cpu);
    if (!guest && g_sink == nullptr) {
        return false; // not the guest's, and nobody asked for host faults
    }
    if (g_reporting.exchange(1) != 0) {
        return false; // a fault while reporting: let it through
    }
    mt_crash_info info{};
    info.kind = guest ? MT_CRASH_HARDWARE : MT_CRASH_HOST;
    info.access = access;
    info.what = what;
    info.host_pc = host_pc;
    info.host_addr = host_addr;
    static mt_cpu no_guest{}; // a host fault on a thread without a guest cpu
    if (guest) {
        info.has_fault = 1;
        const std::uint64_t at = host_addr - base;
        info.fault_addr = at > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(at);
    }
    const std::size_t n =
        mt_crash_format(cpu != nullptr ? cpu : &no_guest, &info, g_crash_buf, sizeof g_crash_buf);
    const std::size_t len = n < sizeof g_crash_buf ? n : sizeof g_crash_buf - 1u;
    write_stderr(g_crash_buf, len);
    write_crash_file(g_crash_buf, len);
    if (g_sink != nullptr) {
        g_sink(g_crash_buf, len, 1, g_sink_user);
    }
    mt_diag_finish();
    return true;
}

#if defined(__SWITCH__)

// The handler is libnx's (crash_horizon.cpp).

#elif !defined(_WIN32)

constexpr int k_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_prev[sizeof k_signals / sizeof k_signals[0]];

const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

void context_of(void *uctx, std::uint64_t *pc, int *access) {
    *pc = 0;
    *access = MT_ACCESS_UNKNOWN;
    if (uctx == nullptr) {
        return;
    }
    auto *uc = static_cast<ucontext_t *>(uctx);
#if defined(__APPLE__) && defined(__aarch64__)
    *pc = uc->uc_mcontext->__ss.__pc;
    *access = mt_crash_access_from_esr(uc->uc_mcontext->__es.__esr);
#elif defined(__APPLE__) && defined(__x86_64__)
    *pc = uc->uc_mcontext->__ss.__rip;
    *access = (uc->uc_mcontext->__es.__err & 2u) != 0u ? MT_ACCESS_WRITE : MT_ACCESS_READ;
#elif defined(__linux__) && defined(__x86_64__)
    *pc = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    *access = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0 ? MT_ACCESS_WRITE : MT_ACCESS_READ;
#elif defined(__linux__) && defined(__aarch64__)
    *pc = uc->uc_mcontext.pc;
#else
    (void)uc;
#endif
}

void on_signal(int sig, siginfo_t *si, void *uctx) {
    std::uint64_t pc = 0;
    int access = MT_ACCESS_UNKNOWN;
    context_of(uctx, &pc, &access);
    if (sig != SIGSEGV && sig != SIGBUS) {
        access = MT_ACCESS_UNKNOWN;
    }
    // si_addr means nothing for a signal sent by kill() or raise() (an abort).
    const auto addr =
        reinterpret_cast<std::uintptr_t>(si != nullptr && si->si_code > 0 ? si->si_addr : nullptr);
    report_hardware(signal_name(sig), addr, pc, access);
    // Hand the signal on: restore the previous action and return, so the faulting
    // instruction runs again under it (or the default action ends the process).
    for (std::size_t i = 0; i < sizeof k_signals / sizeof k_signals[0]; i++) {
        if (k_signals[i] == sig) {
            sigaction(sig, &g_prev[i], nullptr);
        }
    }
    if (si == nullptr || si->si_code <= 0) {
        raise(sig); // sent by kill(): returning would not repeat it
    }
}

#else // _WIN32

LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    const char *what = nullptr;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        what = "EXCEPTION_ACCESS_VIOLATION";
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        what = "EXCEPTION_IN_PAGE_ERROR";
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    int access = MT_ACCESS_UNKNOWN;
    switch (r->ExceptionInformation[0]) {
    case 0:
        access = MT_ACCESS_READ;
        break;
    case 1:
        access = MT_ACCESS_WRITE;
        break;
    case 8:
        access = MT_ACCESS_EXECUTE;
        break;
    default:
        break;
    }
    report_hardware(what, static_cast<std::uint64_t>(r->ExceptionInformation[1]),
                    reinterpret_cast<std::uintptr_t>(r->ExceptionAddress), access);
    return EXCEPTION_CONTINUE_SEARCH; // the process still dies of it
}

// What no one handled: the fault outside the guest (or the vectored handler's report already
// went out, and g_reporting makes this one do nothing). Runs only for unhandled exceptions, so
// a graphics driver's own caught access violations are not reported.
LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    const char *what = "EXCEPTION";
    std::uint64_t addr = 0;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        what = "EXCEPTION_ACCESS_VIOLATION";
        addr =
            r->NumberParameters >= 2 ? static_cast<std::uint64_t>(r->ExceptionInformation[1]) : 0u;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        what = "EXCEPTION_ILLEGAL_INSTRUCTION";
        break;
    case EXCEPTION_STACK_OVERFLOW:
        what = "EXCEPTION_STACK_OVERFLOW";
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        what = "EXCEPTION_INT_DIVIDE_BY_ZERO";
        break;
    default:
        break;
    }
    report_hardware(what, addr, reinterpret_cast<std::uintptr_t>(r->ExceptionAddress),
                    MT_ACCESS_UNKNOWN);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() on Windows raises SIGABRT and never reaches the exception filter.
void on_abort(int) { report_hardware("SIGABRT", 0u, 0u, MT_ACCESS_UNKNOWN); }

#endif

} // namespace

extern "C" const char *mt_trap_kind_name(std::uint32_t kind) noexcept {
    switch (kind) {
    case MT_TRAP_UNSUPPORTED:
        return "unsupported instruction";
    case MT_TRAP_BAD_RETURN:
        return "return to an address other than lr at entry";
    case MT_TRAP_FELL_OFF:
        return "execution ran past the end of a function";
    case MT_TRAP_UNREACHABLE:
        return "unreachable code reached";
    case MT_TRAP_UNLIFTED:
        return "function that could not be lifted";
    case MT_TRAP_UDF:
        return "udf, bkpt or svc";
    case MT_TRAP_IMPORT:
        return "unimplemented import";
    case MT_TRAP_NO_FUNCTION:
        return "no guest function at an indirect target";
    case MT_TRAP_SJLJ:
        return "SjLj unwinder misuse";
    case MT_TRAP_UNCAUGHT:
        return "uncaught exception";
    case MT_TRAP_TERMINATE:
        return "std::terminate";
    case MT_TRAP_MEMORY:
        return "memory fault";
    case MT_TRAP_OBJC:
        return "undeliverable Objective-C message";
    case MT_TRAP_ABORT:
        return "the guest aborted (abort, assert, malloc error)";
    case MT_TRAP_PURE_VIRT:
        return "pure virtual function called";
    case MT_CRASH_HARDWARE:
        return "hardware fault";
    case MT_CRASH_HOST:
        return "host crash (outside the guest)";
    default:
        return "unknown trap";
    }
}

extern "C" int mt_crash_access_from_esr(std::uint32_t esr) noexcept {
    const u32 ec = esr >> 26;
    if (ec == 0x24u || ec == 0x25u) { // data abort: WnR is bit 6
        return (esr & 0x40u) != 0u ? MT_ACCESS_WRITE : MT_ACCESS_READ;
    }
    if (ec == 0x20u || ec == 0x21u) { // instruction abort
        return MT_ACCESS_EXECUTE;
    }
    return MT_ACCESS_UNKNOWN;
}

extern "C" std::uint32_t mt_crash_nearest_function(const mt_fn_entry *table, std::uint32_t count,
                                                   std::uint64_t host_pc,
                                                   std::uint64_t *offset) noexcept {
    constexpr std::uint64_t k_reach = 1u << 20; // no generated function is this long
    u32 best = 0;
    std::uint64_t best_at = 0;
    for (u32 i = 0; table != nullptr && i < count; i++) {
        const auto at = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(table[i].fn));
        if (table[i].fn != nullptr && at <= host_pc && (best == 0u || at > best_at)) {
            best = table[i].addr;
            best_at = at;
        }
    }
    if (best == 0u || host_pc - best_at >= k_reach) {
        return 0u;
    }
    if (offset != nullptr) {
        *offset = host_pc - best_at;
    }
    return best;
}

extern "C" bool mt_crash_report_hardware(const char *what, std::uint64_t host_addr,
                                         std::uint64_t host_pc, int access) noexcept {
    return report_hardware(what, host_addr, host_pc, access);
}

extern "C" bool mt_crash_installed(void) noexcept { return g_installed.load() != 0; }

extern "C" std::uint32_t mt_crash_backtrace(const mt_cpu *cpu, std::uint32_t *out,
                                            std::uint32_t max) noexcept {
    mt_mem *mem = mem_of(cpu);
    u32 fp = cpu->r[7], n = 0;
    while (n < max && fp != 0u && (fp & 3u) == 0u && frame_readable(cpu, mem, fp)) {
        u32 next = 0, lr = 0;
        std::memcpy(&next, cpu->mem + fp, 4); // the host is little-endian (compiler.h)
        std::memcpy(&lr, cpu->mem + fp + 4u, 4);
        if (lr == 0u) {
            break;
        }
        out[n++] = lr;
        if (next <= fp) { // a caller's frame is always higher up the stack
            break;
        }
        fp = next;
    }
    return n;
}

extern "C" std::size_t mt_crash_format(const mt_cpu *cpu, const mt_crash_info *info, char *buf,
                                       std::size_t size) noexcept {
    Text t{buf, buf != nullptr ? size : 0u, 0u};
    if (t.size > 0u) {
        buf[0] = '\0';
    }
    mt_mem *mem = mem_of(cpu);
    const bool hardware = info->kind == MT_CRASH_HARDWARE || info->kind == MT_CRASH_HOST;
    put(t, "=== guest crash: %s ===\n", mt_trap_kind_name(info->kind));
    if (hardware) {
        char sym[192];
        u32 fn = 0;
        host_symbol(info->host_pc, sym, sizeof sym, &fn);
        put(t, "%s at host pc 0x%llx%s%s, host address 0x%llx\n",
            info->what != nullptr ? info->what : "fault",
            static_cast<unsigned long long>(info->host_pc), sym[0] != '\0' ? " " : "", sym,
            static_cast<unsigned long long>(info->host_addr));
        if (fn != 0u) {
            put(t, "in guest function 0x%08X", fn);
            annotate(t, mem, fn, true);
            put(t, " (the guest pc is not known in generated code)\n");
        } else {
            put(t, "guest pc unknown (the host pc is in no generated function)\n");
        }
    } else {
        put(t, "pc 0x%08X", info->pc);
        annotate(t, mem, info->pc, true);
        put(t, "; detail 0x%08X", info->detail);
        switch (info->kind) {
        case MT_TRAP_IMPORT: {
            const char *name = mt_diag_import_name(info->detail);
            put(t, " (import %s)", name != nullptr ? name : "?");
            break;
        }
        case MT_TRAP_OBJC: {
            const char *sel = guest_string(cpu, info->detail);
            if (sel != nullptr) {
                put(t, " (selector %s)", sel);
            }
            break;
        }
        case MT_TRAP_NO_FUNCTION:
        case MT_TRAP_BAD_RETURN:
            annotate(t, mem, info->detail, true);
            break;
        default:
            break;
        }
        put(t, "\n");
    }
    if (info->has_fault != 0u) {
        char where[160];
        const u32 a = info->fault_addr;
        const bool outside = static_cast<std::uint64_t>(a) >= cpu->mem_size;
        if (outside) {
            std::snprintf(where, sizeof where, "outside guest memory (it ends at 0x%08llx)",
                          static_cast<unsigned long long>(cpu->mem_size));
        } else {
            mt_mem_describe(mem, a, where, sizeof where);
        }
        put(t, "fault: ");
        if (info->fault_size != 0u) {
            put(t, "%u-byte ", info->fault_size);
        }
        put(t, "%s at guest 0x%08X: %s\n", access_name(info->access), a, where);
    }
    if (cpu->mem == nullptr && info->kind == MT_CRASH_HOST) {
        put(t, "no guest cpu on this thread\n=== end of guest crash ===\n");
        return t.len;
    }
    put(t, "registers (as last stored to mt_cpu):\n");
    static const char *const k_names[16] = {"r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
                                            "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
    for (int i = 0; i < 16; i++) {
        put(t, "  %-3s 0x%08X", k_names[i], cpu->r[i]);
        annotate(t, mem, cpu->r[i], false);
        put(t, "\n");
    }
    put(t, "  cpsr 0x%08X\n", cpu->cpsr);
    u32 frames[k_max_frames];
    const u32 n = mt_crash_backtrace(cpu, frames, k_max_frames);
    put(t, "backtrace (r7 frame chain%s):\n", n == k_max_frames ? ", truncated" : "");
    u32 i0 = 0;
    if (!hardware && info->pc != 0u) {
        put(t, "  #0 0x%08X", info->pc);
        annotate(t, mem, info->pc, true);
        put(t, "\n");
        i0 = 1;
    }
    if (n == 0u) {
        put(t, "  (no valid frame record at r7 0x%08X)\n", cpu->r[7]);
    }
    for (u32 i = 0; i < n; i++) {
        put(t, "  #%u 0x%08X", i + i0, frames[i]);
        if (frames[i] == MT_HLE_RETURN) {
            put(t, " (return into an HLE function)");
        } else {
            annotate(t, mem, frames[i], true);
        }
        put(t, "\n");
    }
    put(t, "=== end of guest crash ===\n");
    return t.len;
}

extern "C" void mt_crash_report_trap(const mt_cpu *cpu, std::uint32_t kind, std::uint32_t pc,
                                     std::uint32_t detail) noexcept {
    mt_crash_info info{};
    info.kind = kind;
    info.pc = pc;
    info.detail = detail;
    mt_mem_fault_info fault{};
    if (kind == MT_TRAP_MEMORY && mt_mem_last_fault(&fault) == 0 && fault.addr == detail) {
        info.has_fault = 1;
        info.fault_addr = fault.addr;
        info.fault_size = fault.size;
    } else if (kind == MT_TRAP_MEMORY) {
        info.has_fault = 1;
        info.fault_addr = detail;
    }
    const std::size_t n = mt_crash_format(cpu, &info, nullptr, 0) + 1u;
    char *buf = static_cast<char *>(std::malloc(n));
    if (buf == nullptr) {
        return;
    }
    mt_crash_format(cpu, &info, buf, n);
    write_crash_file(buf, n - 1u);
    if (g_sink != nullptr) {
        g_sink(buf, n - 1u, 0, g_sink_user);
    }
    for (char *line = buf; *line != '\0';) {
        char *nl = std::strchr(line, '\n');
        if (nl != nullptr) {
            *nl = '\0';
        }
        mt_diag_log(line);
        if (nl == nullptr) {
            break;
        }
        line = nl + 1;
    }
    std::free(buf);
}

extern "C" int mt_crash_install(void) noexcept {
    if (g_installed.exchange(1) != 0) {
        return 0;
    }
#if defined(_WIN32)
    if (AddVectoredExceptionHandler(1, on_exception) == nullptr) {
        g_installed = 0;
        return -1;
    }
    if (g_sink != nullptr) {
        SetUnhandledExceptionFilter(on_unhandled);
        signal(SIGABRT, on_abort);
    }
    return 0;
#elif defined(__SWITCH__)
    // libnx reports a CPU exception through __libnx_exception_handler, not a signal.
    mt_crash_horizon_install();
    return 0;
#else
    // An alternate stack, so that a host stack overflow (deep guest recursion) is reported
    // too. Allocated once and never freed: the handlers stay for the life of the process.
    stack_t ss{};
    ss.ss_size = 1u << 16;
    ss.ss_sp = std::malloc(ss.ss_size);
    if (ss.ss_sp != nullptr) {
        sigaltstack(&ss, nullptr);
    }
    struct sigaction sa{};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (std::size_t i = 0; i < sizeof k_signals / sizeof k_signals[0]; i++) {
        if (sigaction(k_signals[i], &sa, &g_prev[i]) != 0) {
            return -1;
        }
    }
    return 0;
#endif
}

extern "C" void mt_crash_set_cpu(mt_cpu *cpu) noexcept { t_cpu = cpu; }

extern "C" mt_cpu *mt_crash_get_cpu(void) noexcept { return t_cpu; }

extern "C" void mt_crash_set_file(const char *path) noexcept {
    if (path == nullptr || std::strlen(path) >= k_file_max) {
        g_file[0] = '\0';
        return;
    }
    std::memcpy(g_file, path, std::strlen(path) + 1u);
}

extern "C" void mt_crash_set_host_functions(const mt_fn_entry *table,
                                            std::uint32_t count) noexcept {
    g_fns = count != 0u ? table : nullptr;
    g_fn_count = table != nullptr ? count : 0u;
}

extern "C" void mt_crash_set_sink(mt_crash_sink_fn sink, void *user) noexcept {
    g_sink = sink;
    g_sink_user = user;
}
