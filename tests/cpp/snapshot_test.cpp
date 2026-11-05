// Save states (#202, ADR 0105) on a synthetic guest: the state file's format and its refusals,
// the checksums, the safe-point check, the census, and a save and an in-process restore of guest
// memory, the heap's bookkeeping, the ObjC runtime's tables and the value host states (CF,
// Foundation, the run loop). No state file is ever written to disk: every file here is made in
// memory from synthetic objects.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/cf.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/snapshot.h"
#include "maplethief/uikit.h"
#include "mem/mem_internal.h"
#include "objc/objc_internal.h"
#include "snapshot/snapshot_internal.h"
#include "snapshot/state_file.h"
#include "uikit/uikit_internal.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Bytes = std::vector<u8>;

std::string hex(const u8 *p, std::size_t n) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; i++) {
        s += k[p[i] >> 4];
        s += k[p[i] & 15];
    }
    return s;
}

u32 lo(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return static_cast<u32>(b);
}
u32 hi(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return static_cast<u32>(b >> 32);
}

constexpr u32 k_segment = 0x00100000u; // a synthetic writable image segment
constexpr u32 k_segment_size = 0x8000u;

// A guest address space with a writable image segment, the runtime's host classes and CF, and
// the safe-point check pointed at no game words.
struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;

    World() {
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        mt_segment seg{};
        std::memcpy(seg.name, "__DATA", 7);
        seg.vmaddr = k_segment;
        seg.vmsize = k_segment_size;
        seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
        const u8 file[16] = {0xDA, 0x7A};
        seg.filesize = sizeof file;
        REQUIRE(mt_mem_map_segment(mem, &seg, file, sizeof file) == 0);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top - 64u;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        mt_cf_start(&cpu, rt);
        const mt_snapshot_config sc{0, 0, 0};
        mt_snapshot_configure(&sc);
    }
    ~World() {
        mt_snapshot_configure(nullptr);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
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
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
    u32 constant(mt_data_init_fn hook) {
        const u32 slot = mt_heap_calloc(mem, 1, 4);
        hook(&cpu, 0, slot);
        mt_cf::make_constants(&cpu);
        u32 v;
        std::memcpy(&v, cpu.mem + slot, 4);
        return v;
    }
    std::u16string text(u32 str) {
        const std::u16string *s = mt_cf::string_chars(&cpu, str);
        return s != nullptr ? *s : u"<none>";
    }
    long class_index(const char *name) {
        return mt_objc_impl::class_index(rt, mt_objc_class_named(rt, name));
    }
};

// Saves the world into a state file; REQUIREs success.
Bytes save(World &w, u32 flags = 0, u32 slot = MT_SNAPSHOT_ANY_SLOT) {
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    std::memset(meta.binary_sha256, 0xAB, 32);
    meta.slot = slot;
    meta.flags = flags;
    u8 *bytes = nullptr;
    std::size_t n = 0;
    char why[256] = {};
    const int rc = mt_snapshot_save(&w.cpu, &meta, &bytes, &n, why, sizeof why);
    INFO(why);
    REQUIRE(rc == 0);
    Bytes out(bytes, bytes + n);
    std::free(bytes);
    return out;
}

// The sections of a state file (inflated), by name.
std::vector<mt_snap::Section> sections_of(const Bytes &file) {
    mt_snapshot_header h{};
    std::vector<mt_snap::Section> s;
    std::string why;
    REQUIRE(mt_snap::read_state_file(file.data(), file.size(), nullptr, &h, &s, &why));
    return s;
}

std::string restore_why(World &w, const Bytes &file, const mt_snapshot_expect *ex = nullptr) {
    char why[256] = {};
    const int rc = mt_snapshot_restore(&w.cpu, file.data(), file.size(), ex, nullptr, why,
                                       sizeof why);
    return rc == 0 ? std::string() : std::string(why);
}

std::string header_why(const Bytes &file, const mt_snapshot_expect *ex = nullptr) {
    char why[256] = {};
    mt_snapshot_header h{};
    return mt_snapshot_read_header(file.data(), file.size(), ex, &h, why, sizeof why) == 0
               ? std::string()
               : std::string(why);
}

