// The CoreGraphics objects and HLE functions (#51, ADR 0036).
//
// Every CG object the game holds (CGContextRef, CGImageRef, CGColorSpaceRef, CGDataProviderRef,
// CGGradientRef, CGColorRef, CGPathRef) is a guest __NSCFType instance whose contents are host
// state (ADR 0032): one state type per kind, holding a pointer to a host object. So CFRetain,
// CFRelease and -retain/-release work on all of them, and the guest only ever sees opaque
// pointers, except where CG hands out memory (CGColorGetComponents: a guest copy of the
// components; a bitmap context over caller memory draws into that guest memory).
//
// The game's own uses (docs/re/imports.md): TextCache_DrawText 0x9031 (a 512x32 or 32x32
// premultiplied RGBA context over a guest buffer, white fill, UIKit text), the texture loader
// FUN_00009534 (a power-of-two context, flipped CTM, CGContextDrawImage), the map tile and
// portrait builders FUN_0000d6d0 / FUN_0001e96c (CGImageGet*, CGDataProviderCopyData,
// CGImageCreate over a CFData provider), -[TouchKeyPadView colorAtPixel:onImage:] 0xB4DB5
// (a 1x1 context, kCGBlendModeCopy, translated CTM). Chartboost's gradients, clip paths and
// CGRect helpers are implemented too, though ads are off (ADR 0026).
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_hle.h"
#include "cg/cg_objects.h"
#include "cg/cg_snapshot.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "objc/objc_internal.h"

namespace mt_cg {

namespace {

// --- per-runtime state and logging ---------------------------------------------------------------

struct Module {
    mt_objc *rt = nullptr;
    u32 binding = 0; // CF's binding to rt (a new runtime at the same address starts afresh)
    std::set<std::string> logged; // log_once keys
};
Module g_module;

// --- host state of the guest objects ------------------------------------------------------------

// The state block of each kind holds one pointer to a heap object of that kind; destroy()
// releases the guest objects it holds, then deletes it.
template <typename T> void destroy_box(mt_cpu *cpu, u32, void *state) {
    T *p = *static_cast<T **>(state);
    if (p != nullptr) {
        p->release(cpu);
        delete p;
    }
}

template <typename T> T *find(mt_cpu *cpu, u32 obj, const mt_objc_state_type &type) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return nullptr;
    }
    void *s = mt_objc_find_state(rt, obj, &type);
    return s != nullptr ? *static_cast<T **>(s) : nullptr;
}

// A new __NSCFType instance (retain count 1) owning `value`; 0 if out of memory.
template <typename T>
u32 make(mt_cpu *cpu, const mt_objc_state_type &type, std::unique_ptr<T> value) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = rt != nullptr ? mt_cf::create(cpu, mt_cf::classes(cpu).type) : 0u;
    void *s = obj != 0 ? mt_objc_state(rt, obj, &type) : nullptr;
    if (s == nullptr) {
        if (obj != 0) {
            mt_objc_release(cpu, obj);
        }
        log(cpu, "out of memory for a %s", type.name);
        return 0;
    }
    *static_cast<T **>(s) = value.release();
    return obj;
}

struct ColorSpace {
    void release(mt_cpu *) {}
};
struct ColorObj {
    Color value;
    u32 components = 0; // guest copy: 4 floats
    void release(mt_cpu *cpu) {
        if (components != 0) {
            mt_heap_free(mt_mem_from_cpu(cpu), components);
        }
    }
};
struct Provider {
    u32 data = 0; // guest CFData, retained
    std::shared_ptr<const std::vector<u8>> host;
    void release(mt_cpu *cpu) { mt_objc_release(cpu, data); }
};
struct ImageObj {
    ImageInfo info;
    void release(mt_cpu *cpu) {
        mt_objc_release(cpu, info.space);
        mt_objc_release(cpu, info.provider);
    }
};
struct GradientObj {
    Gradient g;
    void release(mt_cpu *) {}
};
struct PathObj {
    Path p;
    void release(mt_cpu *) {}
};
struct ContextObj {
    Canvas canvas;
    u32 data = 0;         // guest pixel memory, or 0 for `host`
    std::vector<u8> host; // pixel memory the context owns
    u32 bytes_per_row = 0, bitmap_info = 0;
    Format format;
    std::shared_ptr<TextShadow> shadow; // guest memory only (hires_text.cpp)
    ContextObj(u32 w, u32 h) : canvas(w, h) {}
    void release(mt_cpu *) {}
};

const mt_objc_state_type k_space_state = {"CGColorSpace", sizeof(void *), destroy_box<ColorSpace>};
const mt_objc_state_type k_color_state = {"CGColor", sizeof(void *), destroy_box<ColorObj>};
const mt_objc_state_type k_provider_state = {"CGDataProvider", sizeof(void *),
                                             destroy_box<Provider>};
const mt_objc_state_type k_image_state = {"CGImage", sizeof(void *), destroy_box<ImageObj>};
const mt_objc_state_type k_gradient_state = {"CGGradient", sizeof(void *),
                                             destroy_box<GradientObj>};
