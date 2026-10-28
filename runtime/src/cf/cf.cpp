// CoreFoundation's core (#48, ADR 0032): configuration, binding to the ObjC runtime, object
// lifetime (immortal constants, CFRelease), numbers and booleans, and the CF data imports.
//
// The data imports (kCFBooleanTrue, kCFRunLoopCommonModes, ...) are placed before the ObjC
// runtime has its host classes, so their init hooks only record where they are; mt_cf_start
// (in the boot's "objc-image" step, after the host classes) creates their objects and stores
// the pointers.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "maplethief/memory.h"
#include "objc/objc_internal.h"

namespace mt_cf {

namespace {

Config g_config = {"", MT_CF_DEFAULT_BUNDLE_PATH, "en", 0};

// What a CF data import points at.
enum class Constant : u32 {
    null,       // kCFAllocatorDefault: NULL, which CF reads as the default allocator
    bool_true,  // kCFBooleanTrue
    bool_false, // kCFBooleanFalse
    nan,        // kCFNumberNaN
    neg_inf,    // kCFNumberNegativeInfinity
    pos_inf,    // kCFNumberPositiveInfinity
    string,     // a CFString constant with `text`
};

struct Pending {
    const std::uint8_t *mem; // the guest memory the import was placed in (cpu->mem)
    u32 addr;
    Constant kind;
    const char *text;
};

struct State {
    mt_objc *rt = nullptr;
    Classes cls{};
    std::unordered_set<u32> immortal;
    u32 bool_true = 0, bool_false = 0;
};

State g_state;
std::vector<Pending> g_pending;
u32 g_binding = 0;

const mt_objc_state_type k_number_state = {"CFNumber", sizeof(Number), nullptr};

void bind(mt_objc *rt) {
    if (g_state.rt == rt) {
        return;
    }
    g_state = State{};
    g_binding++;
    runloop_forget();
    bundle_forget();
    g_state.rt = rt;
    if (rt != nullptr) {
        g_state.cls.string = mt_objc_class_named(rt, "__NSCFString");
        g_state.cls.constant_string = mt_objc_class_named(rt, "__NSCFConstantString");
        g_state.cls.data = mt_objc_class_named(rt, "__NSCFData");
        g_state.cls.number = mt_objc_class_named(rt, "__NSCFNumber");
        g_state.cls.boolean = mt_objc_class_named(rt, "__NSCFBoolean");
        g_state.cls.type = mt_objc_class_named(rt, "__NSCFType");
    }
}

void put32(mt_cpu *cpu, u32 addr, u32 value) { std::memcpy(mt_g2h(cpu, addr, 4), &value, 4); }

u32 make_constant(mt_cpu *cpu, const Pending &p) {
    Number n{6u, true, 0, 0.0}; // kCFNumberFloat64Type
    switch (p.kind) {
    case Constant::null:
        return 0;
    case Constant::bool_true:
        return boolean(cpu, true);
    case Constant::bool_false:
        return boolean(cpu, false);
    case Constant::nan:
        n.f = std::numeric_limits<double>::quiet_NaN();
        break;
    case Constant::neg_inf:
        n.f = -std::numeric_limits<double>::infinity();
        break;
    case Constant::pos_inf:
        n.f = std::numeric_limits<double>::infinity();
        break;
    case Constant::string:
        return string_create_constant(cpu, p.text);
    }
    const u32 obj = number_create(cpu, n);
    make_immortal(cpu, obj);
    return obj;
}

void note(mt_cpu *cpu, u32 addr, Constant kind, const char *text = nullptr) {
    g_pending.push_back(Pending{cpu->mem, addr, kind, text});
}

} // namespace

const Config &config() noexcept {
    if (g_config.generation == 0) {
        // Never configured: the environment's settings (a developer run without a platform).
        mt_cf_config cfg = mt_cf_config_default();
        mt_cf_config_from_env(&cfg);
        mt_cf_configure(&cfg);
    }
    return g_config;
}

mt_objc *runtime(mt_cpu *) {
    if (g_state.rt == nullptr || g_state.rt != mt_objc_current()) {
        if (mt_objc_current() != nullptr) {
            bind(mt_objc_current());
        }
    }
    return g_state.rt;
}

u32 binding() noexcept { return g_binding; }

const Classes &classes(mt_cpu *cpu) {
    runtime(cpu);
    return g_state.cls;
}

void log(mt_cpu *cpu, const char *fmt, ...) noexcept {
    char line[512] = "cf: ";
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + 4, sizeof line - 4, fmt, ap);
    va_end(ap);
    mt_objc *rt = runtime(cpu);
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void misuse(mt_cpu *cpu, const char *function, const char *what, u32 obj) noexcept {
    log(cpu, "%s: %s (0x%08X), called from 0x%08X: stopping, as the device would crash", function,
        what, static_cast<unsigned>(obj), static_cast<unsigned>(cpu->r[MT_LR]));
    mt_trap(cpu, MT_TRAP_ABORT, cpu->r[MT_LR], obj);
}

mt_mem *heap(mt_cpu *cpu) noexcept { return mt_mem_from_cpu(cpu); }

void make_immortal(mt_cpu *cpu, u32 obj) {
    runtime(cpu);
    if (obj != 0) {
        g_state.immortal.insert(obj);
    }
}

bool is_immortal(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return false;
    }
    const u32 isa = mt_objc_class_of(rt, obj);
    return (isa != 0 && (isa == g_state.cls.constant_string || isa == g_state.cls.boolean)) ||
           g_state.immortal.count(obj) != 0;
}

