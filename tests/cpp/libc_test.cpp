// The libc subset of the HLE layer (ADR 0028, #44): every libSystem C-library import, called
// as the guest calls it (arguments in r0-r3 and on the guest stack, iOS armv7 rules), on a
// real guest address space (ADR 0025). The printf family is checked against the host's
// printf for a matrix of format strings, and against iOS's own output where the hosts
// differ (those cases are also compared with the host on Apple platforms).
#include "maplethief/hle/libc.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "guest_trap.h"
#include "hle/format.h"
#include "hle/hle.h"
#include "maplethief/hle/abi.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"

namespace {

constexpr std::uint32_t k_caller = 0x0002A3C5u; // lr of every call: a Thumb return address

// A guest address space with a stack and a cpu, and the libc's output and log captured.
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    std::string out[3];
    std::vector<std::string> log;
    std::uint64_t now_us = 0;
    mt_libc_config base{}; // the configuration with the capturing sinks

    Guest() {
        mt_mem_config cfg = mt_mem_config_default();
        mem = mt_mem_create(&cfg);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0u, &stack) == 0);
        mt_libc_config lc = mt_libc_config_default();
        lc.write = [](void *user, int fd, const char *data, std::size_t len) {
            static_cast<Guest *>(user)->out[fd].append(data, len);
        };
        lc.write_user = this;
        lc.log = [](void *user, const char *line) {
            static_cast<Guest *>(user)->log.emplace_back(line);
        };
        lc.log_user = this;
        lc.clock = [](void *user) { return static_cast<Guest *>(user)->now_us; };
        lc.clock_user = this;
        base = lc;
        mt_libc_configure(&base);
    }
    ~Guest() {
        mt_libc_configure(nullptr);
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    std::uint8_t *host(std::uint32_t addr) const { return cpu.mem + addr; }
    std::uint32_t alloc(std::uint32_t n) {
        const std::uint32_t a = mt_heap_calloc(mem, 1u, n);
        REQUIRE(a != 0u);
        return a;
    }
    std::uint32_t str(const char *s) {
        const auto n = static_cast<std::uint32_t>(std::strlen(s)) + 1u;
        const std::uint32_t a = alloc(n);
        std::memcpy(host(a), s, n);
        return a;
    }
    std::string cstr(std::uint32_t addr) const {
        return reinterpret_cast<const char *>(host(addr));
    }
    std::uint32_t get32(std::uint32_t addr) const {
        std::uint32_t v;
        std::memcpy(&v, host(addr), 4u);
        return v;
    }
    void put32(std::uint32_t addr, std::uint32_t v) { std::memcpy(host(addr), &v, 4u); }

    // Lays out the argument words like the guest's caller: r0-r3, then [sp], [sp+4], ...
    void args(const std::vector<std::uint32_t> &words) {
        cpu.r[MT_SP] = stack.top - 0x400u;
        for (std::size_t i = 0; i < words.size(); i++) {
            if (i < 4u) {
                cpu.r[i] = words[i];
            } else {
                put32(cpu.r[MT_SP] + 4u * static_cast<std::uint32_t>(i - 4u), words[i]);
            }
        }
        cpu.r[MT_LR] = k_caller;
    }
    std::uint32_t call(mt_import_fn fn, const std::vector<std::uint32_t> &words) {
        args(words);
        fn(&cpu, 0u);
        return cpu.r[0];
    }
    double call_f64(mt_import_fn fn, const std::vector<std::uint32_t> &words) {
        call(fn, words);
        return mt_f64(static_cast<std::uint64_t>(cpu.r[1]) << 32 | cpu.r[0]);
    }
    // True if the call stopped the guest through mt_trap (the trap is in mt_test::g_trap).
    bool traps(mt_import_fn fn, const std::vector<std::uint32_t> &words) {
        args(words);
        struct Call {
            mt_import_fn fn;
            mt_cpu *cpu;
        } c{fn, &cpu};
        return mt_test::stops(
            [](void *arg) {
                auto *x = static_cast<Call *>(arg);
                x->fn(x->cpu, 0u);
            },
            &c);
    }
};

std::uint32_t lo(double d) { return static_cast<std::uint32_t>(mt_f64_bits(d)); }
std::uint32_t hi(double d) { return static_cast<std::uint32_t>(mt_f64_bits(d) >> 32); }

// --- the printf matrix --------------------------------------------------------------------

// A guest argument and the host value that means the same.
void push(Guest &, std::vector<std::uint32_t> &w, int v) {
    w.push_back(static_cast<std::uint32_t>(v));
}
void push(Guest &, std::vector<std::uint32_t> &w, unsigned v) { w.push_back(v); }
void push(Guest &, std::vector<std::uint32_t> &w, long long v) {
    w.push_back(static_cast<std::uint32_t>(v));
    w.push_back(static_cast<std::uint32_t>(static_cast<unsigned long long>(v) >> 32));
}
void push(Guest &, std::vector<std::uint32_t> &w, unsigned long long v) {
    w.push_back(static_cast<std::uint32_t>(v));
    w.push_back(static_cast<std::uint32_t>(v >> 32));
}
void push(Guest &, std::vector<std::uint32_t> &w, double v) {
    w.push_back(lo(v));
    w.push_back(hi(v));
}
void push(Guest &g, std::vector<std::uint32_t> &w, const char *v) {
    w.push_back(v != nullptr ? g.str(v) : 0u);
}

// What the guest's printf, vsprintf and __snprintf_chk produce for gfmt and the arguments.
// Each path reads the varargs differently: printf from r1 on (so 64-bit values get split
// between r3 and the stack), vsprintf from a va_list in memory, __snprintf_chk from [sp+4].
struct Formatted {
    std::string printf_out, vsprintf_out, snprintf_out;
    std::uint32_t printf_ret = 0, vsprintf_ret = 0, snprintf_ret = 0;
};

