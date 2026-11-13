// Foundation (#49, ADR 0035) on a real guest address space and the runtime's own host classes:
// every class through the messages the game sends (with the iOS argument layout), the binary
// plist reader on synthetic files, the host categories, and the save files written through
// NSFileHandle by one process and read back by the next, a save killed part way (#78), the
// config's sanitising and a Documents directory apart from the data directory.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "foundation/plist.h"
#include "maplethief/cf.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "maplethief/textpack.h"
#include "objc_test.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

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

// A directory of its own per test process (ctest runs the cases in parallel processes).
fs::path unique_dir(const char *what) {
    return fs::temp_directory_path() /
           (std::string("maplethief-foundation-test-") + what + "-" +
            std::to_string(std::random_device{}()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// --- binary plists for the tests ---------------------------------------------------------------

// A plist value to write: a tree of these. A dictionary's entries are VEntry, defined after V:
// libstdc++ needs a std::pair complete where V's default constructor is, so clang rejects a
// vector of pair<string, V> inside V.
struct VEntry;
struct V {
    enum Kind { ascii, utf16, integer, boolean, array, dict } kind = ascii;
    std::string a;
    std::u16string u;
    std::int64_t i = 0;
    bool b = false;
    std::vector<V> items;
    std::vector<VEntry> entries;
};
struct VEntry {
    std::string first;
    V second;
};
V str(const char *s) { return V{V::ascii, s, {}, 0, false, {}, {}}; }
V ustr(const std::u16string &s) { return V{V::utf16, {}, s, 0, false, {}, {}}; }
V num(std::int64_t i) { return V{V::integer, {}, {}, i, false, {}, {}}; }
V yes(bool b) { return V{V::boolean, {}, {}, 0, b, {}, {}}; }
V arr(std::vector<V> items) { return V{V::array, {}, {}, 0, false, std::move(items), {}}; }
V dict(std::vector<VEntry> e) { return V{V::dict, {}, {}, 0, false, {}, std::move(e)}; }

// Writes "bplist00" with 1-byte object references and 2-byte offsets (fine for test files).
struct BplistWriter {
    std::vector<std::vector<std::uint8_t>> objects;

    static void marker(std::vector<std::uint8_t> &o, std::uint8_t type, std::size_t n) {
        if (n < 15) {
            o.push_back(static_cast<std::uint8_t>(type | n));
        } else {
            o.push_back(static_cast<std::uint8_t>(type | 0x0F));
            o.push_back(0x11); // a 2-byte integer
            o.push_back(static_cast<std::uint8_t>(n >> 8));
            o.push_back(static_cast<std::uint8_t>(n));
        }
    }
    std::uint8_t add(std::vector<std::uint8_t> o) {
        objects.push_back(std::move(o));
        REQUIRE(objects.size() <= 255u);
        return static_cast<std::uint8_t>(objects.size() - 1);
    }
    std::uint8_t write(const V &v) {
        std::vector<std::uint8_t> o;
        switch (v.kind) {
        case V::ascii:
            marker(o, 0x50, v.a.size());
            o.insert(o.end(), v.a.begin(), v.a.end());
            return add(o);
        case V::utf16:
            marker(o, 0x60, v.u.size());
            for (const char16_t c : v.u) {
                o.push_back(static_cast<std::uint8_t>(c >> 8));
                o.push_back(static_cast<std::uint8_t>(c));
            }
            return add(o);
        case V::integer:
            o.push_back(0x13);
            for (int k = 7; k >= 0; k--) {
                o.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(v.i) >> (8 * k)));
            }
            return add(o);
        case V::boolean:
            o.push_back(v.b ? 0x09 : 0x08);
            return add(o);
        case V::array: {
            const std::uint8_t self = add({});
            std::vector<std::uint8_t> refs;
            for (const V &item : v.items) {
                refs.push_back(write(item));
            }
            marker(o, 0xA0, refs.size());
            o.insert(o.end(), refs.begin(), refs.end());
            objects[self] = o;
            return self;
        }
        case V::dict: {
            const std::uint8_t self = add({});
            std::vector<std::uint8_t> keys, values;
            for (const auto &e : v.entries) {
                keys.push_back(write(str(e.first.c_str())));
                values.push_back(write(e.second));
            }
            marker(o, 0xD0, keys.size());
            o.insert(o.end(), keys.begin(), keys.end());
            o.insert(o.end(), values.begin(), values.end());
            objects[self] = o;
            return self;
        }
        }
        return 0;
    }
};

std::string bplist(const V &top) {
    BplistWriter w;
    const std::uint8_t root = w.write(top);
    std::string out = "bplist00";
    std::vector<std::size_t> offsets;
    for (const auto &o : w.objects) {
        offsets.push_back(out.size());
        out.append(o.begin(), o.end());
    }
    const std::size_t table = out.size();
    for (const std::size_t off : offsets) {
        out.push_back(static_cast<char>(off >> 8));
        out.push_back(static_cast<char>(off));
    }
    std::string trailer(32, '\0');
    trailer[6] = 2; // offset size
    trailer[7] = 1; // object ref size
    trailer[15] = static_cast<char>(w.objects.size());
    trailer[23] = static_cast<char>(root);
    trailer[30] = static_cast<char>(table >> 8);
    trailer[31] = static_cast<char>(table);
    return out + trailer;
}

bool parse(const std::string &bytes, mt_ns::PlistValue *v, std::string *error = nullptr) {
    return mt_ns::plist_parse(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), v,
                              error);
}

// --- the fixture --------------------------------------------------------------------------------

// A guest process: memory, a main stack, the runtime with its host classes, CF bound to it, an
// autorelease pool, and a data directory (kept when `dir` is given: a "restart").
struct Ns {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    fs::path data_dir;

    explicit Ns(fs::path dir = unique_dir("data")) : data_dir(std::move(dir)) {
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
        REQUIRE(mt_stubs_set_data_dir(data_dir.string().c_str()) == 0);
        pool = mt_objc_pool_push(rt);
    }
    ~Ns() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
        mt_stubs_set_data_dir(nullptr);
    }
    Ns(const Ns &) = delete;
    Ns &operator=(const Ns &) = delete;

    u32 cls(const char *name) const { return mt_objc_class_named(rt, name); }
    const char *class_name(u32 obj) const {
        return mt_objc_class_name(rt, mt_objc_class_of(rt, obj));
    }
    u32 send(u32 obj, const char *sel, std::initializer_list<u32> args = {}) {
        return mt_objc_msg(&cpu, obj, mt_objc_sel(rt, sel), args.begin(),
                           static_cast<u32>(args.size()));
    }
    u32 cstr(const std::string &s) {
        const u32 g = mt_heap_calloc(mem, 1, static_cast<u32>(s.size()) + 1);
        std::memcpy(cpu.mem + g, s.data(), s.size());
        return g;
    }
    // [NSString stringWithUTF8String:s]
    u32 nsstr(const char *s) { return send(cls("NSString"), "stringWithUTF8String:", {cstr(s)}); }
    std::string text(u32 str) { return mt_ns::string_utf8(&cpu, str); }
    std::string utf8_of(u32 str) {
        const u32 p = send(str, "UTF8String");
        return p != 0 ? std::string(reinterpret_cast<const char *>(cpu.mem + p)) : "<NULL>";
    }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    u32 retain_count(u32 obj) const { return mt_objc_retain_count(rt, obj); }
    // An NSData with `bytes`.
    u32 data(const std::string &bytes) {
        const u32 d = send(cls("NSData"), "alloc");
        return send(d, "initWithBytes:length:", {cstr(bytes), static_cast<u32>(bytes.size())});
    }
    std::string bytes_of(u32 d) {
        const u32 p = send(d, "bytes"), n = send(d, "length");
        return std::string(reinterpret_cast<const char *>(cpu.mem + p), n);
    }
};

// Runs fn on the fixture; true if it stopped through mt_trap (a raised NSException, an
// unrecognized selector). The registers come back as they were: the stop skipped the message's
// own restore of sp and lr.
bool raises(Ns &w, void (*fn)(Ns &)) {
    struct Arg {
        Ns *w;
        void (*fn)(Ns &);
    } arg{&w, fn};
    const mt_cpu saved = w.cpu;
    const bool stopped = objc_test::stops(
        [](void *p) {
            const Arg *a = static_cast<Arg *>(p);
            a->fn(*a->w);
        },
        &arg);
    w.cpu = saved;
    return stopped;
}

// --- a synthetic bundle
// ---------------------------------------------------------------------------

