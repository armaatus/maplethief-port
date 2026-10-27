// The Objective-C runtime over guest metadata (ADR 0026), on synthetic metadata built here:
// class realization, host classes, categories, selector canonicalization, lookup and caches,
// host overrides, and the objc_msgSend family entering recompiled (here: test) functions.
// tests/test_objc_runtime.py runs the same code on a synthetic Mach-O image, and
// tests/test_objc_runtime_real.py on the game's own metadata.
#include "maplethief/objc/objc.h"

#include <doctest/doctest.h>

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "objc_test.h"

#if defined(_MSC_VER)
// setjmp in C++ (C4611): no object with a destructor lives across it here (see trapped()).
#pragma warning(disable : 4611)
#endif

namespace {

using u32 = std::uint32_t;

// --- test doubles of the runtime's trap and dispatch -----------------------------------------

std::jmp_buf *g_trap_jmp = nullptr;
using objc_test::g_trap;
std::map<u32, mt_guest_fn> g_extra_imps; // objc_test::add_imp

// Guest IMPs of the synthetic image, and what the test function behind each saw.
constexpr u32 k_imp_init = 0x1001;
constexpr u32 k_imp_size = 0x1011;
constexpr u32 k_imp_draw = 0x1021;
constexpr u32 k_imp_widget = 0x1031;
constexpr u32 k_imp_gadget_size = 0x1041;
constexpr u32 k_imp_extra = 0x1051;
constexpr u32 k_imp_make = 0x1061;
constexpr u32 k_imp_cat_init = 0x1071;

struct Seen {
    u32 imp, r0, r1, r2, r3, lr;
};
std::vector<Seen> g_seen;
u32 g_dispatches = 0;

template <u32 Imp> void guest_method(mt_cpu *MT_RESTRICT cpu) {
    g_seen.push_back({Imp, cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r[MT_LR]});
    cpu->r[0] = Imp + 0x100u; // a recognizable result
}

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

// Host methods.
u32 g_host_calls = 0;
void host_release(mt_cpu *MT_RESTRICT cpu) {
    g_host_calls++;
    cpu->r[0] = 0x5E1Fu;
}
void host_alloc(mt_cpu *MT_RESTRICT cpu) {
    g_host_calls++;
    cpu->r[0] = 0xA110Cu;
}
void host_init_with_frame(mt_cpu *MT_RESTRICT cpu) {
    g_host_calls++;
    // Arguments of -initWithFrame:(CGRect): self, _cmd, then the rect's 4 words r2, r3, [sp],
    // [sp+4]; return self.
    (void)cpu;
}
// -frame returns a CGRect through objc_msgSend_stret: r0 = result, r1 = self, r2 = _cmd.
void host_frame(mt_cpu *MT_RESTRICT cpu) {
    g_host_calls++;
    const u32 out = cpu->r[0];
    const u32 rect[4] = {1u, 2u, 3u, 4u};
    std::memcpy(cpu->mem + out, rect, sizeof rect);
}
void override_size(mt_cpu *MT_RESTRICT cpu) { cpu->r[0] = 0x0BE7u; }
void override_shared(mt_cpu *MT_RESTRICT cpu) { cpu->r[0] = 0; } // like +[Chartboost shared…]

// A 1 MiB guest: "text" (strings) from 0x1000, "data" (metadata) from 0x10000, objects from
// 0x80000, the bump allocator's heap from 0xC0000, the stack below 0x100000.
struct Guest {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x100000);
    mt_cpu cpu{};
    u32 text = 0x2000, data = 0x10000, heap = 0xC0000;
    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = mem.size();
        cpu.r[MT_SP] = 0xFFF00u;
        cpu.r[MT_LR] = 0x2345u;
    }
    void put32(u32 addr, u32 v) { std::memcpy(&mem[addr], &v, 4); }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, &mem[addr], 4);
        return v;
    }
    u32 str(const char *s) {
        const u32 at = text;
        std::memcpy(&mem[at], s, std::strlen(s) + 1);
        text += static_cast<u32>(std::strlen(s)) + 1;
        return at;
    }
    u32 words(std::initializer_list<u32> ws) {
        const u32 at = data;
        for (u32 w : ws) {
            put32(data, w);
            data += 4;
        }
        return at;
    }
    // method_list_t with flag bits in entsize, as the compiler emits them.
    u32 methods(std::initializer_list<std::pair<const char *, u32>> ms) {
        const u32 at = words({12u | 3u, static_cast<u32>(ms.size())});
        for (const auto &m : ms) {
            words({str(m.first), str("v8@0:4"), m.second});
        }
        return at;
    }
    u32 ro(u32 flags, u32 start, u32 size, const char *name, u32 methods) {
        return words({flags, start, size, 0u, str(name), methods, 0u, 0u, 0u, 0u});
    }
    // class_t pair; the superclass words are patched in later (they are binds in a real image).
    std::pair<u32, u32> klass(const char *name, u32 start, u32 size, u32 inst, u32 cls) {
        const u32 meta = words({0u, 0u, 0u, 0u, ro(1u, 20u, 20u, name, cls)});
        const u32 c = words({meta, 0u, 0u, 0u, ro(0u, start, size, name, inst) | 1u});
        return {c, meta};
    }
};

