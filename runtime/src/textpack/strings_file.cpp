// A text pack's .strings files (#189, ADR 0108): see strings_file.h.
#include "textpack/strings_file.h"

#include <cstdio>

#include "maplethief/textpack.h"

namespace mt_textpack {

namespace {

using u32 = std::uint32_t;

class Parser {
  public:
    Parser(const std::uint8_t *p, std::size_t n) : p_(p), n_(n) {}

    bool run(StringsFile *out, std::string *why) {
        if (n_ >= 3 && p_[0] == 0xEF && p_[1] == 0xBB && p_[2] == 0xBF) {
            i_ = 3;
        }
        while (true) {
            if (!skip(why)) {
                return false;
            }
            if (i_ >= n_) {
                return true;
            }
            std::string key, value;
            if (p_[i_] != '"') {
                return fail(why, "expected a quoted key");
            }
            if (!string(&key, why) || !skip(why)) {
                return false;
            }
            if (i_ >= n_ || p_[i_] != '=') {
                return fail(why, "expected '=' after the key");
            }
            i_++;
            if (!skip(why)) {
                return false;
            }
            if (i_ >= n_ || p_[i_] != '"') {
                return fail(why, "expected a quoted value after '='");
            }
            const u32 value_line = line_;
            if (!string(&value, why) || !skip(why)) {
                return false;
            }
            if (i_ >= n_ || p_[i_] != ';') {
                return fail(why, "expected ';' after the value");
            }
            i_++;
            if (key.empty()) {
                return fail_at(why, value_line, "an empty key");
            }
            if (key.size() > MT_TEXTPACK_MAX_KEY) {
                return fail_at(why, value_line,
                               "a key longer than " + std::to_string(MT_TEXTPACK_MAX_KEY) +
                                   " bytes");
            }
            if (value.size() > MT_TEXTPACK_MAX_VALUE) {
                return fail_at(why, value_line,
                               "a value longer than " + std::to_string(MT_TEXTPACK_MAX_VALUE) +
                                   " bytes");
            }
            if (out->entries.count(key) != 0) {
                out->duplicates++;
            } else if (out->entries.size() >= MT_TEXTPACK_MAX_ENTRIES) {
                return fail_at(why, value_line,
                               "more than " + std::to_string(MT_TEXTPACK_MAX_ENTRIES) + " entries");
            }
            out->entries[key] = std::move(value);
        }
    }

  private:
    bool fail(std::string *why, const std::string &what) const { return fail_at(why, line_, what); }
    static bool fail_at(std::string *why, u32 line, const std::string &what) {
        *why = "line " + std::to_string(line) + ": " + what;
        return false;
    }

    // White space and comments.
    bool skip(std::string *why) {
        while (i_ < n_) {
            const std::uint8_t c = p_[i_];
            if (c == '\n') {
                line_++;
                i_++;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                i_++;
            } else if (c == '/' && i_ + 1 < n_ && p_[i_ + 1] == '*') {
                const u32 start = line_;
                i_ += 2;
                while (i_ + 1 < n_ && !(p_[i_] == '*' && p_[i_ + 1] == '/')) {
                    line_ += p_[i_] == '\n' ? 1 : 0;
                    i_++;
                }
                if (i_ + 1 >= n_) {
                    return fail_at(why, start, "a comment without its end (*/)");
                }
                i_ += 2;
            } else if (c == '/' && i_ + 1 < n_ && p_[i_ + 1] == '/') {
                while (i_ < n_ && p_[i_] != '\n') {
                    i_++;
                }
            } else {
                return true;
            }
        }
        return true;
    }

    static int hex(std::uint8_t c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    }

    // \Uxxxx after the backslash: the UTF-16 unit, or -1.
    long unit() {
        if (i_ + 5 > n_) {
            return -1;
        }
        long v = 0;
        for (std::size_t k = 1; k <= 4; k++) {
            const int h = hex(p_[i_ + k]);
            if (h < 0) {
                return -1;
            }
            v = v * 16 + h;
        }
        i_ += 5;
        return v;
    }

