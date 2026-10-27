// libSystem.B.dylib, the C library part (ADR 0028): the heap functions on the guest heap
// (ADR 0025), the mem and str functions, atoi/atol, the printf family over the guest format
// engine (format.h), floor/pow/trunc, gettimeofday, abort/__assert_rtn and the standard FILE
// objects behind ___stderrp. exit is process start-up's (runtime/src/boot/process.c, #43). sysctl
// and the socket stubs are in libc_sys.cpp.
//
// Every function reads its arguments with maplethief/hle/abi.h (iOS armv7 rules, ADR 0019)
// and every guest pointer goes through the checked mt_g2h / mt_g2h_str, so a bad pointer
// stops the guest with the caller's address. Results follow iOS's libc where the guest can
// see them (the values of strcmp and memcmp, puts returning '\n', atoi saturating).
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hle/format.h"
#include "hle/hle.h"
#include "hle/libc_internal.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/libc.h"
#include "maplethief/memory.h"

// --- configuration and diagnostics ---------------------------------------------------------

namespace {

mt_libc_config g_cfg = mt_libc_config_default();

// The model codes of the devices the game ran on (iOS 4 and 5 era).
constexpr struct {
    const char *machine;
    const char *model;
} k_models[] = {
    {"iPhone2,1", "N88AP"}, {"iPhone3,1", "N90AP"}, {"iPhone3,3", "N92AP"},
    {"iPhone4,1", "N94AP"}, {"iPod3,1", "N18AP"},   {"iPod4,1", "N81AP"},
    {"iPod5,1", "N78AP"},   {"iPad1,1", "K48AP"},   {"iPad2,1", "K93AP"},
};

uint64_t host_clock_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace

extern "C" mt_libc_config mt_libc_config_default(void) noexcept {
    mt_libc_config cfg{};
    std::snprintf(cfg.hw_machine, sizeof cfg.hw_machine, "%s", MT_LIBC_DEFAULT_HW_MACHINE);
    cfg.en0_mac[0] = 0x02u; // locally administered, unicast; the rest 0
    return cfg;
}

extern "C" int mt_libc_config_from_env(mt_libc_config *cfg) noexcept {
    char value[64] = {};
#if defined(_MSC_VER)
    char *dup = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&dup, &len, MT_LIBC_HW_MACHINE_ENV) != 0 || dup == nullptr) {
        return 0;
    }
    std::snprintf(value, sizeof value, "%s", dup);
    std::free(dup);
#else
    const char *env = std::getenv(MT_LIBC_HW_MACHINE_ENV);
    if (env == nullptr) {
        return 0;
    }
    std::snprintf(value, sizeof value, "%s", env);
#endif
    const std::size_t n = std::strlen(value);
    if (n == 0u || n >= sizeof cfg->hw_machine) {
        mt_libc_log("%s=%s is not a hw.machine string (1-31 bytes); keeping %s",
                    MT_LIBC_HW_MACHINE_ENV, value, cfg->hw_machine);
        return -1;
    }
    std::memcpy(cfg->hw_machine, value, n + 1u);
    return 0;
}

extern "C" void mt_libc_configure(const mt_libc_config *cfg) noexcept {
    g_cfg = cfg != nullptr ? *cfg : mt_libc_config_default();
    g_cfg.hw_machine[sizeof g_cfg.hw_machine - 1u] = 0;
}

extern "C" const char *mt_libc_hw_model(const char *machine) noexcept {
    for (const auto &m : k_models) {
        if (machine != nullptr && std::strcmp(machine, m.machine) == 0) {
            return m.model;
        }
    }
    return "Unknown";
}

const mt_libc_config &mt_libc_cfg() noexcept { return g_cfg; }

void mt_libc_log(const char *fmt, ...) noexcept {
    char line[512] = "libc: ";
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + 6, sizeof line - 6u, fmt, ap);
    va_end(ap);
    if (g_cfg.log != nullptr) {
        g_cfg.log(g_cfg.log_user, line);
        return;
    }
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void mt_libc_write(int fd, const char *data, std::size_t len) noexcept {
    if (g_cfg.write != nullptr) {
        g_cfg.write(g_cfg.write_user, fd, data, len);
        return;
    }
    std::FILE *f = fd == 2 ? stderr : stdout;
    std::fwrite(data, 1u, len, f);
    std::fflush(f);
}