// A synthetic state file of two sections (and its header), written through the file layer.
Bytes synthetic_file(bool store, mt_snapshot_header *header = nullptr) {
    mt_snapshot_header h{};
    h.format = MT_SNAPSHOT_FORMAT;
    std::memset(h.binary_sha256, 0x5A, 32);
    std::snprintf(h.port_version, sizeof h.port_version, "%s", "9.9.9-test");
    h.slot = 2;
    h.guest_unix_us = 1735689600123456ull;
    std::vector<mt_snap::Section> sections;
    sections.push_back(mt_snap::Section{"alpha", Bytes(5000, 0x11)});
    Bytes mixed;
    for (u32 i = 0; i < 3000; i++) {
        mixed.push_back(static_cast<u8>(i * 7 + (i >> 5)));
    }
    sections.push_back(mt_snap::Section{"beta.two", mixed});
    sections.push_back(mt_snap::Section{"empty", {}});
    Bytes out;
    std::string why;
    REQUIRE(mt_snap::write_state_file(h, sections, store, &out, &why));
    if (header != nullptr) {
        *header = h;
    }
    return out;
}

void put_u32(Bytes &b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; i++) {
        b[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
u32 get_u32(const Bytes &b, std::size_t at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 |
           static_cast<u32>(b[at + 2]) << 16 | static_cast<u32>(b[at + 3]) << 24;
}
void fix_header_crc(Bytes &b) { put_u32(b, 100, mt_crc32(0, b.data(), 100)); }

// Where the first section's fields are: name length at 104, then name, flags, raw, stored, CRC.
struct SectionAt {
    std::size_t flags, raw, stored, crc, data;
};
SectionAt first_section(const Bytes &b) {
    const std::size_t name = 105 + b[104];
    return SectionAt{name, name + 4, name + 8, name + 12, name + 16};
}

} // namespace

TEST_CASE("snapshot: SHA-256 and CRC-32 match their test vectors") {
    u8 d[32];
    mt_sha256(reinterpret_cast<const u8 *>(""), 0, d);
    CHECK(hex(d, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    mt_sha256(reinterpret_cast<const u8 *>("abc"), 3, d);
    CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; // 56 bytes
    mt_sha256(reinterpret_cast<const u8 *>(two), std::strlen(two), d);
    CHECK(hex(d, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    mt_sha256(reinterpret_cast<const u8 *>(million.data()), million.size(), d);
    CHECK(hex(d, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(mt_crc32(0, reinterpret_cast<const u8 *>("123456789"), 9) == 0xCBF43926u);
    // Continued over pieces, as over the whole.
    const u32 part = mt_crc32(0, reinterpret_cast<const u8 *>("12345"), 5);
    CHECK(mt_crc32(part, reinterpret_cast<const u8 *>("6789"), 4) == 0xCBF43926u);
}

TEST_CASE("snapshot: the state file round-trips, stored and deflated") {
    for (const bool store : {true, false}) {
        CAPTURE(store);
        mt_snapshot_header h{};
        const Bytes file = synthetic_file(store, &h);
        mt_snapshot_header got{};
        std::vector<mt_snap::Section> sections;
        std::string why;
        REQUIRE(mt_snap::read_state_file(file.data(), file.size(), nullptr, &got, &sections, &why));
        CHECK(got.format == MT_SNAPSHOT_FORMAT);
        CHECK(std::memcmp(got.binary_sha256, h.binary_sha256, 32) == 0);
        CHECK(std::string(got.port_version) == "9.9.9-test");
        CHECK(got.slot == 2u);
        CHECK(got.guest_unix_us == 1735689600123456ull);
        CHECK(got.sections == 3u);
        REQUIRE(sections.size() == 3u);
        CHECK(sections[0].name == "alpha");
        CHECK(sections[0].bytes == Bytes(5000, 0x11));
        CHECK(sections[1].name == "beta.two");
        CHECK(sections[1].bytes.size() == 3000u);
        CHECK(sections[1].bytes[2999] == static_cast<u8>(2999 * 7 + (2999 >> 5)));
        CHECK(sections[2].bytes.empty());
        // Deflating pays: 5000 equal bytes take a few dozen.
        CHECK((store ? file.size() > 8000u : file.size() < 4000u));
        CHECK(header_why(file).empty());
    }
}

TEST_CASE("snapshot: a reader refuses a damaged or foreign state file with its reason") {
    const Bytes good = synthetic_file(false);
    REQUIRE(header_why(good).empty());
    const auto refused = [](Bytes b, const char *reason, const mt_snapshot_expect *ex = nullptr) {
        const std::string why = header_why(b, ex);
        INFO(why);
        CHECK(why.find(reason) != std::string::npos);
    };
    {
        Bytes b = good;
        b[0] ^= 1u;
        refused(b, "wrong magic");
        refused(Bytes(good.begin(), good.begin() + 4), "wrong magic");
    }
    {
        Bytes b = good;
        put_u32(b, 8, 2);
        fix_header_crc(b);
        refused(b, "format version 2, this port reads 1");
    }
    {
        Bytes b = good;
        b[50] ^= 0x40u; // the port's version
        refused(b, "the header's CRC does not match");
    }
    refused(Bytes(good.begin(), good.begin() + 60), "truncated or malformed header");
    {
        mt_snapshot_expect ex = mt_snapshot_expect_default();
        u8 other[32];
        std::memset(other, 0x5B, 32);
        ex.binary_sha256 = other;
        refused(good, "a state of another game binary", &ex);
        std::memset(other, 0x5A, 32);
        CHECK(header_why(good, &ex).empty());
        ex.slot = 1;
        refused(good, "a state of character slot 2, not 1", &ex);
        ex.slot = 2;
        CHECK(header_why(good, &ex).empty());
    }
    const SectionAt s = first_section(good);
    {
        Bytes b = good;
        b[s.data + 3] ^= 0x01u;
        refused(b, "section alpha: bad CRC");
    }
    {
        Bytes b = good;
        put_u32(b, s.raw, get_u32(b, s.raw) + 1); // the CRC holds, the inflate does not
        Bytes keep = b;
        mt_snapshot_header h{};
        std::vector<mt_snap::Section> sections;
        std::string why;
        CHECK_FALSE(mt_snap::read_state_file(b.data(), b.size(), nullptr, &h, &sections, &why));
        CHECK(why == "section alpha: does not inflate to its length");
    }
    {
        Bytes b = good;
        put_u32(b, s.raw, mt_snap::k_max_section + 1u);
        refused(b, "section alpha: oversized");
    }
    {
        Bytes b = good;
        put_u32(b, s.stored, 0x7FFFFFF0u);
        refused(b, "bytes run past the end of the file");
    }
    refused(Bytes(good.begin(), good.end() - 1), "section empty: truncated");
    refused(Bytes(good.begin(), good.end() - 40), "run past the end of the file");
    refused(Bytes(good.begin(), good.begin() + static_cast<long>(s.raw)), "section alpha: truncated");
    {
        Bytes b = good;
        b.push_back(0);
        refused(b, "1 bytes after the last section");
    }
    {
        Bytes b = good;
        put_u32(b, 96, 9); // more sections than the file has
        fix_header_crc(b);
        refused(b, "truncated or malformed name");
    }
    {
        Bytes b = synthetic_file(true);
        put_u32(b, first_section(b).flags, 4); // an unknown flag
        refused(b, "malformed lengths or flags");
    }
}

TEST_CASE("snapshot: the safe point refuses outside the field, with a touch, a key or work "
          "pending") {
    World w;
    char why[160] = {};
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 1);
    // The game's words (synthetic here): the game mode and the open popup.
    const u32 words = mt_heap_calloc(w.mem, 1, 8);
    w.put32(words, 4);              // an NPC chat
    w.put32(words + 4, 0xFFFFFFFFu); // no popup
    const mt_snapshot_config cfg{words, words + 4, 0};
    mt_snapshot_configure(&cfg);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "not in the field (game mode 4)");
    w.put32(words, 9);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "during a scene");
    w.put32(words, 0);
    w.put32(words + 4, 0x68);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "a popup is open");
    w.put32(words + 4, 0xFFFFFFFFu);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 1);
    // The key pad: no pad seen (-1) is not the field's panel 0.
    const mt_snapshot_config with_panel{words, words + 4, 1};
    mt_snapshot_configure(&with_panel);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "not in the field (the key pad shows another panel)");
    mt_snapshot_configure(&cfg);
    // A touch queued for the next turn.
    REQUIRE(mt_uikit_touch(1, MT_UIKIT_TOUCH_BEGAN, 100, 100) == 0);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "a touch is down");
    mt_ui::touch_reset();
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 1);
    // A held key.
    mt_input_press(MT_ACTION_RIGHT);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "a key is held");
    mt_input_configure(nullptr);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 1);
    // Autoreleased objects waiting for their pool.
    const u32 pool = mt_objc_pool_push(w.rt);
    mt_objc_autorelease(&w.cpu, mt_cf::string_create(&w.cpu, u"later", false));
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 0);
    CHECK(std::string(why) == "autoreleased objects are waiting");
    // Save and restore refuse with the same reason, and change nothing.
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    u8 *bytes = nullptr;
    std::size_t n = 0;
    CHECK(mt_snapshot_save(&w.cpu, &meta, &bytes, &n, why, sizeof why) == -1);
    CHECK(bytes == nullptr);
    CHECK(std::string(why) == "autoreleased objects are waiting");
    mt_objc_pool_pop(w.rt, pool);
    CHECK(mt_snapshot_safe(&w.cpu, why, sizeof why) == 1);
}

