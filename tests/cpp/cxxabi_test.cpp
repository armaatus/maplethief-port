// The C++ ABI part of the HLE layer (#45, ADR 0031): operator new/delete on the guest heap,
// __cxa_pure_virtual, the type_info vtable init hooks and the host type_info virtuals that
// match catch clauses (__do_catch, __do_upcast) over guest type_info objects laid out as GCC
// 4.2 lays them out. The end-to-end exception through the SjLj personality is
// tests/recomp/sjlj/rtti.sjlj (tests/test_recomp_sjlj.py).
#include "maplethief/hle/cxxabi.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "diag_fixture.h"
#include "guest_trap.h"
#include "hle/hle.h"
#include "maplethief/diag.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hle/libc.h"
#include "maplethief/memory.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_caller = 0x0002A3C5u; // lr of every call: a Thumb return address

// A guest address space with a stack and a cpu; guest output and the HLE log captured.
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    std::string err; // guest stderr
    std::vector<std::string> log;
    mt_libc_config lc{};

    Guest() {
        mt_mem_config cfg = mt_mem_config_default();
        mem = mt_mem_create(&cfg);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0u, &stack) == 0);
        cpu.r[MT_SP] = stack.top - 0x400u;
        lc = mt_libc_config_default();
        lc.write = [](void *user, int fd, const char *data, std::size_t len) {
            if (fd == 2) {
                static_cast<Guest *>(user)->err.append(data, len);
            }
        };
        lc.write_user = this;
        lc.log = [](void *user, const char *line) {
            static_cast<Guest *>(user)->log.emplace_back(line);
        };
        lc.log_user = this;
        mt_libc_configure(&lc);
    }
    ~Guest() {
        mt_libc_configure(nullptr);
        mt_cxx_set_type_info_vtables(0u, 0u, 0u);
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4u);
        return v;
    }
    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4u); }
    u32 words(const std::vector<u32> &w) {
        const u32 a = mt_heap_calloc(mem, 1u, static_cast<u32>(4u * w.size()));
        REQUIRE(a != 0u);
        for (std::size_t i = 0; i < w.size(); i++) {
            put32(a + 4u * static_cast<u32>(i), w[i]);
        }
        return a;
    }
    u32 str(const char *s) {
        const auto n = static_cast<u32>(std::strlen(s)) + 1u;
        const u32 a = mt_heap_malloc(mem, n);
        REQUIRE(a != 0u);
        std::memcpy(cpu.mem + a, s, n);
        return a;
    }
    u32 call(mt_import_fn fn, u32 r0) {
        cpu.r[0] = r0;
        cpu.r[MT_LR] = k_caller;
        fn(&cpu, 0u);
        return cpu.r[0];
    }
    // True if the call stopped the guest through mt_trap (the trap is in mt_test::g_trap).
    bool traps(mt_import_fn fn, u32 r0) {
        cpu.r[0] = r0;
        cpu.r[MT_LR] = k_caller;
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

// The three vtables placed as the boot places them (64-byte data imports, ids 3, 4, 5), and
// type_info objects built in guest memory with GCC 4.2's layout.
struct Rtti {
    Guest &g;
    u32 vt_class, vt_si, vt_vmi;

    explicit Rtti(Guest &guest) : g(guest) {
        vt_class = mt_heap_calloc(g.mem, 1u, 64u);
        vt_si = mt_heap_calloc(g.mem, 1u, 64u);
        vt_vmi = mt_heap_calloc(g.mem, 1u, 64u);
        mt_cxx_init_class_type_info_vtable(&g.cpu, 3u, vt_class);
        mt_cxx_init_si_class_type_info_vtable(&g.cpu, 4u, vt_si);
        mt_cxx_init_vmi_class_type_info_vtable(&g.cpu, 5u, vt_vmi);
    }
    u32 cls(const char *name) { return g.words({vt_class + 8u, g.str(name)}); }
    u32 si(const char *name, u32 base) { return g.words({vt_si + 8u, g.str(name), base}); }
    struct Base {
        u32 type;
        std::int32_t offset; // a virtual base: the offset in the vtable that holds its offset
        bool is_virtual;
        bool is_public;
    };
    u32 vmi(const char *name, u32 flags, const std::vector<Base> &bases) {
        std::vector<u32> w{vt_vmi + 8u, g.str(name), flags, static_cast<u32>(bases.size())};
        for (const Base &b : bases) {
            w.push_back(b.type);
            w.push_back(static_cast<u32>(b.offset) << 8 | (b.is_virtual ? 1u : 0u) |
                        (b.is_public ? 2u : 0u));
        }
        return g.words(w);
    }
};

bool catches(Guest &g, u32 catch_type, u32 thrown, u32 *object = nullptr) {
    return mt_cxx_can_catch(&g.cpu, catch_type, thrown, object) != 0;
}

} // namespace