Formatted run_formats(Guest &g, const char *gfmt, const std::vector<std::uint32_t> &w) {
    Formatted f;
    const std::uint32_t fmt = g.str(gfmt);
    std::vector<std::uint32_t> words{fmt};
    words.insert(words.end(), w.begin(), w.end());
    g.out[1].clear();
    f.printf_ret = g.call(mt_hle_printf, words);
    f.printf_out = g.out[1];

    const std::uint32_t buf = g.alloc(1024u);
    const std::uint32_t ap = g.alloc(4u * static_cast<std::uint32_t>(w.size()) + 4u);
    for (std::size_t i = 0; i < w.size(); i++) {
        g.put32(ap + 4u * static_cast<std::uint32_t>(i), w[i]);
    }
    f.vsprintf_ret = g.call(mt_hle_vsprintf, {buf, fmt, ap});
    f.vsprintf_out = g.cstr(buf);

    std::vector<std::uint32_t> chk{buf, 1024u, 0u, 1024u, fmt};
    chk.insert(chk.end(), w.begin(), w.end());
    std::memset(g.host(buf), 0x55, 1024u);
    f.snprintf_ret = g.call(mt_hle_snprintf_chk, chk);
    f.snprintf_out = g.cstr(buf);
    return f;
}

void check_formatted(const Formatted &f, const std::string &expect, const char *gfmt) {
    INFO("format: \"", gfmt, "\"");
    CHECK(f.printf_out == expect);
    CHECK(f.vsprintf_out == expect);
    CHECK(f.snprintf_out == expect);
    CHECK(f.printf_ret == expect.size());
    CHECK(f.vsprintf_ret == expect.size());
    CHECK(f.snprintf_ret == expect.size());
}

// The guest format gfmt must print what the host's snprintf prints for hfmt with the same
// values. hfmt differs from gfmt only where the guest's sizes differ from the host's (long,
// size_t and ptrdiff_t are 32 bits on armv7). Every case has at least one argument.
template <typename... A> void same_as_host(Guest &g, const char *gfmt, const char *hfmt, A... a) {
    static_assert(sizeof...(A) > 0, "every host case has an argument");
    char host[512];
    std::snprintf(host, sizeof host, hfmt, a...);
    std::vector<std::uint32_t> w;
    (push(g, w, a), ...);
    check_formatted(run_formats(g, gfmt, w), host, gfmt);
}

// iOS's output where hosts differ (%p, %a, inf/nan, FreeBSD's padding of %s and unknown
// conversions, guest-sized long). host_too: the host's types match the guest's, so on Apple
// platforms the host's printf (the same FreeBSD code) must agree as well.
template <typename... A>
void like_ios(Guest &g, const char *fmt, const char *expect, bool host_too, A... a) {
    std::vector<std::uint32_t> w;
    (push(g, w, a), ...);
    check_formatted(run_formats(g, fmt, w), expect, fmt);
#if defined(__APPLE__)
    if constexpr (sizeof...(A) > 0) {
        if (host_too) {
            char host[512];
            std::snprintf(host, sizeof host, fmt, a...);
            CHECK_MESSAGE(std::string(host) == expect, "host printf: ", fmt);
        }
    }
#else
    (void)host_too;
#endif
}

} // namespace

// --- printf family -------------------------------------------------------------------------

