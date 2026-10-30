// UIKit's drawing layer (#51, ADR 0036): the UIGraphics context stack, UIImage, UIColor,
// UIBezierPath and UIKit's C drawing functions. The views that draw with them are #54's.
//
// What the game does with them (docs/re/imports.md lists the senders):
//   +[UIImage imageNamed:]              80 sites: the shell's buttons and backgrounds
//   -[UIImage initWithData:]            the texture loader FUN_00009534 (then CGImageGet*, a
//                                       POT bitmap context, CGContextDrawImage)
//   -initWithCGImage:, PNGRepresentation  the map tile cutter FUN_0000d6d0
//   UIGraphicsBeginImageContext, -drawInRect:, ...GetImageFromCurrentImageContext
//                                       -[EAGLView imageWithImage:scaledToSize:] (0x3251)
//   UIGraphicsPush/PopContext           TextCache_DrawText (0x9031) around -drawAtPoint:withFont:
//   UIColor                             view backgrounds (#54) and Chartboost's drawRect:s
//   UIBezierPath, gradients, clip       Chartboost's buttons and progress bar only
//
// An image context is a CGBitmapContext with host memory, premultiplied RGBA, its CTM flipped
// so that user space is y-down with the origin at the top left, as UIKit's is. The screen
// scale is 1 (the game is a 320 x 480 iPhone OS 3 app; iOS 4 ran it unscaled).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_hle.h"
#include "cg/cg_objects.h"
#include "maplethief/cg.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace mt_cg {

namespace {

// --- UIGraphics' context stack -------------------------------------------------------------------

struct Entry {
    u32 context = 0;    // retained
    bool image = false; // made by UIGraphicsBeginImageContext*
    float scale = 1;
};

struct UIKit {
    std::vector<Entry> stack;
    std::map<std::string, u32> named; // +imageNamed: cache, UIImages retained
    // The image hook's replacements by bundle file (#153): host CGImages, retained (0 when the
    // hook kept the file's image), and the hook they came from. Asked once per file and hook.
    struct Display {
        u32 image = 0;
        std::uint64_t hook = 0;
    };
    std::map<std::string, Display> display;
};
UIKit g_uikit;

// The image hook (maplethief/cg.h, #153): kept across runtimes.
struct ImageHook {
    mt_cg_image_replace_fn fn = nullptr;
    void *user = nullptr;
    mt_cg_image_stats stats{};
    std::uint64_t generation = 0; // counts mt_cg_set_image_replace_hook calls
};
ImageHook g_image_hook;

void push(mt_cpu *cpu, const Entry &e) {
    runtime(cpu);
    g_uikit.stack.push_back(e);
}

// Pops the top entry and releases its context; false if the stack is empty.
bool pop(mt_cpu *cpu, const char *function) {
    runtime(cpu);
    if (g_uikit.stack.empty()) {
        log_once(cpu, function, "%s: the context stack is empty (called from 0x%08X)", function,
                 static_cast<unsigned>(cpu->r[MT_LR]));
        return false;
    }
    const u32 ctx = g_uikit.stack.back().context;
    g_uikit.stack.pop_back();
    mt_objc_release(cpu, ctx);
    return true;
}

const Entry *top(mt_cpu *cpu) {
    runtime(cpu);
    return g_uikit.stack.empty() ? nullptr : &g_uikit.stack.back();
}

// The current context's canvas, or false (logged once under `what`).
bool current_canvas(mt_cpu *cpu, const char *what, Canvas **canvas, Surface *s) {
    if (context_canvas(cpu, current_context(cpu), canvas, s)) {
        return true;
    }
    log_once(cpu, what, "%s: no current context (called from 0x%08X): nothing drawn", what,
             static_cast<unsigned>(cpu->r[MT_LR]));
    return false;
}

// UIGraphicsBeginImageContextWithOptions: a host-memory context of the size in pixels, y-down.
void begin_image_context(mt_cpu *cpu, float w, float h, bool opaque, float scale) {
    if (!(scale > 0)) {
        scale = 1; // 0: the main screen's scale
    }
    const double pw = std::ceil(static_cast<double>(w) * scale - 1e-6);
    const double ph = std::ceil(static_cast<double>(h) * scale - 1e-6);
    u32 ctx = 0;
    if (pw >= 1 && ph >= 1 && pw <= 0x4000 && ph <= 0x4000) {
        const u32 width = static_cast<u32>(pw), height = static_cast<u32>(ph);
        const u32 space = color_space_create(cpu);
        ctx = context_create(cpu, 0, width, height, 8, width * 4, space,
                             (opaque ? k_alpha_none_skip_last : k_alpha_premultiplied_last) |
                                 k_byte_order_32_big);
        mt_objc_release(cpu, space);
        Canvas *canvas = nullptr;
        Surface s;
        if (context_canvas(cpu, ctx, &canvas, &s)) {
            canvas->state().ctm = Affine::scale(scale, -static_cast<double>(scale))
                                      .then(Affine::translate(0, height));
        }
    } else {
        log(cpu, "UIGraphicsBeginImageContext: invalid size %g x %g (called from 0x%08X)",
            static_cast<double>(w), static_cast<double>(h), static_cast<unsigned>(cpu->r[MT_LR]));
    }
    // A failed begin still pushes, so that the caller's UIGraphicsEndImageContext balances it;
    // drawing into it is ignored and the image is nil, as on the device.
    push(cpu, {ctx, true, scale});
}

// --- UIImage -------------------------------------------------------------------------------------

struct ImageState {
    u32 cgimage; // retained
    float scale;
    // What the compositor draws instead (#153): the image hook's replacement, retained, or 0.
    u32 display;
};

void image_destroy(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<ImageState *>(state)->cgimage);
    mt_objc_release(cpu, static_cast<ImageState *>(state)->display);
}
const mt_objc_state_type k_image_state = {"UIImage", sizeof(ImageState), image_destroy};

