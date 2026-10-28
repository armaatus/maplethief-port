// CoreGraphics / UIKit drawing on the real bundle (#51, ADR 0036), local only: skipped (exit
// 77) without work/ipa/ (scripts/bootstrap).
//
//   1. Every PNG file of MapleThief.app (1,322, 113 of them Apple CgBI) decodes through
//      +[UIImage imageWithContentsOfFile:] to the size in its IHDR, with the alpha info the
//      texture loaders expect (kCGImageAlphaLast with an alpha channel or tRNS, else
//      kCGImageAlphaNoneSkipLast); the root PNGs also through +imageNamed:.
//   2. Every PNG packed in png/**/*.dat (473, split at their IEND chunks) decodes through
//      -[UIImage initWithData:], as the engine's texture loader FUN_00009534 does.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_hle.h"
#include "cg/cg_objects.h"
#include "guest_trap.h"
#include "maplethief/cf.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

std::vector<u8> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

std::string utf8(const fs::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

u32 be32(const u8 *p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

// What a PNG's chunks say: size, whether it is CgBI, whether it has alpha (colour type 4/6 or
// a tRNS chunk), and its length up to the end of IEND (0 if malformed).
struct PngHeader {
    u32 width = 0, height = 0;
    bool cgbi = false, alpha = false;
    std::size_t length = 0;
};
PngHeader png_header(const u8 *p, std::size_t n) {
    PngHeader h;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || std::memcmp(p, sig, 8) != 0) {
        return h;
    }
    std::size_t at = 8;
    while (at + 12 <= n) {
        const u32 len = be32(p + at);
        const char *type = reinterpret_cast<const char *>(p + at + 4);
        if (at + 12 + std::size_t(len) > n) {
            return h;
        }
        if (std::memcmp(type, "CgBI", 4) == 0) {
            h.cgbi = true;
        } else if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            h.width = be32(p + at + 8);
            h.height = be32(p + at + 12);
            const u8 colour_type = p[at + 8 + 9];
            h.alpha = h.alpha || colour_type == 4 || colour_type == 6;
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            h.alpha = true;
        }
        at += 12 + std::size_t(len);
        if (std::memcmp(type, "IEND", 4) == 0) {
            h.length = at;
            return h;
        }
    }
    return h;
}

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
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        mt_objc_add_host_classes(rt);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
    }
    ~World() {
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    u32 str(const std::string &s) {
        std::u16string u;
        mt_cf::decode(reinterpret_cast<const u8 *>(s.data()), s.size(), mt_cf::k_enc_utf8, u);
        return mt_cf::string_create(&cpu, u, false);
    }
    // Drains the autorelease pool, so that a long loop does not fill the guest heap.
    void drain() {
        mt_objc_pool_pop(rt, pool);
        pool = mt_objc_pool_push(rt);
    }
    // The UIImage's size and alpha info match the header.
    bool matches(u32 uiimage, const PngHeader &h) {
        const u32 cg = uiimage != 0 ? mt_objc_send0(&cpu, uiimage, "CGImage") : 0u;
        const mt_cg::ImageInfo *info = mt_cg::image_info(&cpu, cg);
        return info != nullptr && info->width == h.width && info->height == h.height &&
               (info->bitmap_info & mt_cg::k_alpha_info_mask) ==
                   (h.alpha ? mt_cg::k_alpha_last : mt_cg::k_alpha_none_skip_last);
    }
};

void bundle_pngs(const fs::path &root) {
    mt_cf_config cfg = mt_cf_config_default();
    const std::string dir = root.string();
    cfg.bundle_dir = dir.c_str();
    mt_cf_configure(&cfg);
    World w;
    const u32 uiimage = mt_objc_class_named(w.rt, "UIImage");
    u32 files = 0, cgbi = 0, bad = 0, named = 0, bad_named = 0;
    u32 packs = 0, pieces = 0, bad_pieces = 0;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const fs::path rel = e.path().lexically_relative(root);
        const std::string ext = rel.extension().string();
        if (ext == ".png") {
            const std::vector<u8> bytes = read_file(e.path());
            const PngHeader h = png_header(bytes.data(), bytes.size());
            const std::string guest = mt_cf::bundle_guest_path(utf8(rel));
            const u32 img =
                mt_objc_send1(&w.cpu, uiimage, "imageWithContentsOfFile:", w.str(guest));
            if (h.length == 0 || !w.matches(img, h)) {
                if (bad++ < 10) {
                    std::printf("     %s: %s\n", utf8(rel).c_str(),
                                img == 0 ? "no image" : "size or alpha differ");
                }
            }
            files++;
            cgbi += h.cgbi ? 1u : 0u;
            if (!rel.has_parent_path() || rel.parent_path().empty()) {
                const u32 n =
                    mt_objc_send1(&w.cpu, uiimage, "imageNamed:", w.str(utf8(rel.stem())));
                named++;
                bad_named += w.matches(n, h) ? 0u : 1u;
            }
        } else if (ext == ".dat" && utf8(rel).rfind("png/", 0) == 0) {
            const std::vector<u8> bytes = read_file(e.path());
            packs++;
            std::size_t at = 0;
            while (at < bytes.size()) {
                const PngHeader h = png_header(bytes.data() + at, bytes.size() - at);
                if (h.length == 0) {
                    bad_pieces++;
                    std::printf("     %s: no PNG at offset %zu\n", utf8(rel).c_str(), at);
                    break;
                }
                const u32 data = mt_cf::data_create(&w.cpu, bytes.data() + at,
                                                    static_cast<u32>(h.length), false);
                const u32 img = mt_objc_send1(&w.cpu, mt_objc_send0(&w.cpu, uiimage, "alloc"),
                                              "initWithData:", data);
                bad_pieces += w.matches(img, h) ? 0u : 1u;
                mt_objc_release(&w.cpu, img);
                mt_objc_release(&w.cpu, data);
                pieces++;
                at += h.length;
            }
        }
        w.drain();
    }
    check(files == 1322 && cgbi == 113 && bad == 0,
          std::to_string(files) + " PNG files (" + std::to_string(cgbi) +
              " CgBI) decode through +[UIImage imageWithContentsOfFile:] with their IHDR size "
              "and alpha");
    check(named > 100 && bad_named == 0,
          std::to_string(named) + " root PNGs decode through +[UIImage imageNamed:]");
    check(packs == 40 && pieces == 473 && bad_pieces == 0,
          std::to_string(pieces) + " PNGs of " + std::to_string(packs) +
              " png/**/*.dat packs decode through -[UIImage initWithData:]");
    mt_cf_configure(nullptr);
}

} // namespace

extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, std::uint32_t target, std::uint32_t site) noexcept {
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

int main() {
    const fs::path bundle = MT_BUNDLE_DIR;
    if (!fs::is_directory(bundle)) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", MT_BUNDLE_DIR);
        return 77;
    }
    bundle_pngs(bundle);
    return g_failures == 0 ? 0 : 1;
}