TEST_CASE("printf, vsprintf and __snprintf_chk match the host's printf: integers") {
    Guest g;
    same_as_host(g, "%d", "%d", 0);
    same_as_host(g, "%d|%i", "%d|%i", 42, -42);
    same_as_host(g, "%d", "%d", std::numeric_limits<int>::min());
    same_as_host(g, "[%5d][%-5d][%05d]", "[%5d][%-5d][%05d]", 42, -42, -42);
    same_as_host(g, "[%+d][% d][%+d][% d]", "[%+d][% d][%+d][% d]", 7, 7, -7, -7);
    same_as_host(g, "[%.3d][%8.3d][%-8.3d][%08.3d]", "[%.3d][%8.3d][%-8.3d][%08.3d]", 5, -5, 5, 5);
    same_as_host(g, "[%.0d][%+.0d][%5.0d]", "[%.0d][%+.0d][%5.0d]", 0, 0, 0);
    same_as_host(g, "%u", "%u", 4294967295u);
    same_as_host(g, "[%x][%X][%#x][%#X][%#10x][%-#10x]", "[%x][%X][%#x][%#X][%#10x][%-#10x]",
                 0xDEADBEEFu, 0xDEADBEEFu, 255u, 255u, 26u, 26u);
    same_as_host(g, "[%o][%#o][%#5o][%.4o]", "[%o][%#o][%#5o][%.4o]", 8u, 8u, 8u, 8u);
    same_as_host(g, "[%hd][%hu][%hhd][%hhu][%hx]", "[%hd][%hu][%hhd][%hhu][%hx]", 70000, 65537u,
                 300, 257u, 0x12345u);
    // long, size_t and ptrdiff_t are one word on the guest.
    same_as_host(g, "[%ld][%lu][%lx][%li]", "[%d][%u][%x][%i]", -5, 4000000000u, 0xFFFFFFFFu, 9);
    same_as_host(g, "[%zu][%zd][%zx][%td]", "[%u][%d][%x][%d]", 4294967295u, -1, 255u, -3);
    same_as_host(g, "[%lld][%llu][%llx][%llX]", "[%lld][%llu][%llx][%llX]", -5LL,
                 18446744073709551615ULL, 0x123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
    same_as_host(g, "[%qd][%jd][%ju]", "[%lld][%lld][%llu]", -9000000000LL, 9000000000LL,
                 18446744073709551615ULL);
    same_as_host(g, "[%lld][%.20lld][%-25lld]", "[%lld][%.20lld][%-25lld]",
                 std::numeric_limits<long long>::min(), -1LL, 123LL);
}

TEST_CASE("printf, vsprintf and __snprintf_chk match the host's printf: strings, chars, %%") {
    Guest g;
    same_as_host(g, "%s", "%s", "hello");
    same_as_host(g, "[%10s][%-10s][%.3s][%10.2s][%-6.1s][%.0s]",
                 "[%10s][%-10s][%.3s][%10.2s][%-6.1s][%.0s]", "abc", "abc", "abcdef", "xyz", "pq",
                 "gone");
    same_as_host(g, "[%s][%s]", "[%s][%s]", "", "\xEC\x95\x88\xEB\x85\x95"); // UTF-8 bytes
    same_as_host(g, "[%c][%5c][%-3c][%c]", "[%c][%5c][%-3c][%c]", 'A', 'b', 'c', 0x141);
    same_as_host(g, "100%% %d%%", "100%% %d%%", 5);
    same_as_host(g, "%s=%d (%s) %c", "%s=%d (%s) %c", "hp", 120, "max", '!');
}

TEST_CASE("printf, vsprintf and __snprintf_chk match the host's printf: floating point") {
    Guest g;
    same_as_host(g, "[%f][%.2f][%10.3f][%-10.1f][%+f][% f]",
                 "[%f][%.2f][%10.3f][%-10.1f][%+f][% f]", 3.14159, 3.14159, -2.5, 2.25, 1.0, 1.0);
    same_as_host(g, "[%010.4f][%-010.2f][%+08.2f][%#.0f][%.0f][%.0f][%.0f]",
                 "[%010.4f][%-010.2f][%+08.2f][%#.0f][%.0f][%.0f][%.0f]", -3.5, 1.5, 2.0, 2.0, 0.5,
                 1.5, 2.5);
    same_as_host(g, "[%f][%f][%.10f][%F]", "[%f][%f][%.10f][%F]", 1e20, -0.0, 1.0 / 3.0, 1.5);
    same_as_host(g, "[%e][%E][%.0e][%#.0e][%+.3e][%12.4e]", "[%e][%E][%.0e][%#.0e][%+.3e][%12.4e]",
                 12345.678, 12345.678, 5.5, 5.0, -1234.5, 1e-300);
    same_as_host(g, "[%g][%g][%G][%g][%g][%#g][%.17g][%.3g]",
                 "[%g][%g][%G][%g][%g][%#g][%.17g][%.3g]", 0.0001, 1e-5, 1e-10, 123456789.0,
                 100000.0, 1.0, 0.1, 2.0 / 3.0);
    // long double is double on armv7: %Lf takes two words.
    same_as_host(g, "[%Lf][%LG]", "[%f][%G]", 2.5, 1e30);
    // %f of a float argument: the caller promotes it to double.
    same_as_host(g, "%.6f", "%.6f", static_cast<double>(1.1f));
}

TEST_CASE("printf, vsprintf and __snprintf_chk match the host's printf: * and mixed words") {
    Guest g;
    same_as_host(g, "[%*d][%-*d][%*d]", "[%*d][%-*d][%*d]", 6, 42, 6, 42, -6, 42);
    same_as_host(g, "[%.*f][%.*f][%*.*s]", "[%.*f][%.*f][%*.*s]", 2, 3.14159, -1, 3.14159, 8, 3,
                 "abcdef");
    // printf: int r1, int r2, double r3 and [sp] (split), then the stack.
    same_as_host(g, "%d %d %f %s %lld %c", "%d %d %f %s %lld %c", 1, 2, 0.5, "x", -7LL, 'z');
    // A 64-bit value right after the format: r1:r2, no alignment (iOS).
    same_as_host(g, "%lld|%d|%e|%d", "%lld|%d|%e|%d", 0x100000002LL, 3, 4.0, 5);
}

TEST_CASE("the printf engine prints what iOS prints where hosts differ") {
    Guest g;
    // %p: 0x and lower-case hex of a 32-bit pointer; "0x0" for NULL.
    like_ios(g, "[%p][%p][%10p][%-10p][%010p][%.6p][%#p]",
             "[0x0][0x1234][    0x1234][0x1234    ][0x00001234][0x000012][0x12]", false, 0u,
             0x1234u, 0x1234u, 0x1234u, 0x1234u, 0x12u, 0x12u);
    // %a: FreeBSD's __hdtoa, normalized, including its rounding quirk for a dropped 8.
    like_ios(g, "[%a][%a][%a][%a][%a][%A]",
             "[0x1p+0][0x0p+0][-0x0p+0][0x1p-1074][0x1.fffffffffffffp+1023][0X1.FFP+7]", true, 1.0,
             0.0, -0.0, 5e-324, 1.7976931348623157e308, 255.5);
    like_ios(g, "[%.1a][%.0a][%#.0a][%.3a][%a][%a][%010a][%+a]",
             "[0x2.0p+0][0x1p+0][0x1.p+0][0x1.555p-2][0x1.999999999999ap-4][0x1p-1022]["
             "0x00001p+0][+0x1p+1]",
             true, 1.99, 1.5, 1.0, 1.0 / 3.0, 0.1, 2.2250738585072014e-308, 1.0, 2.0);
    like_ios(g, "[%.0a][%.0a][%.1a][%.1a][%.1a][%.2a][%.0a][%a][%.20a]",
             "[0x2p+0][0x1p+0][0x1.1p+0][0x1.2p+0][0x1.fp+0][0x1.ffp+0][0x2p+0]["
             "0x1.ffffffffffffep-1023][0x1.00000000000000000000p+0]",
             true, 1.75, 1.5000001, 0x1.18p0, 0x1.28p0, 0x1.f8p0, 0x1.ff8p0, 0x1.fp0,
             2.2250738585072009e-308, 1.0);
    // inf and nan: no 0 padding, and a nan never has a sign.
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    like_ios(g, "[%f][%F][%e][%+f][%05f][%-+6f][%g][% F][%a][%+5.3e]",
             "[inf][INF][-inf][nan][  inf][-inf  ][nan][NAN][-inf][  nan]", true, inf, inf, -inf,
             nan, inf, -inf, -nan, -nan, -inf, -nan);
    // The 0 flag pads strings, chars and unknown conversions; '%' takes a width.
    like_ios(g, "[%05s][%-5s][%05c][%s]", "[000ab][ab   ][0000x][(null)]", true, "ab", "ab", 'x',
             static_cast<const char *>(nullptr));
    like_ios(g, "[%5k][%05k][%5%][%-5%][%@][%.3s]%d", "[    k][0000k][    %][%    ][@][(nu]1",
             false, static_cast<const char *>(nullptr), 1);
    // # with octal and hex at zero, precision with #o.
    like_ios(g, "[%#o][%#.0o][%#x][%#.3o][%.0x]", "[0][0][0][010][]", true, 0u, 0u, 0u, 8u, 0u);
    // FreeBSD's %D %O %U are %ld %lo %lu; long is one guest word.
    like_ios(g, "[%D][%O][%U][%ld][%lx]", "[-5][10][9][-1][ffffffff]", false, -5, 8u, 9u, -1,
             0xFFFFFFFFu);
    // The ' flag groups nothing in the C locale.
    like_ios(g, "[%'d][%'.2f]", "[1234567][1234.50]", true, 1234567, 1234.5);
}

TEST_CASE("printf: %n stores the count so far, %lc/%ls narrow in the C locale") {
    Guest g;
    const std::uint32_t n32 = g.alloc(16u);
    const std::uint32_t fmt = g.str("ab%nc%hhn%lln!");
    g.put32(n32 + 4u, 0xFFFFFFFFu);
    CHECK(g.call(mt_hle_printf, {fmt, n32, n32 + 4u, n32 + 8u}) == 4u);
    CHECK(g.out[1] == "abc!");
    CHECK(g.get32(n32) == 2u);
    CHECK(g.get32(n32 + 4u) == 0xFFFFFF03u); // one byte written
    CHECK(g.get32(n32 + 8u) == 3u);
    CHECK(g.get32(n32 + 12u) == 0u);

    const std::uint32_t wide = g.alloc(16u);
    g.put32(wide, 'h');
    g.put32(wide + 4u, 0xE9u);
    g.out[1].clear();
    CHECK(g.call(mt_hle_printf, {g.str("[%lc][%ls][%C][%.1ls]"), 0xE9u, wide, 'x', wide}) == 13u);
    CHECK(g.out[1] == "[\xE9][h\xE9][x][h]");
    // A code point the C locale cannot narrow: an encoding error, the call returns -1.
    g.out[1].clear();
    CHECK(g.call(mt_hle_printf, {g.str("%lc"), 0x263Au}) == 0xFFFFFFFFu);
}

TEST_CASE("the format engine routes extension conversions such as %@ to a hook") {
    Guest g;
    const std::uint32_t words = g.alloc(16u);
    const mt_format_hooks hooks = {
        "@",
        [](void *user, mt_cpu *, const mt_format_spec &spec, mt_args *args, std::string &out) {
            *static_cast<int *>(user) += 1;
            char desc[32];
            std::snprintf(desc, sizeof desc, "<obj %X>", mt_arg_u32(args));
            mt_format_pad_string(out, spec, desc, std::strlen(desc));
        },
        nullptr};
    int calls = 0;
    mt_format_hooks counted = hooks;
    counted.user = &calls;
    // Three objects and an int in a va_list; the output is appended after "> ".
    g.put32(words, 0x1234u);
    g.put32(words + 4u, 0x5678u);
    g.put32(words + 8u, 0x9ABCu);
    g.put32(words + 12u, 7u);
    mt_args va = mt_args_va_list(&g.cpu, words);
    std::string text = "> ";
    const int n = mt_format(&g.cpu, "[%@][%-12@][%.4@] %d", &va, &counted, text);
    CHECK(text == "> [<obj 1234>][<obj 5678>  ][<obj] 7");
    CHECK(n == static_cast<int>(text.size()) - 2);
    CHECK(calls == 3);
    // Without the hook, %@ is an unknown conversion and reads nothing.
    va = mt_args_va_list(&g.cpu, words + 12u);
    text.clear();
    CHECK(mt_format(&g.cpu, "%@ %d", &va, nullptr, text) == 3);
    CHECK(text == "@ 7");
}

TEST_CASE("puts and vfprintf write to the guest's stdout and stderr") {
    Guest g;
    CHECK(g.call(mt_hle_puts, {g.str("Init OpenAL")}) == '\n');
    CHECK(g.out[1] == "Init OpenAL\n");

    // ___stderrp: a data import whose init hook points it at a guest FILE for fd 2.
    const std::uint32_t stderrp = g.alloc(4u);
    mt_hle_init_stderrp(&g.cpu, 0u, stderrp);
    const std::uint32_t file = g.get32(stderrp);
    REQUIRE(file != 0u);
    CHECK(mt_heap_usable_size(g.mem, file) >= 88u);
    std::int16_t flags, fd;
    std::memcpy(&flags, g.host(file + 12u), 2u);
    std::memcpy(&fd, g.host(file + 14u), 2u);
    CHECK(flags == 0x000A); // __SWR | __SNBF
    CHECK(fd == 2);
    // The same object every time, in this address space.
    const std::uint32_t again = g.alloc(4u);
    mt_hle_init_stderrp(&g.cpu, 0u, again);
    CHECK(g.get32(again) == file);
    const std::uint32_t stdoutp = g.alloc(4u);
    mt_hle_init_stdoutp(&g.cpu, 0u, stdoutp);
    const std::uint32_t stdinp = g.alloc(4u);
    mt_hle_init_stdinp(&g.cpu, 0u, stdinp);
    CHECK(g.get32(stdoutp) != file);
    CHECK(g.get32(stdinp) != g.get32(stdoutp));

    const std::uint32_t ap = g.alloc(8u);
    g.put32(ap, g.str("bson"));
    g.put32(ap + 4u, 3u);
    CHECK(g.call(mt_hle_vfprintf, {file, g.str("%s error %d\n"), ap}) == 13u);
    CHECK(g.out[2] == "bson error 3\n");
    g.out[1].clear();
    CHECK(g.call(mt_hle_vfprintf, {g.get32(stdoutp), g.str("%d"), ap + 4u}) == 1u);
    CHECK(g.out[1] == "3");

    // A FILE that is neither: nothing written, -1 and a log line.
    const std::uint32_t other = g.alloc(88u);
    const std::int16_t five = 5;
    std::memcpy(g.host(other + 14u), &five, 2u);
    g.out[1].clear();
    g.out[2].clear();
    CHECK(g.call(mt_hle_vfprintf, {other, g.str("x"), ap}) == 0xFFFFFFFFu);
    CHECK(g.out[1].empty());
    CHECK(g.out[2].empty());
    REQUIRE(g.log.size() == 1u);
    CHECK(g.log[0].find("fd 5") != std::string::npos);
}

TEST_CASE("__snprintf_chk truncates like snprintf and aborts on an object smaller than maxlen") {
    Guest g;
    const std::uint32_t buf = g.alloc(16u);
    const std::uint32_t fmt = g.str("%s!");
    std::memset(g.host(buf), 'Z', 16u);
    CHECK(g.call(mt_hle_snprintf_chk, {buf, 4u, 0u, 16u, fmt, g.str("hello")}) == 6u);
    CHECK(g.cstr(buf) == "hel");
    CHECK(g.host(buf)[4] == 'Z');
    // maxlen 0 writes nothing.
    CHECK(g.call(mt_hle_snprintf_chk, {buf, 0u, 0u, 16u, fmt, g.str("x")}) == 2u);
    CHECK(g.cstr(buf) == "hel");
    // The compiler knew the object has 8 bytes, but maxlen says 16: __chk_fail.
    CHECK(g.traps(mt_hle_snprintf_chk, {buf, 16u, 0u, 8u, fmt, g.str("x")}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == buf);
}

// --- memory and strings --------------------------------------------------------------------

TEST_CASE("malloc, calloc, realloc and free use the guest heap") {
    Guest g;
    const std::uint32_t a = g.call(mt_hle_malloc, {100u});
    CHECK(a != 0u);
    CHECK(a % 16u == 0u);
    CHECK(mt_heap_usable_size(g.mem, a) >= 100u);
    std::memcpy(g.host(a), "keep me", 8u);
    const std::uint32_t b = g.call(mt_hle_realloc, {a, 5000u});
    CHECK(b != 0u);
    CHECK(g.cstr(b) == "keep me");
    const std::uint32_t c = g.call(mt_hle_calloc, {10u, 10u});
    CHECK(c != 0u);
    for (std::uint32_t i = 0; i < 100u; i++) {
        CHECK(g.host(c)[i] == 0u);
    }
    CHECK(g.call(mt_hle_malloc, {0u}) != 0u);                  // Darwin: a unique minimum block
    const std::uint32_t d = g.call(mt_hle_realloc, {0u, 32u}); // realloc(NULL) is malloc
    CHECK(mt_heap_usable_size(g.mem, d) >= 32u);
    g.call(mt_hle_free, {b});
    g.call(mt_hle_free, {0u});
    CHECK(mt_heap_usable_size(g.mem, b) == 0u);
    CHECK(g.call(mt_hle_malloc, {0xFFFFFFF0u}) == 0u); // out of heap
    CHECK(g.log.empty());
}

TEST_CASE("an invalid free or realloc aborts the guest with the caller and the pointer") {
    Guest g;
    const std::uint32_t a = g.call(mt_hle_malloc, {16u});
    g.call(mt_hle_free, {a});
    CHECK(g.traps(mt_hle_free, {a})); // double free
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == a);
    REQUIRE(g.log.size() == 1u);
    CHECK(g.log[0].find("pointer being freed was not allocated") != std::string::npos);
    CHECK(g.log[0].find("0x0002A3C5") != std::string::npos);
    CHECK(g.traps(mt_hle_realloc, {a + 4u, 32u}));
    CHECK(mt_test::g_trap.detail == a + 4u);
    CHECK(g.log.back().find("realloc'd") != std::string::npos);
}

TEST_CASE("memcpy, memset and memcmp") {
    Guest g;
    const std::uint32_t a = g.alloc(64u), b = g.alloc(64u);
    std::memcpy(g.host(a), "0123456789", 10u);
    CHECK(g.call(mt_hle_memcpy, {b, a, 10u}) == b);
    CHECK(std::memcmp(g.host(b), "0123456789", 10u) == 0);
    CHECK(g.call(mt_hle_memcpy, {a + 2u, a, 8u}) == a + 2u); // overlapping, like memmove
    CHECK(std::memcmp(g.host(a), "0101234567", 10u) == 0);
    CHECK(g.call(mt_hle_memcpy, {0u, 0u, 0u}) == 0u); // n = 0 touches nothing
    CHECK(g.call(mt_hle_memset, {b, 0x1FFu, 4u}) == b);
    CHECK(std::memcmp(g.host(b),
                      "\xFF\xFF\xFF\xFF"
                      "456789",
                      10u) == 0);
    CHECK(g.call(mt_hle_memcmp, {a, a, 10u}) == 0u);
    std::memcpy(g.host(b), "ab\x80", 3u);
    std::memcpy(g.host(a), "ab\x01", 3u);
    CHECK(static_cast<std::int32_t>(g.call(mt_hle_memcmp, {a, b, 3u})) == 1 - 0x80);
    CHECK(static_cast<std::int32_t>(g.call(mt_hle_memcmp, {b, a, 3u})) == 0x80 - 1);
    CHECK(g.call(mt_hle_memcmp, {0u, 0u, 0u}) == 0u);
    // A bad pointer stops the guest with the caller as the PC.
    CHECK(g.traps(mt_hle_memset, {8u, 0u, 4u}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_MEMORY);
    CHECK(mt_test::g_trap.pc == k_caller);
}

TEST_CASE("strlen, strcpy, strcat and strcmp") {
    Guest g;
    const std::uint32_t s = g.str("MapleThief");
    CHECK(g.call(mt_hle_strlen, {s}) == 10u);
    CHECK(g.call(mt_hle_strlen, {g.str("")}) == 0u);
    const std::uint32_t buf = g.alloc(32u);
    CHECK(g.call(mt_hle_strcpy, {buf, s}) == buf);
    CHECK(g.cstr(buf) == "MapleThief");
    CHECK(g.call(mt_hle_strcat, {buf, g.str(" 1.5")}) == buf);
    CHECK(g.cstr(buf) == "MapleThief 1.5");
    CHECK(g.call(mt_hle_strcmp, {buf, buf}) == 0u);
    CHECK(static_cast<std::int32_t>(g.call(mt_hle_strcmp, {g.str("abc"), g.str("abd")})) == -1);
    CHECK(static_cast<std::int32_t>(g.call(mt_hle_strcmp, {g.str("ab"), g.str("a")})) == 'b');
    CHECK(static_cast<std::int32_t>(g.call(mt_hle_strcmp, {g.str("\xC0"), g.str("A")})) ==
          0xC0 - 'A'); // unsigned bytes
    CHECK(g.traps(mt_hle_strlen, {0u}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_MEMORY);
    CHECK(mt_test::g_trap.pc == k_caller);
}

TEST_CASE("atoi and atol: 32-bit, saturating like strtol") {
    Guest g;
    const auto atoi = [&](const char *s) {
        return static_cast<std::int32_t>(g.call(mt_hle_atoi, {g.str(s)}));
    };
    const auto atol = [&](const char *s) {
        return static_cast<std::int32_t>(g.call(mt_hle_atol, {g.str(s)}));
    };
    CHECK(atoi("42") == 42);
    CHECK(atoi("  \t\n-17xyz") == -17);
    CHECK(atoi("+8") == 8);
    CHECK(atoi("") == 0);
    CHECK(atoi("abc") == 0);
    CHECK(atoi("0x10") == 0);
    CHECK(atoi("2147483647") == 2147483647);
    CHECK(atoi("2147483648") == std::numeric_limits<std::int32_t>::max());
    CHECK(atoi("-2147483648") == std::numeric_limits<std::int32_t>::min());
    CHECK(atoi("-99999999999999999999") == std::numeric_limits<std::int32_t>::min());
    CHECK(atol("123456") == 123456);
    CHECK(atol("99999999999") == std::numeric_limits<std::int32_t>::max());
}

// --- math and time -------------------------------------------------------------------------

TEST_CASE("floor, trunc and pow take and return doubles in core registers") {
    Guest g;
    CHECK(g.call_f64(mt_hle_floor, {lo(-2.5), hi(-2.5)}) == -3.0);
    CHECK(g.call_f64(mt_hle_floor, {lo(7.99), hi(7.99)}) == 7.0);
    CHECK(std::signbit(g.call_f64(mt_hle_floor, {lo(-0.0), hi(-0.0)})));
    CHECK(g.call_f64(mt_hle_trunc, {lo(-2.5), hi(-2.5)}) == -2.0);
    CHECK(g.call_f64(mt_hle_trunc, {lo(2.9), hi(2.9)}) == 2.0);
    CHECK(g.call_f64(mt_hle_pow, {lo(2.0), hi(2.0), lo(10.0), hi(10.0)}) == 1024.0);
    CHECK(g.call_f64(mt_hle_pow, {lo(9.0), hi(9.0), lo(0.5), hi(0.5)}) == 3.0);
    CHECK(g.call_f64(mt_hle_pow, {lo(0.0), hi(0.0), lo(0.0), hi(0.0)}) == 1.0);
    CHECK(std::isinf(g.call_f64(mt_hle_pow, {lo(10.0), hi(10.0), lo(400.0), hi(400.0)})));
}

TEST_CASE("gettimeofday fills a 32-bit timeval from the configured clock") {
    Guest g;
    g.now_us = 1288000000123456ull;
    const std::uint32_t tv = g.alloc(16u);
    const std::uint32_t tz = g.alloc(8u);
    std::memset(g.host(tz), 0xEE, 8u);
    CHECK(g.call(mt_hle_gettimeofday, {tv, tz}) == 0u);
    CHECK(g.get32(tv) == 1288000000u);
    CHECK(g.get32(tv + 4u) == 123456u);
    CHECK(g.get32(tv + 8u) == 0u); // 8 bytes only
    CHECK(g.get32(tz) == 0u);
    CHECK(g.get32(tz + 4u) == 0u);
    CHECK(g.call(mt_hle_gettimeofday, {0u, 0u}) == 0u);

    // The host clock without a hook: some time after this test was written.
    mt_libc_configure(nullptr);
    CHECK(g.call(mt_hle_gettimeofday, {tv, 0u}) == 0u);
    CHECK(g.get32(tv) > 1700000000u); // after 2023-11
}

// --- sysctl and the network -----------------------------------------------------------------

TEST_CASE("sysctl and sysctlbyname report the configured device") {
    Guest g;
    const std::uint32_t mib = g.alloc(24u), len = g.alloc(4u), buf = g.alloc(64u);
    g.put32(mib, 6u);      // CTL_HW
    g.put32(mib + 4u, 1u); // HW_MACHINE
    // Size query, then the value with its NUL.
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, 0u, len, 0u, 0u}) == 0u);
    CHECK(g.get32(len) == 10u);
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, buf, len, 0u, 0u}) == 0u);
    CHECK(g.cstr(buf) == "iPhone3,1");
    // A short buffer: what fits, ENOMEM, and *oldlenp keeps the buffer's size.
    std::memset(g.host(buf), 0, 64u);
    g.put32(len, 6u);
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, buf, len, 0u, 0u}) == 0xFFFFFFFFu);
    CHECK(g.cstr(buf) == "iPhone");
    CHECK(g.get32(len) == 6u);
    // hw.model and hw.ncpu.
    g.put32(mib + 4u, 2u);
    g.put32(len, 64u);
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, buf, len, 0u, 0u}) == 0u);
    CHECK(g.cstr(buf) == "N90AP");
    g.put32(mib + 4u, 3u);
    g.put32(len, 64u);
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, buf, len, 0u, 0u}) == 0u);
    CHECK(g.get32(len) == 4u);
    CHECK(g.get32(buf) == 1u);
    // Setting a value, or a node we do not have: -1 and a log line.
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, 0u, len, buf, 4u}) == 0xFFFFFFFFu);
    g.put32(mib, 1u); // CTL_KERN
    CHECK(g.call(mt_hle_sysctl, {mib, 2u, buf, len, 0u, 0u}) == 0xFFFFFFFFu);
    CHECK(g.log.size() == 2u);

    // sysctlbyname, as Chartboost asks for hw.machine; and a configured iPod touch 4.
    mt_libc_config cfg = g.base;
    std::snprintf(cfg.hw_machine, sizeof cfg.hw_machine, "iPod4,1");
    mt_libc_configure(&cfg);
    const std::uint32_t name = g.str("hw.machine");
    CHECK(g.call(mt_hle_sysctlbyname, {name, 0u, len, 0u, 0u}) == 0u);
    CHECK(g.get32(len) == 8u);
    CHECK(g.call(mt_hle_sysctlbyname, {name, buf, len, 0u, 0u}) == 0u);
    CHECK(g.cstr(buf) == "iPod4,1");
    g.put32(len, 64u);
    CHECK(g.call(mt_hle_sysctlbyname, {g.str("hw.model"), buf, len, 0u, 0u}) == 0u);
    CHECK(g.cstr(buf) == "N81AP");
    g.put32(len, 64u);
    CHECK(g.call(mt_hle_sysctlbyname, {g.str("hw.memsize"), buf, len, 0u, 0u}) == 0u);
    CHECK(g.get32(len) == 8u);
    CHECK(g.get32(buf) == (256u << 20));
    CHECK(g.call(mt_hle_sysctlbyname, {g.str("kern.osversion"), buf, len, 0u, 0u}) == 0xFFFFFFFFu);
    CHECK(std::string(mt_libc_hw_model("iPhone3,1")) == "N90AP");
    CHECK(std::string(mt_libc_hw_model("Pixel")) == "Unknown");
}