// --- guest objects: the standard FILEs ------------------------------------------------------

namespace {

// iOS's 32-bit struct __sFILE is 88 bytes; _flags (short) is at +12 and _file (short) at +14.
constexpr std::uint32_t k_file_size = 88u;
constexpr std::uint32_t k_file_flags = 12u;
constexpr std::uint32_t k_file_fd = 14u;

// The FILE objects of stdin, stdout and stderr in the guest heap, made on first use. The guest
// is single-threaded (ADR 0014); a new address space (a test) gets new objects.
struct StdFiles {
    const std::uint8_t *base = nullptr; // cpu->mem they belong to
    std::uint32_t file[3] = {};
};
StdFiles g_files;

mt_mem *guest_mem(mt_cpu *cpu, const char *what) {
    mt_mem *mem = mt_mem_from_cpu(cpu);
    if (mem == nullptr) {
        mt_libc_log("%s: the guest cpu is not attached to an address space", what);
    }
    return mem;
}

std::uint32_t std_file(mt_cpu *cpu, int fd) {
    if (g_files.base != cpu->mem) {
        g_files = StdFiles{};
        g_files.base = cpu->mem;
    }
    std::uint32_t &f = g_files.file[fd];
    if (f == 0u) {
        mt_mem *mem = guest_mem(cpu, "standard FILE");
        f = mem != nullptr ? mt_heap_calloc(mem, 1u, k_file_size) : 0u;
        if (f != 0u) {
            // FreeBSD's std(): __SRD for stdin, __SWR for stdout, __SWR|__SNBF for stderr.
            static constexpr std::uint16_t k_flags[3] = {0x0004u, 0x0008u, 0x000Au};
            const std::uint16_t file_fd = static_cast<std::uint16_t>(fd);
            std::memcpy(cpu->mem + f + k_file_flags, &k_flags[fd], 2u);
            std::memcpy(cpu->mem + f + k_file_fd, &file_fd, 2u);
        }
    }
    return f;
}

void init_std_pointer(mt_cpu *cpu, std::uint32_t addr, int fd) {
    mt_st32(cpu, addr, std_file(cpu, fd), 0u);
}

// The descriptor of a guest FILE *, or -1.
int file_fd(mt_cpu *cpu, std::uint32_t stream) {
    std::int16_t fd;
    std::memcpy(&fd, mt_g2h(cpu, stream, k_file_size) + k_file_fd, 2u);
    return fd;
}

// Stops the guest the way iOS's libc does when it calls abort().
MT_NORETURN void guest_abort(mt_cpu *cpu, std::uint32_t detail) {
    mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], detail);
}

void write_text(int fd, const std::string &text) { mt_libc_write(fd, text.data(), text.size()); }

} // namespace