u32 create(mt_cpu *cpu, u32 cls) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && cls != 0 ? mt_objc_create_instance(rt, cls) : 0;
}

u32 number_create(mt_cpu *cpu, const Number &value) {
    const u32 obj = create(cpu, classes(cpu).number);
    auto *s = obj != 0 ? static_cast<Number *>(mt_objc_state(runtime(cpu), obj, &k_number_state))
                       : nullptr;
    if (s == nullptr) {
        return 0;
    }
    *s = value;
    return obj;
}

bool number_value(mt_cpu *cpu, u32 obj, Number *out) {
    mt_objc *rt = runtime(cpu);
    const auto *s = rt != nullptr && obj != 0
                        ? static_cast<const Number *>(mt_objc_find_state(rt, obj, &k_number_state))
                        : nullptr;
    if (s == nullptr) {
        return false;
    }
    *out = *s;
    return true;
}

u32 make_constants(mt_cpu *cpu) {
    u32 filled = 0;
    const std::vector<Pending> pending = std::move(g_pending);
    g_pending.clear();
    for (const Pending &p : pending) {
        if (p.mem != cpu->mem) {
            continue; // placed in another guest address space (an earlier process)
        }
        const u32 obj = make_constant(cpu, p);
        if (obj == 0 && p.kind != Constant::null) {
            log(cpu, "no guest memory for the constant at 0x%08X", static_cast<unsigned>(p.addr));
            continue;
        }
        put32(cpu, p.addr, obj);
        filled++;
    }
    return filled;
}

u32 boolean(mt_cpu *cpu, bool value) {
    runtime(cpu);
    u32 &slot = value ? g_state.bool_true : g_state.bool_false;
    if (slot == 0) {
        slot = create(cpu, g_state.cls.boolean);
        auto *s = slot != 0
                      ? static_cast<Number *>(mt_objc_state(g_state.rt, slot, &k_number_state))
                      : nullptr;
        if (s != nullptr) {
            *s = Number{7u, false, value ? 1 : 0, value ? 1.0 : 0.0}; // kCFNumberCharType
        }
    }
    return slot;
}

} // namespace mt_cf

using namespace mt_cf;

// --- configuration ------------------------------------------------------------------------------

extern "C" mt_cf_config mt_cf_config_default(void) noexcept {
    mt_cf_config cfg;
    cfg.bundle_dir = nullptr;
    cfg.bundle_path = nullptr;
    std::memset(cfg.language, 0, sizeof cfg.language);
    std::memcpy(cfg.language, "en", 3);
    return cfg;
}

extern "C" int mt_cf_config_from_env(mt_cf_config *cfg) noexcept {
    const char *dir = std::getenv(MT_CF_BUNDLE_DIR_ENV);
    if (dir != nullptr && dir[0] != '\0') {
        cfg->bundle_dir = dir;
    }
    const char *lang = std::getenv(MT_CF_LANGUAGE_ENV);
    if (lang == nullptr || lang[0] == '\0') {
        return 0;
    }
    if (std::strcmp(lang, "en") != 0 && std::strcmp(lang, "ko") != 0) {
        std::fprintf(stderr, "cf: %s=%s: not a language the bundle has (en, ko); ignored\n",
                     MT_CF_LANGUAGE_ENV, lang);
        return -1;
    }
    std::memset(cfg->language, 0, sizeof cfg->language);
    std::memcpy(cfg->language, lang, std::strlen(lang));
    return 0;
}