TEST_CASE("sysctl(NET_RT_IFLIST) gives en0's MAC where -[UIDevice macaddress] reads it") {
    Guest g;
    mt_libc_config cfg = g.base;
    const std::uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(cfg.en0_mac, mac, 6u);
    mt_libc_configure(&cfg);
    // The game's sequence at 0xC1025: if_nametoindex("en0"), then mib {CTL_NET, AF_ROUTE, 0,
    // AF_LINK, NET_RT_IFLIST, index}, a size query, malloc, the read.
    const std::uint32_t index = g.call(mt_hle_if_nametoindex, {g.str("en0")});
    CHECK(index == 2u);
    CHECK(g.call(mt_hle_if_nametoindex, {g.str("lo0")}) == 1u);
    CHECK(g.call(mt_hle_if_nametoindex, {g.str("pdp_ip0")}) == 0u);
    const std::uint32_t mib = g.alloc(24u), len = g.alloc(4u);
    const std::uint32_t words[6] = {4u, 17u, 0u, 18u, 3u, index};
    std::memcpy(g.host(mib), words, sizeof words);
    CHECK(g.call(mt_hle_sysctl, {mib, 6u, 0u, len, 0u, 0u}) == 0u);
    const std::uint32_t size = g.get32(len);
    CHECK(size == 132u);
    const std::uint32_t buf = g.call(mt_hle_malloc, {size});
    CHECK(g.call(mt_hle_sysctl, {mib, 6u, buf, len, 0u, 0u}) == 0u);
    // if_msghdr: msglen, version 5, RTM_IFINFO, index.
    CHECK((g.get32(buf) & 0xFFFFu) == 132u);
    CHECK(g.host(buf)[2] == 5u);
    CHECK(g.host(buf)[3] == 0x0Eu);
    CHECK((g.get32(buf + 12u) & 0xFFFFu) == 2u);
    // sockaddr_dl at +0x70: len, AF_LINK, index, IFT_ETHER, nlen 3, alen 6, "en0".
    const std::uint8_t *sdl = g.host(buf + 0x70u);
    CHECK(sdl[0] == 20u);
    CHECK(sdl[1] == 18u);
    CHECK(sdl[5] == 3u);
    CHECK(sdl[6] == 6u);
    CHECK(std::memcmp(sdl + 8, "en0", 3u) == 0);
    // What the game computes: ptr = buf + 0x70 + 8 + sdl_nlen (0xC1162-0xC116E).
    const std::uint8_t *lladdr = g.host(buf + 0x70u + 8u + sdl[5]);
    CHECK(std::memcmp(lladdr, mac, 6u) == 0);
    // Default MAC: 02:00:00:00:00:00.
    mt_libc_configure(&g.base);
    CHECK(g.call(mt_hle_sysctl, {mib, 6u, buf, len, 0u, 0u}) == 0u);
    const std::uint8_t zero_mac[6] = {0x02, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(g.host(buf + 0x70u + 11u), zero_mac, 6u) == 0);
    // Another interface index: nothing to list.
    g.put32(mib + 20u, 1u);
    CHECK(g.call(mt_hle_sysctl, {mib, 6u, 0u, len, 0u, 0u}) == 0u);
    CHECK(g.get32(len) == 0u);
}

