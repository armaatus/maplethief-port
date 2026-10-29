// CoreFoundation on the real game (#48, ADR 0032), local only: skipped (exit 77) without
// work/ipa/ and work/slices/ (scripts/bootstrap).
//
//   1. Every file of every folder of MapleThief.app resolves through the game's own calls,
//      CFBundleCopyResourceURLForLocalization(main, name, NULL, folder, "English") and
//      CFURLCreateDataAndPropertiesFromResource, with exactly the file's bytes; the
//      localized files also resolve by localization (English -> en.lproj, Korean -> ko.lproj).
//   2. All 367 CFString literals of the slice's __cfstring decode (366 ASCII, one UTF-16).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "guest_trap.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/memory.h"
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

std::string utf8(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    World() {
        mem = mt_mem_create(nullptr);
        mt_mem_attach(mem, &cpu);
        mt_stack_create(mem, 0, &stack);
        cpu.r[MT_SP] = stack.top;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        mt_objc_add_host_classes(rt);
        mt_cf_start(&cpu, rt);
        mt_objc_pool_push(rt);
    }
    ~World() {
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    u32 call(mt_import_fn fn, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        cpu.r[MT_SP] = sp - 16u;
        u32 i = 0;
        for (const u32 a : args) {
            if (i < 4) {
                cpu.r[i] = a;
            } else {
                std::memcpy(cpu.mem + cpu.r[MT_SP] + 4u * (i - 4u), &a, 4);
            }
            i++;
        }
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
    u32 str(const std::string &s) {
        return s.empty() ? 0u
                         : mt_cf::string_create(&cpu, std::u16string(s.begin(), s.end()), false);
    }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    // The game's two calls; false if there is no URL or no data.
    bool load(const std::string &name, const std::string &dir, const std::string &loc,
              std::vector<std::uint8_t> &out) {
        const u32 bundle = call(mt_hle_CFBundleGetMainBundle, {});
        const u32 n = str(name), d = str(dir), l = str(loc);
        const u32 url = call(mt_hle_CFBundleCopyResourceURLForLocalization, {bundle, n, 0, d, l});
        for (const u32 s : {n, d, l}) {
            if (s != 0) {
                mt_objc_release(&cpu, s);
            }
        }
        if (url == 0) {
            return false;
        }
        const u32 slots = mt_heap_calloc(mem, 1, 12);
        const u32 ok =
            call(mt_hle_CFURLCreateDataAndPropertiesFromResource, {0, url, slots, 0, 0, slots + 8});
        const u32 data = get32(slots);
        mt_heap_free(mem, slots);
        mt_objc_release(&cpu, url);
        if (ok != 1u || data == 0) {
            return false;
        }
        const u32 len = call(mt_hle_CFDataGetLength, {data});
        const u32 ptr = call(mt_hle_CFDataGetBytePtr, {data});
        out.assign(cpu.mem + ptr, cpu.mem + ptr + len);
        mt_objc_release(&cpu, data);
        return true;
    }
};

void bundle_files(const fs::path &root) {
    mt_cf_config cfg = mt_cf_config_default();
    const std::string dir = root.string();
    cfg.bundle_dir = dir.c_str();
    mt_cf_configure(&cfg);
    World w;
    std::map<std::string, u32> folders; // folder -> files checked
    u32 files = 0, bad = 0;
    std::uint64_t bytes = 0;
    std::vector<std::uint8_t> got;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const fs::path rel = e.path().lexically_relative(root);
        const std::string folder = utf8(rel.parent_path());
        const std::vector<std::uint8_t> want = read_file(e.path());
        if (!w.load(utf8(rel.filename()), folder, "English", got) || got != want) {
            if (bad++ < 10) {
                std::printf("     %s: %s\n", utf8(rel).c_str(),
                            got.empty() ? "no data" : "bytes differ");
            }
        }
        folders[folder]++;
        files++;
        bytes += want.size();
    }
    check(bad == 0 && files > 2000,
          std::to_string(files) + " files (" + std::to_string(bytes) + " bytes) in " +
              std::to_string(folders.size()) +
              " folders resolve through CFBundleCopyResourceURLForLocalization with their bytes");
    // Localized files by localization, as NSBundle (#49) will look them up.
    const std::vector<std::uint8_t> en = read_file(root / "en.lproj" / "Scripts.strings");
    const std::vector<std::uint8_t> ko = read_file(root / "ko.lproj" / "Scripts.strings");
    std::vector<std::uint8_t> a, b;
    check(w.load("Scripts.strings", "", "English", a) && a == en &&
              w.load("Scripts.strings", "", "Korean", b) && b == ko && en != ko,
          "Scripts.strings: English -> en.lproj, Korean -> ko.lproj");
    check(w.load("v1.0.0.png", "", "en", a) &&
              a == read_file(root / "English.lproj" / "v1.0.0.png"),
          "v1.0.0.png: en -> English.lproj");
    mt_cf_configure(nullptr);
}

void constant_strings(const std::vector<std::uint8_t> &slice) {
    mt_image_info info;
    if (mt_image_info_read(slice.data(), slice.size(), &info) != 0) {
        check(false, "the slice's sections");
        return;
    }
    const mt_section *s = mt_image_section(&info, "__DATA", "__cfstring");
    World w;
    if (s == nullptr || mt_mem_map_macho(w.mem, slice.data(), slice.size()) != 0) {
        check(false, "__cfstring mapped");
        return;
    }
    // The image's isa words are binds to ___CFConstantStringClassReference; the boot binds
    // them to the class object of __NSCFConstantString, which this does by hand.
    const u32 cls = mt_objc_class_named(w.rt, "__NSCFConstantString");
    u32 total = 0, ascii = 0, utf16 = 0, bad = 0;
    std::u16string terms;
    for (u32 at = s->addr; at + 16 <= s->addr + s->size; at += 16, total++) {
        std::memcpy(w.cpu.mem + at, &cls, 4);
        const std::u16string *text = mt_cf::string_chars(&w.cpu, at);
        if (text == nullptr) {
            bad++;
            continue;
        }
        if (w.get32(at + 4) == 0x7D0u) {
            utf16++;
            terms = *text;
        } else {
            bool all_ascii = true;
            for (const char16_t c : *text) {
                all_ascii &= c < 0x80;
            }
            ascii += all_ascii ? 1u : 0u;
        }
    }
    check(total == 367 && bad == 0 && ascii == 366 && utf16 == 1 &&
              terms.rfind(u"넥슨 모바일", 0) == 0,
          std::to_string(total) + " CFString literals decode: " + std::to_string(ascii) +
              " ASCII, " + std::to_string(utf16) + " UTF-16 (the terms of service)");
}

} // namespace

extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, std::uint32_t target, std::uint32_t site) noexcept {
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

int main() {
    const fs::path bundle = MT_BUNDLE_DIR;
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    if (!fs::is_directory(bundle) || slice.empty()) {
        std::printf("skipped: %s or %s is missing (run scripts/bootstrap)\n", MT_BUNDLE_DIR,
                    MT_ARMV7_SLICE);
        return 77;
    }
    bundle_files(bundle);
    constant_strings(slice);
    return g_failures == 0 ? 0 : 1;
}
