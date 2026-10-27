// CoreFoundation (#48, ADR 0032) on a real guest address space and the ObjC runtime's host
// classes: the bridged classes and constant strings, every CF function import through its HLE
// entry (with the iOS argument layout), the encodings, the bundle on a synthetic app
// directory, and the run loop driven on a clock the test sets. Guest callouts are test
// functions reached through objc_test.cpp's mt_dispatch.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "maplethief/cf.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/libc.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "objc_test.h"
#include "stubs/stubs_internal.h"

namespace {

using u32 = std::uint32_t;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

bool logged(const char *text) {
    for (const auto &l : g_log) {
        if (l.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u32 lo(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return static_cast<u32>(b);
}
u32 hi(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return static_cast<u32>(b >> 32);
}

// A guest address space with the runtime's host classes and CF bound to it.
struct Cf {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;

    Cf() {
        g_log.clear();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
    }
    ~Cf() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Cf(const Cf &) = delete;
    Cf &operator=(const Cf &) = delete;

    u32 pool = 0;

    // Calls an HLE function with word arguments laid out as the iOS convention does: r0-r3,
    // then the guest stack.
    u32 call(mt_import_fn fn, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        const u32 n = static_cast<u32>(args.size());
        const u32 stacked = n > 4 ? n - 4 : 0;
        cpu.r[MT_SP] = sp - ((stacked * 4u + 15u) & ~15u);
        u32 i = 0;
        for (const u32 a : args) {
            if (i < 4) {
                cpu.r[i] = a;
            } else {
                put32(cpu.r[MT_SP] + 4u * (i - 4u), a);
            }
            i++;
        }
        cpu.r[MT_LR] = 0x2345u;
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
    double call_f64(mt_import_fn fn) {
        fn(&cpu, 0);
        std::uint64_t b = (static_cast<std::uint64_t>(cpu.r[1]) << 32) | cpu.r[0];
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }

    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    u32 bytes(const void *p, u32 n) {
        const u32 g = mt_heap_calloc(mem, 1, n + 1);
        std::memcpy(cpu.mem + g, p, n);
        return g;
    }
    u32 cstr(const char *s) { return bytes(s, static_cast<u32>(std::strlen(s))); }
    u32 words(std::initializer_list<u32> ws) {
        const u32 g = mt_heap_calloc(mem, 1, 4u * static_cast<u32>(ws.size()));
        u32 i = 0;
        for (const u32 w : ws) {
            put32(g + 4u * i++, w);
        }
        return g;
    }
    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    const char *class_name(u32 obj) const {
        return mt_objc_class_name(rt, mt_objc_class_of(rt, obj));
    }
    std::u16string text(u32 str) {
        const std::u16string *s = mt_cf::string_chars(&cpu, str);
        return s != nullptr ? *s : u"<none>";
    }
    // A data import's guest slot, filled through its init hook and mt_cf_start.
    u32 constant(mt_data_init_fn hook) {
        const u32 slot = mt_heap_calloc(mem, 1, 4);
        hook(&cpu, 0, slot);
        mt_cf::make_constants(&cpu);
        return get32(slot);
    }
    u32 string(const char *ascii) {
        return mt_cf::string_create(&cpu, std::u16string(ascii, ascii + std::strlen(ascii)), false);
    }
};

// Runs fn on the fixture w; true if it stopped through mt_trap.
bool stops(Cf &w, void (*fn)(Cf &)) {
    struct Arg {
        Cf *w;
        void (*fn)(Cf &);
    } arg{&w, fn};
    return objc_test::stops(
        [](void *p) {
            const Arg *a = static_cast<Arg *>(p);
            a->fn(*a->w);
        },
        &arg);
}

// --- guest callouts --------------------------------------------------------------------------

struct Fire {
    double now;
    u32 timer, info;
};
std::vector<Fire> g_fires;
double g_now = 0;
u32 g_next_imp = 0x00F40001u;

u32 imp(mt_guest_fn fn) {
    const u32 at = g_next_imp;
    g_next_imp += 0x10u;
    objc_test::add_imp(at, fn);
    return at;
}

void record_fire(mt_cpu *cpu) { g_fires.push_back(Fire{g_now, cpu->r[0], cpu->r[1]}); }

// The game's callout pattern (RunLoopTimer_Arm): re-arm 1 ms after now from the callout.
void rearm_fire(mt_cpu *cpu) {
    record_fire(cpu);
    const double next = g_now + 0.001;
    cpu->r[1] = lo(next);
    cpu->r[2] = hi(next);
    mt_hle_CFRunLoopTimerSetNextFireDate(cpu, 0);
}

u32 g_retained_info = 0, g_released_info = 0;
void ctx_retain(mt_cpu *cpu) { g_retained_info = cpu->r[0]; } // returns info (r0)
void ctx_release(mt_cpu *cpu) { g_released_info = cpu->r[0]; }

u32 create_timer(Cf &w, double fire, double interval, u32 callout, u32 context) {
    return w.call(mt_hle_CFRunLoopTimerCreate,
                  {0, lo(fire), hi(fire), lo(interval), hi(interval), 0, 0, callout, context});
}

std::uint64_t g_clock_us = 0;
std::uint64_t test_clock(void *) { return g_clock_us; }

} // namespace

// --- objects and strings ----------------------------------------------------------------------

TEST_CASE("CF: the bridged classes, and the constant-string isa import as __NSCFConstantString") {
    Cf w;
    CHECK(mt_objc_superclass(w.rt, w.klass("__NSCFString")) == w.klass("NSMutableString"));
    CHECK(mt_objc_superclass(w.rt, w.klass("NSMutableString")) == w.klass("NSString"));
    CHECK(mt_objc_superclass(w.rt, w.klass("__NSCFConstantString")) == w.klass("__NSCFString"));
    CHECK(mt_objc_superclass(w.rt, w.klass("__NSCFData")) == w.klass("NSMutableData"));
    CHECK(mt_objc_superclass(w.rt, w.klass("__NSCFBoolean")) == w.klass("NSNumber"));
    CHECK(mt_objc_superclass(w.rt, w.klass("NSNumber")) == w.klass("NSValue"));
    CHECK(mt_objc_superclass(w.rt, w.klass("__NSCFType")) == w.klass("NSObject"));

    // In a start-up, the placed ___CFConstantStringClassReference object is the class.
    mt_objc *rt = mt_objc_create(&w.cpu, mt_mem_guest_alloc, w.mem, nullptr);
    const char *names[] = {"_OBJC_CLASS_$_NSObject", "_OBJC_METACLASS_$_NSObject",
                           "___CFConstantStringClassReference"};
    mt_import_desc descs[3];
    u32 addrs[3];
    for (u32 i = 0; i < 3; i++) {
        descs[i] = mt_import_desc{names[i], "lib", MT_IMPORT_DATA, 0, i == 2 ? 48u : 20u, 4u};
        addrs[i] = mt_heap_calloc(w.mem, 1, 48);
    }
    CHECK(mt_objc_bind_imports(rt, descs, 3, addrs) == 2); // NSObject, __NSCFConstantString
    CHECK(mt_objc_add_host_classes(rt) == 0u);
    CHECK(mt_objc_class_named(rt, "__NSCFConstantString") == addrs[2]);
    CHECK(w.get32(addrs[2] + 4) == mt_objc_class_named(rt, "__NSCFString")); // superclass
    mt_objc_destroy(rt);
}

TEST_CASE("CF: constant strings decode from their literal and are immortal") {
    Cf w;
    const u32 cls = w.klass("__NSCFConstantString");
    const u32 ascii = w.words({cls, 0x7C8u, w.cstr("English"), 7});
    const char16_t korean[] = u"넥슨";
    const u32 utf16 = w.words({cls, 0x7D0u, w.bytes(korean, 4), 2});
    CHECK(w.text(ascii) == u"English");
    CHECK(w.text(utf16) == u"넥슨");
    CHECK(mt_cf::is_string(&w.cpu, ascii));
    CHECK_FALSE(mt_cf::string_is_mutable(&w.cpu, ascii));
    for (int i = 0; i < 3; i++) {
        mt_objc_release(&w.cpu, ascii); // ignored: never deallocated
    }
    CHECK(mt_objc_retain(&w.cpu, ascii) == ascii);
    CHECK(mt_objc_retain_count(w.rt, ascii) != 0u);
    CHECK(mt_objc_send0(&w.cpu, ascii, "retainCount") == 0xFFFFFFFFu);
    CHECK(w.get32(ascii) == cls);
    CHECK(w.text(ascii) == u"English");
    // A malformed literal (chars outside guest memory) is no string.
    const u32 bad = w.words({cls, 0x7C8u, 0xFFFFFF00u, 64});
    CHECK(mt_cf::string_chars(&w.cpu, bad) == nullptr);
    CHECK(logged("malformed constant string"));
}

TEST_CASE("CF: CFStringCreateWithCString in ASCII, UTF-8, EUC-KR and CP949") {
    Cf w;
    const u32 s = w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("hero.dat"), 0x600u});
    CHECK(std::string(w.class_name(s)) == "__NSCFString");
    CHECK(w.text(s) == u"hero.dat");
    CHECK(mt_objc_retain_count(w.rt, s) == 1u);
    CHECK_FALSE(mt_cf::string_is_mutable(&w.cpu, s));
    const u32 utf8 = w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("소환"), 0x08000100u});
    CHECK(w.text(utf8) == u"소환");
    // docs/ANALYSIS.md: the Korean tables' "bc d2 c8 af" is 소환 in EUC-KR.
    const u32 euc =
        w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("\xBC\xD2\xC8\xAF.dat"), 0x0940u});
    CHECK(w.text(euc) == u"소환.dat");
    // CP949's extension (0x8141 = U+AC02) is not EUC-KR.
    CHECK(w.text(w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("\x81\x41"), 0x0422u})) ==
          u"갂");
    CHECK(w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("\x81\x41"), 0x0940u}) == 0u);
    // Bytes the encoding does not have give NULL; so does an encoding the runtime lacks.
    CHECK(w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("caf\xE9"), 0x600u}) == 0u);
    CHECK(w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("\xC3"), 0x08000100u}) == 0u);
    CHECK(w.call(mt_hle_CFStringCreateWithCString, {0, w.cstr("x"), 0u}) == 0u);
    CHECK(logged("encoding 0x00000000 is not supported"));
    // CFRelease deallocates it (toll-free: -release).
    w.call(mt_hle_CFRelease, {s});
    CHECK(w.get32(s) == 0u);
    CHECK(stops(w, [](Cf &c) { c.call(mt_hle_CFStringCreateWithCString, {0, 0, 0x600u}); }));
    CHECK(objc_test::g_trap[0] == MT_TRAP_ABORT);
}