extern "C" {

void mt_hle_init_stdinp(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    init_std_pointer(cpu, addr, 0);
}
void mt_hle_init_stdoutp(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    init_std_pointer(cpu, addr, 1);
}
void mt_hle_init_stderrp(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    init_std_pointer(cpu, addr, 2);
}

// --- the heap -------------------------------------------------------------------------------

// void *malloc(size_t size)
void mt_hle_malloc(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t size = mt_arg_u32(&a);
    mt_mem *mem = guest_mem(cpu, "malloc");
    mt_ret_u32(cpu, mem != nullptr ? mt_heap_malloc(mem, size) : 0u);
}

// void *calloc(size_t count, size_t size)
void mt_hle_calloc(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t count = mt_arg_u32(&a), size = mt_arg_u32(&a);
    mt_mem *mem = guest_mem(cpu, "calloc");
    mt_ret_u32(cpu, mem != nullptr ? mt_heap_calloc(mem, count, size) : 0u);
}

// void *realloc(void *ptr, size_t size). Also reached through a function pointer (bson).
void mt_hle_realloc(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t ptr = mt_arg_u32(&a), size = mt_arg_u32(&a);
    mt_mem *mem = guest_mem(cpu, "realloc");
    if (mem == nullptr) {
        mt_ret_u32(cpu, 0u);
        return;
    }
    if (ptr != 0u && mt_heap_usable_size(mem, ptr) == 0u) {
        mt_libc_log("malloc: *** error for object 0x%08X: pointer being realloc'd was not "
                    "allocated (realloc called from 0x%08X)",
                    static_cast<unsigned>(ptr), static_cast<unsigned>(cpu->r[MT_LR]));
        guest_abort(cpu, ptr);
    }
    mt_ret_u32(cpu, mt_heap_realloc(mem, ptr, size));
}

// void free(void *ptr)
void mt_hle_free(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t ptr = mt_arg_u32(&a);
    mt_mem *mem = guest_mem(cpu, "free");
    if (mem != nullptr && mt_heap_free(mem, ptr) != 0) {
        mt_libc_log("malloc: *** error for object 0x%08X: pointer being freed was not "
                    "allocated (free called from 0x%08X)",
                    static_cast<unsigned>(ptr), static_cast<unsigned>(cpu->r[MT_LR]));
        guest_abort(cpu, ptr);
    }
}

// --- memory and strings ---------------------------------------------------------------------

// void *memcpy(void *dst, const void *src, size_t n): overlap-safe, like iOS's memcpy.
void mt_hle_memcpy(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t dst = mt_arg_u32(&a), src = mt_arg_u32(&a), n = mt_arg_u32(&a);
    if (n != 0u) {
        std::memmove(mt_g2h(cpu, dst, n), mt_g2h(cpu, src, n), n);
    }
    mt_ret_u32(cpu, dst);
}

// void *memset(void *dst, int c, size_t n)
void mt_hle_memset(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t dst = mt_arg_u32(&a), c = mt_arg_u32(&a), n = mt_arg_u32(&a);
    if (n != 0u) {
        std::memset(mt_g2h(cpu, dst, n), static_cast<int>(c & 0xFFu), n);
    }
    mt_ret_u32(cpu, dst);
}

// int memcmp(const void *a, const void *b, size_t n): the difference of the first unequal
// bytes (as unsigned char), 0 if none.
void mt_hle_memcmp(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t p = mt_arg_u32(&a), q = mt_arg_u32(&a), n = mt_arg_u32(&a);
    std::int32_t r = 0;
    if (n != 0u) {
        const std::uint8_t *x = mt_g2h(cpu, p, n), *y = mt_g2h(cpu, q, n);
        for (std::uint32_t i = 0; i < n; i++) {
            if (x[i] != y[i]) {
                r = static_cast<std::int32_t>(x[i]) - static_cast<std::int32_t>(y[i]);
                break;
            }
        }
    }
    mt_ret_u32(cpu, static_cast<std::uint32_t>(r));
}

// size_t strlen(const char *s)
void mt_hle_strlen(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t s = mt_arg_u32(&a);
    mt_ret_u32(cpu, static_cast<std::uint32_t>(std::strlen(mt_g2h_str(cpu, s))));
}

// char *strcpy(char *dst, const char *src)
void mt_hle_strcpy(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t dst = mt_arg_u32(&a), src = mt_arg_u32(&a);
    const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(mt_g2h_str(cpu, src))) + 1u;
    std::memmove(mt_g2h(cpu, dst, n), cpu->mem + src, n);
    mt_ret_u32(cpu, dst);
}

// char *strcat(char *dst, const char *src)
void mt_hle_strcat(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t dst = mt_arg_u32(&a), src = mt_arg_u32(&a);
    const std::uint32_t end = dst + static_cast<std::uint32_t>(std::strlen(mt_g2h_str(cpu, dst)));
    const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(mt_g2h_str(cpu, src))) + 1u;
    std::memmove(mt_g2h(cpu, end, n), cpu->mem + src, n);
    mt_ret_u32(cpu, dst);
}

