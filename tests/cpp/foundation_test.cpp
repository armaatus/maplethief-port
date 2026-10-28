// Foundation (#49, ADR 0035) on a real guest address space and the runtime's own host classes:
// every class through the messages the game sends (with the iOS argument layout), the binary
// plist reader on synthetic files, the host method sets, and the save files written through
// NSFileHandle by one process and read back by the next.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
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
#include "maplethief/stubs.h"
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

// A plist value to write: a tree of these.
struct V {
    enum Kind { ascii, utf16, integer, boolean, array, dict } kind = ascii;
    std::string a;
    std::u16string u;
    std::int64_t i = 0;
    bool b = false;
    std::vector<V> items;
    std::vector<std::pair<std::string, V>> entries;
};
V str(const char *s) { return V{V::ascii, s, {}, 0, false, {}, {}}; }
V ustr(const std::u16string &s) { return V{V::utf16, {}, s, 0, false, {}, {}}; }
V num(std::int64_t i) { return V{V::integer, {}, {}, i, false, {}, {}}; }
V yes(bool b) { return V{V::boolean, {}, {}, 0, b, {}, {}}; }
V arr(std::vector<V> items) { return V{V::array, {}, {}, 0, false, std::move(items), {}}; }
V dict(std::vector<std::pair<std::string, V>> e) {
    return V{V::dict, {}, {}, 0, false, {}, std::move(e)};
}

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

// --- host method sets
// -------------------------------------------------------------------------------

namespace {
void drawing_test_method(mt_cpu *cpu) { cpu->r[0] = 0x51u; }
} // namespace

TEST_CASE("Foundation: host method sets add selectors to another area's class") {
    Ns w;
    // #51's NSString drawing additions, from their own file: no edit of Foundation's tables.
    static const mt_objc_host_method k_drawing[] = {{"sizeWithFont:", drawing_test_method}};
    static const mt_objc_host_methods k_set = {
        "NSString", "UIKit drawing (#51)", k_drawing, 1, nullptr, 0};
    REQUIRE(mt_objc_add_host_methods(w.rt, &k_set) == 0);
    const u32 s = w.nsstr("text");
    CHECK(w.send(s, "sizeWithFont:", {0}) == 0x51u);
    mt_objc_method m;
    REQUIRE(mt_objc_lookup(w.rt, w.cls("NSString"), mt_objc_sel(w.rt, "sizeWithFont:"), &m) == 1);
    CHECK(m.origin == static_cast<u32>(MT_OBJC_HOST));
    CHECK(m.owner == w.cls("NSString"));

    // A selector two areas define is a mistake, reported, not a silent override.
    static const mt_objc_host_method k_dup[] = {{"length", drawing_test_method}};
    static const mt_objc_host_methods k_dup_set = {"NSString", "someone", k_dup, 1, nullptr, 0};
    CHECK(mt_objc_add_host_methods(w.rt, &k_dup_set) == -1);
    CHECK(logged("host methods of someone: -[NSString length] is already defined"));
    CHECK(w.send(s, "length") == 4u);
    static const mt_objc_host_methods k_nowhere = {"NoSuchClass", "someone", k_dup, 1, nullptr, 0};
    CHECK(mt_objc_add_host_methods(w.rt, &k_nowhere) == -1);
    CHECK(logged("host methods of someone: no host class NoSuchClass"));

    // The registry: Foundation's sets, every one added by mt_objc_add_host_classes.
    u32 n = 0;
    const mt_objc_host_methods *const *sets = mt_objc_host_method_registry(&n);
    CHECK(n == 5u);
    for (u32 i = 0; i < n; i++) {
        CHECK(sets[i] != nullptr);
    }
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
    CHECK(mt_ns::host_path(w.text(p), false) == (b.dir / "sound/bgm01.mp3").string());
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
    std::string theif(1124, '\0'), quest(4781, '\0');
    for (std::size_t i = 0; i < theif.size(); i++) {
        theif[i] = static_cast<char>(i * 7 + 1);
    }
    for (std::size_t i = 0; i < quest.size(); i++) {
        quest[i] = static_cast<char>(i * 13 + 5);
    }
    {
        Ns w(dir);
        CHECK(game_read(w, "theif0") == "<missing>");
        game_write(w, "theif0", theif);
        game_write(w, "quest0", quest);
        CHECK(game_read(w, "theif0") == theif); // and within the process
    } // the process ends: runtime, guest memory and every object are gone
    {
        Ns w(dir);
        CHECK(game_read(w, "theif0") == theif);
        CHECK(game_read(w, "quest0") == quest);
        // On the host: <data dir>/Documents/<name>, the bytes as the game wrote them.
        CHECK(read_host(dir / "Documents" / "theif0") == theif);
        // The game writes at offset 0 and never truncates: a shorter write keeps the tail.
        game_write(w, "theif0", "AB");
        CHECK(game_read(w, "theif0") == "AB" + theif.substr(2));
        // Mode 4: fileHandleForWritingAtPath: + truncateFileAtOffset:0, then writeData:.
        const u32 path = documents_path(w, "theif0");
        const u32 h = w.send(w.cls("NSFileHandle"), "fileHandleForWritingAtPath:", {path});
        REQUIRE(h != 0u);
        w.send(h, "truncateFileAtOffset:", {0, 0});
        w.send(h, "writeData:", {w.data("xyz")});
        w.send(h, "closeFile");
        CHECK(game_read(w, "theif0") == "xyz");
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
                     "fileHandleForReadingAtPath:", {documents_path(w, "cupon")}) == 0u);
        CHECK(w.send(w.cls("NSFileHandle"),
                     "fileHandleForUpdatingAtPath:", {documents_path(w, "cupon")}) == 0u);

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

TEST_CASE("Foundation: guest paths reach the home directory and the bundle only") {
    Bundle b("en");
    Ns w;
    w.cpu.r[0] = 0;
    mt_hle_NSHomeDirectory(&w.cpu, 0);
    const std::string home = w.text(w.cpu.r[0]);
    CHECK(home == "/var/mobile/Applications/00000000-0000-0000-0000-000000000000");
    CHECK(home == mt_ns::home_guest_path());
    const std::string data = w.data_dir.string();
    CHECK(mt_ns::host_path(home + "/Documents/theif0", true) == data + "/Documents/theif0");
    CHECK(mt_ns::host_path(home + "//Documents/./theif0", true).empty());
    CHECK(mt_ns::host_path(home + "/Documents//theif0/", true) == data + "/Documents/theif0");
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
