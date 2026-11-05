// The offline service stubs' shared machinery (#52, maplethief/stubs.h): deferred deliveries
// and mt_stubs_pump, the per-runtime reset, logging, guest strings, the data directory and
// file writing.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "maplethief/saves.h"
#include "objc/objc_internal.h"
#include "snapshot/snapshot_internal.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

constexpr u32 k_path_max = 1024;

struct Delivery {
    Deliver fn;
    u32 words[k_delivery_words];
};

// File-scope state (no function-local statics: see stubs_internal.h). The guest is single
// threaded (ADR 0014), and so are the stubs.
struct State {
    mt_objc *rt = nullptr; // the runtime the waiting deliveries and module state belong to
    Delivery *queue = nullptr;
    u32 count = 0, cap = 0;
    char data_dir[k_path_max] = {};    // mt_stubs_set_data_dir, "" for the default
    char default_dir[k_path_max] = {}; // computed on first use
    bool default_dir_known = false;
};
State g;

void drop_all() {
    g.count = 0;
    gamekit_reset();
    storekit_reset();
}

bool append(char *out, u32 size, const char *a, const char *b) {
    const int n = std::snprintf(out, size, "%s%s", a, b);
    return n >= 0 && static_cast<u32>(n) < size;
}

// The environment variable `name` into out[0..size); false if unset, empty or too long.
bool env_get(const char *name, char *out, u32 size) {
    out[0] = '\0';
#if defined(_MSC_VER)
    char *value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return false;
    }
    const bool ok = value[0] != '\0' && append(out, size, value, "");
    std::free(value);
    return ok;
#else
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && append(out, size, value, "");
#endif
}