u32 bump(void *user, u32 size, u32 align) {
    auto *g = static_cast<Guest *>(user);
    const u32 at = (g->heap + align - 1u) & ~(align - 1u);
    g->heap = at + size;
    return at;
}

const mt_objc_host_method k_nsobject_inst[] = {{"release", host_release}};
const mt_objc_host_method k_nsobject_cls[] = {{"alloc", host_alloc}};
const mt_objc_host_method k_uiview_inst[] = {{"initWithFrame:", host_init_with_frame},
                                             {"frame", host_frame}};
const mt_objc_host_class k_nsobject = {"NSObject", nullptr,        4, k_nsobject_inst,
                                       1,          k_nsobject_cls, 1};
const mt_objc_host_class k_uiview = {"UIView", "NSObject", 46, k_uiview_inst, 2, nullptr, 0};

// The synthetic image:
//   Widget : UIView      -init, -size, -drawRect:, +widget; ivars from 48
//   Gadget : Widget      -size
//   UIView (Extras)      -extra, +make
//   Widget (Fix)         -init (replaces Widget's)
// and three selrefs: "size", "extra" and another copy of "size". The method lists have their
// own copies of the strings, as two images would.
struct Image {
    Guest g;
    mt_objc *rt = nullptr;
    u32 nsobject = 0, nsobject_meta = 0, uiview = 0, uiview_meta = 0;
    u32 widget = 0, widget_meta = 0, gadget = 0, gadget_meta = 0;
    u32 selref_size = 0, selref_extra = 0, selref_size2 = 0, size_copy = 0, size_copy2 = 0;
    mt_objc_image img{};

    explicit Image(u32 policy = MT_UNIMPLEMENTED_STOP, u32 widget_start = 48) {
        g_log.clear();
        g_seen.clear();
        g_host_calls = 0;
        g_dispatches = 0;
        // The imported class objects, as mt_imports_place would place them (zeroed).
        nsobject = 0x80000, nsobject_meta = 0x80020, uiview = 0x80040, uiview_meta = 0x80060;
        auto [w, wm] = g.klass(
            "Widget", widget_start, widget_start + 8,
            g.methods({{"init", k_imp_init}, {"size", k_imp_size}, {"drawRect:", k_imp_draw}}),
            g.methods({{"widget", k_imp_widget}}));
        widget = w, widget_meta = wm;
        auto [gd, gm] = g.klass("Gadget", widget_start + 8, widget_start + 8,
                                g.methods({{"size", k_imp_gadget_size}}), 0u);
        gadget = gd, gadget_meta = gm;
        // Binds and image pointers: superclass words, metaclass isa/superclass words.
        g.put32(widget + 4, uiview);
        g.put32(widget_meta, nsobject_meta);
        g.put32(widget_meta + 4, uiview_meta);
        g.put32(gadget + 4, widget);
        g.put32(gadget_meta, nsobject_meta);
        g.put32(gadget_meta + 4, widget_meta);
        // Categories: { name, cls, instanceMethods, classMethods, protocols, properties }.
        const u32 extras = g.words({g.str("Extras"), uiview, g.methods({{"extra", k_imp_extra}}),
                                    g.methods({{"make", k_imp_make}}), 0u, 0u});
        const u32 fix =
            g.words({g.str("Fix"), widget, g.methods({{"init", k_imp_cat_init}}), 0u, 0u, 0u});
        size_copy = g.str("size");
        size_copy2 = g.str("size");
        selref_size = g.words({size_copy});
        selref_extra = g.words({g.str("extra")});
        selref_size2 = g.words({size_copy2});
        img.classlist = g.words({gadget, widget}); // subclass first: superclasses realize first
        img.classlist_count = 2;
        img.catlist = g.words({extras, fix});
        img.catlist_count = 2;
        img.selrefs = selref_size;
        img.selrefs_count = 3;

        mt_import_config cfg = mt_import_config_default();
        cfg.unimplemented = policy;
        cfg.log = capture;
        rt = mt_objc_create(&g.cpu, bump, &g, &cfg);
        REQUIRE(rt != nullptr);
        REQUIRE(mt_objc_add_host_class(rt, &k_nsobject, nsobject, nsobject_meta) == 0);
        REQUIRE(mt_objc_add_host_class(rt, &k_uiview, uiview, uiview_meta) == 0);
        mt_objc_set_current(rt);
    }
    ~Image() { mt_objc_destroy(rt); }
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    u32 load() { return mt_objc_load_image(rt, &img); }
    u32 sel(const char *name) { return mt_objc_sel(rt, name); }
    mt_objc_method lookup(u32 cls, const char *name) {
        mt_objc_method m{};
        CHECK(mt_objc_lookup(rt, cls, sel(name), &m) == 1);
        return m;
    }
    // A zeroed instance of `cls`.
    u32 instance(u32 cls) {
        const u32 obj = bump(&g, 64, 16);
        g.put32(obj, cls);
        return obj;
    }
};

