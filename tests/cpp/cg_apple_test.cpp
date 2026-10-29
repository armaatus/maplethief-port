// Apple's CoreGraphics as the reference for the texture loader's premultiplication (#57, ADR
// 0041), macOS only. The game's texture loader FUN_00009534 draws each decoded PNG 1:1 into a
// calloc'd power-of-two bitmap context (kCGImageAlphaPremultipliedLast or, for images without
// alpha, kCGImageAlphaNoneSkipLast, both | kCGBitmapByteOrder32Big), flipped by
// TranslateCTM(0, H) and ScaleCTM(1, -1), at (0, H - h, w, h), and uploads the buffer as
// GL_RGBA / GL_UNSIGNED_BYTE. The port's CoreGraphics (runtime/src/cg/, ADR 0036) and its
// tests model that as: the straight RGBA of the runtime's PNG decoder, each colour
// round(c * a / 255), alpha as decoded, 0xFF in the skip byte of an opaque image, image row y
// in buffer row H - 1 - y, the rest 0. This program runs the same draw through the real
// CoreGraphics of this Mac (iOS 4 itself cannot run here; the device check is #60) and
// compares every byte with that model:
//
//   1. synthetic, always: every colour x alpha pair (256 x 256) as a straight-alpha image, in
//      a 256 x 256 and, padded, a 200 x 100 in 256 x 128 context; an opaque RGBX image with
//      0x00 and 0x5A in its skip byte into the NoneSkipLast context.
//   2. local, when work/ipa/ is there: every PNG of MapleThief.app (1,322 files) and of the
//      png/**/*.dat packs (473), decoded by ImageIO and drawn with its pixels tagged
//      DeviceRGB: iOS 4 did no colour management, so the two PNGs with an iCCP profile
//      (MainMenuCompanyLogo.png, sprite/title/res/titled_0.png) are matched as the device
//      drew them, not as macOS would convert them.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cg/cg_objects.h"
#include "maplethief/recomp/calls.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

u32 pot(u32 n) {
    u32 p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

u8 premul(u32 c, u32 a) { return static_cast<u8>((c * a + 127) / 255); }

// The loader's draw of `image` through Apple's CoreGraphics; the buffer is W x H RGBA.
std::vector<u8> apple_draw(CGImageRef image, u32 *pw, u32 *ph) {
    const u32 w = static_cast<u32>(CGImageGetWidth(image));
    const u32 h = static_cast<u32>(CGImageGetHeight(image));
    const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
    const bool has_alpha = alpha >= kCGImageAlphaPremultipliedLast && alpha <= kCGImageAlphaFirst;
    *pw = pot(w);
    *ph = pot(h);
    std::vector<u8> buf(static_cast<std::size_t>(*pw) * *ph * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    const u32 info =
        static_cast<u32>(has_alpha ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast) |
        kCGBitmapByteOrder32Big;
    CGContextRef ctx = CGBitmapContextCreate(buf.data(), *pw, *ph, 8, *pw * 4, space, info);
    CGColorSpaceRelease(space);
    if (ctx == nullptr) {
        return {};
    }
    CGContextTranslateCTM(ctx, 0, static_cast<CGFloat>(*ph));
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, static_cast<CGFloat>(*ph - h), w, h), image);
    CGContextRelease(ctx);
    return buf;
}

// The model's buffer for straight RGBA `px` (w x h, rows of `stride` bytes).
std::vector<u8> model_draw(const u8 *px, u32 w, u32 h, std::size_t stride, bool has_alpha, u32 pw,
                           u32 ph) {
    std::vector<u8> buf(static_cast<std::size_t>(pw) * ph * 4, 0);
    for (u32 y = 0; y < h; y++) {
        u8 *row = buf.data() + static_cast<std::size_t>(ph - 1 - y) * pw * 4;
        for (u32 x = 0; x < w; x++) {
            const u8 *s = px + y * stride + x * 4;
            const u32 a = has_alpha ? s[3] : 255u;
            for (int c = 0; c < 3; c++) {
                row[x * 4 + c] = premul(s[c], a);
            }
            row[x * 4 + 3] = static_cast<u8>(a);
        }
    }
    return buf;
}

std::size_t differing(const std::vector<u8> &a, const std::vector<u8> &b) {
    if (a.size() != b.size()) {
        return a.size() + b.size();
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); i++) {
        n += a[i] != b[i] ? 1u : 0u;
    }
    return n;
}

CGImageRef image_of(const std::vector<u8> &px, u32 w, u32 h, u32 stride, u32 info) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, px.data(), px.size(), nullptr);
    CGImageRef image = CGImageCreate(w, h, 8, 32, stride, space, info, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    return image;
}