ImageState *image_state(mt_cpu *cpu, u32 uiimage) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && uiimage != 0
               ? static_cast<ImageState *>(mt_objc_find_state(rt, uiimage, &k_image_state))
               : nullptr;
}

// Gives the UIImage `self` (just allocated) the image `cgimage` (retained); false if out of
// memory.
bool image_init(mt_cpu *cpu, u32 self, u32 cgimage, float scale) {
    mt_objc *rt = runtime(cpu);
    auto *s = static_cast<ImageState *>(mt_objc_state(rt, self, &k_image_state));
    if (s == nullptr) {
        return false;
    }
    const u32 old = s->cgimage;
    s->cgimage = mt_objc_retain(cpu, cgimage);
    s->scale = scale;
    mt_objc_release(cpu, old);
    return true;
}

// A new autoreleased instance of `cls` with the image (retained); 0 if `cgimage` is 0.
u32 image_new(mt_cpu *cpu, u32 cls, u32 cgimage, float scale) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = cgimage != 0 ? mt_objc_create_instance(rt, cls) : 0u;
    if (obj == 0 || !image_init(cpu, obj, cgimage, scale)) {
        mt_objc_release(cpu, obj);
        return 0;
    }
    return mt_objc_autorelease(cpu, obj);
}

// A new CGImage (retain count 1) from the bytes of a data object, or 0 (logged).
u32 cgimage_from_data(mt_cpu *cpu, u32 data, const char *what) {
    u32 addr = 0, length = 0;
    if (data != 0 && !mt_cf::data_bytes(cpu, data, &addr, &length)) {
        // A data object of another class (Foundation, #49): ask it.
        length = mt_objc_send0(cpu, data, "length");
        addr = length != 0 ? mt_objc_send0(cpu, data, "bytes") : 0u;
    }
    if (data == 0 || length == 0 || addr == 0) {
        log(cpu, "%s: no data (0x%08X): nil", what, static_cast<unsigned>(data));
        return 0;
    }
    std::string error;
    const u32 image = image_from_png(cpu, mt_g2h(cpu, addr, length), length, &error);
    if (image == 0) {
        log(cpu, "%s: %u bytes that are no PNG (%s): nil", what, static_cast<unsigned>(length),
            error.c_str());
    }
    return image;
}

// A new CGImage (retain count 1) from a file of the bundle, or 0 (logged).
u32 cgimage_from_bundle(mt_cpu *cpu, const std::string &rel, const char *what) {
    std::vector<std::uint8_t> bytes;
    if (!mt_cf::bundle_read(rel, bytes)) {
        log(cpu, "%s: cannot read %s: nil", what, rel.c_str());
        return 0;
    }
    std::string error;
    const u32 image = image_from_png(cpu, bytes.data(), bytes.size(), &error);
    if (image == 0) {
        log(cpu, "%s: %s is no PNG (%s): nil", what, rel.c_str(), error.c_str());
    }
    return image;
}