// Runs fn(); returns true if it stopped through mt_trap. No C++ object lives across the setjmp.
template <typename F> bool trapped(F fn) {
    std::jmp_buf jmp;
    g_trap_jmp = &jmp;
    if (setjmp(jmp) != 0) {
        g_trap_jmp = nullptr;
        return true;
    }
    fn();
    g_trap_jmp = nullptr;
    return false;
}

} // namespace

namespace objc_test {

u32 g_trap[3];

void add_imp(u32 imp, mt_guest_fn fn) { g_extra_imps[imp] = fn; }

bool stops(void (*fn)(void *), void *arg) {
    return trapped([&] { fn(arg); });
}

} // namespace objc_test

extern "C" void mt_trap(mt_cpu *, u32 kind, u32 pc, u32 detail) noexcept {
    g_trap[0] = kind;
    g_trap[1] = pc;
    g_trap[2] = detail;
    if (g_trap_jmp != nullptr) {
        std::longjmp(*g_trap_jmp, 1);
    }
    std::abort();
}

extern "C" mt_guest_fn mt_dispatch(mt_cpu *, u32 target, u32) noexcept {
    g_dispatches++;
    switch (target) {
    case k_imp_init:
        return guest_method<k_imp_init>;
    case k_imp_size:
        return guest_method<k_imp_size>;
    case k_imp_draw:
        return guest_method<k_imp_draw>;
    case k_imp_widget:
        return guest_method<k_imp_widget>;
    case k_imp_gadget_size:
        return guest_method<k_imp_gadget_size>;
    case k_imp_extra:
        return guest_method<k_imp_extra>;
    case k_imp_make:
        return guest_method<k_imp_make>;
    case k_imp_cat_init:
        return guest_method<k_imp_cat_init>;
    default: {
        const auto it = g_extra_imps.find(target);
        if (it == g_extra_imps.end()) {
            std::abort();
        }
        return it->second;
    }
    }
}

TEST_CASE("loading realizes classes and metaclasses, attaches categories, canonicalizes SELs") {
    Image t;
    CHECK(t.load() == 0u);
    const mt_objc_stats s = mt_objc_get_stats(t.rt);
    CHECK(s.image_classes == 2u);
    CHECK(s.classes == 4u); // NSObject, UIView, Widget, Gadget
    CHECK(s.categories == 2u);
    CHECK(s.problems == 0u);
    CHECK(s.instance_start_problems == 0u);
    // The first string seen for a name is its SEL; a selref to another copy is rewritten.
    CHECK(s.selrefs_changed == 1u);
    CHECK(t.sel("size") == t.size_copy);
    CHECK(t.g.get32(t.selref_size) == t.size_copy);
    CHECK(t.g.get32(t.selref_size2) == t.size_copy);
    CHECK(std::strcmp(mt_objc_sel_name(t.rt, t.size_copy2), "size") == 0);

    CHECK(mt_objc_class_named(t.rt, "Gadget") == t.gadget);
    CHECK(mt_objc_metaclass(t.rt, t.gadget) == t.gadget_meta);
    CHECK(mt_objc_superclass(t.rt, t.gadget) == t.widget);
    CHECK(mt_objc_superclass(t.rt, t.widget) == t.uiview);
    CHECK(mt_objc_superclass(t.rt, t.gadget_meta) == t.widget_meta);
    CHECK(mt_objc_superclass(t.rt, t.uiview_meta) == t.nsobject_meta);
    // A root metaclass inherits from its root class.
    CHECK(mt_objc_superclass(t.rt, t.nsobject_meta) == t.nsobject);
    CHECK(mt_objc_instance_size(t.rt, t.widget) == 56u);
    CHECK(std::strcmp(mt_objc_class_name(t.rt, t.widget_meta), "Widget") == 0);
    // Host class objects get isa and superclass words like libobjc's.
    CHECK(t.g.get32(t.uiview) == t.uiview_meta);
    CHECK(t.g.get32(t.uiview + 4) == t.nsobject);
    CHECK(t.g.get32(t.uiview_meta) == t.nsobject_meta);
    CHECK(t.g.get32(t.nsobject_meta) == t.nsobject_meta);
    CHECK(t.g.get32(t.nsobject_meta + 4) == t.nsobject);
}