TEST_CASE("CF: CFStringAppendCharacters on a mutable string; an immutable one stops") {
    Cf w;
    const u32 m = mt_cf::string_create(&w.cpu, u"ab", true);
    const char16_t more[] = u"c한";
    w.call(mt_hle_CFStringAppendCharacters, {m, w.bytes(more, 4), 2});
    CHECK(w.text(m) == u"abc한");
    w.call(mt_hle_CFStringAppendCharacters, {m, 0, 0});
    CHECK(w.text(m) == u"abc한");
    CHECK(stops(w, [](Cf &c) {
        c.call(mt_hle_CFStringAppendCharacters, {c.string("fixed"), c.cstr("xy"), 1});
    }));
    CHECK(objc_test::g_trap[0] == MT_TRAP_ABORT);
    CHECK(logged("CFStringAppendCharacters: not a mutable CFString"));
}

TEST_CASE("CF: encodings round-trip, and CF <-> NS encoding numbers") {
    using namespace mt_cf;
    const std::u16string text = u"Maple 메이플 \U0001F341";
    for (const u32 enc : {k_enc_utf8, k_enc_unicode, k_enc_utf16be, k_enc_utf16le}) {
        std::string bytes;
        std::u16string back;
        REQUIRE(encode(text.data(), text.size(), enc, bytes));
        REQUIRE(
            decode(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), enc, back));
        CHECK(back == text);
    }
    const std::u16string hangul = u"메이플스토리 소환의 비석 가";
    for (const u32 enc : {k_enc_euc_kr, k_enc_dos_korean}) {
        std::string bytes;
        std::u16string back;
        REQUIRE(encode(hangul.data(), hangul.size(), enc, bytes));
        REQUIRE(
            decode(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), enc, back));
        CHECK(back == hangul);
    }
    // 똠 (U+B620) is one of CP949's extension syllables: EUC-KR has no code for it.
    std::string out;
    const std::u16string ddom = u"똠";
    CHECK_FALSE(encode(ddom.data(), 1, k_enc_euc_kr, out));
    out.clear();
    CHECK(encode(ddom.data(), 1, k_enc_dos_korean, out));
    CHECK(out == "\x8C\x63");
    out.clear();
    const std::u16string latin = u"é";
    CHECK_FALSE(encode(latin.data(), 1, k_enc_ascii, out));
    CHECK(encode(latin.data(), 1, k_enc_iso_latin1, out));
    CHECK(out == "\xE9");
    // UTF-16 with a byte order mark (kCFStringEncodingUnicode honours and drops it).
    std::u16string be;
    CHECK(decode(reinterpret_cast<const std::uint8_t *>("\xFE\xFF\x00\x41"), 4, k_enc_unicode, be));
    CHECK(be == u"A");
    CHECK(to_utf8(u"\xD800x") == "\xEF\xBF\xBDx");

    CHECK(cf_to_ns_encoding(k_enc_euc_kr) == 0x80000940u);
    CHECK(cf_to_ns_encoding(k_enc_utf8) == 4u);
    CHECK(cf_to_ns_encoding(k_enc_ascii) == 1u);
    CHECK(cf_to_ns_encoding(k_enc_mac_roman) == 30u);
    CHECK(ns_to_cf_encoding(0x80000940u) == k_enc_euc_kr);
    CHECK(ns_to_cf_encoding(4u) == k_enc_utf8);
    CHECK(ns_to_cf_encoding(0x94000100u) == k_enc_utf16le);
    Cf w;
    CHECK(w.call(mt_hle_CFStringConvertEncodingToNSStringEncoding, {0x0940u}) == 0x80000940u);
    CHECK(w.call(mt_hle_CFStringConvertEncodingToNSStringEncoding, {0x08000100u}) == 4u);
}