// --- operator new and delete ----------------------------------------------------------------

TEST_CASE("operator new and new[] allocate on the guest heap, delete and delete[] free") {
    Guest g;
    for (mt_import_fn fn : {mt_hle_Znwm, mt_hle_Znam}) {
        const u32 a = g.call(fn, 24u), b = g.call(fn, 24u);
        CHECK(a >= MT_GUEST_HEAP_BASE);
        CHECK(a < MT_GUEST_HEAP_END);
        CHECK(a % MT_HEAP_ALIGN == 0u);
        CHECK(b != a);
        CHECK(mt_heap_usable_size(g.mem, a) >= 24u);
        // new(0) is a unique block of its own
        const u32 z1 = g.call(fn, 0u), z2 = g.call(fn, 0u);
        CHECK(z1 != 0u);
        CHECK(z2 != 0u);
        CHECK(z1 != z2);
        for (u32 p : {a, b, z1, z2}) {
            g.call(fn == mt_hle_Znwm ? mt_hle_ZdlPv : mt_hle_ZdaPv, p);
            CHECK(mt_heap_usable_size(g.mem, p) == 0u);
        }
    }
    g.call(mt_hle_ZdlPv, 0u); // delete of NULL does nothing
    g.call(mt_hle_ZdaPv, 0u);
    CHECK(g.log.empty());
}

TEST_CASE("operator delete of a pointer that is no live block aborts like free") {
    Guest g;
    const u32 a = g.call(mt_hle_Znwm, 8u);
    g.call(mt_hle_ZdlPv, a);
    CHECK(g.traps(mt_hle_ZdlPv, a)); // a double delete
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == a);
    REQUIRE(g.log.size() == 1u);
    char want[160];
    std::snprintf(want, sizeof want,
                  "libc: malloc: *** error for object 0x%08X: pointer being freed was not "
                  "allocated (operator delete called from 0x0002A3C5)",
                  static_cast<unsigned>(a));
    CHECK(g.log[0] == want);
    CHECK(g.traps(mt_hle_ZdaPv, a + 4u));
    CHECK(mt_test::g_trap.detail == a + 4u);
    CHECK(g.log.back().find("(operator delete[] called from 0x0002A3C5)") != std::string::npos);
}

TEST_CASE("operator new without guest heap stops the guest with the size (std::bad_alloc)") {
    Guest g;
    CHECK(g.traps(mt_hle_Znwm, 0xFFFFFFF0u));
    CHECK(mt_test::g_trap.kind == MT_TRAP_ABORT);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(mt_test::g_trap.detail == 0xFFFFFFF0u);
    REQUIRE(g.log.size() == 1u);
    CHECK(g.log[0] == "libc: operator new(4294967280) called from 0x0002A3C5: the guest heap is "
                      "exhausted (std::bad_alloc): aborting");
    CHECK(g.traps(mt_hle_Znam, 0xFFFFFFF0u));
    CHECK(g.log.back().find("operator new[](4294967280)") == 6u);
}

// --- __cxa_pure_virtual ---------------------------------------------------------------------