void synthetic() {
    // Every (colour, alpha) pair: x is the colour, y the alpha; three different channels.
    std::vector<u8> px(256 * 256 * 4);
    for (u32 a = 0; a < 256; a++) {
        for (u32 c = 0; c < 256; c++) {
            u8 *p = &px[(a * 256 + c) * 4];
            p[0] = static_cast<u8>(c);
            p[1] = static_cast<u8>(255 - c);
            p[2] = static_cast<u8>(c * 7);
            p[3] = static_cast<u8>(a);
        }
    }
    for (const u32 size : {256u, 0u}) {
        const u32 w = size != 0 ? 256 : 200, h = size != 0 ? 256 : 100;
        CGImageRef image = image_of(px, w, h, 256 * 4,
                                    static_cast<u32>(kCGImageAlphaLast) | kCGBitmapByteOrder32Big);
        u32 pw = 0, ph = 0;
        const std::vector<u8> got = apple_draw(image, &pw, &ph);
        CGImageRelease(image);
        const std::size_t bad = differing(got, model_draw(px.data(), w, h, 256 * 4, true, pw, ph));
        check(bad == 0, "CoreGraphics draws a straight " + std::to_string(w) + "x" +
                            std::to_string(h) + " image into the loader's " + std::to_string(pw) +
                            "x" + std::to_string(ph) +
                            " premultiplied context as round(c * a / 255), flipped, padding 0 (" +
                            std::to_string(bad) + " bytes differ)");
    }
    // An opaque RGBX image, whatever its skip byte holds: 0xFF in the context's skip byte.
    for (const u8 skip : {u8{0x00}, u8{0x5A}}) {
        for (std::size_t i = 3; i < px.size(); i += 4) {
            px[i] = skip;
        }
        CGImageRef image =
            image_of(px, 200, 100, 256 * 4,
                     static_cast<u32>(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big);
        u32 pw = 0, ph = 0;
        const std::vector<u8> got = apple_draw(image, &pw, &ph);
        CGImageRelease(image);
        const std::size_t bad =
            differing(got, model_draw(px.data(), 200, 100, 256 * 4, false, pw, ph));
        char what[160];
        std::snprintf(what, sizeof what,
                      "an RGBX image (skip byte 0x%02X) into the NoneSkipLast context: 0xFF in the "
                      "skip byte, colours as they are, padding 0 (%zu bytes differ)",
                      skip, bad);
        check(bad == 0, what);
    }
}

std::vector<u8> read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

u32 be32(const u8 *p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

// The length of the PNG at p up to the end of its IEND chunk, 0 if there is none.
std::size_t png_length(const u8 *p, std::size_t n) {
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || std::memcmp(p, sig, 8) != 0) {
        return 0;
    }
    std::size_t at = 8;
    while (at + 12 <= n) {
        const u32 len = be32(p + at);
        const bool end = std::memcmp(p + at + 4, "IEND", 4) == 0;
        at += 12 + std::size_t(len);
        if (end) {
            return at <= n ? at : 0;
        }
    }
    return 0;
}

struct Totals {
    u32 images = 0, bad = 0, undecoded = 0;
    std::size_t bad_bytes = 0;
};

void real_png(const std::string &name, const u8 *data, std::size_t size, Totals &t) {
    mt_cg::Decoded model;
    CFDataRef cf = CFDataCreate(nullptr, data, static_cast<CFIndex>(size));
    CGImageSourceRef source = CGImageSourceCreateWithData(cf, nullptr);
    CGImageRef decoded =
        source != nullptr ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    if (decoded == nullptr || !mt_cg::png_decode(data, size, &model, nullptr)) {
        t.undecoded++;
        std::printf("     %s: not decoded\n", name.c_str());
    } else {
        // ImageIO's pixels in DeviceRGB, as iOS 4 drew them (no colour management). Indexed
        // images keep their own colour space (no profile, nothing to convert).
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef tagged = CGImageCreateCopyWithColorSpace(decoded, space);
        CGColorSpaceRelease(space);
        u32 pw = 0, ph = 0;
        const std::vector<u8> got = apple_draw(tagged != nullptr ? tagged : decoded, &pw, &ph);
        if (tagged != nullptr) {
            CGImageRelease(tagged);
        }
        const std::size_t bad =
            differing(got, model_draw(model.pixels->data(), model.width, model.height,
                                      model.width * 4u, model.has_alpha, pw, ph));
        t.images++;
        t.bad += bad != 0 ? 1u : 0u;
        t.bad_bytes += bad;
        if (bad != 0 && t.bad <= 10) {
            std::printf("     %s: %zu bytes differ\n", name.c_str(), bad);
        }
    }
    if (decoded != nullptr) {
        CGImageRelease(decoded);
    }
    if (source != nullptr) {
        CFRelease(source);
    }
    CFRelease(cf);
}

void real_bundle(const fs::path &root) {
    Totals files, pieces;
    u32 packs = 0;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const fs::path rel = e.path().lexically_relative(root);
        const std::string ext = rel.extension().string(), name = rel.generic_string();
        if (ext == ".png") {
            const std::vector<u8> bytes = read_file(e.path());
            real_png(name, bytes.data(), bytes.size(), files);
        } else if (ext == ".dat" && name.rfind("png/", 0) == 0) {
            const std::vector<u8> bytes = read_file(e.path());
            packs++;
            std::size_t at = 0;
            int i = 0;
            while (at < bytes.size()) {
                const std::size_t n = png_length(bytes.data() + at, bytes.size() - at);
                if (n == 0) {
                    break;
                }
                real_png(name + "#" + std::to_string(i++), bytes.data() + at, n, pieces);
                at += n;
            }
        }
    }
    check(files.images == 1322 && files.undecoded == 0 && files.bad == 0,
          std::to_string(files.images) +
              " PNG files of the bundle through the loader's context: "
              "Apple's CoreGraphics gives the model's bytes (" +
              std::to_string(files.bad) + " files, " + std::to_string(files.bad_bytes) +
              " bytes differ)");
    check(packs == 40 && pieces.images == 473 && pieces.undecoded == 0 && pieces.bad == 0,
          std::to_string(pieces.images) + " PNGs of " + std::to_string(packs) +
              " png/**/*.dat packs the same (" + std::to_string(pieces.bad) + " differ)");
}

} // namespace

// The runtime library wants the generated code's dispatcher; nothing here calls the guest.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, std::uint32_t target, std::uint32_t site) noexcept {
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

int main() {
    synthetic();
    const fs::path bundle = MT_BUNDLE_DIR;
    if (fs::is_directory(bundle)) {
        real_bundle(bundle);
    } else {
        std::printf("real bundle: skipped, %s is missing (run scripts/bootstrap)\n", MT_BUNDLE_DIR);
    }
    return g_failures == 0 ? 0 : 1;
}