// int strcmp(const char *a, const char *b): the difference of the first unequal bytes.
void mt_hle_strcmp(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const auto *x = reinterpret_cast<const std::uint8_t *>(mt_g2h_str(cpu, mt_arg_u32(&a)));
    const auto *y = reinterpret_cast<const std::uint8_t *>(mt_g2h_str(cpu, mt_arg_u32(&a)));
    std::size_t i = 0;
    while (x[i] != 0u && x[i] == y[i]) {
        i++;
    }
    mt_ret_u32(cpu, static_cast<std::uint32_t>(static_cast<std::int32_t>(x[i]) -
                                               static_cast<std::int32_t>(y[i])));
}

} // extern "C"

namespace {

// strtol(s, NULL, 10) with iOS's 32-bit long: leading white space, a sign, decimal digits;
// out of range saturates to LONG_MIN / LONG_MAX.
std::int32_t parse_long(const char *s) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) {
        s++;
    }
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    std::int64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > 0x80000000ll) {
            v = 0x80000000ll; // saturated; keep scanning
        }
    }
    if (neg) {
        return static_cast<std::int32_t>(-v);
    }
    return v > 0x7FFFFFFFll ? INT32_MAX : static_cast<std::int32_t>(v);
}

} // namespace

extern "C" {

// int atoi(const char *s)
void mt_hle_atoi(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_u32(cpu, static_cast<std::uint32_t>(parse_long(mt_g2h_str(cpu, mt_arg_u32(&a)))));
}

// long atol(const char *s): long is 32 bits on armv7.
void mt_hle_atol(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_u32(cpu, static_cast<std::uint32_t>(parse_long(mt_g2h_str(cpu, mt_arg_u32(&a)))));
}

// --- stdio ----------------------------------------------------------------------------------

// int printf(const char *fmt, ...)
void mt_hle_printf(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const char *fmt = mt_g2h_str(cpu, mt_arg_u32(&a));
    std::string text;
    const int n = mt_format(cpu, fmt, &a, nullptr, text);
    write_text(1, text);
    mt_ret_u32(cpu, static_cast<std::uint32_t>(n));
}

// int puts(const char *s): s and a newline on stdout; returns '\n' (FreeBSD's value).
void mt_hle_puts(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    std::string text = mt_g2h_str(cpu, mt_arg_u32(&a));
    text.push_back('\n');
    write_text(1, text);
    mt_ret_u32(cpu, static_cast<std::uint32_t>('\n'));
}

// int vfprintf(FILE *stream, const char *fmt, va_list ap): stdout and stderr only (the game
// has no other FILE: fopen is not imported).
void mt_hle_vfprintf(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t stream = mt_arg_u32(&a), fmt = mt_arg_u32(&a), ap = mt_arg_u32(&a);
    const int fd = file_fd(cpu, stream);
    mt_args va = mt_args_va_list(cpu, ap);
    std::string text;
    int n = mt_format(cpu, mt_g2h_str(cpu, fmt), &va, nullptr, text);
    if (fd == 1 || fd == 2) {
        write_text(fd, text);
    } else {
        mt_libc_log("vfprintf to FILE 0x%08X (fd %d), which is not stdout or stderr, from "
                    "0x%08X: dropped",
                    static_cast<unsigned>(stream), fd, static_cast<unsigned>(cpu->r[MT_LR]));
        n = -1;
    }
    mt_ret_u32(cpu, static_cast<std::uint32_t>(n));
}

// int vsprintf(char *buf, const char *fmt, va_list ap)
void mt_hle_vsprintf(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t buf = mt_arg_u32(&a), fmt = mt_arg_u32(&a), ap = mt_arg_u32(&a);
    mt_args va = mt_args_va_list(cpu, ap);
    std::string text;
    const int n = mt_format(cpu, mt_g2h_str(cpu, fmt), &va, nullptr, text);
    const auto size = static_cast<std::uint32_t>(text.size()) + 1u;
    std::memcpy(mt_g2h(cpu, buf, size), text.c_str(), size);
    mt_ret_u32(cpu, static_cast<std::uint32_t>(n));
}

// int __snprintf_chk(char *buf, size_t maxlen, int flag, size_t slen, const char *fmt, ...):
// snprintf, after the _FORTIFY_SOURCE check that the object is at least maxlen bytes.
void mt_hle_snprintf_chk(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t buf = mt_arg_u32(&a), maxlen = mt_arg_u32(&a);
    mt_arg_u32(&a); // flag
    const std::uint32_t slen = mt_arg_u32(&a), fmt = mt_arg_u32(&a);
    if (slen < maxlen) {
        mt_libc_log("__snprintf_chk: buffer overflow detected (object of %u bytes, maxlen %u) "
                    "at 0x%08X",
                    static_cast<unsigned>(slen), static_cast<unsigned>(maxlen),
                    static_cast<unsigned>(cpu->r[MT_LR]));
        guest_abort(cpu, buf);
    }
    std::string text;
    const int n = mt_format(cpu, mt_g2h_str(cpu, fmt), &a, nullptr, text);
    if (maxlen != 0u) {
        const std::uint32_t keep =
            text.size() < maxlen ? static_cast<std::uint32_t>(text.size()) : maxlen - 1u;
        std::uint8_t *dst = mt_g2h(cpu, buf, keep + 1u);
        std::memcpy(dst, text.data(), keep);
        dst[keep] = 0u;
    }
    mt_ret_u32(cpu, static_cast<std::uint32_t>(n));
}

// --- math -----------------------------------------------------------------------------------

// double floor(double x)
void mt_hle_floor(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_f64(cpu, std::floor(mt_arg_f64(&a)));
}

// double trunc(double x)
void mt_hle_trunc(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_f64(cpu, std::trunc(mt_arg_f64(&a)));
}

// double pow(double x, double y)
void mt_hle_pow(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const double x = mt_arg_f64(&a), y = mt_arg_f64(&a);
    mt_ret_f64(cpu, std::pow(x, y));
}

// --- time -----------------------------------------------------------------------------------

// int gettimeofday(struct timeval *tv, struct timezone *tz): timeval is {int32 tv_sec,
// int32 tv_usec} on armv7; the time zone is always UTC with no DST.
void mt_hle_gettimeofday(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t tv = mt_arg_u32(&a), tz = mt_arg_u32(&a);
    if (tv != 0u) {
        const std::uint64_t us =
            g_cfg.clock != nullptr ? g_cfg.clock(g_cfg.clock_user) : host_clock_us();
        const std::uint32_t words[2] = {static_cast<std::uint32_t>(us / 1000000u),
                                        static_cast<std::uint32_t>(us % 1000000u)};
        std::memcpy(mt_g2h(cpu, tv, 8u), words, 8u);
    }
    if (tz != 0u) {
        std::memset(mt_g2h(cpu, tz, 8u), 0, 8u);
    }
    mt_ret_u32(cpu, 0u);
}

// --- the end of the process -----------------------------------------------------------------

// void abort(void)
void mt_hle_abort(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_libc_log("abort() called from 0x%08X", static_cast<unsigned>(cpu->r[MT_LR]));
    guest_abort(cpu, 0u);
}

// void __assert_rtn(const char *func, const char *file, int line, const char *expr): iOS's
// assert() message on stderr, then abort().
void mt_hle_assert_rtn(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t func = mt_arg_u32(&a), file = mt_arg_u32(&a);
    const std::int32_t line = mt_arg_i32(&a);
    const std::uint32_t expr = mt_arg_u32(&a);
    const char *e = expr != 0u ? mt_g2h_str(cpu, expr) : "(null)";
    const char *fl = file != 0u ? mt_g2h_str(cpu, file) : "(null)";
    char head[64];
    std::string text = "Assertion failed: (";
    text += e;
    text += "), ";
    if (func != 0u) {
        text += "function ";
        text += mt_g2h_str(cpu, func);
        text += ", ";
    }
    std::snprintf(head, sizeof head, ", line %d.\n", static_cast<int>(line));
    text += "file ";
    text += fl;
    text += head;
    write_text(2, text);
    mt_libc_log("__assert_rtn called from 0x%08X", static_cast<unsigned>(cpu->r[MT_LR]));
    guest_abort(cpu, 0u);
}

} // extern "C"