TEST_CASE("__cxa_pure_virtual prints libstdc++'s line and stops with a guest backtrace") {
    namespace fx = mt_diag_fixture;
    Guest g;
    fx::build_frames(&g.cpu);
    const u32 lr = g.cpu.r[MT_LR];
    struct Call {
        mt_cpu *cpu;
    } c{&g.cpu};
    CHECK(mt_test::stops(
        [](void *arg) { mt_hle_cxa_pure_virtual(static_cast<Call *>(arg)->cpu, 0u); }, &c));
    CHECK(mt_test::g_trap.kind == MT_TRAP_PURE_VIRT);
    CHECK(mt_test::g_trap.kind == 16u);
    CHECK(mt_test::g_trap.pc == lr);
    CHECK(mt_test::g_trap.detail == 0u);
    CHECK(g.err == "pure virtual method called\n");

    // What the runtime's mt_trap prints for it (mt_crash_report_trap formats the same).
    REQUIRE(mt_symbols_load_text(fx::k_symbols, sizeof fx::k_symbols - 1u, nullptr) == 4);
    mt_crash_info info{};
    info.kind = mt_test::g_trap.kind;
    info.pc = mt_test::g_trap.pc;
    std::string r(mt_crash_format(&g.cpu, &info, nullptr, 0), '\0');
    mt_crash_format(&g.cpu, &info, r.data(), r.size() + 1u);
    mt_symbols_clear();
    INFO(r);
    CHECK(r.find("=== guest crash: pure virtual function called ===\n"
                 "pc 0x00003021 CFoo::bar+0x20; detail 0x00000000\n") == 0u);
    CHECK(r.find("backtrace (r7 frame chain):\n"
                 "  #0 0x00003021 CFoo::bar+0x20\n"
                 "  #1 0x00002043 CGame::Update+0x42\n"
                 "  #2 0x00004011 main_loop+0x10\n") != std::string::npos);
}

TEST_CASE("std::terminate stops the guest with MT_TRAP_TERMINATE") {
    Guest g;
    CHECK(g.traps(mt_sjlj_hle_terminate, 0u));
    CHECK(mt_test::g_trap.kind == MT_TRAP_TERMINATE);
    CHECK(mt_test::g_trap.pc == k_caller);
    CHECK(std::string(mt_trap_kind_name(MT_TRAP_TERMINATE)) == "std::terminate");
}

// --- the type_info vtables ------------------------------------------------------------------

TEST_CASE("the vtable init hooks lay out the three type_info vtables and name their kind") {
    Guest g;
    Rtti t(g);
    for (auto [vt, id] : {std::pair{t.vt_class, 3u}, {t.vt_si, 4u}, {t.vt_vmi, 5u}}) {
        CHECK(g.get32(vt) == 0u);      // offset-to-top
        CHECK(g.get32(vt + 4u) == 0u); // no type_info of its own
        for (u32 i = 0; i < MT_CXX_VT_SLOTS; i++) {
            CHECK(g.get32(vt + 8u + 4u * i) == MT_IMPORT_THUNK_BASE + 4u * id);
        }
        CHECK(g.get32(vt + 8u + 4u * MT_CXX_VT_SLOTS) == 0u); // the rest of the 64 bytes
    }
    const u32 a = t.cls("1A"), b = t.si("1B", a), c = t.vmi("1C", 0u, {{a, 0, false, true}});
    CHECK(mt_cxx_type_kind(&g.cpu, a) == MT_CXX_TYPE_CLASS);
    CHECK(mt_cxx_type_kind(&g.cpu, b) == MT_CXX_TYPE_SI);
    CHECK(mt_cxx_type_kind(&g.cpu, c) == MT_CXX_TYPE_VMI);
    CHECK(mt_cxx_type_kind(&g.cpu, 0u) == MT_CXX_TYPE_OTHER);
    CHECK(mt_cxx_type_kind(&g.cpu, g.words({0x1234u, 0u})) == MT_CXX_TYPE_OTHER);
    mt_cxx_set_type_info_vtables(0u, 0u, 0u); // not placed: nothing is a class type_info
    CHECK(mt_cxx_type_kind(&g.cpu, a) == MT_CXX_TYPE_OTHER);
}