const mt_objc_state_type k_path_state = {"CGPath", sizeof(void *), destroy_box<PathObj>};
const mt_objc_state_type k_context_state = {"CGContext", sizeof(void *), destroy_box<ContextObj>};

float f32(u32 bits) { return mt_f32(bits); }

// The context of a CGContext* call: NULL or not a context is logged (once) and ignored, as CG
// logs "<Error>: CGContextXXX: invalid context 0x0" and returns.
ContextObj *context_arg(mt_cpu *cpu, u32 ctx, const char *function) {
    ContextObj *c = find<ContextObj>(cpu, ctx, k_context_state);
    if (c == nullptr) {
        log_once(cpu, function, "%s: invalid context 0x%08X (called from 0x%08X): ignored",
                 function, static_cast<unsigned>(ctx), static_cast<unsigned>(cpu->r[MT_LR]));
    }
    return c;
}

Surface surface_of(mt_cpu *cpu, ContextObj &c) {
    Surface s;
    s.width = c.canvas.width();
    s.height = c.canvas.height();
    s.bytes_per_row = c.bytes_per_row;
    s.format = c.format;
    s.pixels = c.data != 0 ? cpu->mem + c.data : c.host.data();
    return s;
}

ImageObj *image_arg(mt_cpu *cpu, u32 image) { return find<ImageObj>(cpu, image, k_image_state); }

// Releases a CG object; NULL is allowed (CGxxxRelease, unlike CFRelease).
void release_nullable(mt_cpu *cpu) {
    const u32 obj = cpu->r[0];
    if (obj != 0) {
        mt_objc_release(cpu, obj);
    }
}

Rect read_rect(mt_args *a) {
    Rect r;
    r.x = mt_arg_f32(a);
    r.y = mt_arg_f32(a);
    r.w = mt_arg_f32(a);
    r.h = mt_arg_f32(a);
    return r;
}

Rect standardized(Rect r) {
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    return r;
}

void store_floats(mt_cpu *cpu, u32 addr, const float *v, u32 n) {
    std::memcpy(mt_g2h(cpu, addr, 4u * n), v, 4u * n);
}

} // namespace

// --- module plumbing -----------------------------------------------------------------------------

mt_objc *runtime(mt_cpu *cpu) {
    mt_objc *rt = mt_cf::runtime(cpu);
    if (rt != g_module.rt || g_module.binding != mt_cf::binding()) {
        g_module.rt = rt;
        g_module.binding = mt_cf::binding();
        g_module.logged.clear();
        uikit_forget();
        text_forget();
        hires_forget();
    }
    return rt;
}