TEST_CASE("lookup: own and inherited methods, categories, host classes, metaclasses") {
    Image t;
    REQUIRE(t.load() == 0u);
    mt_objc_method m = t.lookup(t.widget, "size");
    CHECK(m.guest_imp == k_imp_size);
    CHECK(m.origin == MT_OBJC_IMAGE);
    CHECK(m.owner == t.widget);
    CHECK(t.lookup(t.gadget, "size").guest_imp == k_imp_gadget_size);
    // The category on Widget replaces its -init; Gadget inherits the replacement.
    m = t.lookup(t.gadget, "init");
    CHECK(m.guest_imp == k_imp_cat_init);
    CHECK(m.origin == MT_OBJC_CATEGORY);
    CHECK(m.owner == t.widget);
    // A category on a host class reaches image subclasses.
    CHECK(t.lookup(t.gadget, "extra").guest_imp == k_imp_extra);
    CHECK(t.lookup(t.gadget_meta, "make").guest_imp == k_imp_make);
    // Host methods: inherited from UIView and NSObject.
    m = t.lookup(t.gadget, "initWithFrame:");
    CHECK(m.origin == MT_OBJC_HOST);
    CHECK(m.fn == host_init_with_frame);
    CHECK(m.owner == t.uiview);
    CHECK(t.lookup(t.gadget, "release").fn == host_release);
    // Class methods: the metaclass chain, ending in NSObject's instance methods.
    CHECK(t.lookup(t.gadget_meta, "widget").guest_imp == k_imp_widget);
    CHECK(t.lookup(t.gadget_meta, "alloc").fn == host_alloc);
    CHECK(t.lookup(t.gadget_meta, "release").fn == host_release);
    // Misses: an instance method is no class method, and the reverse.
    mt_objc_method none{};
    CHECK(mt_objc_lookup(t.rt, t.gadget_meta, t.sel("size"), &none) == 0);
    CHECK(mt_objc_lookup(t.rt, t.gadget, t.sel("alloc"), &none) == 0);
    CHECK(mt_objc_lookup(t.rt, 0x1234u, t.sel("size"), &none) == 0);
    // A non-canonical SEL (another copy of the string) finds the same method.
    CHECK(mt_objc_lookup(t.rt, t.gadget, t.size_copy2, &none) == 1);
    CHECK(none.guest_imp == k_imp_gadget_size);
}

TEST_CASE("objc_msgSend enters the method with the sender's registers") {
    Image t;
    REQUIRE(t.load() == 0u);
    const u32 obj = t.instance(t.gadget);
    t.g.cpu.r[0] = obj;
    t.g.cpu.r[1] = t.g.get32(t.selref_size);
    t.g.cpu.r[2] = 0x22u;
    t.g.cpu.r[3] = 0x33u;
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    REQUIRE(g_seen.size() == 1u);
    CHECK(g_seen[0].imp == k_imp_gadget_size);
    CHECK(g_seen[0].r0 == obj);
    CHECK(g_seen[0].r1 == t.sel("size"));
    CHECK(g_seen[0].r2 == 0x22u);
    CHECK(g_seen[0].r3 == 0x33u);
    CHECK(g_seen[0].lr == 0x2345u);
    CHECK(t.g.cpu.r[0] == k_imp_gadget_size + 0x100u);
    // The dispatched host function is cached: a second send does not dispatch again.
    t.g.cpu.r[0] = obj;
    t.g.cpu.r[1] = t.sel("size");
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    CHECK(g_seen.size() == 2u);
    CHECK(g_dispatches == 1u);

    // A class method: the receiver is the class object.
    t.g.cpu.r[0] = t.gadget;
    t.g.cpu.r[1] = t.sel("widget");
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    CHECK(g_seen.back().imp == k_imp_widget);
    CHECK(g_seen.back().r0 == t.gadget);
    t.g.cpu.r[0] = t.gadget;
    t.g.cpu.r[1] = t.sel("alloc");
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    CHECK(t.g.cpu.r[0] == 0xA110Cu);

    // nil: r0 = r1 = 0, nothing runs.
    const auto before = g_seen.size();
    t.g.cpu.r[0] = 0;
    t.g.cpu.r[1] = t.sel("size");
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    CHECK(t.g.cpu.r[0] == 0u);
    CHECK(t.g.cpu.r[1] == 0u);
    CHECK(g_seen.size() == before);
}