TEST_CASE("CF: guest C strings of a string are owned by it and freed with it") {
    Cf w;
    const u32 s = mt_cf::string_create(&w.cpu, u"소환", false);
    const u32 utf8 = mt_cf::string_guest_cstring(&w.cpu, s, mt_cf::k_enc_utf8);
    REQUIRE(utf8 != 0u);
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + utf8)) == "소환");
    CHECK(mt_cf::string_guest_cstring(&w.cpu, s, mt_cf::k_enc_utf8) == utf8); // cached
    const u32 euc = mt_cf::string_guest_cstring(&w.cpu, s, mt_cf::k_enc_euc_kr);
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + euc)) == "\xBC\xD2\xC8\xAF");
    CHECK(mt_cf::string_guest_cstring(&w.cpu, s, mt_cf::k_enc_ascii) == 0u);
    CHECK(mt_heap_usable_size(w.mem, utf8) != 0u);
    mt_objc_release(&w.cpu, s);
    CHECK(mt_heap_usable_size(w.mem, utf8) == 0u);
    CHECK(mt_heap_usable_size(w.mem, euc) == 0u);
    // A mutation makes a new copy; the old one stays valid until the string goes.
    const u32 m = mt_cf::string_create(&w.cpu, u"a", true);
    const u32 first = mt_cf::string_guest_cstring(&w.cpu, m, mt_cf::k_enc_ascii);
    mt_cf::string_mutable_chars(&w.cpu, m)->push_back(u'b');
    const u32 second = mt_cf::string_guest_cstring(&w.cpu, m, mt_cf::k_enc_ascii);
    CHECK(second != first);
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + first)) == "a");
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + second)) == "ab");
}

// --- data ------------------------------------------------------------------------------------