// The image hook's replacement for the bundle file `rel` (whose image is `cgimage`): a host
// CGImage, not retained (g_uikit.display holds it), or 0. The hook is asked once per file.
u32 display_for(mt_cpu *cpu, const std::string &rel, u32 cgimage) {
    if (g_image_hook.fn == nullptr || cgimage == 0) {
        return 0;
    }
    UIKit::Display &cached = g_uikit.display[rel];
    if (cached.hook == g_image_hook.generation) {
        return cached.image;
    }
    mt_objc_release(cpu, cached.image); // another hook's (images that hold it keep it)
    cached = UIKit::Display{};
    u32 display = 0;
    const ImageInfo *info = image_info(cpu, cgimage);
    std::vector<u8> rgba;
    if (info != nullptr && image_rgba(cpu, cgimage, rgba)) {
        const mt_cg_image_file file{rel.c_str(), info->width, info->height, rgba.data()};
        mt_cg_image_replacement out{};
        ++g_image_hook.stats.offered;
        if (g_image_hook.fn(g_image_hook.user, &file, &out) != 0) {
            const u32 k = info->width != 0 ? out.width / info->width : 0u;
            if (out.rgba != nullptr && k >= 1 && k <= MT_GFX_MAX_SCALE &&
                out.width == k * info->width && out.height == k * info->height &&
                out.width <= MT_GFX_MAX_SIZE && out.height <= MT_GFX_MAX_SIZE) {
                auto pixels = std::make_shared<const std::vector<u8>>(
                    out.rgba, out.rgba + static_cast<std::size_t>(out.width) * out.height * 4u);
                display = image_create_host(cpu, out.width, out.height, 32, k_alpha_last,
                                            std::move(pixels));
                ++g_image_hook.stats.replaced;
            } else {
                ++g_image_hook.stats.rejected;
                log(cpu,
                    "UIImage %s: replacement %ux%u for %ux%u rejected: not k times the "
                    "size, k 1..%u",
                    rel.c_str(), static_cast<unsigned>(out.width),
                    static_cast<unsigned>(out.height), static_cast<unsigned>(info->width),
                    static_cast<unsigned>(info->height), static_cast<unsigned>(MT_GFX_MAX_SCALE));
            }
        }
    }
    g_uikit.display[rel] = UIKit::Display{display, g_image_hook.generation};
    return display;
}

// Gives the UIImage the compositor's replacement for its bundle file `rel` (#153).
void set_display(mt_cpu *cpu, u32 uiimage, const std::string &rel) {
    ImageState *s = image_state(cpu, uiimage);
    if (s == nullptr) {
        return;
    }
    const u32 display = display_for(cpu, rel, s->cgimage);
    if (display != 0 && s->display == 0) {
        s->display = mt_objc_retain(cpu, display);
    }
}

std::string utf8_of(mt_cpu *cpu, u32 str) {
    if (const std::u16string *chars = mt_cf::string_chars(cpu, str)) {
        return mt_cf::to_utf8(*chars);
    }
    const u32 utf8 = str != 0 ? mt_objc_send0(cpu, str, "UTF8String") : 0u;
    return utf8 != 0 ? std::string(mt_g2h_str(cpu, utf8)) : std::string();
}

// +[UIImage imageNamed:(NSString *)name]: a file of the bundle (".png" if the name has no
// extension, or if the name with its extension is not found), decoded once and kept for the
// process, as UIKit's cache did.
void image_named(mt_cpu *cpu) {
    const u32 cls = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const std::string name = utf8_of(cpu, mt_arg_u32(&a));
    runtime(cpu);
    const auto cached = g_uikit.named.find(name);
    if (cached != g_uikit.named.end()) {
        mt_ret_u32(cpu, cached->second);
        return;
    }
    const std::size_t slash = name.rfind('/');
    const std::size_t dot = name.rfind('.');
    const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    std::string rel =
        has_ext ? mt_cf::bundle_find(cpu, name.substr(0, dot), name.substr(dot + 1), "", "")
                : std::string();
    if (rel.empty()) {
        // No extension, or a dot that is part of the name ("v1.0.8"): name.png.
        rel = mt_cf::bundle_find(cpu, name, "png", "", "");
    }
    if (rel.empty()) {
        log_once(cpu, "UIImage.imageNamed.missing",
                 "+[UIImage imageNamed:@\"%s\"]: no such file in the bundle: nil", name.c_str());
        mt_ret_u32(cpu, 0);
        return;
    }
    const u32 cgimage = cgimage_from_bundle(cpu, rel, "+[UIImage imageNamed:]");
    const u32 image = image_new(cpu, cls, cgimage, 1);
    mt_objc_release(cpu, cgimage);
    set_display(cpu, image, rel);
    if (image != 0) {
        g_uikit.named[name] = mt_objc_retain(cpu, image);
    }
    mt_ret_u32(cpu, image);
}

// +[UIImage imageWithContentsOfFile:(NSString *)path]: a file of the bundle by its guest path.
void image_with_contents_of_file(mt_cpu *cpu) {
    const u32 cls = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const std::string path = utf8_of(cpu, mt_arg_u32(&a));
    const std::string prefix = mt_cf::bundle_guest_path("") + "/";
    const std::string rel =
        path.compare(0, prefix.size(), prefix) == 0 ? path.substr(prefix.size()) : std::string();
    if (rel.empty() || !mt_cf::bundle_has(cpu, rel)) {
        log(cpu, "+[UIImage imageWithContentsOfFile:@\"%s\"]: not a file of the bundle: nil",
            path.c_str());
        mt_ret_u32(cpu, 0);
        return;
    }
    const u32 cgimage = cgimage_from_bundle(cpu, rel, "+[UIImage imageWithContentsOfFile:]");
    const u32 image = image_new(cpu, cls, cgimage, 1);
    mt_objc_release(cpu, cgimage);
    set_display(cpu, image, rel);
    mt_ret_u32(cpu, image);
}

