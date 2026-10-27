// Diagnostics (ADR 0030): configuration, trace channels, the HLE call trace, the
// unimplemented import/selector report and the function-entry coverage. See
// maplethief/diag.h; the symbol table is symbols.cpp, the crash reporter crash.cpp.
//
// Plain C++ without exceptions, `new`, function-local statics or a C++ runtime library (like
// hle/imports.cpp), so the recompiler's C test harness can link it. The guest is
// single-threaded (ADR 0014), so nothing here locks.
#include "maplethief/diag.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diag_internal.h"

namespace {

using u32 = std::uint32_t;

// --- small helpers ------------------------------------------------------------------------

char *dup(const char *s) {
    if (s == nullptr) {
        return nullptr;
    }
    const std::size_t n = std::strlen(s) + 1u;
    auto *d = static_cast<char *>(std::malloc(n));
    if (d != nullptr) {
        std::memcpy(d, s, n);
    }
    return d;
}

bool empty(const char *s) { return s == nullptr || s[0] == '\0'; }

const char *plural(u32 n, const char *one, const char *many) { return n == 1u ? one : many; }

// "/System/Library/Frameworks/UIKit.framework/UIKit" -> "UIKit"
const char *base_name(const char *path) {
    if (path == nullptr) {
        return "?";
    }
    const char *slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

// Appends to a caller's buffer like snprintf, keeping the full length.
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

Text text(char *buf, std::size_t size) {
    if (buf != nullptr && size > 0u) {
        buf[0] = '\0';
    }
    return Text{buf, buf != nullptr ? size : 0u, 0u};
}

// " Name+0xOFF" for a guest code address, or "".
void put_symbol(Text &t, u32 addr) {
    char sym[160];
    if (mt_symbolize(addr, sym, sizeof sym) != 0u) {
        put(t, " %s", sym);
    }
}

// --- state --------------------------------------------------------------------------------

struct Entry {
    char *key;     // "_name" or "-[Class sel]"
    char *library; // imports: the install name's base name
    u32 kind;      // 0 import, 1 selector
    u32 calls;
    u32 first; // first caller
};

struct Report {
    Entry *items = nullptr;
    u32 count = 0, cap = 0;
    u32 *index = nullptr; // open addressing: item + 1, 0 = free
    u32 index_cap = 0;    // a power of two, at least twice count
};

struct Covered {
    const char *name;
    const std::uint64_t *calls;
};

struct State {
    char *trace_hle = nullptr;
    char *trace = nullptr;
    char *report_path = nullptr;
    char *coverage_path = nullptr;
    mt_log_fn log = nullptr;
    void *log_user = nullptr;
    bool atexit_registered = false;
    bool finished = false;
    // The HLE trace: the wrapped table's real functions.
    const mt_import_desc *descs = nullptr;
    u32 count = 0;
    mt_import_fn *orig = nullptr;
    Report report;
    Covered *covered = nullptr;
    u32 covered_count = 0, covered_cap = 0;
};

State g;

void log_line(const char *line) {
    if (g.log != nullptr) {
        g.log(g.log_user, line);
        return;
    }
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void at_exit() { mt_diag_finish(); }

// --- matching -----------------------------------------------------------------------------

// Glob over [p, pe) against s: * any run, ? one character.
bool glob(const char *p, const char *pe, const char *s) {
    const char *star = nullptr, *star_s = nullptr;
    while (*s != '\0') {
        if (p < pe && (*p == '?' || *p == *s)) {
            p++;
            s++;
        } else if (p < pe && *p == '*') {
            star = p++;
            star_s = s;
        } else if (star != nullptr) {
            p = star + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (p < pe && *p == '*') {
        p++;
    }
    return p == pe;
}

// Calls fn(begin, end) for every comma-separated, blank-trimmed item of `list`.
template <typename Fn> void each_item(const char *list, Fn fn) {
    if (list == nullptr) {
        return;
    }
    const char *p = list;
    for (;;) {
        const char *comma = std::strchr(p, ',');
        const char *end = comma != nullptr ? comma : p + std::strlen(p);
        const char *b = p, *e = end;
        while (b < e && (*b == ' ' || *b == '\t')) {
            b++;
        }
        while (e > b && (e[-1] == ' ' || e[-1] == '\t')) {
            e--;
        }
        if (b < e) {
            fn(b, e);
        }
        if (comma == nullptr) {
            break;
        }
        p = comma + 1;
    }
}

// --- the report ---------------------------------------------------------------------------

u32 hash(u32 kind, const char *key) {
    u32 h = 2166136261u ^ kind;
    for (const char *p = key; *p != '\0'; p++) {
        h = (h ^ static_cast<unsigned char>(*p)) * 16777619u;
    }
    return h;
}

bool reindex(Report &r, u32 cap) {
    auto *index = static_cast<u32 *>(std::calloc(cap, sizeof(u32)));
    if (index == nullptr) {
        return false;
    }
    for (u32 i = 0; i < r.count; i++) {
        u32 slot = hash(r.items[i].kind, r.items[i].key) & (cap - 1u);
        while (index[slot] != 0u) {
            slot = (slot + 1u) & (cap - 1u);
        }
        index[slot] = i + 1u;
    }
    std::free(r.index);
    r.index = index;
    r.index_cap = cap;
    return true;
}

void record(u32 kind, const char *key, const char *library, u32 caller) {
    Report &r = g.report;
    if (r.index_cap == 0u && !reindex(r, 64u)) {
        return;
    }
    const u32 h = hash(kind, key);
    u32 slot = h & (r.index_cap - 1u);
    while (r.index[slot] != 0u) {
        Entry &e = r.items[r.index[slot] - 1u];
        if (e.kind == kind && std::strcmp(e.key, key) == 0) {
            e.calls += e.calls != 0xFFFFFFFFu ? 1u : 0u;
            return;
        }
        slot = (slot + 1u) & (r.index_cap - 1u);
    }
    if (r.count == r.cap) {
        const u32 cap = r.cap != 0u ? r.cap * 2u : 32u;
        auto *items = static_cast<Entry *>(std::realloc(r.items, cap * sizeof(Entry)));
        if (items == nullptr) {
            return;
        }
        r.items = items;
        r.cap = cap;
    }
    Entry &e = r.items[r.count];
    e = Entry{dup(key), dup(library), kind, 1u, caller};
    if (e.key == nullptr) {
        std::free(e.library);
        return;
    }
    r.index[slot] = ++r.count;
    if (r.count * 2u > r.index_cap) {
        reindex(r, r.index_cap * 2u);
    }
}

void free_report(Report &r) {
    for (u32 i = 0; i < r.count; i++) {
        std::free(r.items[i].key);
        std::free(r.items[i].library);
    }
    std::free(r.items);
    std::free(r.index);
    r = Report{};
}

// Writes the report through `out`, one line at a time.
template <typename Out> void emit_report(Out out) {
    const std::size_t n = mt_report_format(nullptr, 0) + 1u;
    char *buf = static_cast<char *>(std::malloc(n));
    if (buf == nullptr) {
        return;
    }
    mt_report_format(buf, n);
    for (char *line = buf; line != nullptr && *line != '\0';) {
        char *nl = std::strchr(line, '\n');
        if (nl != nullptr) {
            *nl = '\0';
        }
        out(line);
        line = nl != nullptr ? nl + 1 : nullptr;
    }
    std::free(buf);
}

std::FILE *open_write(const char *path) {
    std::FILE *f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "wb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(path, "wb");
#endif
    if (f == nullptr) {
        char line[600];
        std::snprintf(line, sizeof line, "diag: cannot write %s", path);
        log_line(line);
    }
    return f;
}

// --- coverage -----------------------------------------------------------------------------

u32 fn_addr(const char *name) {
    if (name == nullptr || name[0] != 'f' || name[1] != '_') {
        return 0;
    }
    return static_cast<u32>(std::strtoul(name + 2, nullptr, 16));
}

int by_addr(const void *a, const void *b) {
    const u32 x = fn_addr(static_cast<const Covered *>(a)->name);
    const u32 y = fn_addr(static_cast<const Covered *>(b)->name);
    return x < y ? -1 : (x > y ? 1 : 0);
}

char *env_dup(const char *name) {
#if defined(_MSC_VER)
    char *v = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0) {
        return nullptr;
    }
    return v; // malloc'ed by the CRT
#else
    return dup(std::getenv(name));
#endif
}

} // namespace

// --- configuration ------------------------------------------------------------------------

extern "C" mt_diag_config mt_diag_config_default(void) noexcept {
    return mt_diag_config{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}

extern "C" int mt_diag_configure(const mt_diag_config *cfg) noexcept {
    const mt_diag_config c = cfg != nullptr ? *cfg : mt_diag_config_default();
    std::free(g.trace_hle);
    std::free(g.trace);
    std::free(g.report_path);
    std::free(g.coverage_path);
    g.trace_hle = empty(c.trace_hle) ? nullptr : dup(c.trace_hle);
    g.trace = empty(c.trace) ? nullptr : dup(c.trace);
    g.report_path = empty(c.report_path) ? nullptr : dup(c.report_path);
    g.coverage_path = empty(c.coverage_path) ? nullptr : dup(c.coverage_path);
    g.log = c.log;
    g.log_user = c.log_user;
    if (!g.atexit_registered) {
        g.atexit_registered = std::atexit(at_exit) == 0;
    }
    if (!empty(c.symbols_path)) {
        const int n = mt_symbols_load(c.symbols_path);
        char line[600];
        if (n < 0) {
            std::snprintf(line, sizeof line, "diag: cannot read the symbol table %s",
                          c.symbols_path);
            log_line(line);
            return -1;
        }
        std::snprintf(line, sizeof line, "%d guest symbols from %s", n, c.symbols_path);
        mt_tracef("diag", "%s", line);
    }
    return 0;
}

extern "C" int mt_diag_configure_from_env(void) noexcept {
    char *hle = env_dup(MT_TRACE_HLE_ENV);
    char *trace = env_dup(MT_TRACE_ENV);
    char *report = env_dup(MT_REPORT_ENV);
    char *coverage = env_dup(MT_COVERAGE_ENV);
    char *symbols = env_dup(MT_SYMBOLS_ENV);
    mt_diag_config cfg = mt_diag_config_default();
    cfg.trace_hle = hle;
    cfg.trace = trace;
    cfg.report_path = report;
    cfg.coverage_path = coverage;
    cfg.symbols_path = symbols;
    cfg.log = g.log;
    cfg.log_user = g.log_user;
    const int status = mt_diag_configure(&cfg);
    std::free(hle);
    std::free(trace);
    std::free(report);
    std::free(coverage);
    std::free(symbols);
    return status;
}

extern "C" void mt_diag_finish(void) noexcept {
    if (g.finished) {
        return;
    }
    g.finished = true;
    const char *path = g.report_path;
    if (path == nullptr || std::strcmp(path, "-") == 0) {
        if (path != nullptr || g.report.count != 0u) {
            emit_report([](const char *line) { log_line(line); });
        }
    } else if (std::FILE *f = open_write(path)) {
        emit_report([f](const char *line) {
            std::fputs(line, f);
            std::fputc('\n', f);
        });
        std::fclose(f);
    }
    if (g.coverage_path != nullptr) {
        const std::size_t n = mt_coverage_format(nullptr, 0) + 1u;
        char *buf = static_cast<char *>(std::malloc(n));
        std::FILE *f = buf != nullptr ? open_write(g.coverage_path) : nullptr;
        if (f != nullptr) {
            mt_coverage_format(buf, n);
            std::fputs(buf, f);
            std::fclose(f);
        }
        std::free(buf);
    }
}

extern "C" void mt_diag_reset(void) noexcept {
    std::free(g.trace_hle);
    std::free(g.trace);
    std::free(g.report_path);
    std::free(g.coverage_path);
    std::free(g.orig);
    std::free(g.covered);
    free_report(g.report);
    mt_symbols_clear();
    const bool registered = g.atexit_registered;
    g = State{};
    g.atexit_registered = registered;
}

// --- trace channels -----------------------------------------------------------------------

extern "C" int mt_trace_enabled(const char *channel) noexcept {
    if (g.trace == nullptr || channel == nullptr) {
        return 0;
    }
    bool on = false;
    const std::size_t n = std::strlen(channel);
    each_item(g.trace, [&](const char *b, const char *e) {
        const auto len = static_cast<std::size_t>(e - b);
        if ((len == 1u && *b == '*') || (len == 3u && std::strncmp(b, "all", 3) == 0) ||
            (len == n && std::strncmp(b, channel, n) == 0)) {
            on = true;
        }
    });
    return on ? 1 : 0;
}

extern "C" void mt_tracef(const char *channel, const char *fmt, ...) noexcept {
    if (mt_trace_enabled(channel) == 0) {
        return;
    }
    char line[1024];
    const int prefix = std::snprintf(line, sizeof line, "%s: ", channel);
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + prefix, sizeof line - static_cast<std::size_t>(prefix), fmt, ap);
    va_end(ap);
    log_line(line);
}

// --- the HLE call trace -------------------------------------------------------------------

extern "C" int mt_trace_filter_match(const char *filters, const char *name) noexcept {
    if (empty(filters) || name == nullptr) {
        return 0;
    }
    bool match = false;
    each_item(filters, [&](const char *b, const char *e) {
        const bool exclude = *b == '!';
        if (exclude) {
            b++;
        }
        bool hit = glob(b, e, name);
        if (!hit && b < e && *b != '_' && *b != '*' && *b != '?' && name[0] == '_') {
            hit = glob(b, e, name + 1);
        }
        if (hit) {
            match = !exclude;
        }
    });
    return match ? 1 : 0;
}

extern "C" std::uint32_t mt_trace_imports_wrap(const mt_import_desc *descs, std::uint32_t count,
                                               mt_import_fn *table) noexcept {
    std::free(g.orig);
    g.orig = nullptr;
    g.descs = descs;
    g.count = count;
    if (g.trace_hle == nullptr || descs == nullptr || table == nullptr || count == 0u) {
        return 0;
    }
    g.orig = static_cast<mt_import_fn *>(std::calloc(count, sizeof(mt_import_fn)));
    if (g.orig == nullptr) {
        return 0;
    }
    u32 wrapped = 0;
    for (u32 id = 0; id < count; id++) {
        g.orig[id] = table[id];
        if ((descs[id].flags & MT_IMPORT_FUNCTION) == 0u || table[id] == mt_trace_import_call ||
            mt_trace_filter_match(g.trace_hle, descs[id].name) == 0) {
            continue;
        }
        table[id] = mt_trace_import_call;
        wrapped++;
    }
    char line[160];
    std::snprintf(line, sizeof line, "hle: tracing %u of %u imports (%s)",
                  static_cast<unsigned>(wrapped), static_cast<unsigned>(count), g.trace_hle);
    log_line(line);
    return wrapped;
}

extern "C" void mt_trace_import_call(mt_cpu *cpu, std::uint32_t id) noexcept {
    const mt_import_fn fn = g.orig != nullptr && id < g.count ? g.orig[id] : nullptr;
    const char *name = g.descs != nullptr && id < g.count ? g.descs[id].name : "?";
    const u32 lr = cpu->r[MT_LR];
    char line[512];
    Text t = text(line, sizeof line);
    put(t, "hle: %s(0x%08X, 0x%08X, 0x%08X, 0x%08X)", name, cpu->r[0], cpu->r[1], cpu->r[2],
        cpu->r[3]);
    // The objc_msgSend family: a SEL is the guest address of its name (ADR 0026).
    if (std::strncmp(name, "_objc_msgSend", 13) == 0) {
        const u32 sel = std::strstr(name, "stret") != nullptr ? cpu->r[2] : cpu->r[1];
        if (sel >= 0x1000u && static_cast<std::uint64_t>(sel) < cpu->mem_size) {
            const std::size_t room = static_cast<std::size_t>(cpu->mem_size - sel);
            const auto *s = reinterpret_cast<const char *>(cpu->mem + sel);
            const std::size_t max = room < 128u ? room : 128u;
            if (std::memchr(s, 0, max) != nullptr) {
                put(t, " [%s]", s);
            }
        }
    }
    put(t, " from 0x%08X", lr);
    put_symbol(t, lr);
    log_line(line);
    if (fn == nullptr) {
        log_line("hle: (no function to call; the table was linked again without the trace)");
        return;
    }
    fn(cpu, id);
    std::snprintf(line, sizeof line, "hle: %s -> 0x%08X (r1 0x%08X)", name, cpu->r[0], cpu->r[1]);
    log_line(line);
}

// --- the unimplemented report -------------------------------------------------------------

extern "C" void mt_report_unimplemented_import(const char *name, const char *library,
                                               std::uint32_t caller) noexcept {
    record(0u, name != nullptr ? name : "?", base_name(library), caller);
}

extern "C" void mt_report_unimplemented_selector(const char *class_name, int is_meta,
                                                 const char *selector,
                                                 std::uint32_t caller) noexcept {
    char key[320];
    std::snprintf(key, sizeof key, "%c[%s %s]", is_meta != 0 ? '+' : '-',
                  class_name != nullptr ? class_name : "(no known class)",
                  selector != nullptr ? selector : "?");
    record(1u, key, nullptr, caller);
}

extern "C" mt_report_stats mt_report_get_stats(void) noexcept {
    mt_report_stats s{0, 0, 0, 0};
    for (u32 i = 0; i < g.report.count; i++) {
        const Entry &e = g.report.items[i];
        if (e.kind == 0u) {
            s.imports++;
            s.import_calls += e.calls;
        } else {
            s.selectors++;
            s.selector_calls += e.calls;
        }
    }
    return s;
}

extern "C" std::size_t mt_report_format(char *buf, std::size_t size) noexcept {
    Text t = text(buf, size);
    const mt_report_stats s = mt_report_get_stats();
    if (g.report.count == 0u) {
        put(t, "unimplemented report: none\n");
        return t.len;
    }
    put(t, "unimplemented report: %u %s (%u %s), %u %s (%u %s)\n", s.imports,
        plural(s.imports, "import", "imports"), s.import_calls,
        plural(s.import_calls, "call", "calls"), s.selectors,
        plural(s.selectors, "selector", "selectors"), s.selector_calls,
        plural(s.selector_calls, "message", "messages"));
    for (u32 i = 0; i < g.report.count; i++) {
        const Entry &e = g.report.items[i];
        if (e.kind == 0u) {
            put(t, "import %s (%s): %u %s", e.key, e.library != nullptr ? e.library : "?", e.calls,
                plural(e.calls, "call", "calls"));
        } else {
            put(t, "selector %s: %u %s", e.key, e.calls, plural(e.calls, "message", "messages"));
        }
        put(t, ", first from 0x%08X", e.first);
        put_symbol(t, e.first);
        put(t, "\n");
    }
    return t.len;
}

// --- coverage -----------------------------------------------------------------------------

extern "C" void mt_trace_fn_first(mt_cpu *cpu, const char *fn_name,
                                  const std::uint64_t *calls) noexcept {
    if (g.covered_count == g.covered_cap) {
        const u32 cap = g.covered_cap != 0u ? g.covered_cap * 2u : 1024u;
        auto *c = static_cast<Covered *>(std::realloc(g.covered, cap * sizeof(Covered)));
        if (c == nullptr) {
            return;
        }
        g.covered = c;
        g.covered_cap = cap;
    }
    g.covered[g.covered_count++] = Covered{fn_name, calls};
    if (mt_trace_enabled("entry") != 0) {
        char sym[160];
        const u32 addr = fn_addr(fn_name);
        mt_symbolize(addr, sym, sizeof sym);
        mt_tracef("entry", "%s%s%s (first entry, from 0x%08X)", fn_name, sym[0] != '\0' ? " " : "",
                  sym, cpu != nullptr ? cpu->r[MT_LR] : 0u);
    }
}

extern "C" std::uint32_t mt_coverage_count(void) noexcept { return g.covered_count; }

extern "C" std::size_t mt_coverage_format(char *buf, std::size_t size) noexcept {
    Text t = text(buf, size);
    put(t, "# maplethief coverage 1: %u guest functions entered (pointer, entries, name)\n",
        g.covered_count);
    auto *sorted = static_cast<Covered *>(std::malloc((g.covered_count + 1u) * sizeof(Covered)));
    if (sorted == nullptr) {
        return t.len;
    }
    if (g.covered_count != 0u) {
        std::memcpy(sorted, g.covered, g.covered_count * sizeof(Covered));
    }
    std::qsort(sorted, g.covered_count, sizeof(Covered), by_addr);
    for (u32 i = 0; i < g.covered_count; i++) {
        const u32 addr = fn_addr(sorted[i].name);
        const char *sym = addr != 0u ? mt_symbols_lookup(addr, nullptr, nullptr) : nullptr;
        put(t, "0x%08X %llu %s\n", addr, static_cast<unsigned long long>(*sorted[i].calls),
            sym != nullptr ? sym : sorted[i].name);
    }
    std::free(sorted);
    return t.len;
}

// --- internal (diag/diag_internal.h) -----------------------------------------------------

extern "C" const char *mt_diag_import_name(std::uint32_t id) noexcept {
    return g.descs != nullptr && id < g.count ? g.descs[id].name : nullptr;
}