extern "C" void mt_cf_configure(const mt_cf_config *cfg) noexcept {
    const mt_cf_config d = mt_cf_config_default();
    const mt_cf_config &c = cfg != nullptr ? *cfg : d;
    const u32 generation = g_config.generation + 1;
    g_config.bundle_dir = c.bundle_dir != nullptr ? c.bundle_dir : "";
    g_config.bundle_path = c.bundle_path != nullptr ? c.bundle_path : MT_CF_DEFAULT_BUNDLE_PATH;
    const void *nul = std::memchr(c.language, 0, sizeof c.language);
    g_config.language.assign(c.language, nul != nullptr
                                             ? static_cast<const char *>(nul) - c.language
                                             : sizeof c.language);
    if (g_config.language.empty()) {
        g_config.language = "en";
    }
    g_config.generation = generation;
}

extern "C" const char *mt_cf_language(void) noexcept { return config().language.c_str(); }

// --- start-up -----------------------------------------------------------------------------------

extern "C" std::uint32_t mt_cf_start(mt_cpu *cpu, mt_objc *rt) noexcept {
    g_state.rt = nullptr; // a new process: forget the previous runtime's objects
    bind(rt);
    return make_constants(cpu);
}

// --- HLE: lifetime ------------------------------------------------------------------------------

extern "C" void mt_hle_CFRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 obj = cpu->r[0];
    if (obj == 0) {
        misuse(cpu, "CFRelease", "NULL", 0);
    }
    mt_objc_release(cpu, obj);
}

// --- HLE: the data imports ----------------------------------------------------------------------

extern "C" void mt_hle_init_kCFAllocatorDefault(mt_cpu *cpu, std::uint32_t,
                                                std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::null);
}
extern "C" void mt_hle_init_kCFBooleanTrue(mt_cpu *cpu, std::uint32_t,
                                           std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::bool_true);
}
extern "C" void mt_hle_init_kCFBooleanFalse(mt_cpu *cpu, std::uint32_t,
                                            std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::bool_false);
}
extern "C" void mt_hle_init_kCFNumberNaN(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::nan);
}
extern "C" void mt_hle_init_kCFNumberNegativeInfinity(mt_cpu *cpu, std::uint32_t,
                                                      std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::neg_inf);
}
extern "C" void mt_hle_init_kCFNumberPositiveInfinity(mt_cpu *cpu, std::uint32_t,
                                                      std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::pos_inf);
}
// The string constants carry CF's own values (CFRunLoop.c, CFBundle.c, CFLocale.c).
extern "C" void mt_hle_init_kCFRunLoopCommonModes(mt_cpu *cpu, std::uint32_t,
                                                  std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "kCFRunLoopCommonModes");
}
extern "C" void mt_hle_init_kCFRunLoopDefaultMode(mt_cpu *cpu, std::uint32_t,
                                                  std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "kCFRunLoopDefaultMode");
}
extern "C" void mt_hle_init_kCFBundleIdentifierKey(mt_cpu *cpu, std::uint32_t,
                                                   std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "CFBundleIdentifier");
}
extern "C" void mt_hle_init_kCFBundleVersionKey(mt_cpu *cpu, std::uint32_t,
                                                std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "CFBundleVersion");
}
extern "C" void mt_hle_init_NSLocaleCountryCode(mt_cpu *cpu, std::uint32_t,
                                                std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "kCFLocaleCountryCodeKey");
}
extern "C" void mt_hle_init_NSLocaleLanguageCode(mt_cpu *cpu, std::uint32_t,
                                                 std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "kCFLocaleLanguageCodeKey");
}
extern "C" void mt_hle_init_NSLocalizedDescriptionKey(mt_cpu *cpu, std::uint32_t,
                                                      std::uint32_t addr) noexcept {
    note(cpu, addr, Constant::string, "NSLocalizedDescription");
}