std::FILE *open_file(const char *path, const char *mode) {
#if defined(_MSC_VER)
    std::FILE *f = nullptr;
    return fopen_s(&f, path, mode) == 0 ? f : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

void compute_default_dir() {
    g.default_dir_known = true;
    g.default_dir[0] = '\0';
    char env[k_path_max];
    if (env_get("MAPLETHIEF_DATA_DIR", env, k_path_max)) {
        append(g.default_dir, k_path_max, env, "");
        return;
    }
#if defined(_WIN32)
    if (env_get("APPDATA", env, k_path_max)) {
        append(g.default_dir, k_path_max, env, "\\MapleThief");
    }
#elif defined(__APPLE__)
    if (env_get("HOME", env, k_path_max)) {
        append(g.default_dir, k_path_max, env, "/Library/Application Support/MapleThief");
    }
#else
    if (env_get("XDG_DATA_HOME", env, k_path_max) && env[0] == '/') {
        append(g.default_dir, k_path_max, env, "/maplethief");
    } else if (env_get("HOME", env, k_path_max)) {
        append(g.default_dir, k_path_max, env, "/.local/share/maplethief");
    }
#endif
}

bool is_separator(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

} // namespace

mt_objc *runtime() {
    mt_objc *rt = mt_objc_current();
    if (rt != g.rt) {
        g.rt = rt;
        drop_all();
    }
    return rt;
}

void log(const char *fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char line[540];
    std::snprintf(line, sizeof line, "stubs: %s", text);
    mt_objc *rt = mt_objc_current();
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

bool post(Deliver fn, const u32 *words, u32 count) {
    runtime();
    if (g.count == g.cap) {
        const u32 grown = g.cap != 0 ? g.cap * 2 : 16;
        void *p = std::realloc(g.queue, sizeof(Delivery) * grown);
        if (p == nullptr) {
            log("out of memory: a completion handler is dropped");
            return false;
        }
        g.queue = static_cast<Delivery *>(p);
        g.cap = grown;
    }
    Delivery &d = g.queue[g.count++];
    d.fn = fn;
    for (u32 i = 0; i < k_delivery_words; i++) {
        d.words[i] = i < count ? words[i] : 0u;
    }
    return true;
}

bool string_text(mt_cpu *cpu, u32 str, char *out, u32 size) {
    if (size == 0) {
        return false;
    }
    out[0] = '\0';
    if (str == 0) {
        return false;
    }
    const u32 p = mt_objc_send0(cpu, str, "UTF8String");
    const char *s = p != 0 ? mt_objc_impl::guest_string(mt_objc_current(), p) : nullptr;
    if (s == nullptr) {
        return false;
    }
    std::snprintf(out, size, "%s", s);
    return true;
}

u32 new_string(mt_cpu *cpu, const char *text) {
    mt_objc *rt = runtime();
    const u32 cls = mt_objc_class_named(rt, "NSString");
    const std::size_t n = std::strlen(text) + 1;
    const u32 sp = cpu->r[MT_SP];
    if (cls == 0 || n > 4096 || sp < 8192 || sp > cpu->mem_size) {
        return 0;
    }
    // The C string goes into scratch space below the guest sp; sp moves below it for the
    // send, so the callee's frame does not overwrite it.
    const u32 buf = (sp - static_cast<u32>(n)) & ~7u;
    std::memcpy(cpu->mem + buf, text, n);
    cpu->r[MT_SP] = buf;
    const u32 str = mt_objc_send1(cpu, cls, "stringWithUTF8String:", buf);
    cpu->r[MT_SP] = sp;
    return str;
}

u32 new_data(mt_cpu *cpu, const void *bytes, u32 n) {
    mt_objc *rt = runtime();
    const u32 cls = mt_objc_class_named(rt, "NSData");
    const u32 sp = cpu->r[MT_SP];
    if (cls == 0 || n > 4096 || sp < 8192 || sp > cpu->mem_size) {
        return 0;
    }
    // As new_string: the bytes go below the guest sp for the send.
    const u32 buf = (sp - n) & ~7u;
    std::memcpy(cpu->mem + buf, bytes, n);
    cpu->r[MT_SP] = buf;
    const u32 obj = mt_objc_send0(cpu, cls, "alloc");
    const u32 data = obj != 0 ? mt_objc_send2(cpu, obj, "initWithBytes:length:", buf, n) : 0u;
    cpu->r[MT_SP] = sp;
    return data;
}

bool words_push(Words &w, u32 value) {
    if (w.count == w.cap) {
        const u32 grown = w.cap != 0 ? w.cap * 2 : 8;
        void *p = std::realloc(w.items, sizeof(u32) * grown);
        if (p == nullptr) {
            return false;
        }
        w.items = static_cast<u32 *>(p);
        w.cap = grown;
    }
    w.items[w.count++] = value;
    return true;
}

bool words_remove(Words &w, u32 value) {
    for (u32 i = 0; i < w.count; i++) {
        if (w.items[i] == value) {
            std::memmove(w.items + i, w.items + i + 1, sizeof(u32) * (w.count - i - 1));
            w.count--;
            return true;
        }
    }
    return false;
}

bool words_contains(const Words &w, u32 value) {
    for (u32 i = 0; i < w.count; i++) {
        if (w.items[i] == value) {
            return true;
        }
    }
    return false;
}

void words_free(Words &w) {
    std::free(w.items);
    w.items = nullptr;
    w.count = w.cap = 0;
}

char *read_file(const char *path, std::size_t *size) {
    *size = 0;
    std::FILE *f = open_file(path, "rb");
    if (f == nullptr) {
        return nullptr;
    }
    std::size_t cap = 4096, n = 0;
    char *buf = static_cast<char *>(std::malloc(cap + 1));
    while (buf != nullptr) {
        n += std::fread(buf + n, 1, cap - n, f);
        if (n < cap) {
            break;
        }
        cap *= 2;
        char *grown = static_cast<char *>(std::realloc(buf, cap + 1));
        if (grown == nullptr) {
            std::free(buf);
            buf = nullptr;
        }
        buf = grown;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (buf == nullptr || failed) {
        std::free(buf);
        return nullptr;
    }
    buf[n] = '\0';
    *size = n;
    return buf;
}

bool write_file(const char *path, const char *data, std::size_t size) {
    if (mt_saves_replace_file(path, data, size) != 0) {
        log("cannot write %s", path);
        return false;
    }
    return true;
}

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// A delivery waiting for the next run-loop turn (a GameKit or StoreKit answer) is a host
// function with guest words: no safe point while one waits. A restore drops the present's
// waiting deliveries: they answer requests of the state that is gone.

namespace {

bool save_deliveries(mt_cpu *, mt_snap::Writer &, std::string *why) {
    if (g.count != 0) {
        *why = "a service answer is waiting";
        return false;
    }
    return true;
}
bool load_deliveries(mt_cpu *, mt_snap::Reader &, std::function<void(mt_cpu *)> *apply) {
    *apply = [](mt_cpu *) { g.count = 0; };
    return true;
}

} // namespace

extern const mt_snap::Module k_snap_stubs;
const mt_snap::Module k_snap_stubs = {"stubs", save_deliveries, load_deliveries};

} // namespace mt_stubs_impl

using namespace mt_stubs_impl;

extern "C" std::uint32_t mt_stubs_pump(mt_cpu *cpu) noexcept {
    mt_objc *rt = runtime();
    const u32 due = g.count; // deliveries posted while these run wait for the next pump
    u32 done = 0;
    for (; done < due; done++) {
        const Delivery d = g.queue[done]; // a copy: running it may grow the queue
        const u32 pool = mt_objc_pool_push(rt);
        d.fn(cpu, d.words);
        mt_objc_pool_pop(rt, pool);
        if (g.rt != rt) {
            return done + 1; // a delivery switched runtimes: the queue was dropped
        }
    }
    std::memmove(g.queue, g.queue + done, sizeof(Delivery) * (g.count - done));
    g.count -= done;
    return done;
}

extern "C" std::uint32_t mt_stubs_pending(void) noexcept {
    runtime();
    return g.count;
}

extern "C" void mt_stubs_reset(void) noexcept {
    g.rt = mt_objc_current();
    drop_all();
}

extern "C" const char *mt_stubs_data_dir(void) noexcept {
    if (g.data_dir[0] != '\0') {
        return g.data_dir;
    }
    if (!g.default_dir_known) {
        compute_default_dir();
    }
    return g.default_dir;
}

extern "C" int mt_stubs_set_data_dir(const char *dir) noexcept {
    if (dir == nullptr || dir[0] == '\0') {
        g.data_dir[0] = '\0';
        g.default_dir_known = false; // the environment may have changed
        return 0;
    }
    if (std::strlen(dir) >= k_path_max - 32) {
        return -1;
    }
    std::snprintf(g.data_dir, k_path_max, "%s", dir);
    return 0;
}

extern "C" int mt_stubs_gamecenter_path(char *out, std::uint32_t size) noexcept {
    const char *dir = mt_stubs_data_dir();
    const std::size_t len = std::strlen(dir);
#if defined(_WIN32)
    const char *sep = len != 0 && !is_separator(dir[len - 1]) ? "\\" : "";
#else
    const char *sep = len != 0 && !is_separator(dir[len - 1]) ? "/" : "";
#endif
    const int n = std::snprintf(out, size, "%s%sgamecenter.json", dir, sep);
    return n >= 0 && static_cast<u32>(n) < size ? 0 : -1;
}
