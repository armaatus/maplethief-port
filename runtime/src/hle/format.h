// The guest printf engine (ADR 0028): formats a guest format string with guest arguments,
// with the output of iOS's libc (FreeBSD's vfprintf), whatever the host's printf does.
//
// The arguments come from an mt_args cursor (maplethief/hle/abi.h), so the same engine serves
// variadic calls and va_lists:
//
//     // int printf(const char *fmt, ...)
//     mt_args a = mt_args_begin(cpu);
//     const char *fmt = mt_g2h_str(cpu, mt_arg_u32(&a));
//     std::string text;
//     int n = mt_format(cpu, fmt, &a, nullptr, text);    // the variadic words follow fmt
//
//     // int vsprintf(char *buf, const char *fmt, va_list ap)
//     mt_args va = mt_args_va_list(cpu, ap);             // ap: guest pointer to the words
//     mt_format(cpu, fmt, &va, nullptr, text);
//
// Sizes are the guest's (ILP32): int, long, size_t, ptrdiff_t and pointers are one word;
// long long, intmax_t and double (also long double) are two, 4-byte aligned (ADR 0019). A
// float argument arrives promoted to double.
//
// Conversions: d i o u x X c s p n % e E f F g G a A, and FreeBSD's D O U (= ld lo lu), C S (=
// lc ls); flags - + space # 0 ' (ignored: the C locale has no grouping); width and precision
// as digits or *; lengths hh h l ll q j z t L. %s of NULL prints "(null)", %p prints 0x and
// lower-case hex ("0x0" for NULL), inf and nan print as "inf"/"nan" ("INF"/"NAN" for the
// upper-case conversions) and a nan never gets a sign, %n stores into guest memory, the 0
// flag pads %s and %c with zeros too, an unknown conversion character prints itself (padded).
// Not supported: positional arguments (%1$d), which nothing in the game uses; '$' then counts
// as an unknown conversion. %lc and %ls write UTF-8.
//
// Extension conversions: a caller can route conversion characters to a hook, which reads its
// own arguments from the cursor and appends its text. Foundation (#49) uses it for %@ (an
// object's description) in NSLog and +[NSString stringWithFormat:], and may take %C/%S over
// for unichar:
//
//     void object_hook(void *user, mt_cpu *cpu, const mt_format_spec &spec, mt_args *args,
//                      std::string &out) {
//         std::uint32_t obj = mt_arg_u32(args);
//         std::string desc = describe(cpu, obj);            // UTF-8
//         mt_format_pad_string(out, spec, desc.data(), desc.size());
//     }
//     const mt_format_hooks hooks = {"@", object_hook, nullptr};
//     mt_format(cpu, fmt, &a, &hooks, text);
//
// Without a hook, %@ is an unknown conversion and prints "@", as iOS's printf does.
//
// Guest pointers (%s, %n, %ls) are checked: a bad one stops the guest through mt_mem_fault
// with the guest caller (lr) as the PC.
#ifndef MAPLETHIEF_RUNTIME_HLE_FORMAT_H
#define MAPLETHIEF_RUNTIME_HLE_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "maplethief/hle/abi.h"

// One parsed conversion specification.
struct mt_format_spec {
    char conv = 0;               // the conversion character, e.g. 'd' or '@'
    char length[3] = {};         // "", "hh", "h", "l", "ll", "q", "j", "z", "t" or "L"
    bool minus = false;          // '-': left-justify
    bool plus = false;           // '+'
    bool space = false;          // ' '
    bool alt = false;            // '#'
    bool zero = false;           // '0'
    std::int32_t width = 0;      // 0: none
    std::int32_t precision = -1; // -1: none
};

// Formats one extension conversion: reads its arguments from `args`, appends to `out`.
using mt_format_ext_fn = void (*)(void *user, mt_cpu *cpu, const mt_format_spec &spec,
                                  mt_args *args, std::string &out);

struct mt_format_hooks {
    const char *conversions; // the conversion characters routed to fn, e.g. "@"
    mt_format_ext_fn fn;
    void *user;
};

// Appends the formatted text to `out` and returns the number of bytes appended (what
// printf returns), or -1 if it would exceed INT_MAX (iOS: EOVERFLOW). `hooks` may be NULL.
int mt_format(mt_cpu *cpu, const char *fmt, mt_args *args, const mt_format_hooks *hooks,
              std::string &out);

// Appends `len` bytes of `text` with the spec's precision (a byte limit), width, '-' and '0'
// applied as for %s. For extension hooks.
void mt_format_pad_string(std::string &out, const mt_format_spec &spec, const char *text,
                          std::size_t len);

#endif // MAPLETHIEF_RUNTIME_HLE_FORMAT_H