TEST_CASE("CF: CFDataCreate, CFDataGetLength, CFDataGetBytePtr, CFDataGetBytes") {
    Cf w;
    const u32 src = w.cstr("0123456789");
    const u32 d = w.call(mt_hle_CFDataCreate, {0, src, 10});
    CHECK(std::string(w.class_name(d)) == "__NSCFData");
    CHECK(w.call(mt_hle_CFDataGetLength, {d}) == 10u);
    const u32 ptr = w.call(mt_hle_CFDataGetBytePtr, {d});
    CHECK(ptr != src);
    CHECK(std::memcmp(w.cpu.mem + ptr, "0123456789", 10) == 0);
    // CFRange {2, 5} by value in r1, r2; the buffer in r3.
    const u32 buf = mt_heap_calloc(w.mem, 1, 16);
    w.call(mt_hle_CFDataGetBytes, {d, 2, 5, buf});
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + buf)) == "23456");
    // A range past the end copies what exists and says so.
    std::memset(w.cpu.mem + buf, 0, 16);
    w.call(mt_hle_CFDataGetBytes, {d, 8, 5, buf});
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + buf)) == "89");
    CHECK(logged("CFDataGetBytes: range {8, 5} outside the 10 bytes"));
    // Empty data still has a byte pointer.
    const u32 empty = w.call(mt_hle_CFDataCreate, {0, 0, 0});
    CHECK(w.call(mt_hle_CFDataGetLength, {empty}) == 0u);
    CHECK(w.call(mt_hle_CFDataGetBytePtr, {empty}) != 0u);
    // Released: the bytes go with the object.
    w.call(mt_hle_CFRelease, {d});
    CHECK(mt_heap_usable_size(w.mem, ptr) == 0u);
    // Mutable data (NSMutableData, #49) grows.
    const u32 m = mt_cf::data_create(&w.cpu, reinterpret_cast<const std::uint8_t *>("ab"), 2, true);
    CHECK(mt_cf::data_append(&w.cpu, m, reinterpret_cast<const std::uint8_t *>("cdefgh"), 6));
    CHECK(mt_cf::data_set_length(&w.cpu, m, 10));
    u32 bytes = 0, len = 0;
    CHECK(mt_cf::data_bytes(&w.cpu, m, &bytes, &len));
    CHECK(len == 10u);
    CHECK(std::memcmp(w.cpu.mem + bytes, "abcdefgh\0\0", 10) == 0);
    CHECK_FALSE(mt_cf::data_append(&w.cpu, empty, reinterpret_cast<const std::uint8_t *>("x"), 1));
    CHECK(stops(w, [](Cf &c) { c.call(mt_hle_CFDataGetLength, {0}); }));
    CHECK(stops(w, [](Cf &c) { c.call(mt_hle_CFDataGetLength, {c.string("not data")}); }));
    CHECK(objc_test::g_trap[0] == MT_TRAP_ABORT);
}

TEST_CASE("CF: CFRelease of NULL stops the guest, as the device crashes") {
    Cf w;
    CHECK(stops(w, [](Cf &c) { c.call(mt_hle_CFRelease, {0}); }));
    CHECK(objc_test::g_trap[0] == MT_TRAP_ABORT);
    CHECK(objc_test::g_trap[1] == 0x2345u);
    CHECK(logged("CFRelease: NULL"));
}

// --- constants -------------------------------------------------------------------------------

TEST_CASE("CF: the data imports get their objects at mt_cf_start") {
    Cf w;
    CHECK(w.constant(mt_hle_init_kCFAllocatorDefault) == 0u);
    const u32 t = w.constant(mt_hle_init_kCFBooleanTrue);
    const u32 f = w.constant(mt_hle_init_kCFBooleanFalse);
    CHECK(std::string(w.class_name(t)) == "__NSCFBoolean");
    CHECK(t != f);
    mt_cf::Number n{};
    CHECK(mt_cf::number_value(&w.cpu, t, &n));
    CHECK(n.i == 1);
    CHECK(mt_cf::number_value(&w.cpu, f, &n));
    CHECK(n.i == 0);
    const u32 nan = w.constant(mt_hle_init_kCFNumberNaN);
    const u32 neg = w.constant(mt_hle_init_kCFNumberNegativeInfinity);
    const u32 pos = w.constant(mt_hle_init_kCFNumberPositiveInfinity);
    CHECK(std::string(w.class_name(nan)) == "__NSCFNumber");
    CHECK(mt_cf::number_value(&w.cpu, nan, &n));
    CHECK(std::isnan(n.f));
    CHECK(mt_cf::number_value(&w.cpu, neg, &n));
    CHECK((std::isinf(n.f) && n.f < 0));
    CHECK(mt_cf::number_value(&w.cpu, pos, &n));
    CHECK((std::isinf(n.f) && n.f > 0));
    for (const u32 obj : {nan, pos, t}) { // immortal
        mt_objc_release(&w.cpu, obj);
        mt_objc_release(&w.cpu, obj);
        CHECK(w.get32(obj) != 0u);
    }
    // A number #49 creates is an ordinary object.
    const u32 seven = mt_cf::number_create(&w.cpu, mt_cf::Number{3u, false, 7, 7.0});
    mt_objc_release(&w.cpu, seven);
    CHECK(w.get32(seven) == 0u);

    struct Expected {
        mt_data_init_fn hook;
        const char16_t *text;
    };
    const Expected strings[] = {
        {mt_hle_init_kCFRunLoopCommonModes, u"kCFRunLoopCommonModes"},
        {mt_hle_init_kCFRunLoopDefaultMode, u"kCFRunLoopDefaultMode"},
        {mt_hle_init_kCFBundleIdentifierKey, u"CFBundleIdentifier"},
        {mt_hle_init_kCFBundleVersionKey, u"CFBundleVersion"},
        {mt_hle_init_NSLocaleCountryCode, u"kCFLocaleCountryCodeKey"},
        {mt_hle_init_NSLocaleLanguageCode, u"kCFLocaleLanguageCodeKey"},
    };
    for (const Expected &e : strings) {
        const u32 s = w.constant(e.hook);
        CHECK(std::string(w.class_name(s)) == "__NSCFConstantString");
        CHECK(w.text(s) == e.text);
    }
    // Imports recorded for another address space (an earlier process) are dropped.
    const u32 slot = mt_heap_calloc(w.mem, 1, 4);
    mt_cpu other{};
    other.mem = w.cpu.mem + 0x1000; // any other base
    other.mem_size = w.cpu.mem_size;
    mt_hle_init_kCFBooleanTrue(&other, 0, slot);
    CHECK(mt_cf::make_constants(&w.cpu) == 0u);
    CHECK(w.get32(slot) == 0u);
}

