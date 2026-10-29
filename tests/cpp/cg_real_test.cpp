// CoreGraphics / UIKit drawing on the real bundle (#51, ADR 0036), local only: skipped (exit
// 77) without work/ipa/ (scripts/bootstrap).
//
//   1. Every PNG file of MapleThief.app (1,322, 113 of them Apple CgBI) decodes through
//      +[UIImage imageWithContentsOfFile:] to the size in its IHDR, with the alpha info the
//      texture loaders expect (kCGImageAlphaLast with an alpha channel or tRNS, else
//      kCGImageAlphaNoneSkipLast); the root PNGs also through +imageNamed:.
//   2. Every PNG packed in png/**/*.dat (473, split at their IEND chunks) decodes through
//      -[UIImage initWithData:], as the engine's texture loader FUN_00009534 does.
//   3. Every one of these 1,795 PNGs through FUN_00009534's CoreGraphics sequence (#57, ADR
//      0041): -initWithData:, a calloc'd power-of-two bitmap context (premultipliedLast, or
//      NoneSkipLast without alpha), TranslateCTM(0, H), ScaleCTM(1, -1), DrawImage at
//      (0, H - h, w, h). The buffer the game uploads equals the model that cg_apple_test checks
//      against Apple's CoreGraphics: round(c * a / 255) of the decoded straight RGBA, alpha as
//      decoded (0xFF in the skip byte), image row y in buffer row H - 1 - y, the rest 0.
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

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// The game's power of two (FUN_00006558): n if it is one, else the next one.
u32 pot(u32 n) {
    u32 p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
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
    // A call of an HLE import with AAPCS arguments (r0-r3, then the stack).
    u32 call(void (*fn)(mt_cpu *, u32) noexcept, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        const u32 stacked = args.size() > 4 ? static_cast<u32>(args.size()) - 4u : 0u;
        cpu.r[MT_SP] = sp - 8u * ((stacked + 1u) / 2u);
        u32 i = 0;
        for (u32 a : args) {
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
    // FUN_00009534's draw of the PNG `png` (n bytes): true when the buffer it uploads equals
    // the model (see the file comment).
    bool loader_matches(const u8 *png, std::size_t n) {
        mt_cg::Decoded model;
        if (!mt_cg::png_decode(png, n, &model, nullptr)) {
            return false;
        }
        const u32 data = mt_cf::data_create(&cpu, png, static_cast<u32>(n), false);
        const u32 uiimage = mt_objc_class_named(rt, "UIImage");
        const u32 img =
            mt_objc_send1(&cpu, mt_objc_send0(&cpu, uiimage, "alloc"), "initWithData:", data);
        const u32 cg = img != 0 ? mt_objc_send0(&cpu, img, "CGImage") : 0u;
        bool ok = cg != 0;
        if (ok) {
            const u32 w = call(mt_hle_CGImageGetWidth, {cg});
            const u32 h = call(mt_hle_CGImageGetHeight, {cg});
            const u32 alpha = call(mt_hle_CGImageGetAlphaInfo, {cg});
            const u32 pw = pot(w), ph = pot(h);
            const u32 info = (alpha >= 1 && alpha <= 4 ? 1u : 5u) | 0x4000u;
            const u32 pixels = mt_heap_calloc(mem, pw * ph, 4);
            const u32 space = call(mt_hle_CGColorSpaceCreateDeviceRGB, {});
            const u32 ctx =
                call(mt_hle_CGBitmapContextCreate, {pixels, pw, ph, 8, pw * 4, space, info});
            ok = pixels != 0 && ctx != 0 && w == model.width && h == model.height;
            if (ok) {
                call(mt_hle_CGContextTranslateCTM, {ctx, fbits(0), fbits(static_cast<float>(ph))});
                call(mt_hle_CGContextScaleCTM, {ctx, fbits(1), fbits(-1)});
                call(mt_hle_CGContextDrawImage,
                     {ctx, fbits(0), fbits(static_cast<float>(ph - h)),
                      fbits(static_cast<float>(w)), fbits(static_cast<float>(h)), cg});
                const u8 *got = cpu.mem + pixels;
                for (u32 y = 0; y < ph && ok; y++) {
                    for (u32 x = 0; x < pw && ok; x++) {
                        u8 want[4] = {0, 0, 0, 0};
                        const u32 iy = ph - 1 - y; // the image row in buffer row y
                        if (x < w && iy < h) {
                            const u8 *s = model.pixels->data() + (std::size_t(iy) * w + x) * 4;
                            const u32 a = model.has_alpha ? s[3] : 255u;
                            for (int c = 0; c < 3; c++) {
                                want[c] = static_cast<u8>((s[c] * a + 127) / 255);
                            }
                            want[3] = static_cast<u8>(a);
                        }
                        ok = std::memcmp(got + (std::size_t(y) * pw + x) * 4, want, 4) == 0;
                    }
                }
            }
            if (ctx != 0) {
                call(mt_hle_CGContextRelease, {ctx});
            }
            call(mt_hle_CGColorSpaceRelease, {space});
            if (pixels != 0) {
                mt_heap_free(mem, pixels);
            }
        }
        mt_objc_release(&cpu, img);
        mt_objc_release(&cpu, data);
        return ok;
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
    u32 loaded = 0, bad_loaded = 0;
    auto load = [&](const std::string &name, const u8 *png, std::size_t n) {
        loaded++;
        if (!w.loader_matches(png, n) && bad_loaded++ < 10) {
            std::printf("     %s: the loader's buffer differs from the model\n", name.c_str());
        }
    };
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
            load(utf8(rel), bytes.data(), bytes.size());
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
                load(utf8(rel) + "#" + std::to_string(pieces), bytes.data() + at, h.length);
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
    check(
        loaded == 1322 + 473 && bad_loaded == 0,
        std::to_string(loaded) +
            " PNGs through FUN_00009534's CoreGraphics sequence: the "
            "uploaded buffer is round(c * a / 255) of the straight pixels, flipped, padded with 0 "
            "(" +
            std::to_string(bad_loaded) + " differ)");
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