void log(mt_cpu *cpu, const char *fmt, ...) noexcept {
    char line[512] = "cg: ";
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + 4, sizeof line - 4, fmt, ap);
    va_end(ap);
    mt_objc *rt = runtime(cpu);
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void log_once(mt_cpu *cpu, const char *key, const char *fmt, ...) noexcept {
    runtime(cpu);
    if (!g_module.logged.insert(key).second) {
        return;
    }
    char text[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log(cpu, "%s (logged once)", text);
}

// --- objects
// --------------------------------------------------------------------------------------

u32 color_space_create(mt_cpu *cpu) {
    return make(cpu, k_space_state, std::make_unique<ColorSpace>());
}

bool is_color_space(mt_cpu *cpu, u32 obj) {
    return find<ColorSpace>(cpu, obj, k_space_state) != nullptr;
}

u32 color_create(mt_cpu *cpu, const Color &c) {
    auto v = std::make_unique<ColorObj>();
    v->value = c;
    v->components = mt_heap_calloc(mt_mem_from_cpu(cpu), 4, 4);
    if (v->components == 0) {
        log(cpu, "out of guest memory for a CGColor");
        return 0;
    }
    const float comps[4] = {c.r, c.g, c.b, c.a};
    store_floats(cpu, v->components, comps, 4);
    return make(cpu, k_color_state, std::move(v));
}

bool color_value(mt_cpu *cpu, u32 obj, Color *out) {
    const ColorObj *c = find<ColorObj>(cpu, obj, k_color_state);
    if (c != nullptr) {
        *out = c->value;
    }
    return c != nullptr;
}

u32 provider_create_data(mt_cpu *cpu, u32 cfdata) {
    auto p = std::make_unique<Provider>();
    p->data = mt_objc_retain(cpu, cfdata);
    return make(cpu, k_provider_state, std::move(p));
}

u32 provider_create_bytes(mt_cpu *cpu, std::shared_ptr<const std::vector<u8>> bytes) {
    auto p = std::make_unique<Provider>();
    p->host = std::move(bytes);
    return make(cpu, k_provider_state, std::move(p));
}

bool provider_bytes(mt_cpu *cpu, u32 provider, const u8 **bytes, std::size_t *size) {
    const Provider *p = find<Provider>(cpu, provider, k_provider_state);
    if (p == nullptr) {
        return false;
    }
    if (p->host) {
        *bytes = p->host->data();
        *size = p->host->size();
        return true;
    }
    u32 addr = 0, length = 0;
    if (!mt_cf::data_bytes(cpu, p->data, &addr, &length)) {
        // A data object of another class (Foundation, #49): ask it.
        length = mt_objc_send0(cpu, p->data, "length");
        addr = length != 0 ? mt_objc_send0(cpu, p->data, "bytes") : 0u;
        if (length != 0 && addr == 0) {
            return false;
        }
    }
    *bytes = length != 0 ? mt_g2h(cpu, addr, length) : nullptr;
    *size = length;
    return true;
}

namespace {

// The bytes an image of `info` reads: every row but the last in full, then the last row's pixels.
std::uint64_t bytes_needed(const ImageInfo &info, const Format &f) {
    return info.height == 0 ? 0
                            : static_cast<std::uint64_t>(info.bytes_per_row) * (info.height - 1) +
                                  static_cast<std::uint64_t>(info.width) * f.bytes_per_pixel;
}

} // namespace

u32 image_create(mt_cpu *cpu, const ImageInfo &info) {
    Format f;
    if (!format_of(info.bitmap_info, info.bpc, info.bpp, &f)) {
        log(cpu,
            "CGImageCreate: unsupported format (%u bpc, %u bpp, bitmap info 0x%X), called from "
            "0x%08X",
            static_cast<unsigned>(info.bpc), static_cast<unsigned>(info.bpp),
            static_cast<unsigned>(info.bitmap_info), static_cast<unsigned>(cpu->r[MT_LR]));
        return 0;
    }
    const u8 *bytes = nullptr;
    std::size_t size = 0;
    if (info.width == 0 || info.height == 0 ||
        info.bytes_per_row < info.width * f.bytes_per_pixel ||
        !provider_bytes(cpu, info.provider, &bytes, &size) || size < bytes_needed(info, f)) {
        log(cpu, "CGImageCreate: %ux%u image, %u bytes per row: no provider or too few bytes",
            static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
            static_cast<unsigned>(info.bytes_per_row));
        return 0;
    }
    auto img = std::make_unique<ImageObj>();
    img->info = info;
    img->info.space = mt_objc_retain(cpu, info.space);
    img->info.provider = mt_objc_retain(cpu, info.provider);
    const u32 obj = make(cpu, k_image_state, std::move(img));
    return obj;
}

u32 image_create_host(mt_cpu *cpu, u32 width, u32 height, u32 bpp, u32 bitmap_info,
                      std::shared_ptr<const std::vector<u8>> pixels) {
    ImageInfo info;
    info.width = width;
    info.height = height;
    info.bpc = 8;
    info.bpp = bpp;
    info.bytes_per_row = width * (bpp / 8);
    info.bitmap_info = bitmap_info;
    info.space = color_space_create(cpu);
    info.provider = provider_create_bytes(cpu, std::move(pixels));
    const u32 image = info.space != 0 && info.provider != 0 ? image_create(cpu, info) : 0u;
    mt_objc_release(cpu, info.space);
    mt_objc_release(cpu, info.provider);
    return image;
}

const ImageInfo *image_info(mt_cpu *cpu, u32 image) {
    const ImageObj *i = image_arg(cpu, image);
    return i != nullptr ? &i->info : nullptr;
}

bool image_surface(mt_cpu *cpu, u32 image, ConstSurface *out) {
    const ImageObj *i = image_arg(cpu, image);
    std::size_t size = 0;
    if (i == nullptr || !format_of(i->info.bitmap_info, i->info.bpc, i->info.bpp, &out->format) ||
        !provider_bytes(cpu, i->info.provider, &out->pixels, &size) ||
        size < bytes_needed(i->info, out->format) || out->pixels == nullptr) {
        return false; // the provider's data shrank after CGImageCreate checked it
    }
    out->width = i->info.width;
    out->height = i->info.height;
    out->bytes_per_row = i->info.bytes_per_row;
    return true;
}

bool image_rgba(mt_cpu *cpu, u32 image, std::vector<u8> &rgba) {
    ConstSurface s;
    if (!image_surface(cpu, image, &s)) {
        return false;
    }
    Format straight; // RGBA, not premultiplied
    straight.premultiplied = false;
    rgba.assign(static_cast<std::size_t>(s.width) * s.height * 4, 0);
    for (u32 y = 0; y < s.height; y++) {
        for (u32 x = 0; x < s.width; x++) {
            const u8 *src = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row +
                            static_cast<std::size_t>(x) * s.format.bytes_per_pixel;
            u8 *dst = rgba.data() + (static_cast<std::size_t>(y) * s.width + x) * 4;
            if (!s.format.premultiplied || s.format.a < 0) {
                // Straight in, straight out: no rounding through premultiplied values.
                dst[0] = src[s.format.r];
                dst[1] = src[s.format.g];
                dst[2] = src[s.format.b];
                dst[3] = s.format.a >= 0 ? src[s.format.a] : 0xFF;
            } else {
                store_pixel(straight, dst, load_pixel(s.format, src));
            }
        }
    }
    return true;
}

u32 gradient_create(mt_cpu *cpu, Gradient g) {
    auto v = std::make_unique<GradientObj>();
    v->g = std::move(g);
    return make(cpu, k_gradient_state, std::move(v));
}

const Gradient *gradient_of(mt_cpu *cpu, u32 obj) {
    const GradientObj *g = find<GradientObj>(cpu, obj, k_gradient_state);
    return g != nullptr ? &g->g : nullptr;
}

u32 path_create(mt_cpu *cpu, Path p) {
    auto v = std::make_unique<PathObj>();
    v->p = std::move(p);
    return make(cpu, k_path_state, std::move(v));
}

const Path *path_of(mt_cpu *cpu, u32 obj) {
    const PathObj *p = find<PathObj>(cpu, obj, k_path_state);
    return p != nullptr ? &p->p : nullptr;
}

u32 context_create(mt_cpu *cpu, u32 data, u32 width, u32 height, u32 bpc, u32 bytes_per_row,
                   u32 space, u32 bitmap_info) {
    Format f;
    const bool format_ok =
        context_format_ok(bitmap_info, bpc, 32) && format_of(bitmap_info, bpc, 32, &f);
    if (!format_ok || width == 0 || height == 0 || width > 0x4000u || height > 0x4000u ||
        bytes_per_row < width * 4u || (space != 0 && !is_color_space(cpu, space))) {
        log(cpu,
            "CGBitmapContextCreate: unsupported parameter combination: %u x %u, %u bpc, %u bytes "
            "per row, bitmap info 0x%X (called from 0x%08X)",
            static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(bpc),
            static_cast<unsigned>(bytes_per_row), static_cast<unsigned>(bitmap_info),
            static_cast<unsigned>(cpu->r[MT_LR]));
        return 0;
    }
    auto c = std::make_unique<ContextObj>(width, height);
    c->bytes_per_row = bytes_per_row;
    c->bitmap_info = bitmap_info;
    c->format = f;
    // The memory the context draws into: the last row needs only width * 4 bytes.
    const u32 extent = bytes_per_row * (height - 1) + width * 4u;
    if (data != 0) {
        mt_g2h(cpu, data, extent); // stops the guest, as a fault would, if it is not memory
        c->data = data;
    } else {
        c->host.assign(static_cast<std::size_t>(bytes_per_row) * height, 0);
    }
    return make(cpu, k_context_state, std::move(c));
}

bool context_canvas(mt_cpu *cpu, u32 obj, Canvas **canvas, Surface *surface) {
    ContextObj *c = find<ContextObj>(cpu, obj, k_context_state);
    if (c == nullptr) {
        return false;
    }
    *canvas = &c->canvas;
    *surface = surface_of(cpu, *c);
    return true;
}

std::shared_ptr<TextShadow> *context_text_shadow(mt_cpu *cpu, u32 obj) {
    ContextObj *c = find<ContextObj>(cpu, obj, k_context_state);
    return c != nullptr && c->data != 0 ? &c->shadow : nullptr;
}

u32 context_bitmap_info(mt_cpu *cpu, u32 obj) {
    const ContextObj *c = find<ContextObj>(cpu, obj, k_context_state);
    return c != nullptr ? c->bitmap_info : 0u;
}

} // namespace mt_cg