// +[UIImage imageWithData:(NSData *)data]
void image_with_data(mt_cpu *cpu) {
    const u32 cls = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 data = mt_arg_u32(&a);
    const u32 cgimage = cgimage_from_data(cpu, data, "+[UIImage imageWithData:]");
    const u32 image = image_new(cpu, cls, cgimage, 1);
    mt_objc_release(cpu, cgimage);
    mt_ret_u32(cpu, image);
}

// -[UIImage initWithData:(NSData *)data]: nil (self released) if the data is no image.
void image_init_with_data(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 data = mt_arg_u32(&a);
    const u32 cgimage = cgimage_from_data(cpu, data, "-[UIImage initWithData:]");
    const bool ok = cgimage != 0 && image_init(cpu, self, cgimage, 1);
    mt_objc_release(cpu, cgimage);
    if (!ok) {
        mt_objc_release(cpu, self);
    }
    mt_ret_u32(cpu, ok ? self : 0u);
}

// -[UIImage initWithCGImage:(CGImageRef)image]
void image_init_with_cgimage(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 cgimage = mt_arg_u32(&a);
    const bool ok = image_info(cpu, cgimage) != nullptr && image_init(cpu, self, cgimage, 1);
    if (!ok) {
        log(cpu, "-[UIImage initWithCGImage:]: 0x%08X is no CGImage: nil",
            static_cast<unsigned>(cgimage));
        mt_objc_release(cpu, self);
    }
    mt_ret_u32(cpu, ok ? self : 0u);
}

// -[UIImage CGImage]: not retained.
void image_cgimage(mt_cpu *cpu) { mt_ret_u32(cpu, uiimage_cgimage(cpu, mt_objc_self(cpu))); }

// -[UIImage size] -> CGSize (stret): pixels / scale.
void image_size(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    float size[2] = {0, 0};
    const ImageState *s = image_state(cpu, self);
    const ImageInfo *info = s != nullptr ? image_info(cpu, s->cgimage) : nullptr;
    if (info != nullptr) {
        size[0] = static_cast<float>(info->width) / s->scale;
        size[1] = static_cast<float>(info->height) / s->scale;
    }
    mt_ret_struct(cpu, result, size, sizeof size);
}

// -[UIImage scale] -> CGFloat
void image_scale(mt_cpu *cpu) {
    const ImageState *s = image_state(cpu, mt_objc_self(cpu));
    mt_ret_f32(cpu, s != nullptr ? s->scale : 1.0f);
}

// -[UIImage drawInRect:(CGRect)rect]: the image's top row at the rect's minimum y.
void image_draw_in_rect(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    Rect r;
    r.x = mt_arg_f32(&a);
    r.y = mt_arg_f32(&a);
    r.w = mt_arg_f32(&a);
    r.h = mt_arg_f32(&a);
    Canvas *canvas = nullptr;
    Surface s;
    ConstSurface img;
    if (current_canvas(cpu, "-[UIImage drawInRect:]", &canvas, &s) &&
        image_surface(cpu, uiimage_cgimage(cpu, self), &img)) {
        canvas->draw_image(s, r, img, true);
    }
}

const mt_objc_host_method k_image_class_methods[] = {
    {"imageNamed:", image_named},
    {"imageWithContentsOfFile:", image_with_contents_of_file},
    {"imageWithData:", image_with_data},
};
const mt_objc_host_method k_image_methods[] = {
    {"initWithData:", image_init_with_data},
    {"initWithCGImage:", image_init_with_cgimage},
    {"CGImage", image_cgimage},
    {"size", image_size},
    {"scale", image_scale},
    {"drawInRect:", image_draw_in_rect},
};

// --- UIColor -------------------------------------------------------------------------------------

struct ColorState {
    Color color;
    u32 cgcolor; // made by -CGColor, owned
};

void color_destroy(mt_cpu *cpu, u32, void *state) {
    mt_objc_release(cpu, static_cast<ColorState *>(state)->cgcolor);
}
const mt_objc_state_type k_color_state = {"UIColor", sizeof(ColorState), color_destroy};

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

