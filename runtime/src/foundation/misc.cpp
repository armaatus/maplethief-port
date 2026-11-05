// Foundation's shared internals (#49, ADR 0035), and the small classes: NSURL (the sound files'
// and openURL:'s URLs), NSError (+errorWithDomain:code:userInfo:) and NSThread (+isMainThread).
#include <cstdarg>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "maplethief/recomp/calls.h"
#include "objc/objc_internal.h"
#include "snapshot/snapshot_internal.h"

namespace mt_ns {

namespace {

// Guest objects that belong to one runtime: forgotten when CF binds another runtime (a new
// process, a test's new runtime; mt_cf::binding, as a new runtime can reuse the host address).
struct State {
    mt_objc *rt = nullptr;
    u32 binding = 0;
    std::unordered_map<std::string, u32> singletons;
};
State g_state;

State &state(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    if (g_state.rt != rt || g_state.binding != mt_cf::binding()) {
        g_state = State{};
        g_state.rt = rt;
        g_state.binding = mt_cf::binding();
    }
    return g_state;
}

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

// --- NSURL -------------------------------------------------------------------------------------

struct UrlState {
    std::string *text; // the URL string, or the path of a file URL
    bool is_file;
};

void url_destroy(mt_cpu *, u32, void *p) { delete static_cast<UrlState *>(p)->text; }
const mt_objc_state_type k_url_state = {"NSURL", sizeof(UrlState), url_destroy};

bool url_set(mt_cpu *cpu, u32 url, std::string text, bool is_file) {
    auto *s = static_cast<UrlState *>(mt_objc_state(runtime(cpu), url, &k_url_state));
    if (s == nullptr) {
        return false;
    }
    delete s->text;
    s->text = new std::string(std::move(text));
    s->is_file = is_file;
    return true;
}

// +URLWithString: (nil for nil, as Foundation 4 returns)
void url_with_string(mt_cpu *cpu) {
    const u32 cls = cpu->r[0], str = cpu->r[2];
    if (str == 0) {
        cpu->r[0] = 0;
        return;
    }
    if (!mt_cf::is_string(cpu, str)) {
        raise(cpu, "+[NSURL URLWithString:]", "not an NSString", str);
    }
    const u32 url = mt_objc_create_instance(runtime(cpu), cls);
    if (url == 0 || !url_set(cpu, url, string_utf8(cpu, str), false)) {
        cpu->r[0] = 0;
        return;
    }
    cpu->r[0] = mt_objc_autorelease(cpu, url);
}

// -initFileURLWithPath: (nil raises)
void url_init_file(mt_cpu *cpu) {
    const u32 self = cpu->r[0], path = cpu->r[2];
    if (path == 0 || !mt_cf::is_string(cpu, path)) {
        raise(cpu, "-[NSURL initFileURLWithPath:]", "nil or not an NSString", path);
    }
    if (!url_set(cpu, self, string_utf8(cpu, path), true)) {
        mt_objc_release(cpu, self);
        cpu->r[0] = 0;
    }
}

// -description: the URL string ("file://localhost<path>" for a file URL, as iOS 4 prints it).
void url_description(mt_cpu *cpu) {
    std::string text;
    bool is_file = false;
    url_string(cpu, cpu->r[0], &text, &is_file);
    const u32 s = string_from_utf8(cpu, is_file ? "file://localhost" + text : text);
    cpu->r[0] = s != 0 ? mt_objc_autorelease(cpu, s) : 0u;
}

const mt_objc_host_method k_url_instance[] = {{"initFileURLWithPath:", url_init_file},
                                              {"description", url_description}};
const mt_objc_host_method k_url_class[] = {{"URLWithString:", url_with_string}};

// --- NSError -----------------------------------------------------------------------------------

struct ErrorState {
    u32 domain; // retained NSString
    std::int32_t code;
    u32 user_info; // retained NSDictionary or 0
};

void error_destroy(mt_cpu *cpu, u32, void *p) {
    const auto *s = static_cast<const ErrorState *>(p);
    mt_objc_release(cpu, s->domain);
    mt_objc_release(cpu, s->user_info);
}
const mt_objc_state_type k_error_state = {"NSError", sizeof(ErrorState), error_destroy};

// +errorWithDomain:code:userInfo:: what -[NSFileManager removeItemAtPath:error:] reports a
// failure with (files.cpp). The game's own send is in -[InAppPurchaseManager makePurchase:],
// which nothing calls (docs/re/host-surface.md leaves it out); nothing reads an error back.
void error_with_domain(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 cls = cpu->r[0];
    const u32 domain = mt_arg_u32(&a);
    const std::int32_t code = mt_arg_i32(&a);
    const u32 user_info = mt_arg_u32(&a);
    const u32 err = mt_objc_create_instance(runtime(cpu), cls);
    auto *s = err != 0 ? static_cast<ErrorState *>(mt_objc_state(runtime(cpu), err, &k_error_state))
                       : nullptr;
    if (s == nullptr) {
        cpu->r[0] = 0;
        return;
    }
    s->code = code;
    s->domain = mt_objc_retain(cpu, domain);
    s->user_info = mt_objc_retain(cpu, user_info);
    cpu->r[0] = mt_objc_autorelease(cpu, err);
}

const mt_objc_host_method k_error_class[] = {{"errorWithDomain:code:userInfo:", error_with_domain}};

// --- NSThread ------------------------------------------------------------------------------------

// +isMainThread: whether the sender runs on the runtime's main guest thread (GCD's worker
// threads are others, ADR 0033). No game code creates a thread.
void thread_is_main(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    cpu->r[0] = rt != nullptr && rt->cpu == cpu ? 1u : 0u;
}

const mt_objc_host_method k_thread_class[] = {{"isMainThread", thread_is_main}};

} // namespace

const mt_objc_host_class k_nsurl = {
    "NSURL",     "NSObject",           4, k_url_instance, MT_COUNT(k_url_instance),
    k_url_class, MT_COUNT(k_url_class)};
const mt_objc_host_class k_nserror = {
    "NSError", "NSObject", 4, nullptr, 0, k_error_class, MT_COUNT(k_error_class)};
const mt_objc_host_class k_nsthread = {
    "NSThread", "NSObject", 4, nullptr, 0, k_thread_class, MT_COUNT(k_thread_class)};

bool url_string(mt_cpu *cpu, u32 url, std::string *out, bool *is_file) {
    mt_objc *rt = runtime(cpu);
    const auto *s = rt != nullptr && url != 0
                        ? static_cast<const UrlState *>(mt_objc_find_state(rt, url, &k_url_state))
                        : nullptr;
    if (s == nullptr || s->text == nullptr) {
        return false;
    }
    *out = *s->text;
    *is_file = s->is_file;
    return true;
}

// --- internals -----------------------------------------------------------------------------------

mt_objc *runtime(mt_cpu *cpu) { return mt_cf::runtime(cpu); }

void log(mt_cpu *cpu, const char *fmt, ...) noexcept {
    char line[512] = "foundation: ";
    constexpr std::size_t k_prefix = sizeof "foundation: " - 1;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + k_prefix, sizeof line - k_prefix, fmt, ap);
    va_end(ap);
    mt_objc *rt = runtime(cpu);
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void raise(mt_cpu *cpu, const char *method, const char *what, u32 obj) noexcept {
    log(cpu,
        "%s: %s (0x%08X), sent from 0x%08X: stopping, as the device's uncaught "
        "NSException would",
        method, what, static_cast<unsigned>(obj), static_cast<unsigned>(cpu->r[MT_LR]));
    mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], obj);
}