struct Bundle {
    fs::path dir = unique_dir("bundle");
    std::string path;
    explicit Bundle(const char *language) {
        write("Info.plist", bplist(dict({{"CFBundleIdentifier", str("com.example.thief")},
                                         {"CFBundleDisplayName", str("Thief")},
                                         {"CFBundleVersion", str("1.5")},
                                         {"UIStatusBarHidden", yes(true)},
                                         {"Count", num(-7)},
                                         {"UIDeviceFamily", arr({num(1), num(2)})}})));
        write("en.lproj/InfoPlist.strings", bplist(dict({{"CFBundleDisplayName", str("Maple")}})));
        write("en.lproj/Scripts.strings",
              bplist(dict({{"Bruce.dialog.1", str("I'm Bruce.")}, {"empty", str("")}})));
        write("ko.lproj/Scripts.strings", bplist(dict({{"Bruce.dialog.1", ustr(u"브루스")}})));
        write("sound/bgm01.mp3", "mp3");
        write("broken.strings", "not a plist");
        mt_cf_config cfg = mt_cf_config_default();
        path = dir.string();
        cfg.bundle_dir = path.c_str();
        std::memcpy(cfg.language, language, std::strlen(language) + 1);
        mt_cf_configure(&cfg);
    }
    ~Bundle() {
        mt_cf_configure(nullptr);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Bundle(const Bundle &) = delete;
    Bundle &operator=(const Bundle &) = delete;
    void write(const char *rel, const std::string &bytes) {
        const fs::path p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }
};

std::string read_host(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

// --- host categories
// ------------------------------------------------------------------------------

namespace {
void category_test_method(mt_cpu *cpu) { cpu->r[0] = 0x49u; }
} // namespace

TEST_CASE("Foundation: its selectors and UIKit's drawing ones are host categories on NSString") {
    Ns w;
    // Foundation's (#49) and UIKit's drawing (#51) selectors on CF's bridged NSString, each
    // area from its own file: all host methods of NSString.
    for (const char *sel : {"length", "UTF8String", "stringByAppendingPathComponent:",
                            "sizeWithFont:", "drawAtPoint:withFont:"}) {
        mt_objc_method m;
        INFO(sel);
        REQUIRE(mt_objc_lookup(w.rt, w.cls("NSString"), mt_objc_sel(w.rt, sel), &m) == 1);
        CHECK(m.origin == static_cast<u32>(MT_OBJC_HOST));
        CHECK(m.owner == w.cls("NSString"));
    }
    mt_objc_method m;
    REQUIRE(mt_objc_lookup(w.rt, w.cls("NSObject"), mt_objc_sel(w.rt, "description"), &m) == 1);
    CHECK(m.origin == static_cast<u32>(MT_OBJC_HOST));

    // The registry: Foundation's five categories (NSObject, NSString, NSMutableString, NSData,
    // NSNumber) and #51's NSString drawing.
    u32 n = 0;
    const mt_objc_host_category *const *cats = mt_objc_host_category_registry(&n);
    u32 foundation = 0;
    for (u32 i = 0; i < n; i++) {
        REQUIRE(cats[i] != nullptr);
        for (const mt_objc_host_category *c :
             {&mt_ns::k_nsobject_methods, &mt_ns::k_nsstring_methods,
              &mt_ns::k_nsmutablestring_methods, &mt_ns::k_nsdata_methods,
              &mt_ns::k_nsnumber_methods}) {
            foundation += cats[i] == c ? 1u : 0u;
        }
    }
    CHECK(foundation == 5u);

    // A category from a test: a new selector reaches existing strings; no class, no category.
    static const mt_objc_host_method k_extra[] = {{"foundationTestSelector", category_test_method}};
    static const mt_objc_host_category k_cat = {"NSString", k_extra, 1, nullptr, 0};
    REQUIRE(mt_objc_add_host_category(w.rt, &k_cat) == 0);
    CHECK(w.send(w.nsstr("text"), "foundationTestSelector") == 0x49u);
    static const mt_objc_host_category k_nowhere = {"NoSuchClass", k_extra, 1, nullptr, 0};
    CHECK(mt_objc_add_host_category(w.rt, &k_nowhere) == -1);
}

// --- NSString
// -------------------------------------------------------------------------------------

TEST_CASE("Foundation: NSString on CF's strings, constant strings included") {
    Ns w;
    const u32 s = w.nsstr("Documents");
    CHECK(std::string(w.class_name(s)) == "__NSCFString");
    CHECK(w.send(s, "length") == 9u);
    CHECK(w.utf8_of(s) == "Documents");
    CHECK(w.retain_count(s) == 1u); // autoreleased into the fixture's pool

    // Korean: UTF-16 length; EUC-KR through stringWithCString:encoding: (the game's tables).
    const u32 ko = w.nsstr("\xEB\xA9\x94\xEC\x9D\xB4\xED\x94\x8C"); // 메이플
    CHECK(w.send(ko, "length") == 3u);
    const u32 euc_kr = 0x80000940u; // CFStringConvertEncodingToNSStringEncoding(EUC-KR)
    const u32 k2 =
        w.send(w.cls("NSString"), "stringWithCString:encoding:", {w.cstr("\xB8\xDE"), euc_kr});
    CHECK(w.text(k2) == "\xEB\xA9\x94");
    // Mac OS Korean (0x80000003): CChatBox::drawRequire's etc item names, ASCII in English.
    const u32 k3 =
        w.send(w.cls("NSString"),
               "stringWithCString:encoding:", {w.cstr("Blue Snail Shell \xB8\xDE"), 0x80000003u});
    CHECK(w.text(k3) == "Blue Snail Shell \xEB\xA9\x94");
    CHECK(w.send(w.cls("NSString"), "stringWithCString:encoding:", {w.cstr("abc"), 1u}) != 0u);
    CHECK(w.send(w.cls("NSString"), "stringWithUTF8String:", {w.cstr("\xFF")}) == 0u);
    CHECK(w.send(w.cls("NSString"), "stringWithCString:encoding:", {w.cstr("x"), 0x1234u}) == 0u);
    CHECK(logged("encoding 0x00001234 is not supported"));

    // +alloc / -initWithCString:encoding: (LocalizedCString), release frees it.
    const u32 a = w.send(w.cls("NSString"), "alloc");
    CHECK(std::string(w.class_name(a)) == "__NSCFString");
    CHECK(w.send(a, "initWithCString:encoding:", {w.cstr("Scripts"), 4u}) == a);
    CHECK(w.text(a) == "Scripts");
    w.send(a, "release");
    const u32 bad = w.send(w.cls("NSString"), "alloc");
    CHECK(w.send(bad, "initWithCString:encoding:", {w.cstr("\xFF\xFE"), 4u}) == 0u);

    // Equality and hashing by text; toll-free with constant strings.
    const u32 c = mt_cf::string_create_constant(&w.cpu, "Documents");
    CHECK(std::string(w.class_name(c)) == "__NSCFConstantString");
    CHECK(w.send(c, "length") == 9u);
    CHECK(w.send(s, "isEqualToString:", {c}) == 1u);
    CHECK(w.send(c, "isEqualToString:", {s}) == 1u);
    CHECK(w.send(s, "isEqualToString:", {0}) == 0u);
    CHECK(w.send(s, "isEqual:", {c}) == 1u);
    CHECK(w.send(s, "isEqual:", {w.send(w.cls("NSNull"), "null")}) == 0u);
    CHECK(w.send(s, "hash") == w.send(c, "hash"));
    CHECK(w.send(s, "hash") != w.send(ko, "hash"));
    CHECK(w.utf8_of(c) == "Documents");
    CHECK(w.send(c, "description") == c);

    // stringWithString:, +string.
    const u32 copy = w.send(w.cls("NSString"), "stringWithString:", {c});
    CHECK(copy != c);
    CHECK(w.text(copy) == "Documents");
    CHECK(w.send(w.cls("NSString"), "string") != 0u);
    CHECK(raises(w, [](Ns &x) { x.send(x.cls("NSString"), "stringWithString:", {0}); }));
    CHECK(raises(w, [](Ns &x) { x.send(x.cls("NSString"), "stringWithUTF8String:", {0}); }));
}

TEST_CASE("Foundation: compare:options: and path components") {
    Ns w;
    auto cmp = [&w](const char *a, const char *b, u32 options) {
        return static_cast<std::int32_t>(
            w.send(w.nsstr(a), "compare:options:", {w.nsstr(b), options}));
    };
    const u32 numeric = 64, nocase = 1;
    // +[GameCenterManager IsGameCenterAvailable]: systemVersion vs "4.1", NSNumericSearch.
    CHECK(cmp("4.1", "4.1", numeric) == 0);
    CHECK(cmp("4.0.1", "4.1", numeric) == -1);
    CHECK(cmp("4.10", "4.9", numeric) == 1);
    CHECK(cmp("4.10", "4.9", 0) == -1);
    CHECK(cmp("16.0", "4.1", numeric) == 1);
    CHECK(cmp("007", "7", numeric) == 0);
    CHECK(cmp("4.1", "4.1.0", numeric) == -1);
    CHECK(cmp("abc", "ABC", nocase) == 0);
    CHECK(cmp("abc", "ABC", 0) == 1);
    CHECK(static_cast<std::int32_t>(w.send(w.nsstr("a"), "compare:options:", {0, 0})) == 1);

    auto join = [&w](const char *a, const char *b) {
        return w.text(w.send(w.nsstr(a), "stringByAppendingPathComponent:", {w.nsstr(b)}));
    };
    CHECK(join("/var/mobile/X", "Documents") == "/var/mobile/X/Documents");
    CHECK(join("/var/mobile/X/", "/theif0") == "/var/mobile/X/theif0");
    CHECK(join("", "theif0") == "theif0");
    CHECK(join("/", "a") == "/a");
    CHECK(join("dir", "") == "dir");
}

TEST_CASE("Foundation: formats, %@ and NSMutableString") {
    Ns w;
    const u32 obj = w.send(w.send(w.cls("NSObject"), "alloc"), "init");
    const u32 null = w.send(w.cls("NSNull"), "null");
    // The %@ arguments send -description, which clobbers r0-r3: the later ones still arrive.
    const u32 s = mt_objc_msg(&w.cpu, w.cls("NSString"), mt_objc_sel(w.rt, "stringWithFormat:"),
                              std::vector<u32>{w.nsstr("%@|%d|%@|%s|%@|%x|%@"), w.nsstr("txt"), 42u,
                                               null, w.cstr("cstr"), 0u, 0xBEEFu, obj}
                                  .data(),
                              8);
    char expected[128];
    std::snprintf(expected, sizeof expected, "txt|42|<null>|cstr|(null)|beef|<NSObject: 0x%x>",
                  static_cast<unsigned>(obj));
    CHECK(w.text(s) == expected);
    // A double through the variadic words (low word first).
    const double d = 2.5;
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    const u32 f = w.send(w.cls("NSString"), "stringWithFormat:",
                         {w.nsstr("%.1f%%"), static_cast<u32>(bits), static_cast<u32>(bits >> 32)});
    CHECK(w.text(f) == "2.5%");
    // UTF-8 text in the format and in %@ arguments stays UTF-8.
    CHECK(w.text(w.send(w.cls("NSString"), "stringWithFormat:",
                        {w.nsstr("\xEB\xA9\x94 %@"), w.nsstr("\xEC\x9D\xB4")})) ==
          "\xEB\xA9\x94 \xEC\x9D\xB4");
    // -initWithFormat: (CMSoundPlayer::LoadBGM)
    const u32 i =
        w.send(w.send(w.cls("NSString"), "alloc"), "initWithFormat:", {w.nsstr("bgm%02d"), 7u});
    CHECK(w.text(i) == "bgm07");

    // NSMutableString: +alloc/-initWithCapacity:/-appendFormat: (the push-token text), +string.
    const u32 m = w.send(w.send(w.cls("NSMutableString"), "alloc"), "initWithCapacity:", {64});
    CHECK(mt_cf::string_is_mutable(&w.cpu, m));
    w.send(m, "appendFormat:", {w.nsstr("%02x"), 0xABu});
    w.send(m, "appendFormat:", {w.nsstr("%02x"), 0x01u});
    CHECK(w.text(m) == "ab01");
    const u32 ms = w.send(w.cls("NSMutableString"), "string");
    CHECK(mt_cf::string_is_mutable(&w.cpu, ms));
    CHECK_FALSE(mt_cf::string_is_mutable(&w.cpu, w.send(w.cls("NSString"), "string")));
    // -copy of a mutable string is an immutable copy; of an immutable one, the same object.
    const u32 frozen = w.send(m, "copy");
    CHECK(frozen != m);
    CHECK_FALSE(mt_cf::string_is_mutable(&w.cpu, frozen));
    CHECK(w.send(frozen, "copy") == frozen);
    CHECK(mt_cf::string_is_mutable(&w.cpu, w.send(frozen, "mutableCopy")));
    // Mutating an immutable string raises.
    CHECK(raises(w, [](Ns &x) { x.send(x.nsstr("fixed"), "appendFormat:", {x.nsstr("%d"), 1u}); }));
    // -[NSObject description]
    char desc[64];
    std::snprintf(desc, sizeof desc, "<NSObject: 0x%x>", static_cast<unsigned>(obj));
    CHECK(w.text(w.send(obj, "description")) == desc);
}

// --- NSData, NSNumber, NSNull
// ---------------------------------------------------------------------

TEST_CASE("Foundation: NSData") {
    Ns w;
    const std::string bytes("save\0data", 9);
    const u32 d = w.data(bytes);
    CHECK(std::string(w.class_name(d)) == "__NSCFData");
    CHECK(w.send(d, "length") == 9u);
    CHECK(w.bytes_of(d) == bytes);
    const u32 buf = mt_heap_calloc(w.mem, 1, 16);
    w.send(d, "getBytes:", {buf});
    CHECK(std::string(reinterpret_cast<const char *>(w.cpu.mem + buf), 9) == bytes);
    // -initWithData: copies; -copy too.
    const u32 d2 = w.send(w.send(w.cls("NSData"), "alloc"), "initWithData:", {d});
    CHECK(d2 != d);
    CHECK(w.bytes_of(d2) == bytes);
    CHECK(w.send(d2, "bytes") != w.send(d, "bytes"));
    CHECK(w.bytes_of(w.send(d, "copy")) == bytes);
    const u32 empty = w.send(w.send(w.cls("NSData"), "alloc"), "initWithData:", {0});
    CHECK(w.send(empty, "length") == 0u);
    CHECK(w.send(w.send(w.cls("NSData"), "alloc"), "initWithBytes:length:", {0, 0}) != 0u);
    // NSMutableData from +alloc is mutable (CF's data_append works on it).
    const u32 md = w.send(w.cls("NSMutableData"), "alloc");
    CHECK(mt_cf::data_append(&w.cpu, md, reinterpret_cast<const std::uint8_t *>("ab"), 2));
    CHECK_FALSE(mt_cf::data_append(&w.cpu, d, reinterpret_cast<const std::uint8_t *>("ab"), 2));
    // -release of the last reference frees it (the game's [[NSData alloc] init...] / -release).
    CHECK(w.retain_count(d2) == 1u);
    w.send(d2, "release");
    CHECK(mt_cf::data_bytes(&w.cpu, d2, &w.cpu.r[4], &w.cpu.r[5]) == false);
    CHECK(raises(w, [](Ns &x) {
        x.send(x.send(x.cls("NSData"), "alloc"), "initWithBytes:length:", {0, 4});
    }));
}

TEST_CASE("Foundation: NSNumber and NSNull") {
    Ns w;
    // +numberWithBool: is kCFBooleanTrue/False (the drawable properties of createGLes).
    const u32 t = w.send(w.cls("NSNumber"), "numberWithBool:", {1});
    const u32 f = w.send(w.cls("NSNumber"), "numberWithBool:", {0});
    CHECK(t == mt_cf::boolean(&w.cpu, true));
    CHECK(f == mt_cf::boolean(&w.cpu, false));
    CHECK(w.send(w.cls("NSNumber"), "numberWithBool:", {0x100}) == f); // a BOOL is one byte
    CHECK(w.text(w.send(t, "description")) == "1");
    CHECK(w.send(t, "isEqual:", {mt_cf::boolean(&w.cpu, true)}) == 1u);
    CHECK(w.send(t, "isEqual:", {f}) == 0u);
    CHECK(w.send(t, "isEqual:", {w.nsstr("1")}) == 0u);
    const u32 pi = mt_cf::number_create(&w.cpu, mt_cf::Number{6u, true, 0, 3.25});
    CHECK(w.text(w.send(pi, "description")) == "3.25");
    CHECK(w.send(pi, "hash") == 3u);

    // +[NSNull null]: one object, never freed (CStateMainMenu's empty slots).
    const u32 n = w.send(w.cls("NSNull"), "null");
    CHECK(n != 0u);
    CHECK(w.send(w.cls("NSNull"), "null") == n);
    for (int i = 0; i < 3; i++) {
        w.send(n, "release");
    }
    CHECK(w.send(n, "retainCount") == 0xFFFFFFFFu);
    CHECK(w.get32(n) == w.cls("NSNull"));
    CHECK(w.text(w.send(n, "description")) == "<null>");
}

// --- collections ---------------------------------------------------------------------------------

namespace {
// -countByEnumeratingWithState:objects:count: as compiled for-in loops call it: a state struct
// and a buffer on the guest, the count (16 in the game) as the stacked argument.
std::vector<u32> enumerate(Ns &w, u32 collection, u32 batch, u32 *mutations_ptr = nullptr) {
    const u32 state = mt_heap_calloc(w.mem, 1, 32), buffer = mt_heap_calloc(w.mem, 4, batch);
    std::vector<u32> out;
    for (;;) {
        const u32 n = w.send(collection,
                             "countByEnumeratingWithState:objects:count:", {state, buffer, batch});
        if (n == 0) {
            break;
        }
        CHECK(w.get32(state + 4) == buffer);
        if (mutations_ptr != nullptr) {
            *mutations_ptr = w.get32(state + 8);
        }
        for (u32 i = 0; i < n; i++) {
            out.push_back(w.get32(buffer + 4 * i));
        }
    }
    return out;
}
} // namespace

TEST_CASE("Foundation: NSArray and NSMutableArray") {
    Ns w;
    const u32 x = w.nsstr("x");
    const u32 one = w.send(w.cls("NSArray"), "arrayWithObject:", {x});
    CHECK(std::string(w.class_name(one)) == "NSArray");
    CHECK(w.send(one, "count") == 1u);
    CHECK(w.send(one, "objectAtIndex:", {0}) == x);
    CHECK(w.retain_count(x) == 2u);
    CHECK(raises(w, [](Ns &a) {
        a.send(a.send(a.cls("NSArray"), "arrayWithObject:", {a.nsstr("y")}), "objectAtIndex:", {1});
    }));
    CHECK(raises(w, [](Ns &a) { a.send(a.cls("NSArray"), "arrayWithObject:", {0}); }));
    // An NSArray is immutable: -addObject: is not one of its selectors.
    CHECK(raises(w, [](Ns &a) {
        a.send(a.send(a.cls("NSArray"), "arrayWithObject:", {a.nsstr("y")}),
               "addObject:", {a.nsstr("z")});
    }));
    CHECK(logged("unrecognized selector -[NSArray addObject:]"));

    // CStateMainMenu::LoadCharacterSlots: +alloc, -initWithCapacity:, -addObject:,
    // -removeAllObjects; the array retains its items and releases them.
    const u32 m = w.send(w.send(w.cls("NSMutableArray"), "alloc"), "initWithCapacity:", {3});
    CHECK(std::string(w.class_name(m)) == "NSMutableArray");
    std::vector<u32> items;
    for (int i = 0; i < 5; i++) {
        items.push_back(w.send(w.send(w.cls("NSObject"), "alloc"), "init"));
        w.send(m, "addObject:", {items.back()});
        CHECK(w.retain_count(items.back()) == 2u);
    }
    CHECK(w.send(m, "count") == 5u);
    u32 mutations = 0;
    CHECK(enumerate(w, m, 2, &mutations) == items);
    CHECK(mutations != 0u);
    const u32 before = w.get32(mutations);
    w.send(m, "addObject:", {x});
    CHECK(w.get32(mutations) != before); // a for-in loop would call objc_enumerationMutation
    w.send(m, "removeAllObjects");
    CHECK(w.send(m, "count") == 0u);
    for (const u32 item : items) {
        CHECK(w.retain_count(item) == 1u);
    }
    // An immutable array's mutationsPtr is its isa word, which never changes.
    u32 fixed = 0;
    CHECK(enumerate(w, one, 16, &fixed) == std::vector<u32>{x});
    CHECK(fixed == one);
    CHECK(raises(w, [](Ns &a) {
        a.send(a.send(a.send(a.cls("NSMutableArray"), "alloc"), "initWithCapacity:", {1}),
               "addObject:", {0});
    }));
    // Releasing the array releases its items.
    const u32 keep = w.send(w.send(w.cls("NSObject"), "alloc"), "init");
    w.send(m, "addObject:", {keep});
    w.send(m, "release");
    CHECK(w.retain_count(keep) == 1u);
}

TEST_CASE("Foundation: NSSet") {
    Ns w;
    const u32 x = w.nsstr("com.example.product");
    const u32 s = w.send(w.cls("NSSet"), "setWithObject:", {x});
    CHECK(w.send(s, "count") == 1u);
    CHECK(enumerate(w, s, 16) == std::vector<u32>{x});
    const u32 all = w.send(s, "allObjects");
    CHECK(std::string(w.class_name(all)) == "NSArray");
    CHECK(w.send(all, "objectAtIndex:", {0}) == x);
    // UIKit's touch sets (#54): distinct members.
    const u32 t1 = w.send(w.send(w.cls("NSObject"), "alloc"), "init");
    const u32 t2 = w.send(w.send(w.cls("NSObject"), "alloc"), "init");
    const u32 members[] = {t1, t2, t1};
    const u32 touches = mt_ns::set_create(&w.cpu, members, 3);
    CHECK(w.send(touches, "count") == 2u);
    CHECK(*mt_ns::set_items(&w.cpu, touches) == std::vector<u32>{t1, t2});
    CHECK(w.retain_count(t1) == 2u);
    w.send(touches, "release");
    CHECK(w.retain_count(t1) == 1u);
}

TEST_CASE("Foundation: NSDictionary and NSMutableDictionary") {
    Ns w;
    const u32 v1 = w.nsstr("value1"), v2 = w.nsstr("value2"), v3 = w.nsstr("value3");
    // +dictionaryWithObjectsAndKeys: with more pairs than registers (the rest on the stack).
    const u32 d = w.send(w.cls("NSDictionary"), "dictionaryWithObjectsAndKeys:",
                         {v1, w.nsstr("k1"), v2, w.nsstr("k2"), v3, w.nsstr("k1"), 0});
    CHECK(std::string(w.class_name(d)) == "NSDictionary");
    CHECK(w.send(d, "count") == 2u);
    CHECK(w.send(d, "objectForKey:", {w.nsstr("k1")}) == v3); // a later key replaces
    CHECK(w.send(d, "objectForKey:", {mt_cf::string_create_constant(&w.cpu, "k2")}) == v2);
    CHECK(w.send(d, "objectForKey:", {w.nsstr("k3")}) == 0u);
    CHECK(w.send(d, "objectForKey:", {0}) == 0u);
    CHECK(w.retain_count(v1) == 1u); // released when replaced
    // Immutable: no -setObject:forKey:.
    CHECK(raises(w, [](Ns &a) {
        a.send(a.send(a.send(a.cls("NSDictionary"), "alloc"), "init"),
               "setObject:forKey:", {a.nsstr("v"), a.nsstr("k")});
    }));
    CHECK(logged("unrecognized selector -[NSDictionary setObject:forKey:]"));

    // Selectors only dead code sends are not there (makePurchase:'s, docs/re/host-surface.md).
    CHECK(raises(w, [](Ns &a) {
        a.send(a.cls("NSDictionary"), "dictionaryWithObject:forKey:", {a.nsstr("v"), a.nsstr("k")});
    }));

    // NSMutableDictionary: the GameCenterManager cache. A mutable key is copied.
    const u32 m = w.send(w.cls("NSMutableDictionary"), "dictionaryWithCapacity:", {4});
    CHECK(std::string(w.class_name(m)) == "NSMutableDictionary");
    const u32 key = w.send(w.cls("NSMutableString"), "string");
    w.send(key, "appendFormat:", {w.nsstr("ach%d"), 1u});
    w.send(m, "setObject:forKey:", {v1, key});
    w.send(key, "appendFormat:", {w.nsstr("x")});
    CHECK(w.send(m, "objectForKey:", {w.nsstr("ach1")}) == v1);
    CHECK(w.send(m, "objectForKey:", {key}) == 0u);
    w.send(m, "setObject:forKey:", {v2, w.nsstr("ach1")});
    CHECK(w.send(m, "count") == 1u);
    CHECK(w.send(m, "objectForKey:", {w.nsstr("ach1")}) == v2);
    // Number keys compare by value.
    w.send(m, "setObject:forKey:", {v3, mt_cf::boolean(&w.cpu, true)});
    CHECK(w.send(m, "objectForKey:", {mt_cf::boolean(&w.cpu, true)}) == v3);
    CHECK(raises(w, [](Ns &a) {
        a.send(a.send(a.cls("NSMutableDictionary"), "dictionaryWithCapacity:", {1}),
               "setObject:forKey:", {0, a.nsstr("k")});
    }));
    // [[NSDictionary alloc] init]: CF's resource properties.
    const u32 empty = w.send(w.send(w.cls("NSDictionary"), "alloc"), "init");
    CHECK(w.send(empty, "count") == 0u);
    // The host API (#117 reads the drawable properties).
    CHECK(mt_ns::dictionary_get(&w.cpu, d, w.nsstr("k2")) == v2);
    const u32 h = mt_ns::dictionary_create(&w.cpu, false);
    CHECK(mt_ns::dictionary_set(&w.cpu, h, w.nsstr("a"), v1));
    CHECK(w.send(h, "objectForKey:", {w.nsstr("a")}) == v1);
}

// --- NSBundle, NSLocale
// -----------------------------------------------------------------------------

TEST_CASE("Foundation: NSBundle resource paths, Info.plist and localized strings") {
    Bundle b("en");
    Ns w;
    const u32 bundle = w.send(w.cls("NSBundle"), "mainBundle");
    CHECK(bundle != 0u);
    CHECK(w.send(w.cls("NSBundle"), "mainBundle") == bundle);
    w.send(bundle, "release");
    CHECK(w.send(bundle, "retainCount") == 0xFFFFFFFFu);

    // -pathForResource:ofType:inDirectory: (the sound loaders): a guest path in the bundle.
    const u32 p = w.send(bundle, "pathForResource:ofType:inDirectory:",
                         {w.nsstr("bgm01"), w.nsstr("mp3"), w.nsstr("sound")});
    CHECK(w.text(p) == std::string(MT_CF_DEFAULT_BUNDLE_PATH) + "/sound/bgm01.mp3");
    CHECK(w.send(bundle, "pathForResource:ofType:inDirectory:",
                 {w.nsstr("bgm99"), w.nsstr("mp3"), w.nsstr("sound")}) == 0u);
    CHECK(logged("no such resource"));
    // ... which the save-file code may read, not write.
    CHECK(fs::path(mt_ns::host_path(w.text(p), false)).lexically_normal() ==
          (b.dir / "sound/bgm01.mp3").lexically_normal());
    CHECK(mt_ns::host_path(w.text(p), true).empty());

    // -objectForInfoDictionaryKey: localized first (InfoPlist.strings), then Info.plist.
    auto info = [&w, bundle](const char *key) {
        return w.send(bundle, "objectForInfoDictionaryKey:", {w.nsstr(key)});
    };
    CHECK(w.text(info("CFBundleIdentifier")) == "com.example.thief");
    CHECK(w.text(info("CFBundleDisplayName")) == "Maple");
    CHECK(info("UIStatusBarHidden") == mt_cf::boolean(&w.cpu, true));
    mt_cf::Number n;
    REQUIRE(mt_cf::number_value(&w.cpu, info("Count"), &n));
    CHECK(n.i == -7);
    const u32 family = info("UIDeviceFamily");
    CHECK(std::string(w.class_name(family)) == "NSArray");
    CHECK(w.send(family, "count") == 2u);
    CHECK(info("NoSuchKey") == 0u);
    // kCFBundleIdentifierKey is a CF constant string (CStateTitle's key).
    const u32 slot = mt_heap_calloc(w.mem, 1, 4);
    mt_hle_init_kCFBundleIdentifierKey(&w.cpu, 0, slot);
    mt_cf::make_constants(&w.cpu);
    CHECK(w.text(w.send(bundle, "objectForInfoDictionaryKey:", {w.get32(slot)})) ==
          "com.example.thief");

    // -localizedStringForKey:value:table: (LocalizedCString, table "Scripts").
    auto loc = [&w, bundle](const char *key, u32 value, const char *table) {
        return w.send(bundle, "localizedStringForKey:value:table:",
                      {w.nsstr(key), value, table != nullptr ? w.nsstr(table) : 0u});
    };
    CHECK(w.text(loc("Bruce.dialog.1", 0, "Scripts")) == "I'm Bruce.");
    CHECK(w.text(loc("empty", 0, "Scripts")).empty());
    CHECK(w.text(loc("missing", 0, "Scripts")) == "missing");
    const u32 fallback = w.nsstr("fallback");
    CHECK(loc("missing", fallback, "Scripts") == fallback);
    CHECK(w.text(loc("missing", w.nsstr(""), "Scripts")) == "missing");
    CHECK(w.text(loc("Bruce.dialog.1", 0, nullptr)) == "Bruce.dialog.1"); // no Localizable
    CHECK(w.text(loc("x", 0, "broken")) == "x");
    CHECK(logged("broken.strings: not a binary plist"));
    CHECK(w.text(w.send(bundle, "localizedStringForKey:value:table:", {0, 0, 0})).empty());
}

TEST_CASE("Foundation: a text pack's Scripts.strings answers first; a key it lacks falls back") {
    Bundle b("en");
    Ns w;
    const fs::path pack = unique_dir("textpack");
    fs::create_directories(pack);
    std::ofstream(pack / "textpack.strings")
        << "\"format\" = \"1\"; \"name\" = \"t\"; \"language\" = \"de\";";
    std::ofstream(pack / "Scripts.strings")
        << "\"Bruce.dialog.1\" = \"Ich bin Bruce aus H\xC3\xA9n\xC3\xA9sys.\";";
    const u32 bundle = w.send(w.cls("NSBundle"), "mainBundle");
    auto loc = [&w, bundle](const char *key, const char *table) {
        return w.send(bundle,
                      "localizedStringForKey:value:table:", {w.nsstr(key), 0u, w.nsstr(table)});
    };
    REQUIRE(mt_textpack_open(pack.string().c_str()) == 1);
    // The game's LocalizedCString then takes -UTF8String: the pack's UTF-8.
    CHECK(w.utf8_of(loc("Bruce.dialog.1", "Scripts")) ==
          "Ich bin Bruce aus H\xC3\xA9n\xC3\xA9sys.");
    CHECK(w.text(loc("empty", "Scripts")).empty()); // not in the pack: the bundle's
    CHECK(w.text(loc("missing", "Scripts")) == "missing");
    // TextCache_DrawText's +stringWithCString:encoding: EUC-KR (0x80000940) reads it whole.
    const u32 drawn =
        w.send(w.cls("NSString"),
               "stringWithCString:encoding:", {w.cstr("H\xC3\xA9n\xC3\xA9sys"), 0x80000940u});
    CHECK(w.text(drawn) == "H\xC3\xA9n\xC3\xA9sys");
    mt_textpack_close();
    CHECK(w.text(loc("Bruce.dialog.1", "Scripts")) == "I'm Bruce.");
    std::error_code ec;
    fs::remove_all(pack, ec);
}

TEST_CASE("Foundation: NSBundle and NSLocale follow the language") {
    Bundle b("ko");
    Ns w;
    const u32 bundle = w.send(w.cls("NSBundle"), "mainBundle");
    CHECK(w.text(w.send(bundle, "localizedStringForKey:value:table:",
                        {w.nsstr("Bruce.dialog.1"), 0, w.nsstr("Scripts")})) ==
          "\xEB\xB8\x8C\xEB\xA3\xA8\xEC\x8A\xA4");
    // No ko.lproj/InfoPlist.strings: CF's resource lookup falls back to the development region
    // (en.lproj), as CFBundle does; Info.plist's own value only when no lproj has the file.
    CHECK(w.text(w.send(bundle, "objectForInfoDictionaryKey:", {w.nsstr("CFBundleDisplayName")})) ==
          "Maple");

    const u32 locale = w.send(w.cls("NSLocale"), "currentLocale");
    CHECK(w.send(w.cls("NSLocale"), "currentLocale") == locale);
    const u32 slot = mt_heap_calloc(w.mem, 1, 8);
    mt_hle_init_NSLocaleCountryCode(&w.cpu, 0, slot);
    mt_hle_init_NSLocaleLanguageCode(&w.cpu, 0, slot + 4);
    mt_cf::make_constants(&w.cpu);
    CHECK(w.text(w.send(locale, "objectForKey:", {w.get32(slot)})) == "KR");
    CHECK(w.text(w.send(locale, "objectForKey:", {w.get32(slot + 4)})) == "ko");
    CHECK(w.send(locale, "objectForKey:", {w.nsstr("kCFLocaleCurrencyCodeKey")}) == 0u);
    const u32 langs = w.send(w.cls("NSLocale"), "preferredLanguages");
    CHECK(w.send(langs, "count") == 2u);
    CHECK(w.text(w.send(langs, "objectAtIndex:", {0})) == "ko");
    CHECK(w.text(w.send(langs, "objectAtIndex:", {1})) == "en");

    mt_cf_config cfg = mt_cf_config_default();
    cfg.bundle_dir = b.path.c_str();
    mt_cf_configure(&cfg); // English
    CHECK(w.text(w.send(locale, "objectForKey:", {w.get32(slot)})) == "US");
    const u32 en = w.send(w.cls("NSLocale"), "preferredLanguages");
    CHECK(w.send(en, "count") == 1u);
    CHECK(w.text(w.send(en, "objectAtIndex:", {0})) == "en");
}

// --- binary plists
// -----------------------------------------------------------------------------------

TEST_CASE("Foundation: the binary plist reader") {
    mt_ns::PlistValue v;
    REQUIRE(parse(bplist(dict({{"s", str("ascii")},
                               {"u", ustr(u"브")},
                               {"i", num(-2)},
                               {"b", yes(false)},
                               {"a", arr({num(1), str("two")})},
                               {"long", str("a string longer than fifteen bytes")}})),
                  &v));
    CHECK(v.kind == mt_ns::PlistValue::dict);
    CHECK(v.entries.size() == 6u);
    CHECK(v.find(u"s")->s == u"ascii");
    CHECK(v.find(u"u")->s == u"브");
    CHECK(v.find(u"i")->i == -2);
    CHECK(v.find(u"b")->kind == mt_ns::PlistValue::boolean);
    CHECK(v.find(u"a")->items.size() == 2u);
    CHECK(v.find(u"a")->items[1].s == u"two");
    CHECK(v.find(u"long")->s.size() == 34u);
    CHECK(v.find(u"none") == nullptr);

    // Reals, data and 1-byte integers, written by hand: [1.5 (f64), 0.5 (f32), <01 02>, 200].
    std::string raw = "bplist00";
    const std::string objects[] = {
        std::string("\xA4\x01\x02\x03\x04", 5),
        std::string("\x23\x3F\xF8\x00\x00\x00\x00\x00\x00", 9),
        std::string("\x22\x3F\x00\x00\x00", 5),
        std::string("\x42\x01\x02", 3),
        std::string("\x10\xC8", 2),
    };
    std::vector<std::size_t> offs;
    for (const auto &o : objects) {
        offs.push_back(raw.size());
        raw += o;
    }
    const std::size_t table = raw.size();
    for (const std::size_t o : offs) {
        raw.push_back(static_cast<char>(o));
    }
    std::string trailer(32, '\0');
    trailer[6] = 1;
    trailer[7] = 1;
    trailer[15] = 5;
    trailer[31] = static_cast<char>(table);
    raw += trailer;
    REQUIRE(parse(raw, &v));
    REQUIRE(v.items.size() == 4u);
    CHECK(v.items[0].f == 1.5);
    CHECK(v.items[1].f == 0.5);
    CHECK(v.items[2].bytes == std::vector<std::uint8_t>{1, 2});
    CHECK(v.items[3].i == 200);

    // Malformed files fail with a reason, never crash.
    std::string error;
    CHECK_FALSE(parse("bplist00", &v, &error));
    CHECK(error == "not a binary plist");
    CHECK_FALSE(parse("<?xml version=\"1.0\"?><plist/>", &v, &error));
    const std::string good = bplist(arr({str("x"), arr({num(1)})}));
    std::string bad = good;
    bad[bad.size() - 25] = 9; // object ref size 9
    CHECK_FALSE(parse(bad, &v, &error));
    CHECK(error == "bad trailer");
    bad = good;
    bad[bad.size() - 9] = 7; // top object out of range
    CHECK_FALSE(parse(bad, &v, &error));
    // A cycle: the array (object 0) holds itself.
    std::string cyc = "bplist00\xA1";
    cyc.push_back('\0');
    cyc += std::string("\x08", 1);
    std::string t2(32, '\0');
    t2[6] = 1;
    t2[7] = 1;
    t2[15] = 1;
    t2[31] = 10;
    CHECK_FALSE(parse(cyc + t2, &v, &error));
    CHECK(error == "reference cycle");
    // A reference past the object count (object 7 of 1).
    std::string oob = "bplist00\xA1";
    oob.push_back('\x07');
    oob += std::string("\x08", 1);
    CHECK_FALSE(parse(oob + t2, &v, &error));
    CHECK(error == "object reference out of range");
    // Every truncation and every single-byte change of a good file: no crash.
    for (std::size_t n = 0; n < good.size(); n++) {
        parse(good.substr(0, n), &v);
        for (const int delta : {1, 0x0F, 0x80}) {
            std::string m = good;
            m[n] = static_cast<char>(m[n] + delta);
            parse(m, &v);
        }
    }
    // Shared references that would blow up are bounded: 20 levels of [x, x], each the level below.
    std::string deep = "bplist00";
    std::vector<std::size_t> o2;
    for (int level = 0; level < 20; level++) {
        o2.push_back(deep.size());
        deep +=
            std::string("\xA2", 1) + static_cast<char>(level + 1) + static_cast<char>(level + 1);
    }
    o2.push_back(deep.size());
    deep += std::string("\x08", 1);
    const std::size_t t3 = deep.size();
    for (const std::size_t o : o2) {
        deep.push_back(static_cast<char>(o));
    }
    std::string tr(32, '\0');
    tr[6] = 1;
    tr[7] = 1;
    tr[15] = 21;
    tr[31] = static_cast<char>(t3);
    CHECK_FALSE(parse(deep + tr, &v, &error));
    CHECK(error == "too many object references");
}

namespace {

// A binary plist of hand-written objects (1-byte offsets and references), object `top` the root.
std::string raw_bplist(const std::vector<std::string> &objects, std::uint8_t top = 0) {
    std::string out = "bplist00";
    std::vector<std::size_t> offsets;
    for (const std::string &o : objects) {
        offsets.push_back(out.size());
        out += o;
    }
    const std::size_t table = out.size();
    for (const std::size_t o : offsets) {
        out.push_back(static_cast<char>(o));
    }
    std::string trailer(32, '\0');
    trailer[6] = 1;
    trailer[7] = 1;
    trailer[15] = static_cast<char>(objects.size());
    trailer[23] = static_cast<char>(top);
    trailer[31] = static_cast<char>(table);
    return out + trailer;
}

} // namespace

TEST_CASE("Foundation: the binary plist reader's refusals, each by its message (#413)") {
    mt_ns::PlistValue v;
    std::string error;
    const auto refused = [&v, &error](const std::string &file, const char *message) {
        error.clear();
        CHECK_FALSE(parse(file, &v, &error));
        CHECK(error == message);
    };
    // Depth: 64 nested arrays around a number are read; 65 are not.
    V nested = num(7);
    for (int i = 0; i < 64; i++) {
        nested = arr({nested});
    }
    REQUIRE(parse(bplist(nested), &v, &error));
    refused(bplist(arr({nested})), "nested too deeply");
    // A cycle through two arrays: 0 holds 1, 1 holds 0.
    refused(raw_bplist({std::string("\xA1\x01", 2), std::string("\xA1\x00", 2)}),
            "reference cycle");
    // The shared-reference bound: count x 16 + 4,096 objects parsed. 13 levels of [x, x] parse
    // 2^14 - 1 = 16,383 objects (over 14 x 16 + 4,096); 11 levels parse 4,095.
    const auto doubling = [](int levels) {
        std::vector<std::string> objects;
        for (int level = 0; level < levels; level++) {
            objects.push_back(std::string("\xA2", 1) + static_cast<char>(level + 1) +
                              static_cast<char>(level + 1));
        }
        objects.emplace_back("\x08", 1);
        return raw_bplist(objects);
    };
    REQUIRE(parse(doubling(11), &v, &error));
    refused(doubling(13), "too many object references");
    // The trailer and the root.
    refused("bplist00", "not a binary plist");
    refused("bplist01" + std::string(40, '\0'), "not a binary plist");
    std::string top = raw_bplist({std::string("\x08", 1)});
    top[top.size() - 9] = 1; // the root, object 1 of 1
    refused(top, "bad trailer");
    std::string cut = raw_bplist({std::string("\x08", 1), std::string("\x09", 1)});
    cut[cut.size() - 17] = 9; // 9 objects, a table of 2
    refused(cut, "bad trailer");
    // Truncated or out-of-range objects, each with its own message.
    refused(raw_bplist({std::string("\x5F\x10\xF0", 3) + "ab"}), "object runs past the end");
    refused(raw_bplist({std::string("\x4F\x10\xF0", 3)}), "object runs past the end");
    refused(raw_bplist({std::string("\x5F\x20\x01", 3)}), "bad length object");
    refused(raw_bplist({std::string("\x6F\x10\xF0", 3)}), "UTF-16 string out of bounds");
    refused(raw_bplist({std::string("\xAF\x10\xF0", 3)}), "collection out of bounds");
    refused(raw_bplist({std::string("\xA1\x05", 2)}), "object reference out of range");
    std::string offset = raw_bplist({std::string("\x08", 1)});
    offset[offset.size() - 33] = static_cast<char>(offset.size() - 4); // into the trailer
    refused(offset, "object offset out of bounds");
    refused(raw_bplist({std::string("\x15", 1)}), "integer too wide");
    refused(raw_bplist({std::string("\x24", 1)}), "bad real or date");
    refused(raw_bplist({std::string("\x07", 1)}), "unknown simple object");
    refused(raw_bplist({std::string("\xF0", 1)}), "unknown object type");
    refused(raw_bplist({std::string("\x52\x41\xC3", 3)}), "non-ASCII byte in an ASCII string");
    refused(raw_bplist({std::string("\xD1\x01\x01", 3), std::string("\x10\x01", 2)}),
            "dictionary key is not a string");
    // Every truncation of a good file is refused with one of the messages above, never "".
    const std::string good = bplist(dict({{"k", arr({str("x"), num(-1), yes(true)})}}));
    for (std::size_t n = 0; n < good.size(); n++) {
        CAPTURE(n);
        error.clear();
        CHECK_FALSE(parse(good.substr(0, n), &v, &error));
        CHECK_FALSE(error.empty());
    }
}

// --- the save files
// ----------------------------------------------------------------------------------

namespace {

// SaveIO_DocumentsPath 0x6EF1: [[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"]
// stringByAppendingPathComponent:[NSString stringWithUTF8String:name]]
u32 documents_path(Ns &w, const char *name) {
    w.cpu.r[0] = 0;
    mt_hle_NSHomeDirectory(&w.cpu, 0);
    const u32 home = w.cpu.r[0];
    const u32 docs = w.send(home, "stringByAppendingPathComponent:", {w.nsstr("Documents")});
    return w.send(docs, "stringByAppendingPathComponent:", {w.nsstr(name)});
}

// A new character (docs/formats/save.md, "A new character"), its EXP and meso from `seed`: a
// character file that mt_saves_check passes.
std::string character(char seed) {
    std::string b(0x464, '\0');
    b[2] = 1;
    b[3] = seed, b[0x11] = seed;
    for (std::size_t i = 0; i < 48; i++) {
        b[0x015 + 16 * i] = '\xFF';
        b[0x325 + 3 * i] = '\xFF';
    }
    b[0x325] = 3, b[0x326] = 5, b[0x328] = 6, b[0x329] = 5;
    for (std::size_t i = 0; i < 5; i++) {
        b[0x3C2 + 16 * i] = '\xFF';
    }
    b[0x3C2 + 16] = 0x48;
    b[0x41C] = 2, b[0x41D] = 8;
    return b;
}

// The game's write (0xB729 after opening in mode 8): create the file if missing, then
// fileHandleForUpdatingAtPath:, writeData: of [[NSData alloc] initWithBytes:length:],
// synchronizeFile, closeFile.
void game_write(Ns &w, const char *name, const std::string &bytes) {
    const u32 path = documents_path(w, name);
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    if (w.send(fm, "fileExistsAtPath:", {path}) == 0) {
        CHECK(w.send(fm, "createFileAtPath:contents:attributes:", {path, 0, 0}) == 1u);
    }
    const u32 h = w.send(w.cls("NSFileHandle"), "fileHandleForUpdatingAtPath:", {path});
    REQUIRE(h != 0u);
    const u32 d = w.data(bytes);
    w.send(h, "writeData:", {d});
    w.send(d, "release");
    w.send(h, "synchronizeFile");
    w.send(h, "closeFile");
}

// The game's read (0xB7C9): the size from availableData.length, then readDataOfLength: and
// getBytes: into a buffer.
std::string game_read(Ns &w, const char *name) {
    const u32 path = documents_path(w, name);
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    if (w.send(fm, "fileExistsAtPath:", {path}) == 0) {
        return "<missing>";
    }
    const u32 r = w.send(w.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {path});
    REQUIRE(r != 0u);
    const u32 size = w.send(w.send(r, "availableData"), "length");
    w.send(r, "closeFile");
    const u32 h = w.send(w.cls("NSFileHandle"), "fileHandleForUpdatingAtPath:", {path});
    const u32 d = w.send(h, "readDataOfLength:", {size});
    const u32 buf = mt_heap_calloc(w.mem, 1, size + 1);
    w.send(d, "getBytes:", {buf});
    w.send(h, "closeFile");
    return std::string(reinterpret_cast<const char *>(w.cpu.mem + buf), size);
}

} // namespace

TEST_CASE("Foundation: save files written through NSFileHandle persist across process restarts") {
    const fs::path dir = unique_dir("saves");
    // Files the checks pass (#413): a new character and a quest file with nothing started. The
    // file layer's own behaviour (offsets, truncation) is shown on cupon, which is not checked.
    const std::string theif = character(42), quest(4781, '\0');
    std::string cupon(5962, '\0');
    for (std::size_t i = 0; i < cupon.size(); i++) {
        cupon[i] = static_cast<char>(i * 7 + 1);
    }
    {
        Ns w(dir);
        CHECK(game_read(w, "theif0") == "<missing>");
        game_write(w, "theif0", theif);
        game_write(w, "quest0", quest);
        game_write(w, "cupon", cupon);
        CHECK(game_read(w, "theif0") == theif); // and within the process
    } // the process ends: runtime, guest memory and every object are gone
    {
        Ns w(dir);
        CHECK(game_read(w, "theif0") == theif);
        CHECK(game_read(w, "quest0") == quest);
        // On the host: <data dir>/Documents/<name>, the bytes as the game wrote them.
        CHECK(read_host(dir / "Documents" / "theif0") == theif);
        // The game writes at offset 0 and never truncates: a shorter write keeps the tail.
        game_write(w, "cupon", "AB");
        CHECK(game_read(w, "cupon") == "AB" + cupon.substr(2));
        // Mode 4: fileHandleForWritingAtPath: + truncateFileAtOffset:0, then writeData:.
        const u32 path = documents_path(w, "cupon");
        const u32 h = w.send(w.cls("NSFileHandle"), "fileHandleForWritingAtPath:", {path});
        REQUIRE(h != 0u);
        w.send(h, "truncateFileAtOffset:", {0, 0});
        w.send(h, "writeData:", {w.data("xyz")});
        w.send(h, "closeFile");
        CHECK(game_read(w, "cupon") == "xyz");
        // A handle for writing does not read; a closed one does nothing more.
        CHECK(raises(w, [](Ns &x) {
            const u32 p = documents_path(x, "theif0");
            x.send(x.send(x.cls("NSFileHandle"), "fileHandleForWritingAtPath:", {p}),
                   "availableData");
        }));
        CHECK(raises(w, [](Ns &x) {
            const u32 p = documents_path(x, "theif0");
            const u32 hh = x.send(x.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {p});
            x.send(hh, "closeFile");
            x.send(hh, "availableData");
        }));
        // Missing files: no handle (the game checks fileExistsAtPath: first anyway).
        CHECK(w.send(w.cls("NSFileHandle"),
                     "fileHandleForReadingAtPath:", {documents_path(w, "CTerms")}) == 0u);
        CHECK(w.send(w.cls("NSFileHandle"),
                     "fileHandleForUpdatingAtPath:", {documents_path(w, "CTerms")}) == 0u);

        // Deleting a slot (0x9462D): NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
        // NSUserDomainMask, YES)[0], "%@/%@", removeItemAtPath:error:.
        w.cpu.r[0] = 9;
        w.cpu.r[1] = 1;
        w.cpu.r[2] = 1;
        mt_hle_NSSearchPathForDirectoriesInDomains(&w.cpu, 0);
        const u32 dirs = w.cpu.r[0];
        REQUIRE(w.send(dirs, "count") == 1u);
        const u32 docs = w.send(dirs, "objectAtIndex:", {0});
        CHECK(w.text(docs) == w.text(documents_path(w, "")));
        const u32 quest_path = w.send(
            w.cls("NSString"), "stringWithFormat:", {w.nsstr("%@/%@"), docs, w.nsstr("quest0")});
        const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
        CHECK(w.send(fm, "removeItemAtPath:error:", {quest_path, 0}) == 1u);
        CHECK(w.send(fm, "fileExistsAtPath:", {quest_path}) == 0u);
        CHECK_FALSE(fs::exists(dir / "Documents" / "quest0"));
        const u32 err = mt_heap_calloc(w.mem, 1, 4);
        CHECK(w.send(fm, "removeItemAtPath:error:", {quest_path, err}) == 0u);
        CHECK(std::string(w.class_name(w.get32(err))) == "NSError");
        w.cpu.r[0] = 13; // NSCachesDirectory: not one the port has
        w.cpu.r[1] = 1;
        mt_hle_NSSearchPathForDirectoriesInDomains(&w.cpu, 0);
        CHECK(w.send(w.cpu.r[0], "count") == 0u);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --- saving never leaves half a file (#78, ADR 0074) ---------------------------------------------

namespace {

// Two versions of a character file: what the slot held, what the game saves over it.
std::string version(char seed) { return character(seed); }

const char *fault_name(mt_saves_fault f) {
    switch (f) {
    case MT_SAVES_FAULT_OPENED:
        return "the temporary file opened";
    case MT_SAVES_FAULT_HALF_WRITTEN:
        return "half the bytes written";
    case MT_SAVES_FAULT_WRITTEN:
        return "all written, not renamed";
    default:
        return "none";
    }
}

} // namespace

TEST_CASE("Foundation: a save killed at any point leaves the old file whole, and the next one "
          "saves") {
    const std::string old_bytes = version(1), new_bytes = version(2);
    for (const mt_saves_fault fault :
         {MT_SAVES_FAULT_OPENED, MT_SAVES_FAULT_HALF_WRITTEN, MT_SAVES_FAULT_WRITTEN}) {
        CAPTURE(fault_name(fault));
        const fs::path dir = unique_dir("killed");
        const fs::path file = dir / "Documents" / "theif0";
        const fs::path tmp = dir / "Documents" / ("theif0" MT_SAVES_TMP_SUFFIX);
        {
            Ns w(dir);
            game_write(w, "theif0", old_bytes);
        }
        CHECK(read_host(file) == old_bytes);
        CHECK_FALSE(fs::exists(tmp));
        {
            Ns w(dir);
            // The process dies at `fault` in the game's save: create (exists) -> updating handle
            // -> writeData: -> synchronizeFile -> closeFile. Every replacement stops there, the
            // handle's end included: nothing after the kill reaches the disk.
            mt_saves_set_fault(fault);
            game_write(w, "theif0", new_bytes);
            CHECK(read_host(file) == old_bytes);
            CHECK(fs::exists(tmp)); // what the killed process left
            CHECK(logged("cannot save"));
        }
        mt_saves_set_fault(MT_SAVES_FAULT_NONE);
        CHECK(read_host(file) == old_bytes);
        {
            Ns w(dir); // the next launch: the slot as it was saved before
            CHECK(game_read(w, "theif0") == old_bytes);
            game_write(w, "theif0", new_bytes);
            CHECK(game_read(w, "theif0") == new_bytes);
        }
        CHECK(read_host(file) == new_bytes);
        CHECK_FALSE(fs::exists(tmp)); // the leftover is overwritten and renamed away
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

TEST_CASE("Foundation: a new slot killed before its first save leaves no file") {
    const fs::path dir = unique_dir("new-slot");
    {
        Ns w(dir);
        mt_saves_set_fault(MT_SAVES_FAULT_HALF_WRITTEN);
        game_write(w, "theif1", version(3)); // LoadSaveData's first write of a new character
        CHECK_FALSE(fs::exists(dir / "Documents" / "theif1"));
    }
    mt_saves_set_fault(MT_SAVES_FAULT_NONE);
    {
        Ns w(dir);
        CHECK(game_read(w, "theif1") == "<missing>"); // an empty slot, not an empty file
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Foundation: a file created with no contents appears when a handle on it saves") {
    Ns w;
    const fs::path file = w.data_dir / "Documents" / "quest2";
    const u32 path = documents_path(w, "quest2");
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:", {path, 0, 0}) == 1u);
    CHECK(w.send(fm, "fileExistsAtPath:", {path}) == 1u); // for the game it is there
    CHECK_FALSE(fs::exists(file));                        // on the disk not yet
    // Removed before any save: gone for both.
    CHECK(w.send(fm, "removeItemAtPath:error:", {path, 0}) == 1u);
    CHECK(w.send(fm, "fileExistsAtPath:", {path}) == 0u);
    // A handle that opens it reads nothing; one that writes saves it.
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:", {path, 0, 0}) == 1u);
    const u32 r = w.send(w.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {path});
    REQUIRE(r != 0u);
    CHECK(w.send(w.send(r, "availableData"), "length") == 0u);
    w.send(r, "closeFile");
    CHECK_FALSE(fs::exists(file));
    const u32 h = w.send(w.cls("NSFileHandle"), "fileHandleForUpdatingAtPath:", {path});
    REQUIRE(h != 0u);
    w.send(h, "closeFile"); // even without a write: the file the game created now exists
    CHECK(fs::exists(file));
    CHECK(read_host(file).empty());
    // createFileAtPath: with contents writes them at once (CTerms).
    const u32 cterms = documents_path(w, "CTerms");
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:",
                 {cterms, w.data(std::string("\x00\x00", 2)), 0}) == 1u);
    CHECK(read_host(w.data_dir / "Documents" / "CTerms") == std::string("\x00\x00", 2));
    std::error_code ec;
    fs::remove_all(w.data_dir, ec);
}

TEST_CASE("Foundation: a config with speed byte 3 loads as a valid speed") {
    Ns w;
    const fs::path file = w.data_dir / "Documents" / "config";
    fs::create_directories(file.parent_path());
    const auto put = [&file](const std::string &bytes) {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    };
    // The defaults (CDataMgr::LoadConfig 0x38611) and every valid value come through as they are.
    for (const char speed : std::initializer_list<char>{0, 1, 2}) {
        const std::string valid{3, speed, 0, 1, 1, speed};
        put(valid);
        g_log.clear();
        CHECK(game_read(w, "config") == valid);
        CHECK_FALSE(logged("past the game's tables"));
    }
    // Speed 3 (ApplyGameSpeed 0x37C41 read table 0x108C58[3]: a tick length of -1) and byte 5
    // past table 0x108C4C read as 2, the fastest speed; the file itself is not changed.
    const std::string bad{3, 3, 0, 1, 1, 3};
    put(bad);
    CHECK(game_read(w, "config") == std::string{3, 2, 0, 1, 1, 2});
    CHECK(logged("config holds values past the game's tables"));
    CHECK(read_host(file) == bad);
    put(std::string{3, '\xFF', 7, 9, 9, '\x80'});
    CHECK(game_read(w, "config") == std::string{3, 2, 7, 9, 9, 2}); // only bytes 1 and 5
    // Saving it back (the options menu) writes what the game holds: valid values.
    game_write(w, "config", game_read(w, "config"));
    CHECK(read_host(file) == std::string{3, 2, 7, 9, 9, 2});
    // Other files with the same bytes are not touched; a short config neither crashes nor grows.
    const std::string cupon{3, 3, 3, 3, 3, 3};
    game_write(w, "cupon", cupon);
    CHECK(game_read(w, "cupon") == cupon);
    put(std::string{1, 9});
    CHECK(game_read(w, "config") == std::string{1, 2});
    std::error_code ec;
    fs::remove_all(w.data_dir, ec);
}

namespace {

// Every file under `dir` with its bytes (relative path -> bytes).
std::map<std::string, std::string> tree(const fs::path &dir) {
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) {
            files[fs::relative(it->path(), dir).generic_string()] = read_host(it->path());
        }
    }
    return files;
}

struct Refusal {
    std::string name, why, moved_to;
};

void on_refused(const char *name, const char *why, const char *moved_to, void *user) {
    static_cast<std::vector<Refusal> *>(user)->push_back({name, why, moved_to});
}

} // namespace

TEST_CASE("Foundation: a theif or quest copied in that fails the checks is moved aside, and the "
          "game sees no file (#413)") {
    Ns w;
    std::vector<Refusal> heard;
    mt_saves_set_refused_hook(on_refused, &heard);
    const fs::path docs = w.data_dir / "Documents";
    fs::create_directories(docs);
    const auto put = [&docs](const char *name, const std::string &bytes) {
        std::ofstream(docs / name, std::ios::binary | std::ios::trunc) << bytes;
    };
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    // A valid character copied in comes through as it is; so does the game's own save.
    put("theif0", character(5));
    CHECK(game_read(w, "theif0") == character(5));
    game_write(w, "theif0", character(9));
    CHECK(game_read(w, "theif0") == character(9));
    CHECK(heard.empty());
    CHECK_FALSE(fs::exists(w.data_dir / MT_SAVES_REFUSED_DIR));
    // Level 0 (hero.dat has levels 1-100): fileExistsAtPath: says NO, and the file is moved.
    std::string bad = character(5);
    bad[2] = 0;
    put("theif1", bad);
    put("config", std::string{3, 9, 0, 1, 1, 9}); // only sanitised, never moved
    const std::map<std::string, std::string> before = tree(w.data_dir);
    CHECK(w.send(fm, "fileExistsAtPath:", {documents_path(w, "theif1")}) == 0u);
    REQUIRE(heard.size() == 1u);
    CHECK(heard[0].name == "theif1");
    CHECK(heard[0].why.find("level") != std::string::npos);
    CHECK(logged("theif1 refused"));
    const fs::path aside = fs::path(heard[0].moved_to);
    CHECK(aside.parent_path() == w.data_dir / MT_SAVES_REFUSED_DIR);
    CHECK(read_host(aside / "theif1") == bad);
    CHECK(read_host(aside / MT_SAVES_REFUSED_REASON).rfind("theif1: bad_level: ", 0) == 0u);
    // The data folder is as it was, apart from the moved file.
    std::map<std::string, std::string> after = tree(w.data_dir);
    const std::string rel = fs::relative(aside, w.data_dir).generic_string();
    CHECK(after.erase(rel + "/theif1") == 1u);
    CHECK(after.erase(rel + "/" MT_SAVES_REFUSED_REASON) == 1u);
    std::map<std::string, std::string> expected = before;
    CHECK(expected.erase("Documents/theif1") == 1u);
    CHECK(after == expected);
    CHECK(game_read(w, "theif1") == "<missing>"); // asked again: still no file, nothing heard
    CHECK(heard.size() == 1u);
    // A handle opened straight on a broken quest file is nil; one far too large is not read.
    put("quest2", "not a quest");
    CHECK(w.send(w.cls("NSFileHandle"),
                 "fileHandleForReadingAtPath:", {documents_path(w, "quest2")}) == 0u);
    put("theif2", std::string(1024 * 1024, '\x01'));
    CHECK(w.send(w.cls("NSFileHandle"),
                 "fileHandleForUpdatingAtPath:", {documents_path(w, "theif2")}) == 0u);
    REQUIRE(heard.size() == 3u);
    CHECK(heard[1].name == "quest2");
    CHECK(heard[2].name == "theif2");
    CHECK(heard[2].why.find("1048576 bytes") != std::string::npos);
    CHECK_FALSE(fs::exists(docs / "quest2"));
    CHECK_FALSE(fs::exists(docs / "theif2"));
    CHECK(fs::exists(fs::path(heard[2].moved_to) / "theif2"));
    // The game writes a new character over the empty slot.
    game_write(w, "theif1", character(7));
    CHECK(game_read(w, "theif1") == character(7));
    CHECK(read_host(docs / "config") == std::string{3, 9, 0, 1, 1, 9});
    mt_saves_set_refused_hook(nullptr, nullptr);
    std::error_code ec;
    fs::remove_all(w.data_dir, ec);
}

TEST_CASE("Foundation: Documents can live apart from the data directory (the iOS app's own)") {
    const fs::path docs = unique_dir("app-documents");
    Ns w;
    REQUIRE(mt_saves_set_documents_dir((docs.string() + "/").c_str()) == 0);
    CHECK(std::string(mt_saves_documents_dir()) == docs.string());
    w.cpu.r[0] = 0;
    mt_hle_NSHomeDirectory(&w.cpu, 0);
    const std::string home = w.text(w.cpu.r[0]);
    CHECK(mt_ns::host_path(home + "/Documents", true) == docs.string());
    CHECK(mt_ns::host_path(home + "/Documents/theif0", true) == docs.string() + "/theif0");
    CHECK(mt_ns::host_path(home + "/Library/x", true) == w.data_dir.string() + "/Library/x");
    CHECK(mt_ns::host_path(home + "/Documentsx", true) == w.data_dir.string() + "/Documentsx");
    game_write(w, "theif0", version(4));
    CHECK(read_host(docs / "theif0") == version(4));
    CHECK_FALSE(fs::exists(w.data_dir / "Documents" / "theif0"));
    CHECK(game_read(w, "theif0") == version(4));
    REQUIRE(mt_saves_set_documents_dir(nullptr) == 0);
    CHECK(fs::path(mt_saves_documents_dir()) == w.data_dir / "Documents"); // the default again
    std::error_code ec;
    fs::remove_all(docs, ec);
    fs::remove_all(w.data_dir, ec);
}

TEST_CASE("Foundation: guest paths reach the home directory and the bundle only") {
    Bundle b("en");
    Ns w;
    w.cpu.r[0] = 0;
    mt_hle_NSHomeDirectory(&w.cpu, 0);
    const std::string home = w.text(w.cpu.r[0]);
    CHECK(home == "/var/mobile/Applications/00000000-0000-0000-0000-000000000000");
    CHECK(home == mt_ns::home_guest_path());
    const std::string data = w.data_dir.string();
    // Compared as paths: on Windows the data directory's own separators are '\\'.
    CHECK(fs::path(mt_ns::host_path(home + "/Documents/theif0", true)) ==
          fs::path(data + "/Documents/theif0"));
    CHECK(mt_ns::host_path(home + "//Documents/./theif0", true).empty());
    CHECK(fs::path(mt_ns::host_path(home + "/Documents//theif0/", true)) ==
          fs::path(data + "/Documents/theif0"));
    CHECK(mt_ns::host_path(home + "/Documents/../../etc", false).empty());
    CHECK(mt_ns::host_path("/etc/passwd", false).empty());
    CHECK(mt_ns::host_path("Documents/theif0", false).empty());
    CHECK(mt_ns::host_path(home + "x/Documents", false).empty());
    const u32 outside = w.nsstr("/etc/hosts");
    CHECK(w.send(w.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {outside}) == 0u);
    CHECK(logged("is outside the app's home directory"));
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    CHECK(w.send(fm, "fileExistsAtPath:", {outside}) == 0u);
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:", {outside, 0, 0}) == 0u);
    // createFileAtPath: replaces a file, with the contents given.
    const u32 cterms = w.send(w.nsstr((home + "/Documents/CTerms").c_str()), "description");
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:",
                 {cterms, w.data(std::string("\x01\x00", 2)), 0}) == 1u);
    CHECK(read_host(w.data_dir / "Documents" / "CTerms") == std::string("\x01\x00", 2));
    CHECK(w.send(fm, "createFileAtPath:contents:attributes:", {cterms, 0, 0}) == 1u);
    CHECK(read_host(w.data_dir / "Documents" / "CTerms").empty());
    std::error_code ec;
    fs::remove_all(w.data_dir, ec);
}

TEST_CASE("Foundation: a guest path with a backslash, a drive letter or NUL is refused, and the "
          "home and Documents folders cannot be removed (#324)") {
    Ns w;
    const std::string home = mt_ns::home_guest_path();
    // On every system: a '\\' or ':' inside a component would be a separator, a drive or a
    // stream on Windows.
    CHECK(mt_ns::host_path(home + "/Documents/..\\..\\etc", false).empty());
    CHECK(mt_ns::host_path(home + "/Documents/a\\b", true).empty());
    CHECK(mt_ns::host_path(home + "/C:/Windows", false).empty());
    CHECK(mt_ns::host_path(home + "/Documents/C:theif0", true).empty());
    CHECK(mt_ns::host_path(home + "/Documents/theif0:stream", true).empty());
    CHECK(mt_ns::host_path(home + std::string("/Documents/theif0\0x", 19), true).empty());
    CHECK_FALSE(mt_ns::host_path(home + "/Documents/theif0", true).empty());
    // Removing the home directory or Documents would empty the data folder: refused.
    fs::create_directories(w.data_dir / "Documents");
    std::ofstream(w.data_dir / "Documents" / "theif0", std::ios::binary) << "save";
    std::ofstream(w.data_dir / "settings.ini", std::ios::binary) << "x";
    const u32 fm = w.send(w.cls("NSFileManager"), "defaultManager");
    for (const std::string &root : {home, home + "/", home + "/Documents", home + "//Documents/"}) {
        CAPTURE(root);
        CHECK(w.send(fm, "removeItemAtPath:error:", {w.nsstr(root.c_str()), 0}) == 0u);
    }
    CHECK(read_host(w.data_dir / "Documents" / "theif0") == "save");
    CHECK(read_host(w.data_dir / "settings.ini") == "x");
    // A file in them still can be.
    CHECK(w.send(fm, "removeItemAtPath:error:",
                 {w.nsstr((home + "/Documents/theif0").c_str()), 0}) == 1u);
    CHECK_FALSE(fs::exists(w.data_dir / "Documents" / "theif0"));
    std::error_code ec;
    fs::remove_all(w.data_dir, ec);
}

// --- NSURL, NSError, NSThread, NSClassFromString
// ---------------------------------------------------

TEST_CASE("Foundation: NSURL, NSError, NSThread and NSClassFromString") {
    Ns w;
    // -[MainMenuView clickLinkURL1:]: +URLWithString: for -[UIApplication openURL:] (#54).
    const u32 u = w.send(w.cls("NSURL"), "URLWithString:", {w.nsstr("http://m.nexon.com/")});
    std::string s;
    bool is_file = true;
    REQUIRE(mt_ns::url_string(&w.cpu, u, &s, &is_file));
    CHECK(s == "http://m.nexon.com/");
    CHECK_FALSE(is_file);
    CHECK(w.send(w.cls("NSURL"), "URLWithString:", {0}) == 0u);
    // CMSoundPlayer::LoadBGM: [[NSURL alloc] initFileURLWithPath:] for AVAudioPlayer.
    const u32 f = w.send(w.send(w.cls("NSURL"), "alloc"),
                         "initFileURLWithPath:", {w.nsstr("/b/MapleThief.app/sound/bgm01.mp3")});
    REQUIRE(mt_ns::url_string(&w.cpu, f, &s, &is_file));
    CHECK(s == "/b/MapleThief.app/sound/bgm01.mp3");
    CHECK(is_file);
    CHECK(w.text(w.send(f, "description")) == "file://localhost/b/MapleThief.app/sound/bgm01.mp3");
    w.send(f, "release");
    CHECK_FALSE(mt_ns::url_string(&w.cpu, f, &s, &is_file));

    // NSError: what -removeItemAtPath:error: reports with; it keeps its user info.
    const u32 info = mt_ns::dictionary_create(&w.cpu, false);
    const u32 e =
        w.send(w.cls("NSError"),
               "errorWithDomain:code:userInfo:", {w.nsstr("NSCocoaErrorDomain"), 4u, info});
    CHECK(std::string(w.class_name(e)) == "NSError");
    CHECK(w.retain_count(info) == 2u);

    // +[NSThread isMainThread]: the runtime's thread, not another guest thread.
    CHECK(w.send(w.cls("NSThread"), "isMainThread") == 1u);
    mt_cpu other = w.cpu;
    CHECK(mt_objc_msg(&other, w.cls("NSThread"), mt_objc_sel(w.rt, "isMainThread"), nullptr, 0) ==
          0u);

    // NSClassFromString (+[GameCenterManager IsGameCenterAvailable] asks for GKLocalPlayer).
    w.cpu.r[0] = w.nsstr("GKLocalPlayer");
    mt_hle_NSClassFromString(&w.cpu, 0);
    CHECK(w.cpu.r[0] == w.cls("GKLocalPlayer"));
    w.cpu.r[0] = w.nsstr("NoSuchClass");
    mt_hle_NSClassFromString(&w.cpu, 0);
    CHECK(w.cpu.r[0] == 0u);
    w.cpu.r[0] = 0;
    mt_hle_NSClassFromString(&w.cpu, 0);
    CHECK(w.cpu.r[0] == 0u);

    // NSLocalizedDescriptionKey is a CF constant string.
    const u32 slot = mt_heap_calloc(w.mem, 1, 4);
    mt_hle_init_NSLocalizedDescriptionKey(&w.cpu, 0, slot);
    mt_cf::make_constants(&w.cpu);
    CHECK(w.text(w.get32(slot)) == "NSLocalizedDescription");
}