bool color_init(mt_cpu *cpu, u32 obj, Color c) {
    auto *s = static_cast<ColorState *>(mt_objc_state(runtime(cpu), obj, &k_color_state));
    if (s == nullptr) {
        return false;
    }
    s->color = {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
    return true;
}

void color_return_new(mt_cpu *cpu, Color c) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = mt_objc_create_instance(rt, mt_objc_self(cpu));
    if (obj == 0 || !color_init(cpu, obj, c)) {
        mt_objc_release(cpu, obj);
        mt_ret_u32(cpu, 0);
        return;
    }
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, obj));
}

const ColorState *color_state(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && obj != 0
               ? static_cast<const ColorState *>(mt_objc_find_state(rt, obj, &k_color_state))
               : nullptr;
}

Color read_rgba(mt_args *a) {
    Color c;
    c.r = mt_arg_f32(a);
    c.g = mt_arg_f32(a);
    c.b = mt_arg_f32(a);
    c.a = mt_arg_f32(a);
    return c;
}

void color_black(mt_cpu *cpu) { color_return_new(cpu, {0, 0, 0, 1}); }
void color_white(mt_cpu *cpu) { color_return_new(cpu, {1, 1, 1, 1}); }
void color_clear(mt_cpu *cpu) { color_return_new(cpu, {0, 0, 0, 0}); }

// +colorWithRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a
void color_with_rgba(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    color_return_new(cpu, read_rgba(&a));
}

// +colorWithWhite:(CGFloat)white alpha:(CGFloat)alpha
void color_with_white(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const float w = mt_arg_f32(&a), alpha = mt_arg_f32(&a);
    color_return_new(cpu, {w, w, w, alpha});
}

// -initWithRed:green:blue:alpha:
void color_init_rgba(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    if (!color_init(cpu, self, read_rgba(&a))) {
        mt_objc_release(cpu, self);
        mt_ret_u32(cpu, 0);
        return;
    }
    mt_ret_u32(cpu, self);
}

// -CGColor: owned by the UIColor.
void color_cgcolor(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    auto *s = const_cast<ColorState *>(color_state(cpu, self));
    if (s == nullptr) {
        mt_ret_u32(cpu, 0);
        return;
    }
    if (s->cgcolor == 0) {
        s->cgcolor = color_create(cpu, s->color);
    }
    mt_ret_u32(cpu, s->cgcolor);
}

// -set, -setFill, -setStroke: the current context's colours.
void color_apply(mt_cpu *cpu, bool fill, bool stroke, const char *what) {
    const ColorState *s = color_state(cpu, mt_objc_self(cpu));
    Canvas *canvas = nullptr;
    Surface surface;
    if (s == nullptr || !current_canvas(cpu, what, &canvas, &surface)) {
        return;
    }
    if (fill) {
        canvas->state().fill = s->color;
    }
    if (stroke) {
        canvas->state().stroke = s->color;
    }
}
void color_set(mt_cpu *cpu) { color_apply(cpu, true, true, "-[UIColor set]"); }
void color_set_fill(mt_cpu *cpu) { color_apply(cpu, true, false, "-[UIColor setFill]"); }
void color_set_stroke(mt_cpu *cpu) { color_apply(cpu, false, true, "-[UIColor setStroke]"); }

const mt_objc_host_method k_color_class_methods[] = {
    {"blackColor", color_black},
    {"whiteColor", color_white},
    {"clearColor", color_clear},
    {"colorWithRed:green:blue:alpha:", color_with_rgba},
    {"colorWithWhite:alpha:", color_with_white},
};
const mt_objc_host_method k_color_methods[] = {
    {"initWithRed:green:blue:alpha:", color_init_rgba},
    {"CGColor", color_cgcolor},
    {"set", color_set},
    {"setFill", color_set_fill},
    {"setStroke", color_set_stroke},
};

// --- UIBezierPath (Chartboost) -------------------------------------------------------------------

struct BezierPath {
    Path path;
    float line_width = 1;
    u32 cgpath = 0; // made by -CGPath, owned; dropped when the path changes
};

void bezier_destroy(mt_cpu *cpu, u32, void *state) {
    BezierPath *p = *static_cast<BezierPath **>(state);
    if (p != nullptr) {
        mt_objc_release(cpu, p->cgpath);
        delete p;
    }
}
const mt_objc_state_type k_bezier_state = {"UIBezierPath", sizeof(void *), bezier_destroy};

// The path of a UIBezierPath, created on first use; nullptr if out of memory.
BezierPath *bezier(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    void *s = rt != nullptr && obj != 0 ? mt_objc_state(rt, obj, &k_bezier_state) : nullptr;
    if (s == nullptr) {
        return nullptr;
    }
    auto **box = static_cast<BezierPath **>(s);
    if (*box == nullptr) {
        *box = new BezierPath();
    }
    return *box;
}