// --- HLE: CoreGraphics -------------------------------------------------------------------------

using namespace mt_cg;

// CGContextRef CGBitmapContextCreate(void *data, size_t width, size_t height,
//     size_t bitsPerComponent, size_t bytesPerRow, CGColorSpaceRef space, CGBitmapInfo info)
extern "C" void mt_hle_CGBitmapContextCreate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 data = mt_arg_u32(&a), w = mt_arg_u32(&a), h = mt_arg_u32(&a), bpc = mt_arg_u32(&a);
    const u32 bpr = mt_arg_u32(&a), space = mt_arg_u32(&a), info = mt_arg_u32(&a);
    mt_ret_u32(cpu, context_create(cpu, data, w, h, bpc, bpr, space, info));
}

extern "C" void mt_hle_CGContextRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    release_nullable(cpu);
}

extern "C" void mt_hle_CGContextSaveGState(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextSaveGState")) {
        c->canvas.save();
    }
}

extern "C" void mt_hle_CGContextRestoreGState(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextRestoreGState")) {
        if (!c->canvas.restore()) {
            log_once(cpu, "CGContextRestoreGState.empty",
                     "CGContextRestoreGState: nothing saved (called from 0x%08X): ignored",
                     static_cast<unsigned>(cpu->r[MT_LR]));
        }
    }
}

// void CGContextScaleCTM(CGContextRef c, CGFloat sx, CGFloat sy)
extern "C" void mt_hle_CGContextScaleCTM(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextScaleCTM")) {
        GState &gs = c->canvas.state();
        gs.ctm = Affine::scale(f32(cpu->r[1]), f32(cpu->r[2])).then(gs.ctm);
    }
}

// void CGContextTranslateCTM(CGContextRef c, CGFloat tx, CGFloat ty)
extern "C" void mt_hle_CGContextTranslateCTM(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextTranslateCTM")) {
        GState &gs = c->canvas.state();
        gs.ctm = Affine::translate(f32(cpu->r[1]), f32(cpu->r[2])).then(gs.ctm);
    }
}