// --- the bundle ------------------------------------------------------------------------------

namespace {

struct Bundle {
    std::filesystem::path dir;
    explicit Bundle(const char *language) {
        dir = std::filesystem::temp_directory_path() / "maplethief-cf-test-bundle";
        std::filesystem::remove_all(dir);
        write("Icon.png", "icon");
        write("English/data/hero.dat", std::string("hero\0bytes", 10));
        write("Korean/data/hero.dat", "\xBC\xD2");
        write("en.lproj/Scripts.strings", "english scripts");
        write("ko.lproj/Scripts.strings", "korean scripts");
        write("English.lproj/v1.0.0.png", "version");
        write("png/ui/Case.png", "case");
        mt_cf_config cfg = mt_cf_config_default();
        path = dir.string();
        cfg.bundle_dir = path.c_str();
        std::memcpy(cfg.language, language, std::strlen(language) + 1);
        mt_cf_configure(&cfg);
    }
    ~Bundle() {
        mt_cf_configure(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Bundle(const Bundle &) = delete;
    Bundle &operator=(const Bundle &) = delete;
    void write(const char *rel, const std::string &bytes) {
        const std::filesystem::path p = dir / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }
    std::string path;
};

// CFBundleCopyResourceURLForLocalization + CFURLCreateDataAndPropertiesFromResource, as the
// game's FUN_0000a2c0 / FUN_00008f70 call them; "" for no URL.
std::string load(Cf &w, const char *name, const char *type, const char *subdir, const char *loc) {
    const u32 bundle = w.call(mt_hle_CFBundleGetMainBundle, {});
    auto str = [&w](const char *s) { return s != nullptr ? w.string(s) : 0u; };
    const u32 url = w.call(mt_hle_CFBundleCopyResourceURLForLocalization,
                           {bundle, str(name), str(type), str(subdir), str(loc)});
    if (url == 0) {
        return "";
    }
    const u32 out = mt_heap_calloc(w.mem, 1, 16); // data, properties, error code
    w.put32(out + 8, 0xDEADu);
    const u32 ok =
        w.call(mt_hle_CFURLCreateDataAndPropertiesFromResource, {0, url, out, out + 4, 0, out + 8});
    REQUIRE(ok == 1u);
    CHECK(w.get32(out + 8) == 0u);
    const u32 data = w.get32(out);
    const u32 len = w.call(mt_hle_CFDataGetLength, {data});
    const u32 ptr = w.call(mt_hle_CFDataGetBytePtr, {data});
    std::string bytes(reinterpret_cast<const char *>(w.cpu.mem + ptr), len);
    w.call(mt_hle_CFRelease, {url});
    w.call(mt_hle_CFRelease, {data});
    return bytes;
}

} // namespace

TEST_CASE("CF: CFBundle resource lookup reads files of the bundle, with localizations") {
    Cf w;
    Bundle b("en");
    const u32 bundle = w.call(mt_hle_CFBundleGetMainBundle, {});
    CHECK(bundle != 0u);
    CHECK(w.call(mt_hle_CFBundleGetMainBundle, {}) == bundle);
    CHECK(std::string(w.class_name(bundle)) == "__NSCFType");
    mt_objc_release(&w.cpu, bundle); // immortal
    CHECK(w.get32(bundle) != 0u);

    // The game's call: name, no type, the data folder, localization "English".
    CHECK(load(w, "hero.dat", nullptr, "English/data", "English") ==
          std::string("hero\0bytes", 10));
    CHECK(load(w, "hero.dat", nullptr, "Korean/data", "English") == "\xBC\xD2");
    CHECK(load(w, "Icon", "png", nullptr, nullptr) == "icon");
    CHECK(load(w, "Icon", ".png", "", "English") == "icon");
    CHECK(load(w, "data/hero.dat", nullptr, "English", nullptr) == std::string("hero\0bytes", 10));
    // Localized: English -> en.lproj (alias), ko / Korean -> ko.lproj, en -> English.lproj.
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, "English") == "english scripts");
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, "Korean") == "korean scripts");
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, "ko") == "korean scripts");
    CHECK(load(w, "v1.0.0", "png", nullptr, "en") == "version");
    // No localization: the configured language first.
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, nullptr) == "english scripts");
    // Exact case, inside the bundle only.
    CHECK(load(w, "case.png", nullptr, "png/ui", "English").empty());
    CHECK(load(w, "Case.png", nullptr, "png/ui", "English") == "case");
    CHECK(load(w, "../Icon.png", nullptr, "png", nullptr).empty());
    CHECK(load(w, "missing.dat", nullptr, "English/data", "English").empty());
    CHECK(logged("no resource \"missing.dat\""));

    // The properties: an empty NSDictionary when Foundation has the class (here it has not).
    CHECK(logged("no NSDictionary class: properties NULL"));
    // The bundle's file list and guest paths (#49's NSBundle).
    const std::vector<std::string> files = mt_cf::bundle_files(&w.cpu);
    CHECK(files.size() == 7u);
    CHECK(files.front() == "English.lproj/v1.0.0.png");
    CHECK(mt_cf::bundle_guest_path("Icon.png") ==
          std::string(MT_CF_DEFAULT_BUNDLE_PATH) + "/Icon.png");
    CHECK(stops(w, [](Cf &c) {
        c.call(mt_hle_CFBundleCopyResourceURLForLocalization, {c.string("x"), 0, 0, 0, 0});
    }));
}