// The path is about to change: the CGPath handed out describes the old one.
BezierPath *bezier_mutable(mt_cpu *cpu, u32 obj) {
    BezierPath *p = bezier(cpu, obj);
    if (p != nullptr && p->cgpath != 0) {
        mt_objc_release(cpu, p->cgpath);
        p->cgpath = 0;
    }
    return p;
}

u32 bezier_new(mt_cpu *cpu, u32 cls) {
    const u32 obj = mt_objc_create_instance(runtime(cpu), cls);
    if (obj == 0 || bezier(cpu, obj) == nullptr) {
        mt_objc_release(cpu, obj);
        return 0;
    }
    return mt_objc_autorelease(cpu, obj);
}

// +bezierPath
void bezier_path(mt_cpu *cpu) { mt_ret_u32(cpu, bezier_new(cpu, mt_objc_self(cpu))); }

// +bezierPathWithRoundedRect:(CGRect)rect cornerRadius:(CGFloat)radius
void bezier_rounded_rect(mt_cpu *cpu) {
    const u32 cls = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    Rect r;
    r.x = mt_arg_f32(&a);
    r.y = mt_arg_f32(&a);
    r.w = mt_arg_f32(&a);
    r.h = mt_arg_f32(&a);
    const float radius = mt_arg_f32(&a);
    const u32 obj = bezier_new(cpu, cls);
    if (BezierPath *p = obj != 0 ? bezier(cpu, obj) : nullptr) {
        p->path.add_rounded_rect(r, radius);
    }
    mt_ret_u32(cpu, obj);
}

Point read_point(mt_args *a) {
    Point p;
    p.x = mt_arg_f32(a);
    p.y = mt_arg_f32(a);
    return p;
}

void bezier_move_to(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const Point p = read_point(&a);
    if (BezierPath *b = bezier_mutable(cpu, self)) {
        b->path.move_to(p);
    }
}

void bezier_line_to(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const Point p = read_point(&a);
    if (BezierPath *b = bezier_mutable(cpu, self)) {
        b->path.line_to(p);
    }
}

void bezier_close(mt_cpu *cpu) {
    if (BezierPath *b = bezier_mutable(cpu, mt_objc_self(cpu))) {
        b->path.close();
    }
}

void bezier_set_line_width(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const float w = mt_arg_f32(&a);
    if (BezierPath *b = bezier(cpu, self)) {
        b->line_width = w;
    }
}

// -fill: the current context's fill colour, nonzero rule.
void bezier_fill(mt_cpu *cpu) {
    const BezierPath *b = bezier(cpu, mt_objc_self(cpu));
    Canvas *canvas = nullptr;
    Surface s;
    if (b != nullptr && current_canvas(cpu, "-[UIBezierPath fill]", &canvas, &s)) {
        canvas->fill_path(s, b->path);
    }
}

// -stroke: the current context's stroke colour at the path's line width.
void bezier_stroke(mt_cpu *cpu) {
    const BezierPath *b = bezier(cpu, mt_objc_self(cpu));
    Canvas *canvas = nullptr;
    Surface s;
    if (b != nullptr && current_canvas(cpu, "-[UIBezierPath stroke]", &canvas, &s)) {
        const float saved = canvas->state().line_width;
        canvas->state().line_width = b->line_width;
        canvas->stroke_path(s, b->path);
        canvas->state().line_width = saved;
    }
}

// -CGPath: owned by the UIBezierPath.
void bezier_cgpath(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    BezierPath *b = bezier(cpu, self);
    if (b == nullptr) {
        mt_ret_u32(cpu, 0);
        return;
    }
    if (b->cgpath == 0) {
        b->cgpath = path_create(cpu, b->path);
    }
    mt_ret_u32(cpu, b->cgpath);
}

const mt_objc_host_method k_bezier_class_methods[] = {
    {"bezierPath", bezier_path},
    {"bezierPathWithRoundedRect:cornerRadius:", bezier_rounded_rect},
};
const mt_objc_host_method k_bezier_methods[] = {
    {"moveToPoint:", bezier_move_to},
    {"addLineToPoint:", bezier_line_to},
    {"closePath", bezier_close},
    {"setLineWidth:", bezier_set_line_width},
    {"fill", bezier_fill},
    {"stroke", bezier_stroke},
    {"CGPath", bezier_cgpath},
};

} // namespace

// --- shared --------------------------------------------------------------------------------------

void uikit_forget() {
    // The previous runtime's objects are gone with it: nothing to release.
    g_uikit.stack.clear();
    g_uikit.named.clear();
    g_uikit.display.clear();
}

u32 current_context(mt_cpu *cpu) {
    const Entry *e = top(cpu);
    return e != nullptr ? e->context : 0u;
}

u32 uiimage_cgimage(mt_cpu *cpu, u32 uiimage) {
    const ImageState *s = image_state(cpu, uiimage);
    return s != nullptr ? s->cgimage : 0u;
}