// void CGContextSetBlendMode(CGContextRef c, CGBlendMode mode)
extern "C" void mt_hle_CGContextSetBlendMode(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextSetBlendMode")) {
        const u32 mode = cpu->r[1];
        if (mode != k_blend_normal && mode != k_blend_copy && mode != k_blend_clear) {
            log_once(cpu, "CGContextSetBlendMode.mode",
                     "CGContextSetBlendMode: blend mode %u is drawn as normal (the game only sets "
                     "kCGBlendModeCopy)",
                     static_cast<unsigned>(mode));
        }
        c->canvas.state().blend = mode;
    }
}

// void CGContextSetGrayFillColor(CGContextRef c, CGFloat gray, CGFloat alpha)
extern "C" void mt_hle_CGContextSetGrayFillColor(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextSetGrayFillColor")) {
        const float g = f32(cpu->r[1]), alpha = f32(cpu->r[2]);
        c->canvas.state().fill = {g, g, g, alpha};
    }
}

// void CGContextSetTextDrawingMode(CGContextRef c, CGTextDrawingMode mode)
extern "C" void mt_hle_CGContextSetTextDrawingMode(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextSetTextDrawingMode")) {
        const u32 mode = cpu->r[1];
        if (mode != k_text_fill && mode != k_text_invisible) {
            log_once(cpu, "CGContextSetTextDrawingMode.mode",
                     "CGContextSetTextDrawingMode: mode %u draws as kCGTextFill (the game only "
                     "sets kCGTextFill)",
                     static_cast<unsigned>(mode));
        }
        c->canvas.state().text_mode = mode;
    }
}

// void CGContextAddPath(CGContextRef c, CGPathRef path)
extern "C" void mt_hle_CGContextAddPath(mt_cpu *cpu, std::uint32_t) noexcept {
    ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextAddPath");
    const Path *p = path_of(cpu, cpu->r[1]);
    if (c != nullptr && p != nullptr) {
        c->canvas.add_path(*p);
    }
}

// void CGContextClip(CGContextRef c): intersects the clip with the current path (nonzero) and
// clears the path.
extern "C" void mt_hle_CGContextClip(mt_cpu *cpu, std::uint32_t) noexcept {
    if (ContextObj *c = context_arg(cpu, cpu->r[0], "CGContextClip")) {
        c->canvas.clip_to_path();
    }
}

// void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image)
extern "C" void mt_hle_CGContextDrawImage(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 ctx = mt_arg_u32(&a);
    const Rect r = read_rect(&a);
    const u32 image = mt_arg_u32(&a);
    ContextObj *c = context_arg(cpu, ctx, "CGContextDrawImage");
    ConstSurface src;
    if (c == nullptr) {
        return;
    }
    if (!image_surface(cpu, image, &src)) {
        log_once(cpu, "CGContextDrawImage.image", "CGContextDrawImage: invalid image 0x%08X",
                 static_cast<unsigned>(image));
        return;
    }
    Surface dst = surface_of(cpu, *c);
    c->canvas.draw_image(dst, r, src, false);
}

namespace {

// The gradient argument, or nullptr (logged once).
const Gradient *gradient_arg(mt_cpu *cpu, u32 g, const char *function) {
    const Gradient *gr = gradient_of(cpu, g);
    if (gr == nullptr) {
        log_once(cpu, function, "%s: invalid gradient 0x%08X: ignored", function,
                 static_cast<unsigned>(g));
    }
    return gr;
}

} // namespace

// void CGContextDrawLinearGradient(CGContextRef c, CGGradientRef g, CGPoint start,
//     CGPoint end, CGGradientDrawingOptions options)
extern "C" void mt_hle_CGContextDrawLinearGradient(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 ctx = mt_arg_u32(&a), g = mt_arg_u32(&a);
    const Point start{mt_arg_f32(&a), mt_arg_f32(&a)};
    const Point end{mt_arg_f32(&a), mt_arg_f32(&a)};
    const u32 options = mt_arg_u32(&a);
    ContextObj *c = context_arg(cpu, ctx, "CGContextDrawLinearGradient");
    const Gradient *gr = gradient_arg(cpu, g, "CGContextDrawLinearGradient");
    if (c != nullptr && gr != nullptr) {
        Surface dst = surface_of(cpu, *c);
        c->canvas.draw_linear_gradient(dst, *gr, start, end, options);
    }
}

// void CGContextDrawRadialGradient(CGContextRef c, CGGradientRef g, CGPoint startCenter,
//     CGFloat startRadius, CGPoint endCenter, CGFloat endRadius, CGGradientDrawingOptions o)
extern "C" void mt_hle_CGContextDrawRadialGradient(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 ctx = mt_arg_u32(&a), g = mt_arg_u32(&a);
    const Point c0{mt_arg_f32(&a), mt_arg_f32(&a)};
    const double r0 = mt_arg_f32(&a);
    const Point c1{mt_arg_f32(&a), mt_arg_f32(&a)};
    const double r1 = mt_arg_f32(&a);
    const u32 options = mt_arg_u32(&a);
    ContextObj *c = context_arg(cpu, ctx, "CGContextDrawRadialGradient");
    const Gradient *gr = gradient_arg(cpu, g, "CGContextDrawRadialGradient");
    if (c != nullptr && gr != nullptr) {
        Surface dst = surface_of(cpu, *c);
        c->canvas.draw_radial_gradient(dst, *gr, c0, r0, c1, r1, options);
    }
}