TEST_CASE("hw.machine comes from MAPLETHIEF_HW_MACHINE") {
    Guest g;
    mt_libc_config cfg = mt_libc_config_default();
    CHECK(std::string(cfg.hw_machine) == MT_LIBC_DEFAULT_HW_MACHINE);
#if defined(_WIN32)
    _putenv_s(MT_LIBC_HW_MACHINE_ENV, "iPod4,1");
#else
    setenv(MT_LIBC_HW_MACHINE_ENV, "iPod4,1", 1);
#endif
    CHECK(mt_libc_config_from_env(&cfg) == 0);
    CHECK(std::string(cfg.hw_machine) == "iPod4,1");
#if defined(_WIN32)
    _putenv_s(MT_LIBC_HW_MACHINE_ENV, "a-machine-name-that-is-far-too-long-for-it");
#else
    setenv(MT_LIBC_HW_MACHINE_ENV, "a-machine-name-that-is-far-too-long-for-it", 1);
#endif
    CHECK(mt_libc_config_from_env(&cfg) == -1);
    CHECK(std::string(cfg.hw_machine) == "iPod4,1");
    CHECK(g.log.size() == 1u);
#if defined(_WIN32)
    _putenv_s(MT_LIBC_HW_MACHINE_ENV, "");
#else
    unsetenv(MT_LIBC_HW_MACHINE_ENV);
#endif
    cfg = mt_libc_config_default();
    CHECK(mt_libc_config_from_env(&cfg) == 0);
    CHECK(std::string(cfg.hw_machine) == MT_LIBC_DEFAULT_HW_MACHINE);
}

