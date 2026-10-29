// The guest symbol table (ADR 0030): function pointer, size and name of every guest function,
// loaded from the text python -m recomp symbols writes. See maplethief/diag.h.
//
// Plain C++ without exceptions, `new` or a C++ runtime library (like hle/imports.cpp), so the
// recompiler's C test harness can link it. Read-only once loaded: the crash reporter looks
// symbols up from a signal handler.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "maplethief/diag.h"

namespace {

struct Sym {
    std::uint32_t start; // code address (bit 0 cleared)
    std::uint32_t end;   // one past the last byte; 0x100000000 is clamped to 0xFFFFFFFF
    std::uint32_t ptr;   // function pointer as written (Thumb bit)
    std::uint32_t name;  // offset into the pool
};

struct Table {
    Sym *items = nullptr;
    std::uint32_t count = 0;
    char *pool = nullptr;
};

Table g_table;

void free_table(Table &t) {
    std::free(t.items);
    std::free(t.pool);
    t = Table{};
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// 1 to 8 hex digits (an optional 0x prefix) at *p; advances past them.
bool parse_hex(const char *&p, const char *end, std::uint32_t *out) {
    if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    std::uint32_t v = 0;
    int digits = 0;
    for (; p < end && hex_value(*p) >= 0; p++) {
        if (++digits > 8) {
            return false;
        }
        v = v << 4 | static_cast<std::uint32_t>(hex_value(*p));
    }
    *out = v;
    return digits > 0;
}

bool skip_blanks(const char *&p, const char *end) {
    const char *start = p;
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p > start;
}

int by_start(const void *a, const void *b) {
    const auto *x = static_cast<const Sym *>(a);
    const auto *y = static_cast<const Sym *>(b);
    if (x->start != y->start) {
        return x->start < y->start ? -1 : 1;
    }
    return 0;
}

} // namespace

extern "C" int mt_symbols_load_text(const char *text, std::size_t len,
                                    std::uint32_t *bad) noexcept {
    std::uint32_t skipped = 0, lines = 0;
    for (std::size_t i = 0; i < len; i++) {
        lines += text[i] == '\n' ? 1u : 0u;
    }
    lines++;
    Table t;
    t.items = static_cast<Sym *>(std::calloc(lines, sizeof(Sym)));
    t.pool = static_cast<char *>(std::malloc(len + 1u));
    if (t.items == nullptr || t.pool == nullptr) {
        free_table(t);
        return -1;
    }
    std::size_t pool_len = 0;
    const char *p = text;
    const char *const text_end = text + len;
    while (p < text_end) {
        const char *eol = static_cast<const char *>(std::memchr(p, '\n', text_end - p));
        const char *end = eol != nullptr ? eol : text_end;
        const char *next = eol != nullptr ? eol + 1 : text_end;
        while (end > p && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        skip_blanks(p, end);
        if (p == end || *p == '#') {
            p = next;
            continue;
        }
        std::uint32_t ptr = 0, size = 0;
        const bool ok = parse_hex(p, end, &ptr) && skip_blanks(p, end) &&
                        parse_hex(p, end, &size) && skip_blanks(p, end) && p < end && size != 0u &&
                        static_cast<std::uint64_t>(ptr & ~1u) + size <= 0x100000000ull;
        if (!ok) {
            skipped++;
            p = next;
            continue;
        }
        Sym &s = t.items[t.count++];
        s.ptr = ptr;
        s.start = ptr & ~1u;
        const std::uint64_t e = static_cast<std::uint64_t>(s.start) + size;
        s.end = e > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(e);
        s.name = static_cast<std::uint32_t>(pool_len);
        std::memcpy(t.pool + pool_len, p, static_cast<std::size_t>(end - p));
        pool_len += static_cast<std::size_t>(end - p);
        t.pool[pool_len++] = '\0';
        p = next;
    }
    std::qsort(t.items, t.count, sizeof(Sym), by_start);
    free_table(g_table);
    g_table = t;
    if (bad != nullptr) {
        *bad = skipped;
    }
    return static_cast<int>(t.count);
}

extern "C" int mt_symbols_load(const char *path) noexcept {
    if (path == nullptr) {
        return -1;
    }
    std::FILE *f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(path, "rb");
#endif
    if (f == nullptr) {
        return -1;
    }
    // Read in chunks: no seeking, so a pipe or a special file works too. At most 64 MiB.
    constexpr std::size_t k_max = std::size_t{64} << 20;
    std::size_t cap = 1u << 16, len = 0;
    char *buf = static_cast<char *>(std::malloc(cap));
    while (buf != nullptr) {
        if (len == cap) {
            char *bigger = cap < k_max ? static_cast<char *>(std::realloc(buf, cap * 2u)) : nullptr;
            if (bigger == nullptr) {
                std::free(buf);
                buf = nullptr;
                break;
            }
            buf = bigger;
            cap *= 2u;
        }
        const std::size_t n = std::fread(buf + len, 1, cap - len, f);
        if (n == 0) {
            break;
        }
        len += n;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (buf == nullptr || failed) {
        std::free(buf);
        return -1;
    }
    const int n = mt_symbols_load_text(buf, len, nullptr);
    std::free(buf);
    return n;
}

extern "C" void mt_symbols_clear(void) noexcept { free_table(g_table); }

extern "C" std::uint32_t mt_symbols_count(void) noexcept { return g_table.count; }

extern "C" const char *mt_symbols_lookup(std::uint32_t addr, std::uint32_t *offset,
                                         std::uint32_t *ptr) noexcept {
    const Table &t = g_table;
    const std::uint32_t key = addr & ~1u;
    std::uint32_t lo = 0, hi = t.count;
    while (lo < hi) { // first symbol starting above key
        const std::uint32_t mid = lo + (hi - lo) / 2u;
        if (t.items[mid].start <= key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == 0u) {
        return nullptr;
    }
    const Sym &s = t.items[lo - 1u];
    if (key >= s.end) {
        return nullptr;
    }
    if (offset != nullptr) {
        *offset = key - s.start;
    }
    if (ptr != nullptr) {
        *ptr = s.ptr;
    }
    return t.pool + s.name;
}

extern "C" std::size_t mt_symbolize(std::uint32_t addr, char *buf, std::size_t size) noexcept {
    if (size == 0u) {
        return 0;
    }
    std::uint32_t off = 0;
    const char *name = mt_symbols_lookup(addr, &off, nullptr);
    int n = 0;
    if (name == nullptr) {
        buf[0] = '\0';
    } else if (off == 0u) {
        n = std::snprintf(buf, size, "%s", name);
    } else {
        n = std::snprintf(buf, size, "%s+0x%X", name, static_cast<unsigned>(off));
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(n) < size ? static_cast<std::size_t>(n) : size - 1u;
}