TEST_CASE("objc_msgSend_stret and objc_msgSendSuper2") {
    Image t;
    REQUIRE(t.load() == 0u);
    const u32 obj = t.instance(t.gadget);
    const u32 out = bump(&t.g, 16, 4);
    t.g.cpu.r[0] = out;
    t.g.cpu.r[1] = obj;
    t.g.cpu.r[2] = t.sel("frame");
    mt_hle_objc_msgSend_stret(&t.g.cpu, 0u);
    CHECK(g_host_calls == 1u);
    CHECK(t.g.get32(out) == 1u);
    CHECK(t.g.get32(out + 12) == 4u);
    // nil: the result memory is not written.
    t.g.put32(out, 0xEEEEu);
    t.g.cpu.r[0] = out;
    t.g.cpu.r[1] = 0u;
    t.g.cpu.r[2] = t.sel("frame");
    mt_hle_objc_msgSend_stret(&t.g.cpu, 0u);
    CHECK(t.g.get32(out) == 0xEEEEu);
    CHECK(g_host_calls == 1u);

    // [super size] in Gadget: struct objc_super { self, Gadget } starts at Widget.
    const u32 sup = bump(&t.g, 8, 4);
    t.g.put32(sup, obj);
    t.g.put32(sup + 4, t.gadget);
    t.g.cpu.r[0] = sup;
    t.g.cpu.r[1] = t.sel("size");
    mt_hle_objc_msgSendSuper2(&t.g.cpu, 0u);
    REQUIRE(!g_seen.empty());
    CHECK(g_seen.back().imp == k_imp_size);
    CHECK(g_seen.back().r0 == obj);
    // [super initWithFrame:] in Widget reaches the host UIView.
    t.g.put32(sup + 4, t.widget);
    t.g.cpu.r[0] = sup;
    t.g.cpu.r[1] = t.sel("initWithFrame:");
    mt_hle_objc_msgSendSuper2(&t.g.cpu, 0u);
    CHECK(g_host_calls == 2u);
    // A class method through super: the current class is the metaclass.
    t.g.put32(sup, t.gadget);
    t.g.put32(sup + 4, t.gadget_meta);
    t.g.cpu.r[0] = sup;
    t.g.cpu.r[1] = t.sel("widget");
    mt_hle_objc_msgSendSuper2(&t.g.cpu, 0u);
    CHECK(g_seen.back().imp == k_imp_widget);
    CHECK(g_seen.back().r0 == t.gadget);
}

TEST_CASE("host overrides replace image methods and reach subclasses") {
    Image t;
    REQUIRE(t.load() == 0u);
    // Warm the cache first: an override must invalidate it.
    CHECK(t.lookup(t.gadget, "init").guest_imp == k_imp_cat_init);
    REQUIRE(mt_objc_override(t.rt, "Widget", 0, "init", override_size) == 0);
    mt_objc_method m = t.lookup(t.gadget, "init");
    CHECK(m.origin == MT_OBJC_OVERRIDE);
    CHECK(m.fn == override_size);
    CHECK(m.guest_imp == 0u);
    // Gadget's own -size is untouched by an override of Widget's.
    REQUIRE(mt_objc_override(t.rt, "Widget", 0, "size", override_size) == 0);
    CHECK(t.lookup(t.gadget, "size").guest_imp == k_imp_gadget_size);
    // A class method, as for +[Chartboost sharedChartboost].
    REQUIRE(mt_objc_override(t.rt, "Gadget", 1, "sharedGadget", override_shared) == 0);
    t.g.cpu.r[0] = t.gadget;
    t.g.cpu.r[1] = t.sel("sharedGadget");
    mt_hle_objc_msgSend(&t.g.cpu, 0u);
    CHECK(t.g.cpu.r[0] == 0u);
    CHECK(mt_objc_override(t.rt, "Nope", 0, "init", override_size) == -1);
    CHECK(mt_objc_get_stats(t.rt).overrides == 3u);
}