    static void append(std::string *out, u32 cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // A quoted string at p_[i_] == '"'.
    bool string(std::string *out, std::string *why) {
        const u32 start = line_;
        i_++;
        while (true) {
            if (i_ >= n_) {
                return fail_at(why, start, "a string without its closing quote");
            }
            const std::uint8_t c = p_[i_];
            if (c == '"') {
                i_++;
                return true;
            }
            if (c == 0) {
                return fail(why, "a NUL character");
            }
            if (c != '\\') {
                line_ += c == '\n' ? 1 : 0;
                out->push_back(static_cast<char>(c));
                i_++;
                continue;
            }
            if (i_ + 1 >= n_) {
                return fail_at(why, start, "a string without its closing quote");
            }
            const std::uint8_t e = p_[i_ + 1];
            switch (e) {
            case '"':
            case '\\':
            case '\'':
                out->push_back(static_cast<char>(e));
                i_ += 2;
                continue;
            case 'n':
                out->push_back('\n');
                i_ += 2;
                continue;
            case 't':
                out->push_back('\t');
                i_ += 2;
                continue;
            case 'r':
                out->push_back('\r');
                i_ += 2;
                continue;
            case 'U':
            case 'u': {
                i_++;
                const long hi = unit();
                if (hi < 0) {
                    return fail(why, "\\U needs four hex digits");
                }
                u32 cp = static_cast<u32>(hi);
                if (hi >= 0xDC00 && hi < 0xE000) {
                    return fail(why, "a lone surrogate (\\U" + hex4(cp) + ")");
                }
                if (hi >= 0xD800 && hi < 0xDC00) {
                    long lo = -1;
                    if (i_ + 1 < n_ && p_[i_] == '\\' && (p_[i_ + 1] == 'U' || p_[i_ + 1] == 'u')) {
                        i_++;
                        lo = unit();
                    }
                    if (lo < 0xDC00 || lo >= 0xE000) {
                        return fail(why, "a lone surrogate (\\U" + hex4(cp) + ")");
                    }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<u32>(lo) - 0xDC00u);
                }
                if (cp == 0) {
                    return fail(why, "a NUL character");
                }
                append(out, cp);
                continue;
            }
            default:
                return fail(why, std::string("an unknown escape \\") +
                                     (e >= 0x20 && e < 0x7F ? std::string(1, static_cast<char>(e))
                                                            : "x" + hex2(e)));
            }
        }
    }

    static std::string hex2(u32 v) {
        char b[8];
        std::snprintf(b, sizeof b, "%02X", static_cast<unsigned>(v & 0xFF));
        return b;
    }
    static std::string hex4(u32 v) {
        char b[8];
        std::snprintf(b, sizeof b, "%04X", static_cast<unsigned>(v & 0xFFFF));
        return b;
    }

    const std::uint8_t *p_;
    std::size_t n_;
    std::size_t i_ = 0;
    u32 line_ = 1;
};

} // namespace

bool valid_utf8(const std::uint8_t *p, std::size_t n, std::size_t *bad) {
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t c = p[i];
        u32 len = 0, cp = 0, min = 0;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2, cp = c & 0x1Fu, min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3, cp = c & 0x0Fu, min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4, cp = c & 0x07u, min = 0x10000;
        } else {
            if (bad != nullptr) {
                *bad = i;
            }
            return false;
        }
        bool ok = i + len <= n;
        for (u32 k = 1; ok && k < len; k++) {
            ok = (p[i + k] & 0xC0u) == 0x80u;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        ok = ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp < 0xE000);
        if (!ok) {
            if (bad != nullptr) {
                *bad = i;
            }
            return false;
        }
        i += len;
    }
    return true;
}

unsigned utf8_length(std::uint8_t lead) {
    return lead >= 0xC0 && lead <= 0xDF   ? 2u
           : lead >= 0xE0 && lead <= 0xEF ? 3u
           : lead >= 0xF0 && lead <= 0xF7 ? 4u
                                          : 1u;
}

std::string utf8_cut(const std::string &s, std::size_t max) {
    if (s.size() <= max) {
        return s;
    }
    std::size_t end = 0;
    while (end < s.size()) {
        const std::size_t len = utf8_length(static_cast<std::uint8_t>(s[end]));
        if (end + len > max) {
            break;
        }
        end += len;
    }
    return s.substr(0, end);
}

bool parse_strings(const std::uint8_t *bytes, std::size_t n, StringsFile *out, std::string *why) {
    *out = StringsFile{};
    if (n > MT_TEXTPACK_MAX_FILE) {
        *why = "larger than " + std::to_string(MT_TEXTPACK_MAX_FILE >> 20) + " MiB";
        return false;
    }
    std::size_t bad = 0;
    if (!valid_utf8(bytes, n, &bad)) {
        u32 line = 1;
        for (std::size_t i = 0; i < bad; i++) {
            line += bytes[i] == '\n' ? 1 : 0;
        }
        char b[96];
        std::snprintf(b, sizeof b, "line %u: invalid UTF-8 (byte 0x%02X at offset %zu)", line,
                      static_cast<unsigned>(bytes[bad]), bad);
        *why = b;
        return false;
    }
    Parser p(bytes, n);
    if (!p.run(out, why)) {
        *out = StringsFile{};
        return false;
    }
    return true;
}

} // namespace mt_textpack