TEST_CASE("the registry lists every C++ ABI import of the game") {
    u32 n = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n);
    const auto find = [&](const char *name) -> const mt_hle_entry * {
        for (u32 i = 0; i < n; i++) {
            if (std::strcmp(hle[i].name, name) == 0) {
                return &hle[i];
            }
        }
        return nullptr;
    };
    // tests/test_hle_cxxabi_real.py checks these against the armv7 slice's imports.
    const struct {
        const char *name;
        mt_import_fn fn;
        mt_data_init_fn init;
    } k_entries[] = {
        {"__Znwm", mt_hle_Znwm, nullptr},
        {"__Znam", mt_hle_Znam, nullptr},
        {"__ZdlPv", mt_hle_ZdlPv, nullptr},
        {"__ZdaPv", mt_hle_ZdaPv, nullptr},
        {"___cxa_pure_virtual", mt_hle_cxa_pure_virtual, nullptr},
        {"__ZSt9terminatev", mt_sjlj_hle_terminate, nullptr},
        {"___cxa_atexit", mt_hle_cxa_atexit, nullptr},
        {"__ZTVN10__cxxabiv117__class_type_infoE", nullptr, mt_cxx_init_class_type_info_vtable},
        {"__ZTVN10__cxxabiv120__si_class_type_infoE", nullptr,
         mt_cxx_init_si_class_type_info_vtable},
        {"__ZTVN10__cxxabiv121__vmi_class_type_infoE", nullptr,
         mt_cxx_init_vmi_class_type_info_vtable},
    };
    for (const auto &want : k_entries) {
        const mt_hle_entry *e = find(want.name);
        REQUIRE_MESSAGE(e != nullptr, want.name);
        CHECK_MESSAGE(e->fn == want.fn, want.name);
        CHECK_MESSAGE(e->init == want.init, want.name);
    }
}

// --- catch matching ---------------------------------------------------------------------------

TEST_CASE("type_info equality: the same object, the same name pointer or the same name") {
    Guest g;
    Rtti t(g);
    const u32 a = t.cls("4Base"), a2 = t.cls("4Base"), other = t.cls("5Other");
    const u32 shared = g.str("6Shared");
    const u32 s1 = g.words({t.vt_class + 8u, shared}), s2 = g.words({t.vt_si + 8u, shared, a});
    const u32 local1 = t.cls("*N12_GLOBAL__N_14LocalE"), local2 = t.cls("*N12_GLOBAL__N_14LocalE");
    CHECK(mt_cxx_same_type(&g.cpu, a, a) == 1);
    CHECK(mt_cxx_same_type(&g.cpu, a, a2) == 1); // two objects of one type (strcmp)
    CHECK(mt_cxx_same_type(&g.cpu, a, other) == 0);
    CHECK(mt_cxx_same_type(&g.cpu, s1, s2) == 1);         // one name pointer
    CHECK(mt_cxx_same_type(&g.cpu, local1, local2) == 0); // '*': local, pointer equality only
    CHECK(mt_cxx_same_type(&g.cpu, local1, local1) == 1);
    // Not class type_infos (an ObjC class, a test's plain address): only the address counts.
    const u32 x = g.words({0u, g.str("4Base")});
    CHECK(mt_cxx_same_type(&g.cpu, x, x) == 1);
    CHECK(mt_cxx_same_type(&g.cpu, x, a) == 0);
    CHECK(catches(g, a2, a));
    CHECK_FALSE(catches(g, x, a));
    CHECK_FALSE(catches(g, a, 0u)); // a thrown type 0 only meets catch (...)
}

TEST_CASE("__si_class_type_info: a class is caught by each of its bases, not the other way") {
    Guest g;
    Rtti t(g);
    const u32 base = t.cls("4Base"), derived = t.si("7Derived", base), mid = t.si("3Mid", derived);
    const u32 other = t.cls("5Other");
    u32 obj = 0x01000100u;
    CHECK(catches(g, base, derived, &obj));
    CHECK(obj == 0x01000100u); // an __si_ base is at offset 0
    CHECK(catches(g, base, mid, &obj));
    CHECK(catches(g, derived, mid, &obj));
    CHECK(catches(g, t.cls("4Base"), mid)); // a base type_info from another object file
    CHECK_FALSE(catches(g, derived, base));
    CHECK_FALSE(catches(g, other, mid));
    CHECK_FALSE(catches(g, mid, derived));
    CHECK(mt_cxx_upcast(&g.cpu, mid, base, nullptr) == 1);
    CHECK(mt_cxx_upcast(&g.cpu, base, mid, nullptr) == 0);
}

