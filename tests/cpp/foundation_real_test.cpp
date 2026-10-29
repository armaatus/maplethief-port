// Foundation on the real bundle (#49, ADR 0035), local only: skipped (exit 77) without work/ipa/
// (scripts/bootstrap).
//
//   1. Every binary plist of MapleThief.app parses (Info.plist, every *.lproj/*.strings) with the
//      entry counts plutil reports; ResourceRules.plist is XML (code signing's, never read by
//      the game) and is rejected cleanly.
//   2. Through NSBundle's messages, as the game sends them: Info.plist keys, and every key of
//      Scripts.strings and ScriptExtra.strings in both languages answers its table's value.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "foundation/foundation.h"
#include "foundation/plist.h"
#include "maplethief/cf.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

std::vector<std::uint8_t> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::string utf8(const std::u16string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); i++) {
        std::uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00u);
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool parse_file(const fs::path &p, mt_ns::PlistValue *v, std::string *error) {
    const std::vector<std::uint8_t> bytes = read_file(p);
    return mt_ns::plist_parse(bytes.data(), bytes.size(), v, error);
}

void ignore_log(void *, const char *) {}

struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    World() {
        mem = mt_mem_create(nullptr);
        mt_mem_attach(mem, &cpu);
        mt_stack_create(mem, 0, &stack);
        cpu.r[MT_SP] = stack.top;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = ignore_log;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        mt_objc_add_host_classes(rt);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
    }
    ~World() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    u32 send(u32 obj, const char *sel, std::initializer_list<u32> args = {}) {
        return mt_objc_msg(&cpu, obj, mt_objc_sel(rt, sel), args.begin(),
                           static_cast<u32>(args.size()));
    }
    u32 str(const std::string &s) { return mt_ns::string_from_utf8(&cpu, s); }
};

void plists(const fs::path &app) {
    struct Want {
        const char *file;
        std::size_t entries;
    };
    const Want wants[] = {
        {"Info.plist", 30},
        {"en.lproj/InfoPlist.strings", 1},
        {"ko.lproj/InfoPlist.strings", 1},
        {"en.lproj/Scripts.strings", 2219},
        {"ko.lproj/Scripts.strings", 2219},
        {"en.lproj/ScriptExtra.strings", 527},
        {"ko.lproj/ScriptExtra.strings", 527},
    };
    for (const Want &w : wants) {
        mt_ns::PlistValue v;
        std::string error;
        const bool ok = parse_file(app / w.file, &v, &error);
        check(ok && v.kind == mt_ns::PlistValue::dict && v.entries.size() == w.entries,
              std::string(w.file) + ": " +
                  (ok ? std::to_string(v.entries.size()) + " entries" : error));
    }
    mt_ns::PlistValue v;
    std::string error;
    check(!parse_file(app / "ResourceRules.plist", &v, &error) && error == "not a binary plist",
          "ResourceRules.plist (XML, code signing's): rejected (" + error + ")");
}

// Every key of <lang>.lproj/<table>.strings answers its value through
// -localizedStringForKey:value:table:.
void tables(const fs::path &app, const char *language, const char *lproj) {
    mt_cf_config cfg = mt_cf_config_default();
    const std::string dir = app.string();
    cfg.bundle_dir = dir.c_str();
    std::memcpy(cfg.language, language, std::strlen(language) + 1);
    mt_cf_configure(&cfg);
    World w;
    const u32 bundle = w.send(mt_objc_class_named(w.rt, "NSBundle"), "mainBundle");
    for (const char *table : {"Scripts", "ScriptExtra"}) {
        mt_ns::PlistValue v;
        std::string error;
        parse_file(app / lproj / (std::string(table) + ".strings"), &v, &error);
        const u32 table_name = w.str(table);
        u32 bad = 0;
        for (const auto &[key, value] : v.entries) {
            const u32 pool = mt_objc_pool_push(w.rt);
            const std::string k = utf8(key), want = utf8(value.s);
            const u32 got =
                w.send(bundle, "localizedStringForKey:value:table:", {w.str(k), 0u, table_name});
            const std::string text = mt_ns::string_utf8(&w.cpu, got);
            if (text != want && bad++ < 5) {
                std::printf("     %s/%s: got \"%s\"\n", lproj, k.c_str(), text.c_str());
            }
            mt_objc_pool_pop(w.rt, pool);
        }
        check(bad == 0 && !v.entries.empty(),
              std::string(lproj) + "/" + table + ".strings: all " +
                  std::to_string(v.entries.size()) +
                  " keys answer their value through -localizedStringForKey:value:table:");
    }
    const u32 line = w.send(bundle, "localizedStringForKey:value:table:",
                            {w.str("BackChapter1_Quest_Bruce.dialog.1"), 0u, w.str("Scripts")});
    std::printf("     BackChapter1_Quest_Bruce.dialog.1 (%s): %s\n", language,
                mt_ns::string_utf8(&w.cpu, line).c_str());
    if (std::strcmp(language, "en") == 0) {
        check(mt_ns::string_utf8(&w.cpu, line) == "I'm Bruce, the senior researcher.",
              "en: Bruce's first line");
        auto info = [&w, bundle](const char *key) {
            return mt_ns::string_utf8(&w.cpu,
                                      w.send(bundle, "objectForInfoDictionaryKey:", {w.str(key)}));
        };
        check(info("CFBundleIdentifier") == "com.nexonmobile.maplestory.thief",
              "Info.plist CFBundleIdentifier: " + info("CFBundleIdentifier"));
        check(info("CFBundleVersion") == "1.5",
              "Info.plist CFBundleVersion: " + info("CFBundleVersion"));
    } else {
        check(mt_ns::string_utf8(&w.cpu, line) ==
                  "\xEB\x82\x98\xEB\x8A\x94 \xEC\x88\x98\xEC\x84\x9D "
                  "\xEC\x97\xB0\xEA\xB5\xAC\xEC\x9B\x90 \xEB\xB8\x8C\xEB\xA3\xA8\xEC\x8A\xA4"
                  "\xEB\x9D\xBC\xEA\xB3\xA0 \xED\x95\x98\xEB\x84\xA4.",
              "ko: Bruce's first line");
    }
    mt_cf_configure(nullptr);
}

} // namespace

// No guest code runs here.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, std::uint32_t target, std::uint32_t site) noexcept {
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

int main() {
    const fs::path app = MT_BUNDLE_DIR;
    if (!fs::is_regular_file(app / "Info.plist")) {
        std::printf("skipped: %s absent (run scripts/bootstrap)\n", MT_BUNDLE_DIR);
        return 77;
    }
    plists(app);
    tables(app, "en", "en.lproj");
    tables(app, "ko", "ko.lproj");
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
