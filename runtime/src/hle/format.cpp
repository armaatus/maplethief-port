// The guest printf engine; see format.h. The structure follows FreeBSD's vfprintf.c, which
// is what iOS's libc ships: every conversion produces a sign, a prefix (0x) and a body, and
// one routine pads them to the field width.
#include "hle/format.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "maplethief/memory.h"

namespace {

// How many bytes of the guest string at `addr` %s prints: up to the NUL, or at most `limit`
// bytes if limit >= 0 (then no NUL is needed). Checked like mt_g2h_str.
std::size_t guest_strnlen(mt_cpu *cpu, std::uint32_t addr, std::int32_t limit) {
    const std::uint8_t *s = mt_g2h(cpu, addr, 1u);
    const std::uint64_t room = cpu->mem_size - addr;
    const std::uint64_t scan = limit >= 0 && static_cast<std::uint64_t>(limit) < room
                                   ? static_cast<std::uint64_t>(limit)
                                   : room;
    const void *nul = std::memchr(s, 0, static_cast<std::size_t>(scan));
    if (nul != nullptr) {
        return static_cast<std::size_t>(static_cast<const std::uint8_t *>(nul) - s);
    }
    if (limit < 0 || static_cast<std::uint64_t>(limit) > room) {
        mt_mem_fault(cpu, addr, static_cast<std::uint32_t>(room), cpu->r[MT_LR]);
    }
    return static_cast<std::size_t>(scan);
}

bool is_length(const mt_format_spec &s, const char *l) { return std::strcmp(s.length, l) == 0; }

// 64-bit argument: ll, q, j. Everything else integer is one word (ILP32).
bool wide(const mt_format_spec &s) {
    return is_length(s, "ll") || is_length(s, "q") || is_length(s, "j");
}

std::int64_t signed_arg(mt_args *a, const mt_format_spec &s) {
    if (wide(s)) {
        return mt_arg_i64(a);
    }
    const std::uint32_t w = mt_arg_u32(a);
    if (is_length(s, "hh")) {
        return static_cast<std::int8_t>(w);
    }
    if (is_length(s, "h")) {
        return static_cast<std::int16_t>(w);
    }
    return static_cast<std::int32_t>(w);
}

std::uint64_t unsigned_arg(mt_args *a, const mt_format_spec &s) {
    if (wide(s)) {
        return mt_arg_u64(a);
    }
    const std::uint32_t w = mt_arg_u32(a);
    if (is_length(s, "hh")) {
        return static_cast<std::uint8_t>(w);
    }
    if (is_length(s, "h")) {
        return static_cast<std::uint16_t>(w);
    }
    return w;
}

void pad(std::string &out, std::int64_t n, char c) {
    if (n > 0) {
        out.append(static_cast<std::size_t>(n), c);
    }
}

// FreeBSD's output step: [spaces][sign][prefix][zeros][precision zeros][body][spaces].
// `dprec` is the integer precision (body is zero-extended to it), or -1.
void emit(std::string &out, const mt_format_spec &s, char sign, const char *prefix,
          const char *body, std::size_t size, std::int64_t dprec, bool zero_pad) {
    const std::size_t prefix_len = std::strlen(prefix);
    std::int64_t realsz =
        dprec > static_cast<std::int64_t>(size) ? dprec : static_cast<std::int64_t>(size);
    realsz += (sign != 0 ? 1 : 0) + static_cast<std::int64_t>(prefix_len);
    const std::int64_t fill = s.width - realsz;
    if (!s.minus && !zero_pad) {
        pad(out, fill, ' ');
    }
    if (sign != 0) {
        out.push_back(sign);
    }
    out.append(prefix, prefix_len);
    if (!s.minus && zero_pad) {
        pad(out, fill, '0');
    }
    pad(out, dprec - static_cast<std::int64_t>(size), '0');
    out.append(body, size);
    if (s.minus) {
        pad(out, fill, ' ');
    }
}

// d i u o x X p, and D O U.
void format_integer(std::string &out, const mt_format_spec &s, std::uint64_t magnitude, char sign,
                    unsigned base, bool upper, const char *prefix) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[72];
    char *end = buf + sizeof buf;
    char *cp = end;
    // "%.0d" of 0 prints no digits (but "%#.0o" prints "0").
    if (magnitude != 0u || s.precision != 0) {
        do {
            *--cp = digits[magnitude % base];
            magnitude /= base;
        } while (magnitude != 0u);
    }
    if (base == 8u && s.alt && (cp == end || *cp != '0')) {
        *--cp = '0';
    }
    // A precision turns the 0 flag off for integers.
    emit(out, s, sign, prefix, cp, static_cast<std::size_t>(end - cp), s.precision,
         s.zero && s.precision < 0);
}