TEST_CASE("the socket calls are offline stubs; inet_addr and inet_ntoa are real") {
    Guest g;
    const auto inet_addr = [&](const char *s) { return g.call(mt_hle_inet_addr, {g.str(s)}); };
    CHECK(inet_addr("50.17.236.149") == 0x95EC1132u); // CMSocket's server, network order
    CHECK(inet_addr("127.0.0.1") == 0x0100007Fu);
    CHECK(inet_addr("0x7f.1") == 0x0100007Fu);
    CHECK(inet_addr("010.0.0.1") == 0x01000008u); // octal
    CHECK(inet_addr("1.2.3.4 ") == 0x04030201u);
    CHECK(inet_addr("256.1.1.1") == 0xFFFFFFFFu);
    CHECK(inet_addr("1.2.3.4.5") == 0xFFFFFFFFu);
    CHECK(inet_addr("example.com") == 0xFFFFFFFFu);
    CHECK(inet_addr("") == 0xFFFFFFFFu);

    const std::uint32_t text = g.call(mt_hle_inet_ntoa, {0x95EC1132u});
    CHECK(g.cstr(text) == "50.17.236.149");
    CHECK(g.call(mt_hle_inet_ntoa, {0x0100007Fu}) == text); // one static buffer
    CHECK(g.cstr(text) == "127.0.0.1");

    CHECK(g.call(mt_hle_gethostbyname, {g.str("w.nexonmobile.com")}) == 0u);
    CHECK(g.log.size() == 1u);
    CHECK(g.call(mt_hle_setsockopt, {3u, 0xFFFFu, 0x1022u, g.alloc(4u), 4u}) == 0u);
    CHECK(g.call(mt_hle_close, {3u}) == 0u);
}