TEST_CASE("an unrecognized selector stops the guest or returns 0, per the policy") {
    {
        Image t(MT_UNIMPLEMENTED_STOP);
        REQUIRE(t.load() == 0u);
        const u32 obj = t.instance(t.gadget);
        const u32 nope = t.sel("nope:");
        t.g.cpu.r[0] = obj;
        t.g.cpu.r[1] = nope;
        CHECK(trapped([&] { mt_hle_objc_msgSend(&t.g.cpu, 0u); }));
        CHECK(g_trap[0] == MT_TRAP_OBJC);
        CHECK(g_trap[1] == 0x2345u);
        CHECK(g_trap[2] == nope);
        REQUIRE(g_log.size() == 1u);
        CHECK(g_log[0].find("unrecognized selector -[Gadget nope:]") != std::string::npos);
        CHECK(g_log[0].find("called from 0x00002345") != std::string::npos);
        // An object whose isa is no class.
        const u32 bogus = t.instance(0x4444u);
        t.g.cpu.r[0] = bogus;
        t.g.cpu.r[1] = t.sel("size");
        CHECK(trapped([&] { mt_hle_objc_msgSend(&t.g.cpu, 0u); }));
        CHECK(g_log.back().find("no object of a known class") != std::string::npos);
    }
    {
        Image t(MT_UNIMPLEMENTED_CONTINUE);
        REQUIRE(t.load() == 0u);
        for (int i = 0; i < 2; i++) {
            t.g.cpu.r[0] = t.gadget;
            t.g.cpu.r[1] = t.sel("nope");
            CHECK(!trapped([&] { mt_hle_objc_msgSend(&t.g.cpu, 0u); }));
            CHECK(t.g.cpu.r[0] == 0u);
            CHECK(t.g.cpu.r[1] == 0u);
        }
        REQUIRE(g_log.size() == 1u); // logged once under "continue"
        CHECK(g_log[0].find("+[Gadget nope]") != std::string::npos);
    }
}

TEST_CASE("problems: unknown superclass, category on an unknown class, ivars inside the host") {
    {
        // Widget's ivars start at 40, inside UIView's 46 bytes: libobjc would slide them.
        Image t(MT_UNIMPLEMENTED_STOP, 40);
        CHECK(t.load() == 1u);
        CHECK(mt_objc_get_stats(t.rt).instance_start_problems == 1u);
        CHECK(g_log.back().find("Widget starts its ivars at 40") != std::string::npos);
    }
    {
        Image t;
        t.g.put32(t.widget + 4, 0x7777u); // a superclass no one provides
        const u32 cat = t.g.get32(t.img.catlist);
        t.g.put32(cat + 4, 0x8888u);
        // Widget fails, so Gadget (its subclass) fails too, and both categories miss a class.
        // Each failure is logged once.
        CHECK(t.load() == 4u);
        CHECK(mt_objc_get_stats(t.rt).image_classes == 0u);
        REQUIRE(g_log.size() == 4u);
        CHECK(g_log[0].find("superclass 0x00007777 of Widget is neither") != std::string::npos);
        CHECK(g_log[1].find("of Gadget could not be realized") != std::string::npos);
        CHECK(g_log[2].find("Extras is on unknown class 0x00008888") != std::string::npos);
        CHECK(g_log[3].find("Fix is on unrealized class") != std::string::npos);
    }
}

TEST_CASE("selectors registered by host code live in guest memory") {
    Image t;
    REQUIRE(t.load() == 0u);
    const u32 a = mt_objc_sel(t.rt, "applicationDidBecomeActive:");
    CHECK(a >= 0xC0000u); // placed by the allocator
    CHECK(std::strcmp(reinterpret_cast<const char *>(&t.g.mem[a]), "applicationDidBecomeActive:") ==
          0);
    CHECK(mt_objc_sel(t.rt, "applicationDidBecomeActive:") == a);
    // A name the image already has gets the image's string.
    CHECK(mt_objc_sel(t.rt, "extra") == t.g.get32(t.selref_extra));
    CHECK(mt_objc_sel_name(t.rt, 0xFFFFFFF0u) == nullptr); // outside guest memory
}