// %a / %A as FreeBSD's __hdtoa does it: a normalized leading digit (1, or 2 after rounding
// up; 0 only for zero), subnormals normalized too, trailing zeros dropped without a
// precision, and its rounding: up if the first dropped digit is above 8, or is 8 and the
// digit after it is odd.
void format_hex_float(std::string &out, const mt_format_spec &s, double v, char sign) {
    const bool upper = s.conv == 'A';
    const char *xdigits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    std::uint64_t mant = bits & 0xFFFFFFFFFFFFFull;
    int exp = static_cast<int>((bits >> 52) & 0x7FFu);
    // Digits: [0] the leading digit, [1..13] the 52 fraction bits, then zeros.
    unsigned char d[64] = {};
    int e2 = 0;
    if (exp == 0 && mant == 0u) {
        d[0] = 0u;
    } else {
        if (exp == 0) { // subnormal: normalize
            exp = 1;
            while ((mant & (1ull << 52)) == 0u) {
                mant <<= 1;
                exp--;
            }
            mant &= 0xFFFFFFFFFFFFFull;
        }
        e2 = exp - 1023;
        d[0] = 1u;
        for (int i = 0; i < 13; i++) {
            d[1 + i] = static_cast<unsigned char>((mant >> (48 - 4 * i)) & 0xFu);
        }
    }
    int ndigits; // digits after the point
    if (s.precision >= 0) {
        ndigits = s.precision > 60 ? 60 : s.precision;
        if (ndigits < 13 &&
            (d[ndigits + 1] > 8u || (d[ndigits + 1] == 8u && (d[ndigits + 2] & 1u) != 0u))) {
            int i = ndigits;
            while (i > 0 && d[i] == 0xFu) {
                d[i--] = 0u;
            }
            d[i]++; // the leading digit may become 2
        }
    } else {
        ndigits = 13;
        while (ndigits > 0 && d[ndigits] == 0u) {
            ndigits--;
        }
    }
    std::string body;
    body.push_back(xdigits[d[0]]);
    if (ndigits > 0 || s.alt) {
        body.push_back('.');
    }
    for (int i = 1; i <= ndigits; i++) {
        body.push_back(xdigits[i <= 13 ? d[i] : 0]);
    }
    char expbuf[16];
    std::snprintf(expbuf, sizeof expbuf, "%c%c%d", upper ? 'P' : 'p', e2 < 0 ? '-' : '+',
                  e2 < 0 ? -e2 : e2);
    body += expbuf;
    emit(out, s, sign, upper ? "0X" : "0x", body.data(), body.size(), -1, s.zero);
}

// e E f F g G a A.
void format_float(std::string &out, const mt_format_spec &s, double v) {
    char sign = s.plus ? '+' : s.space ? ' ' : '\0';
    if (std::signbit(v)) {
        sign = '-';
    }
    const bool upper = s.conv >= 'A' && s.conv <= 'Z';
    if (std::isnan(v)) {
        emit(out, s, 0, "", upper ? "NAN" : "nan", 3u, -1, false);
        return;
    }
    if (std::isinf(v)) {
        emit(out, s, sign, "", upper ? "INF" : "inf", 3u, -1, false);
        return;
    }
    if (s.conv == 'a' || s.conv == 'A') {
        format_hex_float(out, s, v, sign);
        return;
    }
    // Finite decimal conversions: the host's snprintf with the same spec and a double. The
    // C standard pins these down (and every host we build on rounds correctly), unlike the
    // host's inf/nan, %a and integer behaviour handled above.
    char spec[48];
    char *p = spec;
    *p++ = '%';
    if (s.minus) {
        *p++ = '-';
    }
    if (s.plus) {
        *p++ = '+';
    }
    if (s.space) {
        *p++ = ' ';
    }
    if (s.alt) {
        *p++ = '#';
    }
    if (s.zero) {
        *p++ = '0';
    }
    *p++ = '*';
    *p++ = '.';
    *p++ = '*';
    *p++ = s.conv;
    *p = 0;
    const int prec = s.precision >= 0 ? s.precision : 6;
    char small[512];
    const int n = std::snprintf(small, sizeof small, spec, static_cast<int>(s.width), prec, v);
    if (n < 0) {
        return;
    }
    if (static_cast<std::size_t>(n) < sizeof small) {
        out.append(small, static_cast<std::size_t>(n));
        return;
    }
    std::string big(static_cast<std::size_t>(n) + 1u, '\0');
    std::snprintf(big.data(), big.size(), spec, static_cast<int>(s.width), prec, v);
    out.append(big.data(), static_cast<std::size_t>(n));
}