// --- the end of the process ----------------------------------------------------------------

TEST_CASE("abort, __assert_rtn and exit stop the guest through mt_trap") {
    Guest g;
    CHECK(g.traps(mt_hle_abort, {}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == 0u);

    CHECK(g.traps(mt_hle_assert_rtn, {g.str("-[GameCenterManager callDelegate:withArg:error:]"),
                                      g.str("GameCenterManager.m"), 220u, g.str("0")}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(g.out[2] == "Assertion failed: (0), function -[GameCenterManager "
                      "callDelegate:withArg:error:], file GameCenterManager.m, line 220.\n");
    g.out[2].clear();
    CHECK(g.traps(mt_hle_assert_rtn, {0u, g.str("a.c"), 7u, g.str("x > 0")}));
    CHECK(g.out[2] == "Assertion failed: (x > 0), file a.c, line 7.\n");

    CHECK(g.traps(mt_hle_exit, {1u}));
    CHECK(mt_test::g_trap.kind == MT_TRAP_EXIT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == 1u);
}

TEST_CASE("the registry lists every libc import of the game") {
    std::uint32_t n = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n);
    const auto find = [&](const char *name) -> const mt_hle_entry * {
        for (std::uint32_t i = 0; i < n; i++) {
            if (std::strcmp(hle[i].name, name) == 0) {
                return &hle[i];
            }
        }
        return nullptr;
    };
    // tests/test_hle_libc_real.py checks this list against the armv7 slice's imports.
    static const char *const k_functions[] = {
        "___assert_rtn",  "___snprintf_chk", "_abort",          "_atoi",       "_atol",
        "_calloc",        "_close",          "_exit",           "_floor",      "_free",
        "_gethostbyname", "_gettimeofday",   "_if_nametoindex", "_inet_addr",  "_inet_ntoa",
        "_malloc",        "_memcmp",         "_memcpy",         "_memset",     "_pow",
        "_printf",        "_puts",           "_realloc",        "_setsockopt", "_strcat",
        "_strcmp",        "_strcpy",         "_strlen",         "_sysctl",     "_sysctlbyname",
        "_trunc",         "_vfprintf",       "_vsprintf",       "___divdi3",   "___divsi3",
        "___floatdisf",   "___modsi3",       "___udivdi3",      "___udivsi3",  "___umodsi3",
    };
    for (const char *name : k_functions) {
        const mt_hle_entry *e = find(name);
        CHECK_MESSAGE(e != nullptr, name);
        CHECK_MESSAGE((e != nullptr && e->fn != nullptr), name);
    }
    const mt_hle_entry *stderrp = find("___stderrp");
    REQUIRE(stderrp != nullptr);
    CHECK(stderrp->fn == nullptr);
    CHECK(stderrp->init == mt_hle_init_stderrp);
}