bool inherits(mt_cpu *cpu, u32 cls, u32 ancestor) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || ancestor == 0) {
        return false;
    }
    for (u32 k = cls, steps = 0; k != 0 && steps < 64; k = mt_objc_superclass(rt, k), steps++) {
        if (k == ancestor) {
            return true;
        }
    }
    return false;
}

u32 class_named(mt_cpu *cpu, const char *name) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr ? mt_objc_class_named(rt, name) : 0u;
}

u32 singleton(mt_cpu *cpu, const char *class_name) {
    State &st = state(cpu);
    auto it = st.singletons.find(class_name);
    if (it != st.singletons.end()) {
        return it->second;
    }
    const u32 cls = class_named(cpu, class_name);
    const u32 obj = cls != 0 ? mt_objc_create_instance(st.rt, cls) : 0u;
    if (obj != 0) {
        st.singletons.emplace(class_name, obj);
    }
    return obj;
}

void singleton_retain(mt_cpu *) {} // r0 is self
void singleton_release(mt_cpu *) {}
void singleton_retain_count(mt_cpu *cpu) { cpu->r[0] = 0xFFFFFFFFu; } // UINT_MAX: never freed

} // namespace mt_ns

// --- save states (#202, ADR 0105) ---------------------------------------------------------------

namespace mt_ns {

namespace {

bool save_url(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const UrlState *>(state);
    w.boolean(s->text != nullptr);
    if (s->text != nullptr) {
        w.str(*s->text);
    }
    w.boolean(s->is_file);
    return true;
}

bool load_url(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<UrlState *>(state);
    bool has = false;
    std::string text;
    if (!r.boolean(&has) || (has && !r.str(&text)) || !r.boolean(&s->is_file)) {
        return false;
    }
    s->text = has ? new std::string(std::move(text)) : nullptr;
    return true;
}

void drop_url(void *state) { delete static_cast<UrlState *>(state)->text; }

bool save_error(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    const auto *s = static_cast<const ErrorState *>(state);
    w.u32(s->domain);
    w.u32(static_cast<u32>(s->code));
    w.u32(s->user_info);
    return true;
}

bool load_error(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    auto *s = static_cast<ErrorState *>(state);
    return r.u32(&s->domain) && r.i32(&s->code) && r.u32(&s->user_info);
}

// Foundation's singletons (NSNull, the main NSBundle, the locale, the file manager), by class.
bool save_module(mt_cpu *cpu, mt_snap::Writer &w, std::string *) {
    const State &st = state(cpu);
    std::vector<std::pair<std::string, u32>> sorted(st.singletons.begin(), st.singletons.end());
    std::sort(sorted.begin(), sorted.end());
    w.u32(static_cast<u32>(sorted.size()));
    for (const auto &e : sorted) {
        w.str(e.first);
        w.u32(e.second);
    }
    return true;
}

bool load_module(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    u32 n = 0;
    if (!r.u32(&n) || !r.count_fits(n, 8)) {
        return false;
    }
    std::unordered_map<std::string, u32> singletons;
    for (u32 i = 0; i < n; i++) {
        std::string name;
        u32 obj = 0;
        if (!r.str(&name) || !r.u32(&obj)) {
            return false;
        }
        singletons[name] = obj;
    }
    *apply = [singletons = std::move(singletons)](mt_cpu *cpu) {
        state(cpu).singletons = singletons;
    };
    return true;
}

} // namespace

extern const mt_snap::StateOps k_snap_nsurl;
const mt_snap::StateOps k_snap_nsurl = {&k_url_state, "NSURL", save_url, load_url, drop_url,
                                        nullptr};
extern const mt_snap::StateOps k_snap_error;
const mt_snap::StateOps k_snap_error = {&k_error_state, "NSError", save_error, load_error, nullptr,
                                        nullptr};
extern const mt_snap::Module k_snap_ns;
const mt_snap::Module k_snap_ns = {"ns", save_module, load_module};

} // namespace mt_ns