u32 uiimage_display_cgimage(mt_cpu *cpu, u32 uiimage) {
    const ImageState *s = image_state(cpu, uiimage);
    return s == nullptr ? 0u : s->display != 0 ? s->display : s->cgimage;
}

u32 uiimage_create(mt_cpu *cpu, u32 cgimage) {
    return image_new(cpu, mt_objc_class_named(runtime(cpu), "UIImage"), cgimage, 1);
}

bool uiimage_size(mt_cpu *cpu, u32 uiimage, double *w, double *h) {
    const ImageState *s = image_state(cpu, uiimage);
    const ImageInfo *info = s != nullptr ? image_info(cpu, s->cgimage) : nullptr;
    if (info == nullptr || !(s->scale > 0)) {
        return false;
    }
    *w = info->width / static_cast<double>(s->scale);
    *h = info->height / static_cast<double>(s->scale);
    return true;
}

u32 push_view_context(mt_cpu *cpu, u32 w, u32 h, double x, double y) {
    u32 ctx = 0;
    if (w >= 1 && h >= 1 && w <= 0x4000 && h <= 0x4000) {
        const u32 space = color_space_create(cpu);
        ctx = context_create(cpu, 0, w, h, 8, w * 4, space,
                             k_alpha_premultiplied_last | k_byte_order_32_big);
        mt_objc_release(cpu, space);
        Canvas *canvas = nullptr;
        Surface s;
        if (context_canvas(cpu, ctx, &canvas, &s)) {
            canvas->state().ctm =
                Affine::translate(-x, -y).then(Affine::scale(1, -1)).then(Affine::translate(0, h));
        }
    }
    push(cpu, {ctx, false, 1});
    return ctx;
}

void pop_view_context(mt_cpu *cpu, u32 ctx) {
    const Entry *e = top(cpu);
    if (e == nullptr || e->context != ctx) {
        log(cpu, "UIKit's view context 0x%08X is not on top of the context stack: not popped",
            static_cast<unsigned>(ctx));
        return;
    }
    pop(cpu, "pop_view_context");
}

const mt_objc_host_class k_uiimage = {"UIImage",
                                      "NSObject",
                                      4,
                                      k_image_methods,
                                      sizeof k_image_methods / sizeof k_image_methods[0],
                                      k_image_class_methods,
                                      sizeof k_image_class_methods /
                                          sizeof k_image_class_methods[0]};
const mt_objc_host_class k_uicolor = {"UIColor",
                                      "NSObject",
                                      4,
                                      k_color_methods,
                                      sizeof k_color_methods / sizeof k_color_methods[0],
                                      k_color_class_methods,
                                      sizeof k_color_class_methods /
                                          sizeof k_color_class_methods[0]};
const mt_objc_host_class k_uibezierpath = {"UIBezierPath",
                                           "NSObject",
                                           4,
                                           k_bezier_methods,
                                           sizeof k_bezier_methods / sizeof k_bezier_methods[0],
                                           k_bezier_class_methods,
                                           sizeof k_bezier_class_methods /
                                               sizeof k_bezier_class_methods[0]};

} // namespace mt_cg

// --- HLE: UIKit's C functions --------------------------------------------------------------------

using namespace mt_cg;

// void UIGraphicsBeginImageContext(CGSize size)
extern "C" void mt_hle_UIGraphicsBeginImageContext(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const float w = mt_arg_f32(&a), h = mt_arg_f32(&a);
    begin_image_context(cpu, w, h, false, 1);
}

// void UIGraphicsBeginImageContextWithOptions(CGSize size, BOOL opaque, CGFloat scale)
extern "C" void mt_hle_UIGraphicsBeginImageContextWithOptions(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const float w = mt_arg_f32(&a), h = mt_arg_f32(&a);
    const bool opaque = (mt_arg_u32(&a) & 0xFFu) != 0;
    const float scale = mt_arg_f32(&a);
    begin_image_context(cpu, w, h, opaque, scale);
}

// void UIGraphicsEndImageContext(void): pops the image context.
extern "C" void mt_hle_UIGraphicsEndImageContext(mt_cpu *cpu, std::uint32_t) noexcept {
    const Entry *e = top(cpu);
    if (e != nullptr && !e->image) {
        log_once(cpu, "UIGraphicsEndImageContext.notimage",
                 "UIGraphicsEndImageContext: the current context is no image context (called "
                 "from 0x%08X): not popped",
                 static_cast<unsigned>(cpu->r[MT_LR]));
        return;
    }
    pop(cpu, "UIGraphicsEndImageContext");
}

// CGContextRef UIGraphicsGetCurrentContext(void): not retained.
extern "C" void mt_hle_UIGraphicsGetCurrentContext(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, current_context(cpu));
}