extern "C" void mt_hle_CGColorSpaceCreateDeviceRGB(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, color_space_create(cpu));
}

extern "C" void mt_hle_CGColorSpaceRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    release_nullable(cpu);
}

// const CGFloat *CGColorGetComponents(CGColorRef color): a guest array the color owns.
extern "C" void mt_hle_CGColorGetComponents(mt_cpu *cpu, std::uint32_t) noexcept {
    const ColorObj *c = find<ColorObj>(cpu, cpu->r[0], k_color_state);
    mt_ret_u32(cpu, c != nullptr ? c->components : 0u);
}

// CGGradientRef CGGradientCreateWithColors(CGColorSpaceRef space, CFArrayRef colors,
//     const CGFloat locations[])
extern "C" void mt_hle_CGGradientCreateWithColors(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_arg_u32(&a); // the colour space: device RGB is the only one
    const u32 colors = mt_arg_u32(&a), locations = mt_arg_u32(&a);
    // The array is a guest NSArray (Foundation, #49): read it by message.
    const u32 count = colors != 0 ? mt_objc_send0(cpu, colors, "count") : 0u;
    Gradient g;
    for (u32 i = 0; i < count; i++) {
        const u32 obj = mt_objc_send1(cpu, colors, "objectAtIndex:", i);
        Color col;
        if (!color_value(cpu, obj, &col)) {
            log(cpu, "CGGradientCreateWithColors: element %u (0x%08X) is not a CGColor",
                static_cast<unsigned>(i), static_cast<unsigned>(obj));
            mt_ret_u32(cpu, 0);
            return;
        }
        float loc = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        if (locations != 0) {
            u32 bits;
            std::memcpy(&bits, mt_g2h(cpu, locations + 4u * i, 4), 4);
            loc = f32(bits);
        }
        g.stops.push_back({loc, col});
    }
    if (g.stops.empty()) {
        log(cpu, "CGGradientCreateWithColors: no colours (called from 0x%08X)",
            static_cast<unsigned>(cpu->r[MT_LR]));
        mt_ret_u32(cpu, 0);
        return;
    }
    mt_ret_u32(cpu, gradient_create(cpu, std::move(g)));
}

extern "C" void mt_hle_CGGradientRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    release_nullable(cpu);
}

// CGDataProviderRef CGDataProviderCreateWithCFData(CFDataRef data)
extern "C" void mt_hle_CGDataProviderCreateWithCFData(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 data = cpu->r[0];
    mt_ret_u32(cpu, data != 0 ? provider_create_data(cpu, data) : 0u);
}

// CFDataRef CGDataProviderCopyData(CGDataProviderRef provider): a new CFData with a copy.
extern "C" void mt_hle_CGDataProviderCopyData(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 provider = cpu->r[0];
    const u8 *bytes = nullptr;
    std::size_t size = 0;
    if (!provider_bytes(cpu, provider, &bytes, &size)) {
        log_once(cpu, "CGDataProviderCopyData", "CGDataProviderCopyData: invalid provider 0x%08X",
                 static_cast<unsigned>(provider));
        mt_ret_u32(cpu, 0);
        return;
    }
    // Copied to the host first: the CFData's guest block may come from the same heap.
    const std::vector<u8> copy(bytes, bytes + size);
    mt_ret_u32(cpu, mt_cf::data_create(cpu, copy.data(), static_cast<u32>(copy.size()), false));
}

extern "C" void mt_hle_CGDataProviderRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    release_nullable(cpu);
}

// CGImageRef CGImageCreate(size_t width, size_t height, size_t bitsPerComponent,
//     size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space, CGBitmapInfo info,
//     CGDataProviderRef provider, const CGFloat decode[], bool shouldInterpolate,
//     CGColorRenderingIntent intent)
extern "C" void mt_hle_CGImageCreate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    ImageInfo info;
    info.width = mt_arg_u32(&a);
    info.height = mt_arg_u32(&a);
    info.bpc = mt_arg_u32(&a);
    info.bpp = mt_arg_u32(&a);
    info.bytes_per_row = mt_arg_u32(&a);
    info.space = mt_arg_u32(&a);
    info.bitmap_info = mt_arg_u32(&a);
    info.provider = mt_arg_u32(&a);
    const u32 decode = mt_arg_u32(&a);
    info.interpolate = (mt_arg_u32(&a) & 0xFFu) != 0;
    info.intent = mt_arg_u32(&a);
    if (decode != 0) {
        log_once(cpu, "CGImageCreate.decode",
                 "CGImageCreate: a decode array is ignored (the game passes NULL)");
    }
    mt_ret_u32(cpu, image_create(cpu, info));
}

extern "C" void mt_hle_CGImageRelease(mt_cpu *cpu, std::uint32_t) noexcept {
    release_nullable(cpu);
}

namespace {

// The CGImageGet* functions: one field of the image, 0 for NULL or not an image.
template <u32 ImageInfo::*Field> void image_get(mt_cpu *cpu) {
    const ImageInfo *i = image_info(cpu, cpu->r[0]);
    mt_ret_u32(cpu, i != nullptr ? i->*Field : 0u);
}

} // namespace