// %lc / %ls in the C locale, as iOS converts them: code points below 0x100 become that byte,
// anything else is an encoding error (the call returns -1).
bool narrow(std::uint32_t wc, char *byte) {
    if (wc >= 0x100u) {
        return false;
    }
    *byte = static_cast<char>(wc);
    return true;
}

bool is_hooked(const mt_format_hooks *hooks, char c) {
    return hooks != nullptr && hooks->fn != nullptr && hooks->conversions != nullptr && c != 0 &&
           std::strchr(hooks->conversions, c) != nullptr;
}

} // namespace

void mt_format_pad_string(std::string &out, const mt_format_spec &spec, const char *text,
                          std::size_t len) {
    if (spec.precision >= 0 && static_cast<std::size_t>(spec.precision) < len) {
        len = static_cast<std::size_t>(spec.precision);
    }
    emit(out, spec, 0, "", text, len, -1, spec.zero);
}

int mt_format(mt_cpu *cpu, const char *fmt, mt_args *args, const mt_format_hooks *hooks,
              std::string &out) {
    const std::size_t start = out.size();
    const char *f = fmt;
    for (;;) {
        const char *pct = std::strchr(f, '%');
        if (pct == nullptr) {
            out.append(f);
            break;
        }
        out.append(f, static_cast<std::size_t>(pct - f));
        f = pct + 1;

        // Flags, width, precision and length may come in any order, as in FreeBSD's loop.
        mt_format_spec s;
        std::size_t len_n = 0;
        for (;; f++) {
            const char c = *f;
            if (c == '-') {
                s.minus = true;
            } else if (c == '+') {
                s.plus = true;
            } else if (c == ' ') {
                s.space = true;
            } else if (c == '#') {
                s.alt = true;
            } else if (c == '0') {
                s.zero = true;
            } else if (c == '\'') {
                // thousands grouping: none in the C locale
            } else if (c == '*') {
                const std::int32_t w = mt_arg_i32(args);
                if (w < 0) {
                    s.minus = true;
                    s.width = w == INT32_MIN ? INT32_MAX : -w;
                } else {
                    s.width = w;
                }
            } else if (c >= '1' && c <= '9') {
                std::int64_t w = 0;
                while (*f >= '0' && *f <= '9') {
                    w = w * 10 + (*f++ - '0');
                    w = w > INT32_MAX ? INT32_MAX : w;
                }
                f--;
                s.width = static_cast<std::int32_t>(w);
            } else if (c == '.') {
                if (f[1] == '*') {
                    f++;
                    const std::int32_t p = mt_arg_i32(args);
                    s.precision = p < 0 ? -1 : p;
                } else {
                    std::int64_t p = 0;
                    while (f[1] >= '0' && f[1] <= '9') {
                        p = p * 10 + (*++f - '0');
                        p = p > INT32_MAX ? INT32_MAX : p;
                    }
                    s.precision = static_cast<std::int32_t>(p);
                }
            } else if (c == 'h' || c == 'l') {
                if (len_n == 1u && s.length[0] == c) {
                    s.length[1] = c; // hh, ll
                    len_n = 2u;
                } else {
                    s.length[0] = c;
                    s.length[1] = 0;
                    len_n = 1u;
                }
            } else if (c == 'q' || c == 'j' || c == 'z' || c == 't' || c == 'L') {
                s.length[0] = c;
                s.length[1] = 0;
                len_n = 1u;
            } else {
                break;
            }
        }
        s.conv = *f;
        if (s.conv == 0) {
            break; // a lone '%' at the end prints nothing
        }
        f++;

        if (is_hooked(hooks, s.conv)) {
            hooks->fn(hooks->user, cpu, s, args, out);
            continue;
        }
        char conv = s.conv;
        if (conv == 'D' || conv == 'O' || conv == 'U' || // %ld %lo %lu
            conv == 'C' || conv == 'S') {                // %lc %ls
            conv = static_cast<char>(conv - 'A' + 'a');
            s.length[0] = 'l';
            s.length[1] = 0;
        }
        const char sign_flag = s.plus ? '+' : s.space ? ' ' : '\0';
        switch (conv) {
        case 'd':
        case 'i': {
            const std::int64_t v = signed_arg(args, s);
            const std::uint64_t mag =
                v < 0 ? 0u - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
            format_integer(out, s, mag, v < 0 ? '-' : sign_flag, 10u, false, "");
            break;
        }
        case 'u':
            format_integer(out, s, unsigned_arg(args, s), 0, 10u, false, "");
            break;
        case 'o':
            format_integer(out, s, unsigned_arg(args, s), 0, 8u, false, "");
            break;
        case 'x':
        case 'X': {
            const std::uint64_t v = unsigned_arg(args, s);
            const char *prefix = s.alt && v != 0u ? (conv == 'x' ? "0x" : "0X") : "";
            format_integer(out, s, v, 0, 16u, conv == 'X', prefix);
            break;
        }
        case 'p':
            format_integer(out, s, mt_arg_u32(args), 0, 16u, false, "0x");
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            s.conv = conv;
            format_float(out, s, mt_arg_f64(args)); // L: long double is double on armv7
            break;
        case 'c': {
            const std::uint32_t w = mt_arg_u32(args);
            char c = static_cast<char>(w);
            if (is_length(s, "l") && !narrow(w, &c)) {
                return -1;
            }
            emit(out, s, 0, "", &c, 1u, -1, s.zero);
            break;
        }
        case 's': {
            const std::uint32_t p = mt_arg_u32(args);
            if (is_length(s, "l")) { // a 32-bit wchar_t string
                std::string narrowed;
                if (p == 0u) {
                    narrowed = "(null)";
                } else {
                    for (std::uint32_t at = p;; at += 4u) {
                        if (s.precision >= 0 &&
                            narrowed.size() >= static_cast<std::size_t>(s.precision)) {
                            break;
                        }
                        std::uint32_t wc;
                        std::memcpy(&wc, mt_g2h(cpu, at, 4u), 4u);
                        char c;
                        if (wc == 0u) {
                            break;
                        }
                        if (!narrow(wc, &c)) {
                            return -1;
                        }
                        narrowed.push_back(c);
                    }
                }
                mt_format_pad_string(out, s, narrowed.data(), narrowed.size());
            } else if (p == 0u) {
                mt_format_pad_string(out, s, "(null)", 6u);
            } else {
                const std::size_t n = guest_strnlen(cpu, p, s.precision);
                emit(out, s, 0, "", reinterpret_cast<const char *>(cpu->mem + p), n, -1, s.zero);
            }
            break;
        }
        case 'n': {
            const std::uint32_t p = mt_arg_u32(args);
            const std::uint64_t count = out.size() - start;
            if (is_length(s, "hh")) {
                *mt_g2h(cpu, p, 1u) = static_cast<std::uint8_t>(count);
            } else if (is_length(s, "h")) {
                const auto v = static_cast<std::uint16_t>(count);
                std::memcpy(mt_g2h(cpu, p, 2u), &v, 2u);
            } else if (wide(s)) {
                std::memcpy(mt_g2h(cpu, p, 8u), &count, 8u);
            } else {
                const auto v = static_cast<std::uint32_t>(count);
                std::memcpy(mt_g2h(cpu, p, 4u), &v, 4u);
            }
            break;
        }
        default: // unknown conversion, '%' included: the character itself, padded
            emit(out, s, 0, "", &s.conv, 1u, -1, s.zero);
            break;
        }
    }
    const std::size_t n = out.size() - start;
    return n > static_cast<std::size_t>(INT_MAX) ? -1 : static_cast<int>(n);
}