TEST_CASE("CF: the configured language picks the localization; a missing file is an error") {
    Cf w;
    Bundle b("ko");
    CHECK(std::string(mt_cf_language()) == "ko");
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, nullptr) == "korean scripts");
    CHECK(load(w, "Scripts.strings", nullptr, nullptr, "English") == "english scripts");
    // A URL whose file has gone: FALSE and kCFURLResourceNotFoundError.
    const u32 url = mt_cf::url_create(&w.cpu, "gone.dat");
    const u32 out = mt_heap_calloc(w.mem, 1, 12);
    CHECK(w.call(mt_hle_CFURLCreateDataAndPropertiesFromResource, {0, url, out, 0, 0, out + 8}) ==
          0u);
    CHECK(w.get32(out) == 0u);
    CHECK(static_cast<std::int32_t>(w.get32(out + 8)) == -12);
    mt_cf_configure(nullptr); // no bundle: every lookup fails, logged
    CHECK(load(w, "Icon.png", nullptr, nullptr, nullptr).empty());
    CHECK(logged("no bundle directory configured"));
}

// --- the run loop ----------------------------------------------------------------------------

TEST_CASE("CF: a CFRunLoopTimer fires at the requested interval") {
    Cf w;
    g_fires.clear();
    const u32 common = w.constant(mt_hle_init_kCFRunLoopCommonModes);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    CHECK(w.call(mt_hle_CFRunLoopGetCurrent, {}) == main);
    CHECK(mt_runloop_main(&w.cpu) == main);
    const u32 context = w.words({0, 0x1234u, 0, 0, 0});
    const u32 timer = create_timer(w, 100.0, 0.25, imp(record_fire), context);
    REQUIRE(timer != 0u);
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, common});
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, common}); // again: nothing changes
    CHECK(mt_runloop_timer_count() == 1u);
    w.call(mt_hle_CFRelease, {timer}); // the run loop keeps it
    CHECK(mt_runloop_next_fire_date() == 100.0);

    // 10 ms turns from 99.00 to 101.00: fires at the first turn at or after each grid point.
    double next = 0;
    for (int i = 0; i <= 200; i++) {
        g_now = 99.0 + i / 100.0;
        next = mt_runloop_run_once(&w.cpu, g_now);
    }
    REQUIRE(g_fires.size() == 5u);
    for (std::size_t k = 0; k < g_fires.size(); k++) {
        const double due = 100.0 + 0.25 * static_cast<double>(k);
        CHECK(g_fires[k].now >= due);
        CHECK(g_fires[k].now < due + 0.01);
        CHECK(g_fires[k].timer == timer);
        CHECK(g_fires[k].info == 0x1234u);
    }
    CHECK(next == 101.25);
    // A late turn fires once and skips the missed fires: the grid stays.
    g_fires.clear();
    g_now = 101.9;
    CHECK(mt_runloop_run_once(&w.cpu, g_now) == 102.0);
    CHECK(g_fires.size() == 1u);
}

TEST_CASE("CF: the game's timer pattern: re-armed from its callout with SetNextFireDate") {
    Cf w;
    g_fires.clear();
    const u32 common = w.constant(mt_hle_init_kCFRunLoopCommonModes);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    // RunLoopTimer_Arm: fire = now + 1 ms, interval 1 ms, kCFRunLoopCommonModes.
    const u32 timer = create_timer(w, 10.001, 0.001, imp(rearm_fire), 0);
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, common});
    // 60 Hz turns: one fire per turn, each re-armed 1 ms after that turn.
    for (int i = 0; i < 10; i++) {
        g_now = 10.0 + i / 60.0;
        const double next = mt_runloop_run_once(&w.cpu, g_now);
        if (i > 0) {
            CHECK(next == g_now + 0.001);
        }
    }
    CHECK(g_fires.size() == 9u);
    // SetNextFireDate outside a callout moves it too.
    w.call(mt_hle_CFRunLoopTimerSetNextFireDate, {timer, lo(50.0), hi(50.0)});
    CHECK(mt_runloop_next_fire_date() == 50.0);
    g_now = 49.0;
    mt_runloop_run_once(&w.cpu, g_now);
    CHECK(g_fires.size() == 9u);
}