extern "C" void mt_hle_CGImageGetWidth(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::width>(cpu);
}
extern "C" void mt_hle_CGImageGetHeight(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::height>(cpu);
}
extern "C" void mt_hle_CGImageGetBitsPerComponent(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::bpc>(cpu);
}
extern "C" void mt_hle_CGImageGetBitsPerPixel(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::bpp>(cpu);
}
extern "C" void mt_hle_CGImageGetBytesPerRow(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::bytes_per_row>(cpu);
}
extern "C" void mt_hle_CGImageGetBitmapInfo(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::bitmap_info>(cpu);
}
extern "C" void mt_hle_CGImageGetAlphaInfo(mt_cpu *cpu, std::uint32_t) noexcept {
    const ImageInfo *i = image_info(cpu, cpu->r[0]);
    mt_ret_u32(cpu, i != nullptr ? (i->bitmap_info & k_alpha_info_mask) : 0u);
}
extern "C" void mt_hle_CGImageGetColorSpace(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::space>(cpu);
}
extern "C" void mt_hle_CGImageGetDataProvider(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::provider>(cpu);
}
extern "C" void mt_hle_CGImageGetRenderingIntent(mt_cpu *cpu, std::uint32_t) noexcept {
    image_get<&ImageInfo::intent>(cpu);
}

// bool CGRectContainsPoint(CGRect rect, CGPoint point)
extern "C" void mt_hle_CGRectContainsPoint(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const Rect r = standardized(read_rect(&a));
    const double x = mt_arg_f32(&a), y = mt_arg_f32(&a);
    const bool in = std::isfinite(r.x) && std::isfinite(r.y) && x >= r.x && x < r.x + r.w &&
                    y >= r.y && y < r.y + r.h;
    mt_ret_u32(cpu, in ? 1u : 0u);
}

// CGRect CGRectInset(CGRect rect, CGFloat dx, CGFloat dy): through the hidden result pointer
// (r0), the rect in r1-r3 and [sp], dx and dy at [sp+4] and [sp+8] (ANALYSIS.md). A rect that
// would get a negative size is CGRectNull.
extern "C" void mt_hle_CGRectInset(mt_cpu *cpu, std::uint32_t) noexcept {
    u32 result;
    mt_args a = mt_args_begin_stret(cpu, &result);
    Rect r = standardized(read_rect(&a));
    const double dx = mt_arg_f32(&a), dy = mt_arg_f32(&a);
    float out[4];
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || r.w - 2 * dx < 0 || r.h - 2 * dy < 0) {
        const float inf = std::numeric_limits<float>::infinity();
        out[0] = inf;
        out[1] = inf;
        out[2] = 0;
        out[3] = 0;
    } else {
        out[0] = static_cast<float>(r.x + dx);
        out[1] = static_cast<float>(r.y + dy);
        out[2] = static_cast<float>(r.w - 2 * dx);
        out[3] = static_cast<float>(r.h - 2 * dy);
    }
    mt_ret_struct(cpu, result, out, sizeof out);
}

// CGAffineTransform CGAffineTransformMakeRotation(CGFloat angle): through the hidden result
// pointer; {cos, sin, -sin, cos, 0, 0}.
extern "C" void mt_hle_CGAffineTransformMakeRotation(mt_cpu *cpu, std::uint32_t) noexcept {
    u32 result;
    mt_args a = mt_args_begin_stret(cpu, &result);
    const double angle = mt_arg_f32(&a);
    const float c = static_cast<float>(std::cos(angle)), s = static_cast<float>(std::sin(angle));
    const float out[6] = {c, s, -s, c, 0, 0};
    mt_ret_struct(cpu, result, out, sizeof out);
}

// --- HLE: the data imports --------------------------------------------------------------------

extern "C" void mt_hle_init_CGRectZero(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    const float v[4] = {0, 0, 0, 0};
    store_floats(cpu, addr, v, 4);
}

extern "C" void mt_hle_init_CGRectNull(mt_cpu *cpu, std::uint32_t, std::uint32_t addr) noexcept {
    const float inf = std::numeric_limits<float>::infinity();
    const float v[4] = {inf, inf, 0, 0};
    store_floats(cpu, addr, v, 4);
}

extern "C" void mt_hle_init_CGAffineTransformIdentity(mt_cpu *cpu, std::uint32_t,
                                                      std::uint32_t addr) noexcept {
    const float v[6] = {1, 0, 0, 1, 0, 0};
    store_floats(cpu, addr, v, 6);
}

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// Each CG object's box is saved as its values: guest objects as their addresses, a host-memory
// provider's bytes whole (the decoded PNGs of UIImage: they exist nowhere else). A bitmap
// context has none: contexts only live inside a drawing (TextCache_DrawText, -drawRect:), never
// at a safe point, and the census would name one that did.