// UIImage *UIGraphicsGetImageFromCurrentImageContext(void): autoreleased; nil unless the
// current context is an image context.
extern "C" void mt_hle_UIGraphicsGetImageFromCurrentImageContext(mt_cpu *cpu,
                                                                 std::uint32_t) noexcept {
    const Entry *e = top(cpu);
    Canvas *canvas = nullptr;
    Surface s;
    if (e == nullptr || !e->image || !context_canvas(cpu, e->context, &canvas, &s)) {
        mt_ret_u32(cpu, 0);
        return;
    }
    const float scale = e->scale;
    const u32 row = s.width * 4;
    auto pixels = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(row) * s.height);
    for (u32 y = 0; y < s.height; y++) {
        std::memcpy(pixels->data() + static_cast<std::size_t>(y) * row,
                    s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row, row);
    }
    const u32 info = context_bitmap_info(cpu, e->context);
    const u32 cgimage = image_create_host(cpu, s.width, s.height, 32, info, std::move(pixels));
    const u32 image = image_new(cpu, mt_objc_class_named(runtime(cpu), "UIImage"), cgimage, scale);
    mt_objc_release(cpu, cgimage);
    mt_ret_u32(cpu, image);
}

// void UIGraphicsPushContext(CGContextRef context)
extern "C" void mt_hle_UIGraphicsPushContext(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 ctx = cpu->r[0];
    Canvas *canvas = nullptr;
    Surface s;
    if (!context_canvas(cpu, ctx, &canvas, &s)) {
        log_once(cpu, "UIGraphicsPushContext.invalid",
                 "UIGraphicsPushContext: 0x%08X is no context (called from 0x%08X): pushed anyway",
                 static_cast<unsigned>(ctx), static_cast<unsigned>(cpu->r[MT_LR]));
    }
    push(cpu, {mt_objc_retain(cpu, ctx), false, 1});
}

// void UIGraphicsPopContext(void)
extern "C" void mt_hle_UIGraphicsPopContext(mt_cpu *cpu, std::uint32_t) noexcept {
    pop(cpu, "UIGraphicsPopContext");
}

// NSData *UIImagePNGRepresentation(UIImage *image): autoreleased; RGB if the image has no
// alpha, else RGBA (straight).
extern "C" void mt_hle_UIImagePNGRepresentation(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 uiimage = cpu->r[0];
    const u32 cgimage = uiimage_cgimage(cpu, uiimage);
    const ImageInfo *info = image_info(cpu, cgimage);
    std::vector<u8> rgba;
    if (info == nullptr || !image_rgba(cpu, cgimage, rgba)) {
        log(cpu, "UIImagePNGRepresentation: 0x%08X has no readable image: nil",
            static_cast<unsigned>(uiimage));
        mt_ret_u32(cpu, 0);
        return;
    }
    const u32 alpha = info->bitmap_info & k_alpha_info_mask;
    const bool has_alpha = alpha != k_alpha_none && alpha != k_alpha_none_skip_last &&
                           alpha != k_alpha_none_skip_first;
    u32 channels = 4;
    if (!has_alpha) {
        channels = 3;
        for (std::size_t i = 0, n = rgba.size() / 4; i < n; i++) {
            std::memmove(&rgba[i * 3], &rgba[i * 4], 3);
        }
    }
    std::vector<u8> png;
    if (!png_encode(rgba.data(), info->width, info->height, channels, png) ||
        png.size() > 0xFFFFFFFFu) {
        log(cpu, "UIImagePNGRepresentation: encoding failed: nil");
        mt_ret_u32(cpu, 0);
        return;
    }
    const u32 data = mt_cf::data_create(cpu, png.data(), static_cast<u32>(png.size()), false);
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, data));
}

// void UIRectFill(CGRect rect): the current context's fill colour, kCGBlendModeCopy.
extern "C" void mt_hle_UIRectFill(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    Rect r;
    r.x = mt_arg_f32(&a);
    r.y = mt_arg_f32(&a);
    r.w = mt_arg_f32(&a);
    r.h = mt_arg_f32(&a);
    Canvas *canvas = nullptr;
    Surface s;
    if (current_canvas(cpu, "UIRectFill", &canvas, &s)) {
        canvas->fill_rect(s, r, k_blend_copy);
    }
}

extern "C" void mt_cg_set_image_replace_hook(mt_cg_image_replace_fn hook, void *user) noexcept {
    const std::uint64_t generation = mt_cg::g_image_hook.generation + 1;
    mt_cg::g_image_hook = mt_cg::ImageHook{hook, user, {}, generation};
}

extern "C" mt_cg_image_stats mt_cg_get_image_stats(void) noexcept {
    return mt_cg::g_image_hook.stats;
}