TEST_CASE("CF: one-shot timers, order of due timers, modes, context retain/release") {
    Cf w;
    g_fires.clear();
    g_retained_info = g_released_info = 0;
    const u32 common = w.constant(mt_hle_init_kCFRunLoopCommonModes);
    const u32 dflt = w.constant(mt_hle_init_kCFRunLoopDefaultMode);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    const u32 context = w.words({0, 0x77u, imp(ctx_retain), imp(ctx_release), 0});
    const u32 later = create_timer(w, 5.0, 0, imp(record_fire), context);
    CHECK(g_retained_info == 0x77u);
    const u32 sooner = create_timer(w, 4.0, 0, imp(record_fire), 0);
    const u32 tracking = create_timer(w, 1.0, 0, imp(record_fire), 0);
    w.call(mt_hle_CFRunLoopAddTimer, {main, later, dflt});
    w.call(mt_hle_CFRunLoopAddTimer, {main, sooner, common});
    w.call(mt_hle_CFRunLoopAddTimer, {main, tracking, w.string("UITrackingRunLoopMode")});
    CHECK(logged("will not fire"));
    for (const u32 t : {later, sooner, tracking}) {
        w.call(mt_hle_CFRelease, {t});
    }
    g_now = 6.0;
    CHECK(mt_runloop_run_once(&w.cpu, g_now) == MT_CF_DISTANT_FUTURE);
    REQUIRE(g_fires.size() == 2u);
    CHECK(g_fires[0].timer == sooner); // earliest fire date first
    CHECK(g_fires[1].timer == later);
    // Fired one-shot timers are invalidated and leave the run loop (and are deallocated,
    // which releases the context's info).
    CHECK(mt_runloop_timer_count() == 1u);
    CHECK(g_released_info == 0x77u);
    mt_runloop_run_once(&w.cpu, 7.0);
    CHECK(g_fires.size() == 2u);
    CHECK(stops(w, [](Cf &c) {
        c.call(mt_hle_CFRunLoopAddTimer, {mt_runloop_main(&c.cpu), c.string("no timer"), 0});
    }));
}

namespace {
std::vector<std::string> g_turns;
void turn_a(mt_cpu *, void *user) { g_turns.push_back(static_cast<const char *>(user)); }
// Autoreleases a new object: the turn's pool releases it.
void autorelease_one(mt_cpu *cpu) {
    record_fire(cpu);
    mt_objc_autorelease(cpu, mt_cf::string_create(cpu, u"temp", false));
}
} // namespace

TEST_CASE("CF: per-turn sources (GCD's main queue, #50) and the turn's autorelease pool") {
    Cf w;
    g_turns.clear();
    g_fires.clear();
    static char a[] = "a", b[] = "b";
    CHECK(mt_runloop_add_turn_source(turn_a, a) == 0);
    CHECK(mt_runloop_add_turn_source(turn_a, b) == 0);
    CHECK(mt_runloop_add_turn_source(turn_a, a) == -1);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    const u32 timer = create_timer(w, 1.0, 1.0, imp(autorelease_one), 0);
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, w.constant(mt_hle_init_kCFRunLoopDefaultMode)});
    const u32 before = mt_objc_pool_pending(w.rt);
    mt_runloop_run_once(&w.cpu, 1.0);
    CHECK(g_turns == std::vector<std::string>{"a", "b"});
    CHECK(g_fires.size() == 1u);
    CHECK(mt_objc_pool_pending(w.rt) == before); // drained at the end of the turn
    CHECK(mt_runloop_remove_turn_source(turn_a, a) == 0);
    CHECK(mt_runloop_remove_turn_source(turn_a, a) == -1);
    mt_runloop_run_once(&w.cpu, 1.5);
    CHECK(g_turns == std::vector<std::string>{"a", "b", "b"});
    CHECK(mt_runloop_remove_turn_source(turn_a, b) == 0);
}

namespace {
// GCD (ADR 0033) on a virtual monotonic clock in nanoseconds.
std::uint64_t g_gcd_clock = 0;
std::uint64_t gcd_clock(void *) { return g_gcd_clock; }
void gcd_sleep(void *, std::uint64_t deadline) {
    g_gcd_clock = deadline > g_gcd_clock ? deadline : g_gcd_clock;
}
std::vector<std::string> g_order;
void order_source(mt_cpu *, void *) { g_order.emplace_back("source"); }
void order_timer(mt_cpu *) { g_order.emplace_back("timer"); }
void order_async(mt_cpu *) { g_order.emplace_back("dispatch_async"); }
void order_after(mt_cpu *) { g_order.emplace_back("dispatch_after"); }
void order_handler(mt_cpu *) { g_order.emplace_back("handler's dispatch_async"); }
u32 g_main_q = 0, g_handler_block = 0;
// A deferred delivery of the offline stubs (#52), like a GameKit completion handler that hands
// its result to the main queue.
void order_delivery(mt_cpu *cpu, const u32 *) {
    g_order.emplace_back("stubs delivery");
    cpu->r[0] = g_main_q;
    cpu->r[1] = g_handler_block;
    mt_hle_dispatch_async(cpu, 0);
}
// A global block {isa, BLOCK_IS_GLOBAL, 0, invoke, descriptor {0, 20}}.
u32 global_block(Cf &w, mt_guest_fn invoke) {
    const u32 descriptor = w.words({0, 20});
    return w.words({w.klass("__NSGlobalBlock__"), 1u << 28, 0, imp(invoke), descriptor});
}
} // namespace