TEST_CASE("__vmi_class_type_info: base offsets move the object, non-public and ambiguous bases "
          "do not match") {
    Guest g;
    Rtti t(g);
    const u32 a = t.cls("1A"), b = t.cls("1B"), c = t.si("1C", b);
    // struct M : A, C {}: C (and so B) at +8
    const u32 m = t.vmi("1M", 0u, {{a, 0, false, true}, {c, 8, false, true}});
    u32 obj = 0x01000100u;
    CHECK(catches(g, a, m, &obj));
    CHECK(obj == 0x01000100u);
    CHECK(catches(g, b, m, &obj));
    CHECK(obj == 0x01000108u);
    obj = 0x01000100u;
    CHECK(catches(g, c, m, &obj));
    CHECK(obj == 0x01000108u);

    // struct P : private A, public B {}
    const u32 p = t.vmi("1P", 0u, {{a, 0, false, false}, {b, 4, false, true}});
    obj = 0x01000200u;
    CHECK_FALSE(catches(g, a, p, &obj));
    CHECK(obj == 0x01000200u); // no match leaves it alone
    CHECK(catches(g, b, p, &obj));
    CHECK(obj == 0x01000204u);

    // struct X : public A {}; struct Y : private X, public M {}: A twice, once privately
    const u32 x = t.si("1X", a);
    const u32 y = t.vmi("1Y", 1u, {{x, 0, false, false}, {m, 16, false, true}});
    CHECK_FALSE(catches(g, a, y)); // two A subobjects: ambiguous
    CHECK(catches(g, b, y, &obj));

    // struct N : A, X {}: two public A subobjects, at 0 and at 4
    const u32 n = t.vmi("1N", 1u, {{a, 0, false, true}, {x, 4, false, true}});
    obj = 0x01000300u;
    CHECK_FALSE(catches(g, a, n, &obj));
    CHECK(obj == 0x01000300u);
    CHECK(catches(g, x, n, &obj));
    CHECK(obj == 0x01000304u);
}

TEST_CASE("__vmi_class_type_info: a virtual base is found through the object's vtable, once") {
    Guest g;
    Rtti t(g);
    const u32 v = t.cls("1V");
    // struct L : virtual V {}; struct R : virtual V {}; struct D : L, R {}: the virtual base's
    // offset is at vtable - 12 in both L's and R's vtables.
    const u32 l = t.vmi("1L", 0u, {{v, -12, true, true}});
    const u32 r = t.vmi("1R", 0u, {{v, -12, true, true}});
    const u32 d = t.vmi("1D", 2u, {{l, 0, false, true}, {r, 8, false, true}});
    // A D object: L-in-D's vptr at +0 (V at +32 from there), R-in-D's at +8 (V at +24).
    const u32 vt_l = g.words({32u, 0u, 0u, 0u}) + 12u, vt_r = g.words({24u, 0u, 0u, 0u}) + 12u;
    const u32 object = g.words({vt_l, 0u, vt_r, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
    u32 obj = object;
    CHECK(catches(g, v, d, &obj)); // one V, reached through L and through R
    CHECK(obj == object + 32u);
    obj = object;
    CHECK(catches(g, r, d, &obj));
    CHECK(obj == object + 8u);
    obj = object;
    CHECK(catches(g, v, l, &obj)); // an L alone: V at +32 per its vtable
    CHECK(obj == object + 32u);
    // Without the object the answer is the same; there is just no address to move.
    CHECK(catches(g, v, d));

    // A non-virtual diamond has two V subobjects: struct L2 : V {}; struct R2 : V {};
    const u32 l2 = t.si("2L2", v), r2 = t.si("2R2", v);
    const u32 d2 = t.vmi("2D2", 1u, {{l2, 0, false, true}, {r2, 4, false, true}});
    CHECK_FALSE(catches(g, v, d2));
    CHECK(catches(g, r2, d2));
}

TEST_CASE("the matcher follows the SjLj personality's contract") {
    Guest g;
    Rtti t(g);
    const u32 base = t.cls("4Base"), derived = t.si("7Derived", base);
    // A plain function pointer of the personality's type: the boot installs it.
    mt_sjlj_can_catch_fn fn = mt_cxx_can_catch;
    u32 obj = 0x01000400u;
    CHECK(fn(&g.cpu, base, derived, &obj) == 1);
    CHECK(fn(&g.cpu, base, derived, nullptr) == 1); // exception specifications pass NULL
    // Addresses that are no type_info (the LSDA of a test, an ObjC class) only match
    // themselves, as without a matcher.
    const u32 plain = g.words({0u, 0u});
    CHECK(fn(&g.cpu, plain, plain, &obj) == 1);
    CHECK(fn(&g.cpu, plain, derived, &obj) == 0);
    CHECK(fn(&g.cpu, base, plain, &obj) == 0);
}
