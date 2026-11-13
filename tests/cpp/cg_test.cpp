// CoreGraphics and UIKit drawing (#51, ADR 0036) on a real guest address space: every CG and
// UIKit C import through its HLE entry (with the iOS argument layout), the host classes
// UIImage, UIFont, UIColor, UIBezierPath and NSString's drawing selectors through messages,
// the game's own drawing sequences (TextCache_DrawText, the texture loader FUN_00009534, the
// map tile cutter FUN_0000d6d0, colorAtPixel:onImage:, imageWithImage:scaledToSize:), and
// golden images of synthetic drawings: gradients, a clip, text and image draws
// (tests/golden/data/cg_*.png; MAPLETHIEF_UPDATE_GOLDEN=1 rewrites them).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_hle.h"
#include "cg/cg_objects.h"
#include "golden.h"
#include "maplethief/cf.h"
#include "maplethief/cg.h"
#include "maplethief/gfx.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/snapshot.h" // mt_crc32
#include "maplethief/texpack.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;
namespace golden = maplethief::golden;

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

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
float bf(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// --- a guest array for CGGradientCreateWithColors (Foundation's NSArray is #49's) -----------

std::vector<u32> g_array;
void array_count(mt_cpu *cpu) { cpu->r[0] = static_cast<u32>(g_array.size()); }
void array_object_at(mt_cpu *cpu) { cpu->r[0] = g_array.at(cpu->r[2]); }
const mt_objc_host_method k_array_methods[] = {{"count", array_count},
                                               {"objectAtIndex:", array_object_at}};
const mt_objc_host_class k_test_array = {"CGTestArray", "NSObject", 4, k_array_methods, 2,
                                         nullptr,       0};

// A guest address space with the runtime's host classes and CF bound to it.
struct Cg {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;

    Cg() {
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
        REQUIRE(mt_objc_add_host_class(rt, &k_test_array, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
    }
    ~Cg() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Cg(const Cg &) = delete;
    Cg &operator=(const Cg &) = delete;

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

    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    // objc_msgSend_stret into a fresh guest block of `size` bytes; returns its address.
    u32 send_stret(u32 self, const char *sel, std::initializer_list<u32> args, u32 size) {
        const std::vector<u32> v(args);
        const u32 result = alloc(size);
        mt_objc_msg_stret(&cpu, result, self, mt_objc_sel(rt, sel), v.data(),
                          static_cast<u32>(v.size()));
        return result;
    }

    u32 alloc(u32 n) { return mt_heap_calloc(mem, 1, n); }
    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    float getf(u32 addr) const { return bf(get32(addr)); }
    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    const char *class_name(u32 obj) const {
        return mt_objc_class_name(rt, mt_objc_class_of(rt, obj));
    }
    u32 string(const std::u16string &s) { return mt_cf::string_create(&cpu, s, false); }
    u32 string(const char *ascii) {
        return string(std::u16string(ascii, ascii + std::strlen(ascii)));
    }
    u32 data(const std::vector<u8> &bytes) {
        return mt_cf::data_create(&cpu, bytes.data(), static_cast<u32>(bytes.size()), false);
    }
    u32 floats(std::initializer_list<float> fs) {
        const u32 g = alloc(4u * static_cast<u32>(fs.size()));
        u32 i = 0;
        for (const float f : fs) {
            put32(g + 4u * i++, fb(f));
        }
        return g;
    }

    // A bitmap context over guest memory, as the game makes them.
    u32 space = 0;
    u32 context(u32 buffer, u32 w, u32 h, u32 info) {
        if (space == 0) {
            space = call(mt_hle_CGColorSpaceCreateDeviceRGB, {});
        }
        return call(mt_hle_CGBitmapContextCreate, {buffer, w, h, 8, w * 4, space, info});
    }
    // UIKit's flip of a context (what UIGraphicsBeginImageContext does).
    void flip(u32 ctx, u32 h) {
        call(mt_hle_CGContextTranslateCTM, {ctx, fb(0), fb(static_cast<float>(h))});
        call(mt_hle_CGContextScaleCTM, {ctx, fb(1), fb(-1)});
    }
    u32 color(float r, float g, float b, float a) {
        return send(klass("UIColor"),
                    "colorWithRed:green:blue:alpha:", {fb(r), fb(g), fb(b), fb(a)});
    }
    u32 array(std::initializer_list<u32> objs) {
        g_array = objs;
        return mt_objc_create_instance(rt, klass("CGTestArray"));
    }
    u32 font(const char *name, float size) {
        return send(klass("UIFont"), "fontWithName:size:", {string(name), fb(size)});
    }
};

// Guest memory as a golden image: `w` x `h` RGBA pixels at `addr` (the context's bytes as they
// are: premultiplied).
golden::Image snapshot(const Cg &w, u32 addr, int width, int height) {
    golden::Image img(width, height);
    std::memcpy(img.bytes().data(), w.cpu.mem + addr, img.bytes().size());
    return img;
}

// A UIImage's pixels as a golden image (straight RGBA).
golden::Image uiimage_pixels(Cg &w, u32 uiimage) {
    const u32 cg = w.send(uiimage, "CGImage");
    const mt_cg::ImageInfo *info = mt_cg::image_info(&w.cpu, cg);
    REQUIRE(info != nullptr);
    std::vector<u8> rgba;
    REQUIRE(mt_cg::image_rgba(&w.cpu, cg, rgba));
    golden::Image img(static_cast<int>(info->width), static_cast<int>(info->height));
    img.bytes() = rgba;
    return img;
}

void check_golden(const golden::Image &img, const char *name) {
    const std::filesystem::path path =
        std::filesystem::path(MT_GOLDEN_DATA_DIR) / (std::string(name) + ".png");
    // Floating point differs in the last bit between compilers and FMA contraction: a channel
    // may be off by a little on a few edge pixels.
    const auto r = golden::check_golden(img, path, std::string("cg/") + name, {2, 0.002});
    CHECK_MESSAGE(r.passed, r.message);
}

// A PNG of the synthetic test pattern (straight RGBA, alpha in the bottom half).
std::vector<u8> pattern_png(int width, int height, golden::Image *pattern) {
    *pattern = golden::test_pattern(width, height);
    std::vector<u8> png;
    REQUIRE(mt_cg::png_encode(pattern->bytes().data(), static_cast<u32>(width),
                              static_cast<u32>(height), 4, png));
    return png;
}

u8 premul(u8 c, u8 a) { return static_cast<u8>((c * a + 127) / 255); }

// --- CgBI fixture: Apple's iPhone PNG (CgBI chunk, raw deflate, BGRA, premultiplied) ----------

u32 crc32(const u8 *p, std::size_t n, u32 crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void chunk(std::vector<u8> &out, const char *type, const std::vector<u8> &body) {
    const u32 n = static_cast<u32>(body.size());
    const u8 len[4] = {u8(n >> 24), u8(n >> 16), u8(n >> 8), u8(n)};
    out.insert(out.end(), len, len + 4);
    std::vector<u8> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    out.insert(out.end(), typed.begin(), typed.end());
    const u32 c = crc32(typed.data(), typed.size());
    const u8 crc[4] = {u8(c >> 24), u8(c >> 16), u8(c >> 8), u8(c)};
    out.insert(out.end(), crc, crc + 4);
}

// A CgBI PNG of straight RGBA `rgba` (w x h): stored as premultiplied BGRA, one stored raw
// deflate block (no zlib header), as Xcode's pngcrush wrote them.
std::vector<u8> cgbi_png(const std::vector<u8> &rgba, u32 width, u32 height) {
    std::vector<u8> raw;
    for (u32 y = 0; y < height; y++) {
        raw.push_back(0); // filter: none
        for (u32 x = 0; x < width; x++) {
            const u8 *p = &rgba[(y * width + x) * 4];
            raw.push_back(premul(p[2], p[3]));
            raw.push_back(premul(p[1], p[3]));
            raw.push_back(premul(p[0], p[3]));
            raw.push_back(p[3]);
        }
    }
    const u32 n = static_cast<u32>(raw.size());
    std::vector<u8> deflate = {1, u8(n), u8(n >> 8), u8(~n), u8(~n >> 8)};
    deflate.insert(deflate.end(), raw.begin(), raw.end());
    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    chunk(png, "CgBI", {0x50, 0x00, 0x20, 0x06});
    chunk(png, "IHDR",
          {u8(width >> 24), u8(width >> 16), u8(width >> 8), u8(width), u8(height >> 24),
           u8(height >> 16), u8(height >> 8), u8(height), 8, 6, 0, 0, 0});
    chunk(png, "IDAT", deflate);
    chunk(png, "IEND", {});
    return png;
}

// A synthetic app bundle with images for +imageNamed: / +imageWithContentsOfFile:.
struct Bundle {
    std::filesystem::path dir;
    std::string path;
    Bundle() {
        // One directory per process: CTest runs the test cases in parallel (#153).
        dir = std::filesystem::temp_directory_path() /
              ("maplethief-cg-test-bundle-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(dir);
        golden::Image pattern;
        write("Button.png", pattern_png(12, 10, &pattern));
        write("png/ui/Case.png", pattern_png(8, 8, &pattern));
        write("Broken.png", {'n', 'o', 't', ' ', 'p', 'n', 'g'});
        mt_cf_config cfg = mt_cf_config_default();
        path = dir.string();
        cfg.bundle_dir = path.c_str();
        mt_cf_configure(&cfg);
    }
    ~Bundle() {
        mt_cf_configure(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Bundle(const Bundle &) = delete;
    Bundle &operator=(const Bundle &) = delete;
    void write(const char *rel, const std::vector<u8> &bytes) {
        const std::filesystem::path p = dir / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
};

// Rows of an RGBA buffer with any alpha: [first, last], or {-1, -1}.
std::pair<int, int> ink_rows(const Cg &w, u32 addr, u32 width, u32 height) {
    int first = -1, last = -1;
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            if (w.cpu.mem[addr + (y * width + x) * 4 + 3] != 0) {
                if (first < 0) {
                    first = static_cast<int>(y);
                }
                last = static_cast<int>(y);
                break;
            }
        }
    }
    return {first, last};
}

} // namespace

// --- the imports ------------------------------------------------------------------------------

TEST_CASE("CG: the registry implements every CoreGraphics and UIKit drawing import") {
    // The game's 49 CG / UIGraphics imports (docs/re/imports.md); tests/test_hle_cg_real.py
    // checks the list against the real slice.
    const char *names[] = {"_CGAffineTransformIdentity",
                           "_CGAffineTransformMakeRotation",
                           "_CGBitmapContextCreate",
                           "_CGColorGetComponents",
                           "_CGColorSpaceCreateDeviceRGB",
                           "_CGColorSpaceRelease",
                           "_CGContextAddPath",
                           "_CGContextClip",
                           "_CGContextDrawImage",
                           "_CGContextDrawLinearGradient",
                           "_CGContextDrawRadialGradient",
                           "_CGContextRelease",
                           "_CGContextRestoreGState",
                           "_CGContextSaveGState",
                           "_CGContextScaleCTM",
                           "_CGContextSetBlendMode",
                           "_CGContextSetGrayFillColor",
                           "_CGContextSetTextDrawingMode",
                           "_CGContextTranslateCTM",
                           "_CGDataProviderCopyData",
                           "_CGDataProviderCreateWithCFData",
                           "_CGDataProviderRelease",
                           "_CGGradientCreateWithColors",
                           "_CGGradientRelease",
                           "_CGImageCreate",
                           "_CGImageGetAlphaInfo",
                           "_CGImageGetBitmapInfo",
                           "_CGImageGetBitsPerComponent",
                           "_CGImageGetBitsPerPixel",
                           "_CGImageGetBytesPerRow",
                           "_CGImageGetColorSpace",
                           "_CGImageGetDataProvider",
                           "_CGImageGetHeight",
                           "_CGImageGetRenderingIntent",
                           "_CGImageGetWidth",
                           "_CGImageRelease",
                           "_CGRectContainsPoint",
                           "_CGRectInset",
                           "_CGRectNull",
                           "_CGRectZero",
                           "_UIGraphicsBeginImageContext",
                           "_UIGraphicsBeginImageContextWithOptions",
                           "_UIGraphicsEndImageContext",
                           "_UIGraphicsGetCurrentContext",
                           "_UIGraphicsGetImageFromCurrentImageContext",
                           "_UIGraphicsPopContext",
                           "_UIGraphicsPushContext",
                           "_UIImagePNGRepresentation",
                           "_UIRectFill"};
    CHECK(sizeof names / sizeof names[0] == 49);
    u32 n = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n);
    for (const char *name : names) {
        bool found = false;
        for (u32 i = 0; i < n; i++) {
            if (std::strcmp(hle[i].name, name) == 0) {
                const bool data = std::strcmp(name, "_CGRectNull") == 0 ||
                                  std::strcmp(name, "_CGRectZero") == 0 ||
                                  std::strcmp(name, "_CGAffineTransformIdentity") == 0;
                found = data ? hle[i].init != nullptr : hle[i].fn != nullptr;
            }
        }
        CHECK_MESSAGE(found, name);
    }
}

TEST_CASE("CG: geometry: CGRectZero/Null, CGAffineTransformIdentity, CGRectInset, ContainsPoint") {
    Cg w;
    const u32 slot = w.alloc(24);
    mt_hle_init_CGRectZero(&w.cpu, 0, slot);
    CHECK(w.getf(slot) == 0.0f);
    CHECK(w.getf(slot + 12) == 0.0f);
    mt_hle_init_CGRectNull(&w.cpu, 0, slot);
    CHECK(std::isinf(w.getf(slot)));
    CHECK(std::isinf(w.getf(slot + 4)));
    CHECK(w.getf(slot + 8) == 0.0f);
    mt_hle_init_CGAffineTransformIdentity(&w.cpu, 0, slot);
    const float id[6] = {1, 0, 0, 1, 0, 0};
    for (u32 i = 0; i < 6; i++) {
        CHECK(w.getf(slot + 4 * i) == id[i]);
    }

    // CGRectInset: stret; the rect in r1-r3 + [sp], dx/dy on the stack.
    const u32 out = w.alloc(16);
    w.call(mt_hle_CGRectInset, {out, fb(10), fb(20), fb(100), fb(50), fb(5), fb(4)});
    CHECK(w.getf(out) == 15.0f);
    CHECK(w.getf(out + 4) == 24.0f);
    CHECK(w.getf(out + 8) == 90.0f);
    CHECK(w.getf(out + 12) == 42.0f);
    w.call(mt_hle_CGRectInset, {out, fb(0), fb(0), fb(10), fb(10), fb(6), fb(0)});
    CHECK(std::isinf(w.getf(out))); // negative width: CGRectNull

    // CGRectContainsPoint: min edges in, max edges out.
    CHECK(w.call(mt_hle_CGRectContainsPoint, {fb(0), fb(0), fb(10), fb(10), fb(0), fb(9.5f)}) ==
          1u);
    CHECK(w.call(mt_hle_CGRectContainsPoint, {fb(0), fb(0), fb(10), fb(10), fb(10), fb(5)}) == 0u);
    CHECK(w.call(mt_hle_CGRectContainsPoint, {fb(10), fb(10), fb(-5), fb(-5), fb(6), fb(6)}) == 1u);

    w.call(mt_hle_CGAffineTransformMakeRotation, {slot, fb(1.5707964f)});
    CHECK(w.getf(slot) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(w.getf(slot + 4) == doctest::Approx(1.0));
    CHECK(w.getf(slot + 8) == doctest::Approx(-1.0));
    CHECK(w.getf(slot + 16) == 0.0f);
}

TEST_CASE("CG: bitmap contexts: supported formats, NULL contexts are ignored and logged") {
    Cg w;
    const u32 buf = w.alloc(16 * 16 * 4);
    const u32 ctx = w.context(buf, 16, 16, 0x4001u);
    CHECK(ctx != 0u);
    CHECK(std::strcmp(w.class_name(ctx), "__NSCFType") == 0);
    CHECK(w.context(buf, 16, 16, 1u) != 0u);           // PremultipliedLast
    CHECK(w.context(buf, 16, 16, 5u | 0x4000u) != 0u); // NoneSkipLast
    CHECK(w.context(buf, 16, 16, 2u | 0x2000u) != 0u); // BGRA little-endian
    CHECK(w.context(buf, 16, 16, 3u) == 0u);           // straight alpha: unsupported
    CHECK(logged("CGBitmapContextCreate: unsupported parameter combination"));
    CHECK(w.call(mt_hle_CGBitmapContextCreate, {buf, 16, 16, 8, 32, w.space, 1}) == 0u);

    w.call(mt_hle_CGContextSaveGState, {0});
    CHECK(logged("CGContextSaveGState: invalid context 0x00000000"));
    w.call(mt_hle_CGContextRestoreGState, {ctx});
    CHECK(logged("CGContextRestoreGState: nothing saved"));
    w.call(mt_hle_CGContextRelease, {0}); // NULL is allowed
    w.call(mt_hle_CGContextRelease, {ctx});
    w.call(mt_hle_CGColorSpaceRelease, {w.space});
}

TEST_CASE("CG: colours: UIColor constructors, -CGColor, CGColorGetComponents") {
    Cg w;
    const u32 cls = w.klass("UIColor");
    auto comps = [&w](u32 uicolor) {
        const u32 cg = w.send(uicolor, "CGColor");
        REQUIRE(cg != 0u);
        CHECK(w.send(uicolor, "CGColor") == cg); // made once, owned by the UIColor
        const u32 p = w.call(mt_hle_CGColorGetComponents, {cg});
        return std::vector<float>{w.getf(p), w.getf(p + 4), w.getf(p + 8), w.getf(p + 12)};
    };
    CHECK(comps(w.send(cls, "blackColor")) == std::vector<float>{0, 0, 0, 1});
    CHECK(comps(w.send(cls, "whiteColor")) == std::vector<float>{1, 1, 1, 1});
    CHECK(comps(w.send(cls, "clearColor")) == std::vector<float>{0, 0, 0, 0});
    CHECK(comps(w.send(cls, "colorWithWhite:alpha:", {fb(0.5f), fb(0.25f)})) ==
          std::vector<float>{0.5f, 0.5f, 0.5f, 0.25f});
    CHECK(comps(w.color(0.1f, 0.2f, 0.3f, 0.4f)) == std::vector<float>{0.1f, 0.2f, 0.3f, 0.4f});
    const u32 alloc = w.send(cls, "alloc");
    const u32 c = w.send(alloc, "initWithRed:green:blue:alpha:", {fb(1), fb(0.5f), fb(0), fb(1)});
    CHECK(c == alloc);
    CHECK(comps(c) == std::vector<float>{1, 0.5f, 0, 1});
    // The host's read of a UIColor (#194: the key pad's haptics): its components; nil and
    // anything else 0, the output untouched.
    float rgba[4] = {9, 9, 9, 9};
    CHECK(mt_cg_color_rgba(&w.cpu, w.color(0, 0.5f, 1, 1), rgba) == 1);
    CHECK(std::vector<float>(rgba, rgba + 4) == std::vector<float>{0, 0.5f, 1, 1});
    CHECK(mt_cg_color_rgba(&w.cpu, w.send(cls, "clearColor"), rgba) == 1);
    CHECK(rgba[3] == 0);
    rgba[0] = 9;
    CHECK(mt_cg_color_rgba(&w.cpu, 0, rgba) == 0);
    CHECK(mt_cg_color_rgba(&w.cpu, w.font("Helvetica", 12), rgba) == 0);
    CHECK(rgba[0] == 9);
    w.send(c, "release");
    CHECK(w.call(mt_hle_CGColorGetComponents, {0}) == 0u);
}

// --- golden images ------------------------------------------------------------------------------

TEST_CASE("CG golden: linear and radial gradients") {
    Cg w;
    const u32 red = w.send(w.color(1, 0, 0, 1), "CGColor");
    const u32 yellow = w.send(w.color(1, 1, 0, 1), "CGColor");
    const u32 blue = w.send(w.color(0, 0, 1, 1), "CGColor");
    const u32 clear = w.send(w.color(0, 0, 1, 0), "CGColor");
    const u32 white = w.send(w.color(1, 1, 1, 1), "CGColor");

    // Linear: three stops at explicit locations, horizontal, extended past both ends.
    const u32 buf = w.alloc(64 * 32 * 4);
    const u32 ctx = w.context(buf, 64, 32, 0x4001u);
    const u32 g = w.call(mt_hle_CGGradientCreateWithColors,
                         {w.space, w.array({red, yellow, blue}), w.floats({0, 0.3f, 1})});
    REQUIRE(g != 0u);
    w.call(mt_hle_CGContextDrawLinearGradient, {ctx, g, fb(8), fb(0), fb(56), fb(32), 3});
    // CG's y is up: the bottom-left corner lies before the start (red), the top-right one
    // after the end (blue). RGBA in memory.
    CHECK(w.get32(buf + 31 * 64 * 4) == 0xFF0000FFu);
    CHECK(w.get32(buf + 63 * 4) == 0xFFFF0000u);
    check_golden(snapshot(w, buf, 64, 32), "cg_gradient_linear");
    w.call(mt_hle_CGGradientRelease, {g});

    // Radial: white centre to transparent blue, drawn after the end only, evenly spaced stops.
    const u32 buf2 = w.alloc(64 * 64 * 4);
    const u32 ctx2 = w.context(buf2, 64, 64, 0x4001u);
    const u32 g2 = w.call(mt_hle_CGGradientCreateWithColors, {w.space, w.array({white, clear}), 0});
    REQUIRE(g2 != 0u);
    w.call(mt_hle_CGContextDrawRadialGradient,
           {ctx2, g2, fb(32), fb(32), fb(0), fb(32), fb(32), fb(24), 0});
    CHECK(w.get32(buf2) == 0u); // no extension: the corners stay clear
    check_golden(snapshot(w, buf2, 64, 64), "cg_gradient_radial");
    w.call(mt_hle_CGGradientRelease, {g2});
    w.call(mt_hle_CGContextRelease, {ctx});
    w.call(mt_hle_CGContextRelease, {ctx2});

    // Not a CGColor in the array: NULL, logged.
    CHECK(w.call(mt_hle_CGGradientCreateWithColors, {w.space, w.array({w.string("x")}), 0}) == 0u);
    CHECK(logged("CGGradientCreateWithColors: element 0"));
}

TEST_CASE("CG golden: a rounded-rect clip with a gradient and a stroke (Chartboost's button)") {
    Cg w;
    const u32 buf = w.alloc(64 * 64 * 4);
    const u32 ctx = w.context(buf, 64, 64, 0x4001u);
    w.flip(ctx, 64);
    w.call(mt_hle_UIGraphicsPushContext, {ctx});
    CHECK(w.call(mt_hle_UIGraphicsGetCurrentContext, {}) == ctx);
    w.send(w.send(w.klass("UIColor"), "whiteColor"), "set");
    w.call(mt_hle_UIRectFill, {fb(0), fb(0), fb(64), fb(64)});

    const u32 path =
        w.send(w.klass("UIBezierPath"),
               "bezierPathWithRoundedRect:cornerRadius:", {fb(8), fb(8), fb(48), fb(40), fb(12)});
    REQUIRE(path != 0u);
    const u32 cgpath = w.send(path, "CGPath");
    REQUIRE(cgpath != 0u);
    w.call(mt_hle_CGContextSaveGState, {ctx});
    w.call(mt_hle_CGContextAddPath, {ctx, cgpath});
    w.call(mt_hle_CGContextClip, {ctx});
    const u32 g = w.call(mt_hle_CGGradientCreateWithColors,
                         {w.space,
                          w.array({w.send(w.color(0.4f, 0.9f, 0.3f, 1), "CGColor"),
                                   w.send(w.color(0.1f, 0.4f, 0.1f, 1), "CGColor")}),
                          0});
    w.call(mt_hle_CGContextDrawLinearGradient, {ctx, g, fb(0), fb(8), fb(0), fb(48), 0});
    w.call(mt_hle_CGContextRestoreGState, {ctx});

    // A transparent rect (UIRectFill copies) and a stroked triangle on top, unclipped.
    w.send(w.send(w.klass("UIColor"), "clearColor"), "setFill");
    w.call(mt_hle_UIRectFill, {fb(0), fb(56), fb(8), fb(8)});
    const u32 tri = w.send(w.klass("UIBezierPath"), "bezierPath");
    w.send(tri, "moveToPoint:", {fb(20), fb(52)});
    w.send(tri, "addLineToPoint:", {fb(44), fb(52)});
    w.send(tri, "addLineToPoint:", {fb(32), fb(60)});
    w.send(tri, "closePath");
    w.send(tri, "setLineWidth:", {fb(2)});
    w.send(w.color(0, 0, 0, 1), "setStroke");
    w.send(tri, "stroke");
    w.send(w.color(1, 0, 0, 0.5f), "setFill");
    w.send(tri, "fill");
    w.call(mt_hle_UIGraphicsPopContext, {});
    CHECK(w.call(mt_hle_UIGraphicsGetCurrentContext, {}) == 0u);

    CHECK(w.get32(buf) == 0xFFFFFFFFu);            // outside the clip: white
    CHECK(w.get32(buf + (60 * 64 + 2) * 4) == 0u); // the copied clear rect
    check_golden(snapshot(w, buf, 64, 64), "cg_clip");
    w.call(mt_hle_CGGradientRelease, {g});
    w.call(mt_hle_CGContextRelease, {ctx});
}

TEST_CASE("CG golden: text: Latin and Hangul, synthetic bold, as UIKit draws it") {
    Cg w;
    const u32 marker = w.font("MarkerFelt-Thin", 16);
    const u32 bold = w.font("Helvetica-Bold", 14);
    REQUIRE(marker != 0u);
    REQUIRE(bold != 0u);
    w.call(mt_hle_UIGraphicsBeginImageContext, {fb(200), fb(44)});
    w.send(w.send(w.klass("UIColor"), "whiteColor"), "set");
    w.call(mt_hle_UIRectFill, {fb(0), fb(0), fb(200), fb(44)});
    w.send(w.send(w.klass("UIColor"), "blackColor"), "set");
    const u32 latin = w.string("MapleStory Thief 1.5");
    const u32 size = w.send_stret(latin, "drawAtPoint:withFont:", {fb(4), fb(2), marker}, 8);
    const u32 measured = w.send_stret(latin, "sizeWithFont:", {marker}, 8);
    CHECK(w.getf(size) == w.getf(measured));
    CHECK(w.getf(size + 4) == w.getf(measured + 4));
    CHECK(w.getf(size) > 100.0f);
    CHECK(w.getf(size) == std::floor(w.getf(size))); // whole points
    w.send(w.color(0.6f, 0.1f, 0.1f, 1), "set");
    w.send_stret(w.string(u"메이플스토리 도적 ←→"), "drawAtPoint:withFont:", {fb(4), fb(22), bold},
                 8);
    const u32 image = w.call(mt_hle_UIGraphicsGetImageFromCurrentImageContext, {});
    w.call(mt_hle_UIGraphicsEndImageContext, {});
    REQUIRE(image != 0u);
    check_golden(uiimage_pixels(w, image), "cg_text");

    // Bold is wider than regular at the same size.
    const u32 regular = w.font("Helvetica", 14);
    const u32 s1 = w.send_stret(w.string("Bold"), "sizeWithFont:", {regular}, 8);
    const u32 s2 = w.send_stret(w.string("Bold"), "sizeWithFont:", {bold}, 8);
    CHECK(w.getf(s2) >= w.getf(s1));
}

TEST_CASE("CG golden: TextCache_DrawText's sequence draws upside-down text, as on the device") {
    Cg w;
    // 0x9031: a 512 x 32 premultiplied RGBA context over a global buffer, white fill, push,
    // MarkerFelt-Thin, -drawAtPoint:(0, 0) withFont:, pop, release.
    const u32 buf = w.alloc(512 * 32 * 4);
    const u32 ctx = w.context(buf, 512, 32, 1);
    w.call(mt_hle_CGContextSetGrayFillColor, {ctx, fb(1), fb(1)});
    w.call(mt_hle_CGContextSetTextDrawingMode, {ctx, 0});
    w.call(mt_hle_UIGraphicsPushContext, {ctx});
    const u32 font = w.send(w.font("MarkerFelt-Thin", 12), "fontWithSize:", {fb(18)});
    REQUIRE(font != 0u);
    const u32 text = w.string("Henesys 1/3");
    const u32 size = w.send_stret(text, "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
    w.call(mt_hle_UIGraphicsPopContext, {});
    w.call(mt_hle_CGContextRelease, {ctx});
    CHECK(w.getf(size) > 40.0f);
    CHECK(w.getf(size + 4) >= 18.0f);
    // The context is not flipped: the line lies at the bottom of the bitmap.
    const auto rows = ink_rows(w, buf, 512, 32);
    REQUIRE(rows.first >= 0);
    CHECK(rows.first >= 8);
    CHECK(rows.second >= 28);
    // White text: every inked pixel is grey = alpha (premultiplied white).
    bool white = true;
    for (u32 i = 0; i < 512 * 32; i++) {
        const u8 *p = w.cpu.mem + buf + i * 4;
        white = white && p[0] == p[3] && p[1] == p[3] && p[2] == p[3];
    }
    CHECK(white);
    // The first 128 columns, as the bitmap holds them (row 0 = the top of the texture).
    golden::Image crop(128, 32);
    for (int y = 0; y < 32; y++) {
        std::memcpy(crop.bytes().data() + y * 128 * 4, w.cpu.mem + buf + y * 512 * 4, 128 * 4);
    }
    check_golden(crop, "cg_text_cache");

    // The invisible mode measures without drawing.
    const u32 buf2 = w.alloc(64 * 32 * 4);
    const u32 ctx2 = w.context(buf2, 64, 32, 1);
    w.call(mt_hle_CGContextSetTextDrawingMode, {ctx2, 3});
    w.call(mt_hle_UIGraphicsPushContext, {ctx2});
    w.send_stret(text, "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
    w.call(mt_hle_UIGraphicsPopContext, {});
    CHECK(ink_rows(w, buf2, 64, 32).first == -1);
    w.call(mt_hle_CGContextRelease, {ctx2});
}

namespace {

// TextCache_DrawText's sequence (0x9031) over `buf`: a w x 32 context, the buffer cleared, white
// fill, push, -drawAtPoint:(0, 0) withFont:, pop, release. `clip` clips to a rectangle first.
void text_cache_draw(Cg &w, u32 buf, u32 width, const char *text, u32 font, bool clip = false) {
    const u32 ctx = w.context(buf, width, 32, 1);
    std::memset(w.cpu.mem + buf, 0, static_cast<std::size_t>(width) * 32 * 4);
    w.call(mt_hle_CGContextSetGrayFillColor, {ctx, fb(1), fb(1)});
    w.call(mt_hle_CGContextSetTextDrawingMode, {ctx, 0});
    if (clip) {
        const u32 path = w.send(w.klass("UIBezierPath"), "bezierPathWithRoundedRect:cornerRadius:",
                                {fb(0), fb(0), fb(static_cast<float>(width)), fb(32), fb(0)});
        w.call(mt_hle_CGContextAddPath, {ctx, w.send(path, "CGPath")});
        w.call(mt_hle_CGContextClip, {ctx});
    }
    w.call(mt_hle_UIGraphicsPushContext, {ctx});
    w.send_stret(w.string(text), "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
    w.call(mt_hle_UIGraphicsPopContext, {});
    w.call(mt_hle_CGContextRelease, {ctx});
}

// What glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE) of the buffer shows the replacement hook.
mt_gfx_upload upload_of(const Cg &w, u32 buf, u32 width, u32 height) {
    mt_gfx_upload up{};
    up.texture = 1;
    up.width = width;
    up.height = height;
    up.format = MT_GFX_RGBA;
    up.type = MT_GFX_UNSIGNED_BYTE;
    up.pixels = w.cpu.mem + buf;
    up.hash = mt_gfx_upload_hash(width, height, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, up.pixels);
    return up;
}

// The text drawn straight at k times the size: TextCache's unflipped context, its CTM scaled.
std::vector<u8> text_at_scale(const char *text, float size, u32 width, u32 k) {
    mt_cg::Canvas canvas(width * k, 32 * k);
    canvas.state().fill = mt_cg::Color{1, 1, 1, 1};
    canvas.state().ctm = mt_cg::Affine::scale(k, k);
    std::vector<u8> px(static_cast<std::size_t>(width) * k * 32 * k * 4, 0);
    mt_cg::Surface s;
    s.pixels = px.data();
    s.width = width * k;
    s.height = 32 * k;
    s.bytes_per_row = width * k * 4;
    float tw = 0, th = 0;
    std::u16string chars;
    for (const char *c = text; *c != '\0'; c++) {
        chars.push_back(static_cast<char16_t>(*c));
    }
    mt_cg::draw_text(canvas, s, chars, mt_cg::Font{false, size}, mt_cg::Point{0, 0}, &tw, &th);
    return px;
}

// The rows that have ink (alpha > 0) in an RGBA image: (first, last), (-1, -1) if none.
std::pair<int, int> ink_rows_of(const u8 *px, u32 width, u32 height) {
    int first = -1, last = -1;
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            if (px[(static_cast<std::size_t>(y) * width + x) * 4 + 3] != 0) {
                first = first < 0 ? static_cast<int>(y) : first;
                last = static_cast<int>(y);
                break;
            }
        }
    }
    return {first, last};
}

struct TextScale {
    explicit TextScale(u32 k) { mt_cg_set_text_scale(k); }
    ~TextScale() { mt_cg_set_text_scale(1); }
};

} // namespace

TEST_CASE("CG: hd text: TextCache's text drawn at the text scale replaces its upload (#88)") {
    Cg w;
    const u32 font = w.send(w.font("MarkerFelt-Thin", 12), "fontWithSize:", {fb(18)});
    REQUIRE(font != 0u);
    const u32 buf = w.alloc(512 * 32 * 4);
    mt_gfx_replacement out{};

    // Text scale 1 (the default): no shadow, the hook replaces nothing.
    CHECK(mt_cg_text_scale() == 1u);
    text_cache_draw(w, buf, 512, "Henesys 1/3", font);
    const std::vector<u8> at_1x(w.cpu.mem + buf, w.cpu.mem + buf + 512 * 32 * 4);
    mt_gfx_upload up = upload_of(w, buf, 512, 32);
    CHECK(mt_cg_text_replace(nullptr, &up, &out) == 0);
    CHECK(mt_cg_get_text_stats().shadowed_draws == 0u);

    const TextScale scale(3);
    CHECK(mt_cg_text_scale() == 3u);
    text_cache_draw(w, buf, 512, "Henesys 1/3", font);
    // The 1x buffer is what it always was.
    CHECK(std::memcmp(w.cpu.mem + buf, at_1x.data(), at_1x.size()) == 0);
    up = upload_of(w, buf, 512, 32);
    REQUIRE(mt_cg_text_replace(nullptr, &up, &out) == 1);
    CHECK(out.width == 1536u);
    CHECK(out.height == 96u);
    // The replacement is the text rasterized at 3x, exactly, not the 1x magnified.
    const std::vector<u8> want = text_at_scale("Henesys 1/3", 18, 512, 3);
    CHECK(std::memcmp(out.pixels, want.data(), want.size()) == 0);
    std::size_t grey = 0; // pixels with partial coverage: anti-aliased edges at 3x
    for (std::size_t i = 3; i < want.size(); i += 4) {
        grey += out.pixels[i] != 0 && out.pixels[i] != 255 ? 1 : 0;
    }
    CHECK(grey > 500u);
    // Upside down at the bottom, like the 1x text (ADR 0036), on the same rows times 3.
    const auto rows1 = ink_rows_of(at_1x.data(), 512, 32);
    const auto rows3 = ink_rows_of(out.pixels, 1536, 96);
    REQUIRE(rows1.first >= 0);
    CHECK(std::abs(rows3.first - 3 * rows1.first) <= 3);
    CHECK(std::abs(rows3.second - (3 * rows1.second + 2)) <= 3);
    mt_cg_text_stats st = mt_cg_get_text_stats();
    CHECK(st.shadowed_draws == 1u);
    CHECK(st.rebuilds == 1u); // a new context: its shadow starts from the 1x pixels
    CHECK(st.replacements == 1u);

    // Uploads the shadow does not belong to: another size, another format, changed pixels.
    mt_gfx_upload other = up;
    other.width = 256;
    CHECK(mt_cg_text_replace(nullptr, &other, &out) == 0);
    other = up;
    other.format = 0x1907; // GL_RGB
    CHECK(mt_cg_text_replace(nullptr, &other, &out) == 0);
    w.cpu.mem[buf + 4 * 7] ^= 0x10; // the game writes into the buffer after the text
    other = upload_of(w, buf, 512, 32);
    CHECK(mt_cg_text_replace(nullptr, &other, &out) == 0);

    // One context, two texts, the buffer cleared by the CPU in between: the shadow notices
    // (the hash) and starts again from the 1x pixels, so the first text is gone from it too.
    {
        const u32 ctx = w.context(buf, 512, 32, 1);
        w.call(mt_hle_CGContextSetGrayFillColor, {ctx, fb(1), fb(1)});
        w.call(mt_hle_UIGraphicsPushContext, {ctx});
        w.send_stret(w.string("first"), "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
        std::memset(w.cpu.mem + buf, 0, 512 * 32 * 4);
        w.send_stret(w.string("Ellinia"), "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
        w.call(mt_hle_UIGraphicsPopContext, {});
        w.call(mt_hle_CGContextRelease, {ctx});
        up = upload_of(w, buf, 512, 32);
        REQUIRE(mt_cg_text_replace(nullptr, &up, &out) == 1);
        const std::vector<u8> ellinia = text_at_scale("Ellinia", 18, 512, 3);
        CHECK(std::memcmp(out.pixels, ellinia.data(), ellinia.size()) == 0);
    }
    // TextCache releases the context before it uploads; the last few shadows stay findable.
    std::vector<std::pair<mt_gfx_upload, std::vector<u8>>> uploads;
    for (const char *t : {"a1", "b22", "c333", "d4444", "e55555"}) {
        text_cache_draw(w, buf, 512, t, font);
        std::vector<u8> copy(w.cpu.mem + buf, w.cpu.mem + buf + 512 * 32 * 4);
        mt_gfx_upload u = upload_of(w, buf, 512, 32);
        uploads.emplace_back(u, std::move(copy));
    }
    for (std::size_t i = 0; i < uploads.size(); i++) {
        INFO(i);
        mt_gfx_upload u = uploads[i].first;
        u.pixels = uploads[i].second.data();
        CHECK(mt_cg_text_replace(nullptr, &u, &out) == (i == 0 ? 0 : 1)); // 4 kept
    }

    // A one-character texture (32 x 32) works the same way.
    const u32 small = w.alloc(32 * 32 * 4);
    text_cache_draw(w, small, 32, "A", font);
    up = upload_of(w, small, 32, 32);
    REQUIRE(mt_cg_text_replace(nullptr, &up, &out) == 1);
    CHECK(out.width == 96u);

    // Through a clip: drawn at 1x only (logged once), nothing replaced.
    const std::uint64_t unshadowed = mt_cg_get_text_stats().unshadowed;
    text_cache_draw(w, buf, 512, "clipped", font, true);
    CHECK(mt_cg_get_text_stats().unshadowed == unshadowed + 1);
    CHECK(logged("drawn at 1x only"));
    up = upload_of(w, buf, 512, 32);
    CHECK(mt_cg_text_replace(nullptr, &up, &out) == 0);

    // Host-memory contexts (UIGraphicsBeginImageContext) are never shadowed.
    const std::uint64_t shadowed = mt_cg_get_text_stats().shadowed_draws;
    w.call(mt_hle_UIGraphicsBeginImageContext, {fb(64), fb(32)});
    w.send_stret(w.string("host"), "drawAtPoint:withFont:", {fb(0), fb(0), font}, 8);
    w.call(mt_hle_UIGraphicsEndImageContext, {});
    CHECK(mt_cg_get_text_stats().shadowed_draws == shadowed);

    // Through mt_gfx: the hook installed, the text texture samples the 3x image at its 1x size.
    text_cache_draw(w, buf, 512, "Henesys 1/3", font);
    mt_gfx_set_replace_hook(mt_cg_text_replace, nullptr);
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(mt_gfx_texture_image(tex, 512, 32, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE,
                                 w.cpu.mem + buf) == 0);
    u32 lw = 0, lh = 0, tw = 0, th = 0;
    REQUIRE(mt_gfx_texture_size(tex, &lw, &lh) == 0);
    CHECK(lw == 512u);
    CHECK(lh == 32u);
    const u8 *texels = mt_gfx_texture_texels(tex, &tw, &th);
    REQUIRE(texels != nullptr);
    CHECK(tw == 1536u);
    CHECK(th == 96u);
    CHECK(std::memcmp(texels, want.data(), want.size()) == 0);
    mt_gfx_texture_destroy(tex);
    mt_gfx_set_replace_hook(nullptr, nullptr);

    // Setting the scale resets the counters and forgets the shadows.
    mt_cg_set_text_scale(99);
    CHECK(mt_cg_text_scale() == MT_GFX_MAX_SCALE);
    CHECK(mt_cg_get_text_stats().shadowed_draws == 0u);
    up = upload_of(w, buf, 512, 32);
    CHECK(mt_cg_text_replace(nullptr, &up, &out) == 0);
}

TEST_CASE("CG: UIFont names: the game's faces map to the bundled font, others are nil") {
    Cg w;
    for (const char *name : {"MarkerFelt-Thin", "Helvetica", "Helvetica-Bold", "AppleGothic",
                             "HelveticaNeue-Bold", "ArialMT"}) {
        const u32 f = w.font(name, 12);
        CHECK_MESSAGE(f != 0u, name);
        CHECK(std::strcmp(w.class_name(f), "UIFont") == 0);
    }
    CHECK(w.font("Gulim", 12) == 0u);
    CHECK(logged("+[UIFont fontWithName:@\"Gulim\" size:12]: no such font: nil"));
    const u32 b = w.send(w.klass("UIFont"), "boldSystemFontOfSize:", {fb(13)});
    mt_cg::Font f;
    REQUIRE(mt_cg::font_of(&w.cpu, b, &f));
    CHECK(f.bold);
    CHECK(f.size == 13.0f);
    REQUIRE(mt_cg::font_of(&w.cpu, w.send(b, "fontWithSize:", {fb(20)}), &f));
    CHECK(f.bold);
    CHECK(f.size == 20.0f);

    // Metrics scale with the size; Hangul and Latin have glyphs.
    const mt_cg::FontMetrics m12 = mt_cg::font_metrics({false, 12});
    const mt_cg::FontMetrics m24 = mt_cg::font_metrics({false, 24});
    CHECK(m12.ascender > 8.0f);
    CHECK(m12.descender < 0.0f);
    CHECK(m24.line_height == doctest::Approx(2 * m12.line_height));
    CHECK(mt_cg::text_width(u"메이플", {false, 12}) > 30.0f);
    CHECK(mt_cg::text_width(u"", {false, 12}) == 0.0f);
    CHECK(mt_cg::text_width(u"a\nb", {false, 12}) ==
          doctest::Approx(mt_cg::text_width(u"ab", {false, 12})));

    // Coverage: printable ASCII, the 2,350 Hangul syllables and 94 jamo of KS X 1001 (the
    // game's EUC-KR text). The other KS X 1001 rows are covered in part: NanumGothic has no
    // kana, Cyrillic, Greek, Roman numerals, circled Hangul or unit symbols (557 characters;
    // none occurs in the game's text, tests/test_hle_cg_real.py); the fallback font (Fira Sans,
    // #189, ADR 0108) draws the Cyrillic, Greek and Latin ones of them (140), not the others.
    for (int cp = 0x20; cp < 0x7F; cp++) {
        CHECK_MESSAGE(mt_cg::font_has_glyph(cp), cp);
    }
    int hangul = 0, hangul_missing = 0, other = 0, other_missing = 0;
    for (int lead = 0xA1; lead <= 0xC8; lead++) {
        for (int trail = 0xA1; trail <= 0xFE; trail++) {
            const std::uint8_t code[2] = {static_cast<std::uint8_t>(lead),
                                          static_cast<std::uint8_t>(trail)};
            std::u16string s;
            if (!mt_cf::decode(code, 2, mt_cf::k_enc_euc_kr, s) || s.size() != 1) {
                continue; // an unassigned code
            }
            const bool is_hangul = lead == 0xA4 || lead >= 0xB0;
            // U+3164 HANGUL FILLER (0xA4D4) is invisible: no glyph needed.
            const bool has = mt_cg::font_has_glyph(s[0]) || s[0] == 0x3164;
            (is_hangul ? hangul : other)++;
            (is_hangul ? hangul_missing : other_missing) += has ? 0 : 1;
        }
    }
    CHECK(hangul == 2350 + 94);
    CHECK(hangul_missing == 0);
    CHECK(other == 894);
    CHECK(other_missing == 417);

    // The fallback font (#189, ADR 0108): a text pack's Latin-1, Latin Extended-A, Greek and
    // Cyrillic, none of which NanumGothic has; each advances the pen by its own width.
    for (const int cp : {0xF6, 0xE9, 0xC9, 0xDF, 0x0141, 0x0151, 0x0416, 0x044F, 0x03A9}) {
        CHECK_MESSAGE(mt_cg::font_has_glyph(cp), cp);
    }
    for (int cp = 0xC0; cp <= 0x17F; cp++) {
        CHECK_MESSAGE(mt_cg::font_has_glyph(cp), cp);
    }
    const float ption = mt_cg::text_width(u"Ption", {false, 12});
    CHECK(mt_cg::text_width(u"P\u00F6tion", {false, 12}) > ption + 4.0f);
    CHECK(mt_cg::text_width(u"\u00F6", {false, 12}) ==
          doctest::Approx(mt_cg::text_width(u"\u00F6\u00F6", {false, 12}) / 2));

    // No font: nothing drawn, a zero size.
    const u32 size = w.send_stret(w.string("x"), "sizeWithFont:", {0}, 8);
    CHECK(w.getf(size) == 0.0f);
}

TEST_CASE("CG golden: image draws: the texture loader's flipped POT upload and a scaled copy") {
    Cg w;
    golden::Image pattern;
    const std::vector<u8> png = pattern_png(24, 20, &pattern);

    // FUN_00009534: [[UIImage alloc] initWithData:], then CGImageGet*.
    const u32 uiimage = w.send(w.send(w.klass("UIImage"), "alloc"), "initWithData:", {w.data(png)});
    REQUIRE(uiimage != 0u);
    const u32 cg = w.send(uiimage, "CGImage");
    CHECK(w.call(mt_hle_CGImageGetWidth, {cg}) == 24u);
    CHECK(w.call(mt_hle_CGImageGetHeight, {cg}) == 20u);
    CHECK(w.call(mt_hle_CGImageGetBitsPerComponent, {cg}) == 8u);
    CHECK(w.call(mt_hle_CGImageGetBitsPerPixel, {cg}) == 32u);
    CHECK(w.call(mt_hle_CGImageGetBytesPerRow, {cg}) == 96u);
    CHECK(w.call(mt_hle_CGImageGetAlphaInfo, {cg}) == 3u); // kCGImageAlphaLast
    CHECK(w.call(mt_hle_CGImageGetBitmapInfo, {cg}) == 3u);
    CHECK(w.call(mt_hle_CGImageGetColorSpace, {cg}) != 0u);
    CHECK(w.call(mt_hle_CGImageGetDataProvider, {cg}) != 0u);
    CHECK(w.call(mt_hle_CGImageGetRenderingIntent, {cg}) == 0u);
    CHECK(w.call(mt_hle_CGImageGetWidth, {0}) == 0u);
    const u32 size = w.send_stret(uiimage, "size", {}, 8);
    CHECK(w.getf(size) == 24.0f);
    CHECK(w.getf(size + 4) == 20.0f);
    CHECK(bf(w.send(uiimage, "scale")) == 1.0f);
    CHECK(uiimage_pixels(w, uiimage) == pattern);

    // The POT context: alpha info 1..4 -> kCGImageAlphaPremultipliedLast | big endian,
    // TranslateCTM(0, 32), ScaleCTM(1, -1), DrawImage((0, 32 - h), (w, h)).
    const u32 buf = w.alloc(32 * 32 * 4);
    const u32 ctx = w.context(buf, 32, 32, 0x4001u);
    w.flip(ctx, 32);
    w.call(mt_hle_CGContextDrawImage, {ctx, fb(0), fb(12), fb(24), fb(20), cg});
    w.call(mt_hle_CGContextRelease, {ctx});
    // Whole pixels at 1:1: a copy, the image's top row in the buffer's last row (what
    // glTexImage2D wants), premultiplied.
    bool exact = true;
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 24; x++) {
            const golden::Rgba p = pattern.at(x, y);
            const u8 *q = w.cpu.mem + buf + ((31 - y) * 32 + x) * 4;
            exact = exact && q[0] == premul(p.r, p.a) && q[1] == premul(p.g, p.a) &&
                    q[2] == premul(p.b, p.a) && q[3] == p.a;
        }
    }
    CHECK(exact);
    CHECK(ink_rows(w, buf, 32, 32) == std::pair<int, int>{12, 31});
    check_golden(snapshot(w, buf, 32, 32), "cg_image_texture");

    // -[EAGLView imageWithImage:scaledToSize:] (0x3251): x1.5 into an image context.
    w.call(mt_hle_UIGraphicsBeginImageContext, {fb(36), fb(30)});
    w.send(uiimage, "drawInRect:", {fb(0), fb(0), fb(36), fb(30)});
    const u32 scaled = w.call(mt_hle_UIGraphicsGetImageFromCurrentImageContext, {});
    w.call(mt_hle_UIGraphicsEndImageContext, {});
    REQUIRE(scaled != 0u);
    CHECK(w.call(mt_hle_CGImageGetWidth, {w.send(scaled, "CGImage")}) == 36u);
    check_golden(uiimage_pixels(w, scaled), "cg_image_scaled");
    w.send(uiimage, "release");

    // Not an image: nil, logged.
    const u32 bad =
        w.send(w.send(w.klass("UIImage"), "alloc"), "initWithData:", {w.data({1, 2, 3})});
    CHECK(bad == 0u);
    CHECK(logged("-[UIImage initWithData:]: 3 bytes that are no PNG"));
}

TEST_CASE("CG: CgBI PNGs decode to straight RGBA like ImageIO; PNG representation round-trips") {
    Cg w;
    const std::vector<u8> rgba = {255, 0, 0,   255, 0,   255, 0,  128, 10,  20,  30,  0,
                                  0,   0, 255, 255, 200, 100, 50, 64,  255, 255, 255, 255};
    const std::vector<u8> png = cgbi_png(rgba, 3, 2);
    const u32 img = w.send(w.klass("UIImage"), "imageWithData:", {w.data(png)});
    REQUIRE(img != 0u);
    const golden::Image px = uiimage_pixels(w, img);
    REQUIRE(px.width() == 3);
    // Opaque pixels exactly; translucent ones within the premultiply round trip.
    CHECK(px.at(0, 0) == golden::Rgba{255, 0, 0, 255});
    CHECK(px.at(0, 1) == golden::Rgba{0, 0, 255, 255});
    CHECK(px.at(1, 0).a == 128);
    CHECK(std::abs(px.at(1, 0).g - 255) <= 1);
    CHECK(px.at(2, 0).a == 0);
    CHECK(std::abs(px.at(1, 1).r - 200) <= 4);

    // UIImagePNGRepresentation: a PNG of the same straight pixels.
    const u32 data = w.call(mt_hle_UIImagePNGRepresentation, {img});
    REQUIRE(data != 0u);
    u32 addr = 0, len = 0;
    REQUIRE(mt_cf::data_bytes(&w.cpu, data, &addr, &len));
    mt_cg::Decoded d;
    REQUIRE(mt_cg::png_decode(w.cpu.mem + addr, len, &d, nullptr));
    CHECK(d.width == 3);
    CHECK(d.has_alpha);
    CHECK(std::equal(d.pixels->begin(), d.pixels->end(), px.bytes().begin()));
    CHECK(w.call(mt_hle_UIImagePNGRepresentation, {0}) == 0u);
}

TEST_CASE("CG: a PNG over 8,192 pixels a side or 2^24 pixels is refused before decoding (#324)") {
    const auto decode = [](const std::vector<u8> &png, std::string *why) {
        mt_cg::Decoded d;
        why->clear();
        return mt_cg::png_decode(png.data(), png.size(), &d, why);
    };
    // A small PNG whose header then claims another size (its CRC fixed): what a decoder would
    // allocate for comes from the header alone.
    std::vector<u8> small;
    const std::vector<u8> pixel(4, 0x80);
    REQUIRE(mt_cg::png_encode(pixel.data(), 1, 1, 4, small));
    const auto claiming = [&](u32 width, u32 height) {
        std::vector<u8> png = small;
        for (int i = 0; i < 4; i++) {
            png[16 + i] = static_cast<u8>(width >> (24 - 8 * i));
            png[20 + i] = static_cast<u8>(height >> (24 - 8 * i));
        }
        const u32 crc = mt_crc32(0, png.data() + 12, 17);
        for (int i = 0; i < 4; i++) {
            png[29 + i] = static_cast<u8>(crc >> (24 - 8 * i));
        }
        return png;
    };
    std::string why;
    for (const auto &[width, height] : std::vector<std::pair<u32, u32>>{
             {8193, 1}, {1, 8193}, {8192, 2049}, {4097, 4097}, {8192, 8192}}) {
        CAPTURE(width);
        CAPTURE(height);
        CHECK_FALSE(decode(claiming(width, height), &why));
        CHECK(why == "image too large");
    }
    // A whole, valid PNG of 8,193 x 1: refused as well.
    std::vector<u8> wide;
    const std::vector<u8> row(8193u * 4u, 0x40);
    REQUIRE(mt_cg::png_encode(row.data(), 8193, 1, 4, wide));
    CHECK_FALSE(decode(wide, &why));
    CHECK(why == "image too large");
    // At the limits: decoded.
    std::vector<u8> edge;
    const std::vector<u8> line(8192u * 4u, 0x40);
    REQUIRE(mt_cg::png_encode(line.data(), 8192, 1, 4, edge));
    CHECK(decode(edge, &why));
    CHECK(decode(small, &why));
}

TEST_CASE("CG: the map tile cutter: provider bytes, CGImageCreate, initWithCGImage:, PNG") {
    Cg w;
    golden::Image pattern;
    const u32 sheet =
        w.send(w.klass("UIImage"), "imageWithData:", {w.data(pattern_png(32, 16, &pattern))});
    REQUIRE(sheet != 0u);
    const u32 cg = w.send(sheet, "CGImage");
    // FUN_0000d6d0: CGDataProviderCopyData of the decoded sheet: straight RGBA rows.
    const u32 bytes =
        w.call(mt_hle_CGDataProviderCopyData, {w.call(mt_hle_CGImageGetDataProvider, {cg})});
    u32 addr = 0, len = 0;
    REQUIRE(mt_cf::data_bytes(&w.cpu, bytes, &addr, &len));
    REQUIRE(len == 32u * 16u * 4u);
    CHECK(std::memcmp(w.cpu.mem + addr, pattern.bytes().data(), len) == 0);

    // A 16 x 16 tile copied out by the game, then an image of it with the sheet's parameters.
    std::vector<u8> tile(16 * 16 * 4);
    for (int y = 0; y < 16; y++) {
        std::memcpy(&tile[y * 64], w.cpu.mem + addr + (y * 32 + 16) * 4, 64);
    }
    const u32 tile_data = w.data(tile);
    const u32 provider = w.call(mt_hle_CGDataProviderCreateWithCFData, {tile_data});
    REQUIRE(provider != 0u);
    const u32 space = w.call(mt_hle_CGImageGetColorSpace, {cg});
    const u32 info = w.call(mt_hle_CGImageGetBitmapInfo, {cg});
    const u32 tile_cg =
        w.call(mt_hle_CGImageCreate, {16, 16, 8, 32, 64, space, info, provider, 0, 1, 0});
    REQUIRE(tile_cg != 0u);
    w.call(mt_hle_CGDataProviderRelease, {provider}); // the image keeps it
    const u32 tile_img = w.send(w.send(w.klass("UIImage"), "alloc"), "initWithCGImage:", {tile_cg});
    REQUIRE(tile_img != 0u);
    w.call(mt_hle_CGImageRelease, {tile_cg});
    const golden::Image px = uiimage_pixels(w, tile_img);
    CHECK(std::memcmp(px.bytes().data(), tile.data(), tile.size()) == 0);
    CHECK(w.call(mt_hle_UIImagePNGRepresentation, {tile_img}) != 0u);
    w.send(tile_img, "release");

    // Too few bytes for the image: NULL, logged.
    const u32 small = w.call(mt_hle_CGDataProviderCreateWithCFData, {w.data({1, 2, 3, 4})});
    CHECK(w.call(mt_hle_CGImageCreate, {16, 16, 8, 32, 64, space, info, small, 0, 1, 0}) == 0u);
    CHECK(logged("CGImageCreate: 16x16 image, 64 bytes per row: no provider or too few bytes"));
    w.call(mt_hle_CGDataProviderRelease, {small});
    w.call(mt_hle_CGImageRelease, {0});

    // Mutable data that shrinks after CGImageCreate checked it (#325): the image has no pixels
    // to read any more, so it is not read past the data's end.
    const u32 data = mt_cf::data_create(&w.cpu, tile.data(), static_cast<u32>(tile.size()), true);
    const u32 shrinking = w.call(mt_hle_CGDataProviderCreateWithCFData, {data});
    const u32 shrunk_cg =
        w.call(mt_hle_CGImageCreate, {16, 16, 8, 32, 64, space, info, shrinking, 0, 1, 0});
    REQUIRE(shrunk_cg != 0u);
    std::vector<u8> rgba;
    CHECK(mt_cg::image_rgba(&w.cpu, shrunk_cg, rgba));
    REQUIRE(mt_cf::data_set_length(&w.cpu, data, 4));
    CHECK_FALSE(mt_cg::image_rgba(&w.cpu, shrunk_cg, rgba));
    w.call(mt_hle_CGImageRelease, {shrunk_cg});
    w.call(mt_hle_CGDataProviderRelease, {shrinking});
}

TEST_CASE("CG: colorAtPixel:onImage: reads one pixel through a 1x1 copy context") {
    Cg w;
    golden::Image pattern;
    const u32 img =
        w.send(w.klass("UIImage"), "imageWithData:", {w.data(pattern_png(16, 16, &pattern))});
    const u32 cg = w.send(img, "CGImage");
    // 0xB4DB5: 4 stack bytes (guarded here), the image's bpc and bytes per row (64: wider than
    // the 1-pixel context, which must never write past its first 4 bytes).
    const u32 mem = w.alloc(16);
    std::memset(w.cpu.mem + mem, 0xAB, 16);
    const u32 pixel = mem + 4;
    const u32 bpr = w.call(mt_hle_CGImageGetBytesPerRow, {cg});
    const u32 space = w.call(mt_hle_CGColorSpaceCreateDeviceRGB, {});
    const u32 ctx = w.call(mt_hle_CGBitmapContextCreate, {pixel, 1, 1, 8, bpr, space, 0x4001u});
    REQUIRE(ctx != 0u);
    w.call(mt_hle_CGContextSetBlendMode, {ctx, 17}); // kCGBlendModeCopy
    w.call(mt_hle_CGContextTranslateCTM, {ctx, fb(-5), fb(-12)});
    w.call(mt_hle_CGContextDrawImage, {ctx, fb(0), fb(0), fb(16), fb(16), cg});
    w.call(mt_hle_CGContextRelease, {ctx});
    // Device (5, 12) from the bottom: image row 16 - 1 - 12 = 3 from the top.
    const golden::Rgba p = pattern.at(5, 3);
    const u8 *q = w.cpu.mem + pixel;
    CHECK(q[0] == premul(p.r, p.a));
    CHECK(q[1] == premul(p.g, p.a));
    CHECK(q[2] == premul(p.b, p.a));
    CHECK(q[3] == p.a);
    for (u32 i : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u, 12u, 15u}) {
        CHECK(w.cpu.mem[mem + i] == 0xAB);
    }
}

TEST_CASE("CG: +imageNamed: and +imageWithContentsOfFile: read the bundle; imageNamed caches") {
    Bundle b;
    Cg w;
    const u32 cls = w.klass("UIImage");
    const u32 a = w.send(cls, "imageNamed:", {w.string("Button")});
    REQUIRE(a != 0u);
    CHECK(w.send(cls, "imageNamed:", {w.string("Button.png")}) != 0u);
    CHECK(w.send(cls, "imageNamed:", {w.string("Button")}) == a); // cached
    CHECK(w.call(mt_hle_CGImageGetWidth, {w.send(a, "CGImage")}) == 12u);
    CHECK(w.send(cls, "imageNamed:", {w.string("png/ui/Case.png")}) != 0u);
    CHECK(w.send(cls, "imageNamed:", {w.string("Missing")}) == 0u);
    CHECK(logged("+[UIImage imageNamed:@\"Missing\"]: no such file in the bundle: nil"));
    CHECK(w.send(cls, "imageNamed:", {w.string("Broken")}) == 0u);
    CHECK(logged("Broken.png is no PNG"));

    const std::string path = mt_cf::bundle_guest_path("png/ui/Case.png");
    const u32 c = w.send(cls, "imageWithContentsOfFile:", {w.string(path.c_str())});
    REQUIRE(c != 0u);
    CHECK(w.call(mt_hle_CGImageGetHeight, {w.send(c, "CGImage")}) == 8u);
    CHECK(w.send(cls, "imageWithContentsOfFile:", {w.string("/tmp/Case.png")}) == 0u);
}

TEST_CASE("CG: UIGraphics: the context stack, options, and an unbalanced end") {
    Cg w;
    CHECK(w.call(mt_hle_UIGraphicsGetCurrentContext, {}) == 0u);
    CHECK(w.call(mt_hle_UIGraphicsGetImageFromCurrentImageContext, {}) == 0u);
    w.call(mt_hle_UIGraphicsBeginImageContextWithOptions, {fb(10), fb(6), 1, fb(2)});
    const u32 ctx = w.call(mt_hle_UIGraphicsGetCurrentContext, {});
    REQUIRE(ctx != 0u);
    w.send(w.color(0, 0, 1, 1), "setFill");
    w.call(mt_hle_UIRectFill, {fb(0), fb(0), fb(5), fb(6)});
    const u32 img = w.call(mt_hle_UIGraphicsGetImageFromCurrentImageContext, {});
    REQUIRE(img != 0u);
    const u32 size = w.send_stret(img, "size", {}, 8);
    CHECK(w.getf(size) == 10.0f); // points: 20 x 12 pixels at scale 2
    CHECK(bf(w.send(img, "scale")) == 2.0f);
    const u32 cg = w.send(img, "CGImage");
    CHECK(w.call(mt_hle_CGImageGetWidth, {cg}) == 20u);
    CHECK(w.call(mt_hle_CGImageGetAlphaInfo, {cg}) == 5u); // opaque: NoneSkipLast
    const golden::Image px = uiimage_pixels(w, img);
    CHECK(px.at(9, 11) == golden::Rgba{0, 0, 255, 255});
    CHECK(px.at(10, 0) == golden::Rgba{0, 0, 0, 255});
    w.call(mt_hle_UIGraphicsEndImageContext, {});
    CHECK(w.call(mt_hle_UIGraphicsGetCurrentContext, {}) == 0u);
    w.call(mt_hle_UIGraphicsEndImageContext, {});
    CHECK(logged("UIGraphicsEndImageContext: the context stack is empty"));
    w.call(mt_hle_UIGraphicsPopContext, {});
    CHECK(logged("UIGraphicsPopContext: the context stack is empty"));
    // Drawing without a context is logged, not a crash.
    w.call(mt_hle_UIRectFill, {fb(0), fb(0), fb(1), fb(1)});
    CHECK(logged("UIRectFill: no current context"));
}

// --- the rasterizer's shortcuts (#134) ----------------------------------------------------------

namespace {

struct Lcg {
    u32 s;
    u32 next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    u8 byte() { return static_cast<u8>(next()); }
};

struct ShortcutsOff {
    ShortcutsOff() { mt_cg::set_raster_shortcuts(false); }
    ~ShortcutsOff() { mt_cg::set_raster_shortcuts(true); }
};

// Pixel formats: the compositor's premultiplied RGBA, premultiplied ARGB in little-endian
// memory (BGRA), straight RGBA, RGBX and 24-bit RGB.
mt_cg::Format format_of_kind(int kind) {
    mt_cg::Format f;
    switch (kind) {
    case 1:
        f.r = 2, f.g = 1, f.b = 0, f.a = 3;
        break;
    case 2:
        f.premultiplied = false;
        break;
    case 3:
        f.a = -1, f.skip = 3;
        break;
    case 4:
        f.bytes_per_pixel = 3, f.a = -1;
        break;
    default:
        break;
    }
    return f;
}

// An image whose rows mix runs of zero pixels, opaque pixels and translucent ones.
std::vector<u8> shortcut_image(Lcg &rng, u32 w, u32 h, u32 bpp) {
    std::vector<u8> px(static_cast<std::size_t>(w) * h * bpp, 0);
    for (u32 y = 0; y < h; y++) {
        const u32 style = rng.next() % 4; // 0: all zero, 1: opaque, 2: sparse, 3: random
        for (u32 x = 0; x < w; x++) {
            u8 *p = &px[(static_cast<std::size_t>(y) * w + x) * bpp];
            if (style == 0 || (style == 2 && (x / 3 + y) % 3 != 0)) {
                continue;
            }
            for (u32 c = 0; c < bpp; c++) {
                p[c] = rng.byte();
            }
            if (style == 1 && bpp == 4) {
                p[3] = 255;
            }
        }
    }
    return px;
}

} // namespace

TEST_CASE("CG: the raster shortcuts draw the generic path's bytes (#134)") {
    // Every drawing runs twice from the same pixels: with the shortcuts (whole-pixel rectangle
    // coverage, constant fills, images sampled per row and column, known blends) and without
    // (the generic path). The bytes must be equal.
    constexpr u32 sw = 40, sh = 30;
    Lcg rng{134};
    int draws = 0, differ = 0;
    auto run = [&](const mt_cg::Format &df, const std::vector<u8> &start, auto &&draw) {
        std::vector<u8> fast = start, slow = start;
        for (int pass = 0; pass < 2; pass++) {
            mt_cg::Surface s;
            s.pixels = pass == 0 ? fast.data() : slow.data();
            s.width = sw;
            s.height = sh;
            s.bytes_per_row = sw * df.bytes_per_pixel;
            s.format = df;
            if (pass == 1) {
                ShortcutsOff off;
                draw(s);
            } else {
                draw(s);
            }
        }
        draws++;
        if (fast != slow) {
            differ++;
        }
    };
    // CTMs: none, the compositor's y flip at 1x to 4x, an x flip, a fractional scale, and a
    // quarter turn (generic either way).
    std::vector<mt_cg::Affine> ctms;
    for (const double k : {1.0, 2.0, 3.0, 4.0, 1.7}) {
        ctms.push_back(mt_cg::Affine::scale(k, -k).then(mt_cg::Affine::translate(0, sh)));
    }
    ctms.push_back(mt_cg::Affine{-2, 0, 0, 2, 38, 1});
    ctms.push_back(mt_cg::Affine::scale(1, 1));
    ctms.push_back(mt_cg::Affine{0, 2, -2, 0, 30, 2});
    const mt_cg::Rect rects[] = {{0, 0, 9, 7},    {2, 1, 9, 7},     {1.5, 0.25, 9, 7},
                                 {-3, -2, 13, 9}, {3, 2, 4.5, 3.5}, {1, 1, 18, 14},
                                 {5, 3, 9, 7},    {0, 0, 40, 30}};
    for (int dk : {0, 1, 3}) { // context formats: premultiplied or no alpha
        const mt_cg::Format df = format_of_kind(dk);
        for (int ik = 0; ik < 5; ik++) {
            const mt_cg::Format sf = format_of_kind(ik);
            const std::vector<u8> img = shortcut_image(rng, 9, 7, sf.bytes_per_pixel);
            mt_cg::ConstSurface image;
            image.pixels = img.data();
            image.width = 9;
            image.height = 7;
            image.bytes_per_row = 9 * sf.bytes_per_pixel;
            image.format = sf;
            for (const mt_cg::Affine &ctm : ctms) {
                for (const mt_cg::Rect &r : rects) {
                    for (int variant = 0; variant < 6; variant++) {
                        std::vector<u8> start(static_cast<std::size_t>(sw) * sh *
                                              df.bytes_per_pixel);
                        for (u8 &b : start) {
                            b = rng.byte();
                        }
                        const bool clip = variant == 2 || variant == 5;
                        const float alpha = variant == 3 ? 0.6f : 1.0f;
                        const u32 mode = variant == 4   ? mt_cg::k_blend_copy
                                         : variant == 5 ? mt_cg::k_blend_clear
                                                        : mt_cg::k_blend_normal;
                        const bool top_down = variant % 2 == 0;
                        run(df, start, [&](mt_cg::Surface &s) {
                            mt_cg::Canvas c(sw, sh);
                            c.state().ctm = ctm;
                            c.state().alpha = alpha;
                            c.state().blend = mode;
                            if (clip) {
                                // A rounded clip, or a rectangle (whole pixels under most CTMs).
                                mt_cg::Path p;
                                if (variant == 2) {
                                    p.add_rounded_rect({1, 1, 14, 11}, 3);
                                } else {
                                    p.move_to({2, 2});
                                    p.line_to({22, 2});
                                    p.line_to({22, 17});
                                    p.line_to({2, 17});
                                    p.close();
                                }
                                c.add_path(p);
                                c.clip_to_path();
                            }
                            c.draw_image(s, r, image, top_down);
                            // Fills: opaque, translucent and clear, over the same rect.
                            c.state().fill = variant == 1   ? mt_cg::Color{0.2f, 0.4f, 0.6f, 0.5f}
                                             : variant == 4 ? mt_cg::Color{0, 0, 0, 0}
                                                            : mt_cg::Color{0.9f, 0.1f, 0.3f, 1};
                            c.fill_rect(s, mt_cg::Rect{r.x + 1, r.y + 1, r.w / 2, r.h / 2});
                        });
                    }
                }
            }
        }
    }
    CHECK(draws == 3 * 5 * 8 * 8 * 6);
    CHECK(differ == 0);
    CHECK(mt_cg::raster_shortcuts()); // back on
}

namespace {

// The image hook of the tests (#153): records the files offered and replaces those named in
// `replace` with a k x image of one colour.
struct ImageHookRecord {
    std::vector<std::string> offered;
    std::vector<u32> sizes;
    std::string replace;
    u32 k = 2;
    u32 extra = 0; // added to the width: a wrong size
    std::vector<u8> pixels;
};

int image_hook(void *user, const mt_cg_image_file *image, mt_cg_image_replacement *out) {
    auto *r = static_cast<ImageHookRecord *>(user);
    r->offered.emplace_back(image->file);
    r->sizes.push_back(image->width);
    r->sizes.push_back(image->height);
    if (r->replace != image->file) {
        return 0;
    }
    out->width = image->width * r->k + r->extra;
    out->height = image->height * r->k;
    r->pixels.assign(static_cast<std::size_t>(out->width) * out->height * 4, 0);
    for (std::size_t i = 0; i < r->pixels.size(); i += 4) {
        r->pixels[i + 2] = 255; // opaque blue
        r->pixels[i + 3] = 255;
    }
    out->rgba = r->pixels.data();
    return 1;
}

} // namespace

TEST_CASE("CG: the image hook replaces a bundle image for the compositor only (#153)") {
    Bundle b;
    Cg w;
    ImageHookRecord r;
    r.replace = "Button.png";
    mt_cg_set_image_replace_hook(image_hook, &r);
    const u32 cls = w.klass("UIImage");
    const u32 a = w.send(cls, "imageNamed:", {w.string("Button")});
    REQUIRE(a != 0u);
    // The guest keeps the file's image; the compositor gets the 2x one.
    const u32 cg = w.send(a, "CGImage");
    CHECK(w.call(mt_hle_CGImageGetWidth, {cg}) == 12u);
    CHECK(mt_cg::uiimage_cgimage(&w.cpu, a) == cg);
    const u32 display = mt_cg::uiimage_display_cgimage(&w.cpu, a);
    REQUIRE(display != cg);
    CHECK(w.call(mt_hle_CGImageGetWidth, {display}) == 24u);
    CHECK(w.call(mt_hle_CGImageGetHeight, {display}) == 20u);
    std::vector<u8> rgba;
    REQUIRE(mt_cg::image_rgba(&w.cpu, display, rgba));
    CHECK(rgba[2] == 255);
    CHECK(rgba[3] == 255);
    REQUIRE(r.offered.size() == 1);
    CHECK(r.offered[0] == "Button.png");
    CHECK(r.sizes == std::vector<u32>{12, 10});

    // Once per file: imageWithContentsOfFile: twice asks once; a file the hook keeps draws as it
    // is.
    const std::string path = mt_cf::bundle_guest_path("png/ui/Case.png");
    const u32 c1 = w.send(cls, "imageWithContentsOfFile:", {w.string(path.c_str())});
    const u32 c2 = w.send(cls, "imageWithContentsOfFile:", {w.string(path.c_str())});
    REQUIRE(c1 != 0u);
    REQUIRE(c2 != 0u);
    CHECK(r.offered.size() == 2);
    CHECK(r.offered[1] == "png/ui/Case.png");
    CHECK(mt_cg::uiimage_display_cgimage(&w.cpu, c1) == mt_cg::uiimage_cgimage(&w.cpu, c1));
    // Images from data (the GL texture loader's) are never offered.
    mt_cg_image_stats s = mt_cg_get_image_stats();
    CHECK(s.offered == 2);
    CHECK(s.replaced == 1);
    CHECK(s.rejected == 0);
    mt_cg_set_image_replace_hook(nullptr, nullptr);
}

TEST_CASE("CG: an image hook's replacement of the wrong size is rejected and logged (#153)") {
    Bundle b;
    Cg w;
    ImageHookRecord r;
    r.replace = "Button.png";
    r.extra = 1; // 25 x 20 for 12 x 10
    mt_cg_set_image_replace_hook(image_hook, &r);
    const u32 a = w.send(w.klass("UIImage"), "imageNamed:", {w.string("Button")});
    REQUIRE(a != 0u);
    CHECK(mt_cg::uiimage_display_cgimage(&w.cpu, a) == mt_cg::uiimage_cgimage(&w.cpu, a));
    CHECK(logged("UIImage Button.png: replacement 25x20 for 12x10 rejected"));
    CHECK(mt_cg_get_image_stats().rejected == 1);
    r.extra = 0;
    r.k = 9; // above MT_GFX_MAX_SCALE
    const std::string path = mt_cf::bundle_guest_path("Button.png");
    w.send(w.klass("UIImage"), "imageWithContentsOfFile:", {w.string(path.c_str())});
    CHECK(mt_cg_get_image_stats().offered == 1); // the file was asked about once already
    mt_cg_set_image_replace_hook(nullptr, nullptr);
}

TEST_CASE("CG: a texture pack's ui/ files and the dump of the shell's images (#153)") {
    // One runtime throughout: CG's caches follow the runtime (a second one at the same address
    // would look like the first).
    Bundle b;
    const std::filesystem::path dump(b.dir.string() + "-dump");
    const std::filesystem::path pack(b.dir.string() + "-pack");
    std::filesystem::remove_all(dump);
    std::filesystem::remove_all(pack);
    Cg w;
    const u32 cls = w.klass("UIImage");
    REQUIRE(mt_texpack_dump_open(dump.string().c_str()) == 0);
    const u32 a = w.send(cls, "imageNamed:", {w.string("png/ui/Case.png")});
    REQUIRE(a != 0u);
    mt_texpack_dump_close();
    // The dump holds the decoded image, straight RGBA, under its bundle path.
    const auto img = golden::load_png(dump / "ui" / "png" / "ui" / "Case.png");
    REQUIRE(img.has_value());
    std::vector<u8> rgba;
    REQUIRE(mt_cg::image_rgba(&w.cpu, mt_cg::uiimage_cgimage(&w.cpu, a), rgba));
    CHECK(img->bytes() == rgba);
    CHECK(mt_texpack_get_stats().dumped_ui == 1);
    CHECK(std::filesystem::exists(dump / "manifest.jsonl"));

    // A 2x pack image for Button.png (12 x 10), none for Case.png.
    golden::Image big(24, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 24; ++x) {
            big.set(x, y, {0, 255, 0, 255});
        }
    }
    REQUIRE(golden::write_png(pack / "ui" / "Button.png", big));
    REQUIRE(mt_texpack_open(pack.string().c_str()) == 1);
    const u32 button = w.send(cls, "imageNamed:", {w.string("Button")});
    REQUIRE(button != 0u);
    const u32 display = mt_cg::uiimage_display_cgimage(&w.cpu, button);
    CHECK(w.call(mt_hle_CGImageGetWidth, {display}) == 24u);
    CHECK(w.call(mt_hle_CGImageGetHeight, {display}) == 20u);
    CHECK(w.call(mt_hle_CGImageGetWidth, {mt_cg::uiimage_cgimage(&w.cpu, button)}) == 12u);
    // A new hook asks again about a file seen before (Case.png: the pack has none).
    const std::string path = mt_cf::bundle_guest_path("png/ui/Case.png");
    const u32 c = w.send(cls, "imageWithContentsOfFile:", {w.string(path.c_str())});
    REQUIRE(c != 0u);
    CHECK(mt_cg::uiimage_display_cgimage(&w.cpu, c) == mt_cg::uiimage_cgimage(&w.cpu, c));
    const mt_texpack_stats s = mt_texpack_get_stats();
    CHECK(s.ui_files == 1);
    CHECK(s.ui_images == 2);
    CHECK(s.ui_replaced == 1);
    CHECK(s.ui_rejected == 0);
    mt_texpack_close();
    std::filesystem::remove_all(dump);
    std::filesystem::remove_all(pack);
}