TEST_CASE(
    "CF: a turn runs the sources, the due timers, the stubs' deliveries, then GCD's main queue") {
    mt_gcd_reset();
    mt_gcd_config gcfg = mt_gcd_config_default();
    g_gcd_clock = 1000000000u; // 1 s on GCD's clock (it must never read 0)
    gcfg.clock = gcd_clock;
    gcfg.sleep_until = gcd_sleep;
    REQUIRE(mt_gcd_configure(&gcfg) == 0);
    Cf w;
    g_order.clear();
    const u32 main_q = mt_heap_calloc(w.mem, 1, 64); // _dispatch_main_q and its init hook
    mt_hle_init_dispatch_main_q(&w.cpu, 0, main_q);
    REQUIRE(mt_runloop_add_turn_source(order_source, nullptr) == 0);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    const u32 timer = create_timer(w, 10.0, 0, imp(order_timer), 0);
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, w.constant(mt_hle_init_kCFRunLoopCommonModes)});
    w.call(mt_hle_CFRelease, {timer});

    // dispatch_after(dispatch_time(NOW, 5 ms), main queue): the turn's wake-up comes from GCD's
    // deadline, converted to CFAbsoluteTime (GCD's clock is its own).
    const std::uint64_t when = g_gcd_clock + 5000000u;
    w.call(mt_hle_dispatch_after, {static_cast<u32>(when), static_cast<u32>(when >> 32), main_q,
                                   global_block(w, order_after)});
    CHECK(mt_runloop_next_fire_date() == 10.0);
    CHECK(mt_runloop_next_wake(9.0) == doctest::Approx(9.005));
    // A turn before anything is due runs only the source.
    CHECK(mt_runloop_run_once(&w.cpu, 9.0) == doctest::Approx(9.005));
    CHECK(g_order == std::vector<std::string>{"source"});

    // dispatch_async to the main queue: work is waiting, so the loop should not sleep.
    w.call(mt_hle_dispatch_async, {main_q, global_block(w, order_async)});
    CHECK(mt_runloop_next_wake(9.5) == 9.5);
    // A stubs delivery waiting also means "now".
    g_main_q = main_q;
    g_handler_block = global_block(w, order_handler);
    (void)mt_stubs_impl::runtime(); // the stubs' state belongs to this runtime
    REQUIRE(mt_stubs_impl::post(order_delivery, nullptr, 0));
    CHECK(mt_stubs_pending() == 1u);
    // One turn with everything due: the source, the timer, the stubs' delivery, then the main
    // queue: the dispatch_async block (queued first), the block the delivery queued, then the
    // dispatch_after block (moved to the main queue when the drain found it due).
    g_order.clear();
    g_gcd_clock = when;
    CHECK(mt_runloop_run_once(&w.cpu, 10.0) == MT_CF_DISTANT_FUTURE);
    CHECK(g_order == std::vector<std::string>{"source", "timer", "stubs delivery", "dispatch_async",
                                              "handler's dispatch_async", "dispatch_after"});
    CHECK(mt_stubs_pending() == 0u);
    mt_gcd_stats stats{};
    mt_gcd_get_stats(&stats);
    CHECK(stats.main_blocks == 3u);
    CHECK(stats.timers_fired == 1u);
    CHECK(mt_runloop_remove_turn_source(order_source, nullptr) == 0);
    mt_gcd_reset();
}

TEST_CASE("CF: CFAbsoluteTimeGetCurrent is the libc clock since 2001") {
    Cf w;
    mt_libc_config cfg = mt_libc_config_default();
    cfg.clock = test_clock;
    mt_libc_configure(&cfg);
    g_clock_us = 978307200ull * 1000000ull + 1500000ull; // 2001-01-01 00:00:01.5 UTC
    CHECK(w.call_f64(mt_hle_CFAbsoluteTimeGetCurrent) == 1.5);
    CHECK(mt_cf_absolute_time() == 1.5);
    g_clock_us += 250000ull;
    CHECK(mt_cf_absolute_time() == 1.75);
    mt_libc_configure(nullptr);
    CHECK(mt_cf_absolute_time() > 800000000.0); // the host clock: after 2026
}

TEST_CASE(
    "CF: CFRunLoopAddSource keeps any object (#52's socket source); CFRelease of any object") {
    Cf w;
    // CMSocket::Connect: the offline CFSocket stubs (#52) return plain NSObject instances.
    const u32 source = mt_cf::create(&w.cpu, w.klass("NSObject"));
    REQUIRE(source != 0u);
    const u32 main = w.call(mt_hle_CFRunLoopGetCurrent, {});
    const u32 common = w.constant(mt_hle_init_kCFRunLoopCommonModes);
    w.call(mt_hle_CFRunLoopAddSource, {main, source, common});
    w.call(mt_hle_CFRunLoopAddSource, {main, source, common}); // again: nothing changes
    CHECK(mt_objc_retain_count(w.rt, source) == 2u);
    w.call(mt_hle_CFRelease, {source}); // a release: the run loop still holds it
    CHECK(mt_objc_retain_count(w.rt, source) == 1u);
    CHECK(mt_runloop_run_once(&w.cpu, 1.0) == MT_CF_DISTANT_FUTURE); // never signalled
    // CFRelease of any other object releases it (toll-free): the last one deallocates.
    const u32 plain = mt_cf::create(&w.cpu, w.klass("NSObject"));
    w.call(mt_hle_CFRelease, {plain});
    CHECK(w.get32(plain) == 0u);
    CHECK(stops(w,
                [](Cf &c) { c.call(mt_hle_CFRunLoopAddSource, {mt_runloop_main(&c.cpu), 0, 0}); }));
}