TEST_CASE("snapshot: the census names every live host state type without a serializer") {
    World w;
    // Every registered serializer: a distinct type and a distinct, stable name.
    std::size_t n = 0;
    const mt_snap::StateOps *const *ops = mt_snap::all_ops(&n);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < n; i++) {
        CHECK(mt_snap::ops_for(ops[i]->type) == ops[i]);
        CHECK(std::string(ops[i]->name) == ops[i]->type->name);
        names.emplace_back(ops[i]->name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    // The value types of #202.
    for (const char *t : {"CFString", "CFNumber", "CFData", "CFURL", "CFBundle", "CFRunLoop",
                          "CFRunLoopTimer", "NSArray", "NSSet", "NSDictionary", "NSURL",
                          "NSError"}) {
        CAPTURE(t);
        CHECK(mt_snapshot_has_serializer(t) == 1);
    }
    CHECK(mt_snapshot_has_serializer("UIView") == 0);

    const char *missing[4] = {};
    CHECK(mt_snapshot_missing(w.rt, missing, 4) == 0u);
    mt_cf::string_create(&w.cpu, u"a value", false);
    CHECK(mt_snapshot_missing(w.rt, missing, 4) == 0u);
    // A host state type nobody wrote a serializer for: named, and a save refuses.
    static const mt_objc_state_type k_widget = {"TestWidget", 8, nullptr};
    const u32 obj = mt_objc_create_instance(w.rt, mt_objc_class_named(w.rt, "NSObject"));
    REQUIRE(mt_objc_state(w.rt, obj, &k_widget) != nullptr);
    REQUIRE(mt_snapshot_missing(w.rt, missing, 4) == 1u);
    CHECK(std::string(missing[0]) == "TestWidget");
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    u8 *bytes = nullptr;
    std::size_t size = 0;
    char why[200] = {};
    CHECK(mt_snapshot_save(&w.cpu, &meta, &bytes, &size, why, sizeof why) == -1);
    CHECK(std::string(why) == "live host states without a serializer: TestWidget");
    // A partial state (measurements only) is written, and cannot be restored.
    const Bytes partial = save(w, MT_SNAPSHOT_PARTIAL);
    CHECK(header_why(partial).empty());
    CHECK(restore_why(w, partial) == "a partial state (host states left out): it cannot be restored");
}

TEST_CASE("snapshot: a restore brings back guest memory, the heap, the ObjC tables and the "
          "value host states") {
    World w;
    mt_cpu &cpu = w.cpu;
    // The value host states of #202, each kind at least once.
    const u32 str = mt_cf::string_create(&cpu, u"Maple", false);
    const u32 mstr = mt_cf::string_create(&cpu, u"Thief", true);
    const u32 cstr = mt_cf::string_guest_cstring(&cpu, str, mt_cf::k_enc_utf8);
    REQUIRE(cstr != 0u);
    const u32 num = mt_cf::number_create(&cpu, mt_cf::Number{4u, false, 42, 42.0});
    const u8 raw[5] = {1, 2, 3, 4, 5};
    const u32 data = mt_cf::data_create(&cpu, raw, 5, true);
    const u32 items[2] = {str, num};
    const u32 arr = mt_ns::array_create(&cpu, items, 2, true);
    const u32 set = mt_ns::set_create(&cpu, &data, 1);
    const u32 dict = mt_ns::dictionary_create(&cpu, true);
    REQUIRE(mt_ns::dictionary_set(&cpu, dict, str, num));
    const u32 curl = mt_cf::url_create(&cpu, "Data/Map.img");
    const u32 bundle = w.call(mt_hle_CFBundleGetMainBundle, {});
    const u32 pool = mt_objc_pool_push(w.rt);
    const u32 nsurl = mt_objc_retain(
        &cpu, mt_objc_send1(&cpu, mt_objc_class_named(w.rt, "NSURL"), "URLWithString:", str));
    const u32 error_args[3] = {str, 7u, dict};
    const u32 error = mt_objc_retain(
        &cpu, mt_objc_msg(&cpu, mt_objc_class_named(w.rt, "NSError"),
                          mt_objc_sel(w.rt, "errorWithDomain:code:userInfo:"), error_args, 3));
    mt_objc_pool_pop(w.rt, pool);
    REQUIRE(nsurl != 0u);
    REQUIRE(error != 0u);
    const u32 common = w.constant(mt_hle_init_kCFRunLoopCommonModes);
    const u32 main = w.call(mt_hle_CFRunLoopGetMain, {});
    const u32 timer = w.call(mt_hle_CFRunLoopTimerCreate,
                             {0, lo(100.0), hi(100.0), lo(0.001), hi(0.001), 0, 0, 0x8E29u, 0});
    w.call(mt_hle_CFRunLoopAddTimer, {main, timer, common});
    mt_objc_retain(&cpu, str);
    mt_objc_retain(&cpu, str);
    mt_objc_impl::map_put(w.rt->syncs, mstr, 2);
    const long nsset = w.class_index("NSSet");
    REQUIRE(nsset >= 0);
    const bool nsset_initialized = w.rt->classes[nsset].initialized;
    // Guest bytes in the image, the stack and the heap.
    std::memset(cpu.mem + k_segment + 0x100, 0x5E, 0x2000);
    std::memset(cpu.mem + w.stack.top - 0x400, 0x77, 0x100);
    const u32 big = mt_heap_malloc(w.mem, 3u << 20); // past the first commit step
    std::memset(cpu.mem + big, 0x42, 3u << 20);
    mt_heap_free(w.mem, big); // freed at the top: the top falls, the old bytes stay
    cpu.r[4] = 0x44444444u;
    cpu.d[3] = 0x3333333333333333ull;

    const u32 sels = w.rt->sel_count;
    const u32 retains = mt_objc_retain_count(w.rt, str);
    mt_heap_stats before{};
    mt_heap_get_stats(w.mem, &before);
    const Bytes heap_bytes(cpu.mem + MT_GUEST_HEAP_BASE, cpu.mem + before.committed);
    const Bytes image_bytes(cpu.mem + k_segment, cpu.mem + k_segment + k_segment_size);
    const Bytes stack_bytes(cpu.mem + w.stack.base, cpu.mem + w.stack.top);
    const Bytes file = save(w);
    const std::vector<mt_snap::Section> saved = sections_of(file);

    // The uninterrupted run: what the next allocations get.
    std::vector<u32> uninterrupted;
    for (const u32 size : {24u, 2000u, 100000u, 2u << 20}) {
        uninterrupted.push_back(mt_heap_malloc(w.mem, size));
    }
    uninterrupted.push_back(mt_objc_create_instance(w.rt, mt_objc_class_named(w.rt, "NSObject")));

    // Play on: free and change objects, intern a selector, initialize a class, scribble.
    mt_objc_release(&cpu, arr);
    mt_objc_release(&cpu, dict);
    mt_objc_release(&cpu, set);
    mt_objc_release(&cpu, error);
    mt_objc_release(&cpu, nsurl);
    mt_objc_release(&cpu, curl);
    mt_cf::string_mutable_chars(&cpu, mstr)->append(u" and more");
    mt_objc_retain(&cpu, num);
    mt_objc_release(&cpu, str);
    mt_cf::make_immortal(&cpu, mt_cf::string_create(&cpu, u"new", false));
    REQUIRE(mt_objc_sel(w.rt, "aSelectorInternedAfterTheSave:") != 0u);
    CHECK(w.rt->sel_count == sels + 1);
    w.rt->classes[nsset].initialized = !nsset_initialized;
    mt_objc_impl::map_erase(w.rt->syncs, mstr);
    std::memset(cpu.mem + k_segment, 0xEE, 0x400);
    std::memset(cpu.mem + w.stack.top - 0x400, 0x00, 0x400);
    for (int i = 0; i < 40; i++) {
        mt_heap_malloc(w.mem, 64u + 32u * static_cast<u32>(i));
    }
    const u32 later = mt_heap_malloc(w.mem, 6u << 20); // commits more
    std::memset(cpu.mem + later, 0x99, 6u << 20);
    cpu.r[4] = 0;
    cpu.d[3] = 0;

    // A damaged state is refused and changes nothing.
    {
        Bytes bad = file;
        bad[bad.size() - 3] ^= 0x10u;
        mt_heap_stats now{};
        mt_heap_get_stats(w.mem, &now);
        CHECK(restore_why(w, bad).find("bad CRC") != std::string::npos);
        mt_heap_stats after{};
        mt_heap_get_stats(w.mem, &after);
        CHECK(after.top == now.top);
        CHECK(after.live_blocks == now.live_blocks);
        CHECK(cpu.mem[k_segment] == 0xEEu);
        CHECK(w.text(mstr) == u"Thief and more");
    }
    // So is one taken at another stack position (not at the turn hook).
    cpu.r[MT_SP] -= 16u;
    CHECK(restore_why(w, file).find("stack pointer differs") != std::string::npos);
    cpu.r[MT_SP] += 16u;

    u64 rebased = 0;
    mt_snapshot_expect ex = mt_snapshot_expect_default();
    ex.rebase_clock = [](void *user, u64 t) { *static_cast<u64 *>(user) = t; };
    ex.user = &rebased;
    mt_snapshot_header header{};
    char why[256] = {};
    const int rc = mt_snapshot_restore(&cpu, file.data(), file.size(), &ex, &header, why, sizeof why);
    INFO(why);
    REQUIRE(rc == 0);
    CHECK(rebased == header.guest_unix_us);
    CHECK(rebased != 0u);

    // Guest memory, byte for byte, and the heap as it was.
    mt_heap_stats after{};
    mt_heap_get_stats(w.mem, &after);
    CHECK(after.top == before.top);
    CHECK(after.committed == before.committed);
    CHECK(after.live_blocks == before.live_blocks);
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(Bytes(cpu.mem + MT_GUEST_HEAP_BASE, cpu.mem + before.committed) == heap_bytes);
    CHECK(Bytes(cpu.mem + k_segment, cpu.mem + k_segment + k_segment_size) == image_bytes);
    CHECK(Bytes(cpu.mem + w.stack.base, cpu.mem + w.stack.top) == stack_bytes);
    // Never committed in the saved run: zero, as a fresh commit.
    CHECK(cpu.mem[before.committed + 0x1000] == 0u);
    CHECK(cpu.r[4] == 0x44444444u);
    CHECK(cpu.d[3] == 0x3333333333333333ull);
    // The next allocations are the uninterrupted run's.
    std::vector<u32> restored;
    for (const u32 size : {24u, 2000u, 100000u, 2u << 20}) {
        restored.push_back(mt_heap_malloc(w.mem, size));
    }
    restored.push_back(mt_objc_create_instance(w.rt, mt_objc_class_named(w.rt, "NSObject")));
    CHECK(restored == uninterrupted);

    // The ObjC tables.
    CHECK(mt_objc_retain_count(w.rt, str) == retains);
    CHECK(mt_objc_retain_count(w.rt, num) == 1u + 2u); // the array's and the dictionary's
    u32 depth = 0;
    CHECK(mt_objc_impl::map_get(w.rt->syncs, mstr, &depth));
    CHECK(depth == 2u);
    CHECK(w.rt->classes[nsset].initialized == nsset_initialized);
    CHECK(w.rt->sel_count == sels);
    CHECK(mt_objc_impl::sel_id_of(w.rt, "aSelectorInternedAfterTheSave:") ==
          static_cast<long>(sels)); // interned again, as the first time

    // The value states.
    CHECK(w.text(str) == u"Maple");
    CHECK(w.text(mstr) == u"Thief");
    CHECK(std::strcmp(reinterpret_cast<const char *>(cpu.mem + cstr), "Maple") == 0);
    CHECK(mt_cf::string_guest_cstring(&cpu, str, mt_cf::k_enc_utf8) == cstr);
    mt_cf::Number nv{};
    REQUIRE(mt_cf::number_value(&cpu, num, &nv));
    CHECK(nv.i == 42);
    u32 bytes = 0, length = 0;
    REQUIRE(mt_cf::data_bytes(&cpu, data, &bytes, &length));
    CHECK(length == 5u);
    CHECK(cpu.mem[bytes + 4] == 5u);
    const std::vector<u32> *a = mt_ns::array_items(&cpu, arr);
    REQUIRE(a != nullptr);
    CHECK(*a == std::vector<u32>{str, num});
    const std::vector<u32> *s = mt_ns::set_items(&cpu, set);
    REQUIRE(s != nullptr);
    CHECK(*s == std::vector<u32>{data});
    CHECK(mt_ns::dictionary_get(&cpu, dict, str) == num);
    std::string rel;
    CHECK(mt_cf::url_bundle_path(&cpu, curl, &rel));
    CHECK(rel == "Data/Map.img");
    std::string url;
    bool is_file = true;
    CHECK(mt_ns::url_string(&cpu, nsurl, &url, &is_file));
    CHECK(url == "Maple");
    CHECK_FALSE(is_file);
    CHECK(w.call(mt_hle_CFBundleGetMainBundle, {}) == bundle);
    CHECK(mt_runloop_timer_count() == 1u);
    CHECK(mt_runloop_next_fire_date() == 100.0);
    CHECK(mt_runloop_main(&cpu) == main);
    CHECK_FALSE(mt_cf::is_immortal(&cpu, restored.back()));

    // And saved again, every section is the same bytes: nothing was lost or added.
    std::vector<mt_snap::Section> again = sections_of(save(w));
    REQUIRE(again.size() == saved.size());
    for (std::size_t i = 0; i < saved.size(); i++) {
        CAPTURE(saved[i].name);
        if (saved[i].name == "heap" || saved[i].name == "heap.book" || saved[i].name == "objc" ||
            saved[i].name == "objc.states" || saved[i].name == "cpu") {
            continue; // the restored run has allocated since (above), and called the heap
        }
        CHECK(again[i].name == saved[i].name);
        CHECK(again[i].bytes == saved[i].bytes);
    }
}

TEST_CASE("snapshot: a state round-trips through a second restore unchanged") {
    World w;
    mt_cpu &cpu = w.cpu;
    const u32 items[1] = {mt_cf::string_create(&cpu, u"one", false)};
    mt_ns::array_create(&cpu, items, 1, false);
    const Bytes first = save(w, MT_SNAPSHOT_STORE, 3);
    mt_snapshot_expect ex = mt_snapshot_expect_default();
    ex.slot = 4;
    CHECK(restore_why(w, first, &ex) == "a state of character slot 3, not 4");
    ex.slot = 3;
    CHECK(restore_why(w, first, &ex).empty());
    CHECK(restore_why(w, first, &ex).empty());
    // Saved right after a restore, the state is the same in every section.
    const std::vector<mt_snap::Section> a = sections_of(first), b = sections_of(save(w));
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); i++) {
        CAPTURE(a[i].name);
        CHECK(a[i].bytes == b[i].bytes);
    }
}