namespace mt_cg {
namespace {

template <typename T> const T *box_of(const void *state) {
    return *static_cast<const T *const *>(state);
}
template <typename T> void drop_box(void *state) {
    delete *static_cast<T **>(state);
    *static_cast<T **>(state) = nullptr;
}
// A box saved as a presence flag, then its values.
template <typename T, typename F> bool save_box(const void *state, mt_snap::Writer &w, F values) {
    const T *p = box_of<T>(state);
    w.boolean(p != nullptr);
    if (p != nullptr) {
        values(*p, w);
    }
    return true;
}
template <typename T, typename F> bool load_box(void *state, mt_snap::Reader &r, F values) {
    bool has = false;
    if (!r.boolean(&has)) {
        return false;
    }
    if (!has) {
        return true;
    }
    auto p = std::make_unique<T>();
    if (!values(*p, r)) {
        return false;
    }
    *static_cast<T **>(state) = p.release();
    return true;
}

bool save_space(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<ColorSpace>(state, w, [](const ColorSpace &, mt_snap::Writer &) {});
}
bool load_space(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<ColorSpace>(state, r, [](ColorSpace &, mt_snap::Reader &) { return true; });
}

bool save_color(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<ColorObj>(state, w, [](const ColorObj &c, mt_snap::Writer &o) {
        snap::put(o, c.value);
        o.u32(c.components);
    });
}
bool load_color(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<ColorObj>(state, r, [](ColorObj &c, mt_snap::Reader &in) {
        return snap::get(in, &c.value) && in.u32(&c.components);
    });
}

bool save_provider(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<Provider>(state, w, [](const Provider &p, mt_snap::Writer &o) {
        o.u32(p.data);
        o.boolean(p.host != nullptr);
        if (p.host != nullptr) {
            o.blob(*p.host);
        }
    });
}
bool load_provider(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<Provider>(state, r, [](Provider &p, mt_snap::Reader &in) {
        bool host = false;
        if (!in.u32(&p.data) || !in.boolean(&host)) {
            return false;
        }
        if (host) {
            auto bytes = std::make_shared<std::vector<u8>>();
            if (!in.blob(bytes.get())) {
                return false;
            }
            p.host = std::move(bytes);
        }
        return true;
    });
}

bool save_image(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<ImageObj>(state, w, [](const ImageObj &i, mt_snap::Writer &o) {
        const ImageInfo &f = i.info;
        for (const u32 v : {f.width, f.height, f.bpc, f.bpp, f.bytes_per_row, f.bitmap_info,
                            f.intent, f.space, f.provider}) {
            o.u32(v);
        }
        o.boolean(f.interpolate);
    });
}
bool load_image(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<ImageObj>(state, r, [](ImageObj &i, mt_snap::Reader &in) {
        ImageInfo &f = i.info;
        for (u32 *v : {&f.width, &f.height, &f.bpc, &f.bpp, &f.bytes_per_row, &f.bitmap_info,
                       &f.intent, &f.space, &f.provider}) {
            if (!in.u32(v)) {
                return false;
            }
        }
        return in.boolean(&f.interpolate);
    });
}

bool save_gradient(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<GradientObj>(state, w, [](const GradientObj &g, mt_snap::Writer &o) {
        o.u32(static_cast<u32>(g.g.stops.size()));
        for (const Gradient::Stop &s : g.g.stops) {
            o.f32(s.location);
            snap::put(o, s.color);
        }
    });
}
bool load_gradient(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<GradientObj>(state, r, [](GradientObj &g, mt_snap::Reader &in) {
        u32 n = 0;
        if (!in.u32(&n) || !in.count_fits(n, 20)) {
            return false;
        }
        g.g.stops.resize(n);
        for (Gradient::Stop &s : g.g.stops) {
            if (!in.f32(&s.location) || !snap::get(in, &s.color)) {
                return false;
            }
        }
        return true;
    });
}

bool save_path(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    return save_box<PathObj>(state, w,
                             [](const PathObj &p, mt_snap::Writer &o) { snap::put(o, p.p); });
}
bool load_path(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return load_box<PathObj>(state, r,
                             [](PathObj &p, mt_snap::Reader &in) { return snap::get(in, &p.p); });
}

} // namespace

extern const mt_snap::StateOps k_snap_space, k_snap_color, k_snap_provider, k_snap_image,
    k_snap_gradient, k_snap_path;
const mt_snap::StateOps k_snap_space = {&k_space_state, "CGColorSpace",       save_space,
                                        load_space,     drop_box<ColorSpace>, nullptr};
const mt_snap::StateOps k_snap_color = {&k_color_state, "CGColor",          save_color,
                                        load_color,     drop_box<ColorObj>, nullptr};
const mt_snap::StateOps k_snap_provider = {&k_provider_state, "CGDataProvider",   save_provider,
                                           load_provider,     drop_box<Provider>, nullptr};
const mt_snap::StateOps k_snap_image = {&k_image_state, "CGImage",          save_image,
                                        load_image,     drop_box<ImageObj>, nullptr};
const mt_snap::StateOps k_snap_gradient = {&k_gradient_state, "CGGradient",          save_gradient,
                                           load_gradient,     drop_box<GradientObj>, nullptr};
const mt_snap::StateOps k_snap_path = {&k_path_state, "CGPath",          save_path,
                                       load_path,     drop_box<PathObj>, nullptr};

} // namespace mt_cg
