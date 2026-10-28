// CoreGraphics and UIKit drawing (#51, ADR 0036): what the files of runtime/src/cg/ share, and
// what UIKit's views (#54) draw with.
//
//   raster.cpp   pixel formats, the software rasterizer (Canvas): CTM, graphics-state stack,
//                device clip mask, anti-aliased polygon coverage, fills, gradients, images
//   objects.cpp  the CG objects as guest CF objects (__NSCFType with host state, ADR 0032)
//                and the CoreGraphics HLE functions and data imports
//   codec.cpp    PNG decoding (CgBI included) and encoding with stb_image / stb_image_write
//   text.cpp     the bundled font (stb_truetype), UIFont, NSString's drawing selectors
//   uikit.cpp    UIGraphics' context stack, UIImage, UIColor, UIBezierPath, the UIKit C
//                functions
//
// Coordinates. A CG user space is mapped by the context's CTM to device space (pixels, origin
// bottom-left, y up: Quartz's convention); a Canvas works in raster space (origin top-left,
// y down, row r covering [r, r + 1)), so raster = (x, height - y) of device. UIKit draws as if
// user space were y-down (UIGraphicsBeginImageContext flips the CTM to make it so): text has
// its baseline at y + ascender and glyphs pointing to -y, images have their top row at the
// rect's minimum y. On a context whose CTM is not flipped (the game's TextCache_DrawText), that
// produces upside-down text, as on the device.
//
// Pixels. Contexts draw 8-bit premultiplied RGBA (Pixel) into 32-bit memory of any CG layout
// (Format: component order, byte order, skip byte). A bitmap context made with caller memory
// draws straight into guest memory and never touches row padding.
//
// Single guest thread (ADR 0033): no locks. Every function takes the guest thread's cpu and
// uses the current ObjC runtime.
#ifndef MAPLETHIEF_RUNTIME_CG_CG_OBJECTS_H
#define MAPLETHIEF_RUNTIME_CG_CG_OBJECTS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "maplethief/objc/host.h"

namespace mt_cg {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

// --- CGBitmapInfo (CGImage.h) ---------------------------------------------------------------

enum : u32 {
    k_alpha_none = 0,
    k_alpha_premultiplied_last = 1,
    k_alpha_premultiplied_first = 2,
    k_alpha_last = 3,
    k_alpha_first = 4,
    k_alpha_none_skip_last = 5,
    k_alpha_none_skip_first = 6,
    k_alpha_only = 7,
    k_alpha_info_mask = 0x1Fu,
    k_float_components = 0x100u,
    k_byte_order_mask = 0x7000u,
    k_byte_order_16_little = 0x1000u,
    k_byte_order_32_little = 0x2000u,
    k_byte_order_16_big = 0x3000u,
    k_byte_order_32_big = 0x4000u,
};

// CGBlendMode values the rasterizer implements; others draw as normal (logged once).
enum : u32 { k_blend_normal = 0, k_blend_clear = 16, k_blend_copy = 17 };
// CGTextDrawingMode: fill (0) and invisible (3) are drawn exactly; stroke modes fill (logged).
enum : u32 { k_text_fill = 0, k_text_invisible = 3 };

// The layout of one pixel in memory: byte offsets of the components (a = -1: no alpha byte;
// skip = -1: no skip byte), whether colour is premultiplied by alpha.
struct Format {
    u32 bytes_per_pixel = 4;
    int r = 0, g = 1, b = 2, a = 3, skip = -1;
    bool premultiplied = true;
};

// The format of 8-bit RGB memory described by a CGBitmapInfo (alpha info | byte order), bits
// per component and bits per pixel: 32-bit pixels with any alpha info and either 32-bit byte
// order, or 24-bit RGB without alpha. False for anything else (16-bit, float, alpha-only, grey).
bool format_of(u32 bitmap_info, u32 bpc, u32 bpp, Format *out);
// Whether CGBitmapContextCreate accepts the format: 32 bits, premultiplied or no alpha.
bool context_format_ok(u32 bitmap_info, u32 bpc, u32 bpp);

// A premultiplied 8-bit RGBA pixel.
struct Pixel {
    u8 r = 0, g = 0, b = 0, a = 0;
};
Pixel load_pixel(const Format &f, const u8 *p);
void store_pixel(const Format &f, u8 *p, Pixel px);

// A straight (not premultiplied) colour, components 0..1.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};
Pixel premultiply(const Color &c);

// --- geometry -----------------------------------------------------------------------------------

struct Point {
    double x = 0, y = 0;
};

// x' = a x + c y + tx, y' = b x + d y + ty (CGAffineTransform's fields).
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Point apply_vector(Point p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
    // this, then `next` (CGAffineTransformConcat(this, next)).
    Affine then(const Affine &next) const;
    bool invert(Affine *out) const;
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
};

// A path: subpaths of points (curves flattened when they are added), each closed or not.
struct Subpath {
    std::vector<Point> points;
    bool closed = false;
};
struct Path {
    std::vector<Subpath> subpaths;
    void move_to(Point p);
    void line_to(Point p);
    void close();
    // A rounded rectangle as UIBezierPath draws it: clockwise in a y-down space.
    void add_rounded_rect(const Rect &r, double radius);
    bool empty() const;
};

// --- images ------------------------------------------------------------------------------------

// Pixel memory of an image or a context, top row first.
struct Surface {
    u8 *pixels = nullptr;
    u32 width = 0, height = 0, bytes_per_row = 0;
    Format format;
};
struct ConstSurface {
    const u8 *pixels = nullptr;
    u32 width = 0, height = 0, bytes_per_row = 0;
    Format format;
};

// --- the rasterizer (raster.cpp) -----------------------------------------------------------

// The graphics state of a context (CGContextSaveGState pushes a copy).
struct GState {
    Affine ctm;
    Color fill{0, 0, 0, 1};
    Color stroke{0, 0, 0, 1};
    u32 blend = k_blend_normal;
    u32 text_mode = k_text_fill;
    float line_width = 1;
    // Coverage 0..255 per device pixel (raster order, width * height); null: no clip.
    std::shared_ptr<const std::vector<u8>> clip;
};

// The gradient of CGGradientCreateWithColors: colour stops in location order.
struct Gradient {
    struct Stop {
        float location;
        Color color;
    };
    std::vector<Stop> stops;
    Color at(float t) const;
};

enum : u32 { k_gradient_before_start = 1, k_gradient_after_end = 2 };

// What a CG bitmap context draws with: its size, its graphics state and the current path. The
// pixels are handed to each drawing call, because guest memory is addressed through the cpu.
class Canvas {
  public:
    Canvas(u32 width, u32 height);

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    GState &state() { return gs_; }
    const GState &state() const { return gs_; }
    void save();
    bool restore(); // false if the stack is empty

    // User space -> raster space under the current CTM.
    Affine user_to_raster() const;

    // The current path (raster space): CGContextAddPath, CGContextClip (nonzero rule).
    void add_path(const Path &user_path);
    void clip_to_path();
    void clear_path() { path_.subpaths.clear(); }

    // Fills the path `user_path` (user space) with the fill colour, nonzero rule. `blend`
    // overrides the state's blend mode when not UINT32_MAX.
    void fill_path(Surface &s, const Path &user_path, u32 blend = UINT32_MAX);
    // Strokes `user_path` with the stroke colour and line width: butt caps, no joins.
    void stroke_path(Surface &s, const Path &user_path);
    void fill_rect(Surface &s, const Rect &user_rect, u32 blend = UINT32_MAX);
    // CGContextDrawImage (`top_down` false: the image's top row at the rect's maximum y) or
    // -[UIImage drawInRect:] (`top_down` true: at its minimum y). Bilinear sampling, except
    // for whole-pixel 1:1 placements, which copy.
    void draw_image(Surface &s, const Rect &user_rect, const ConstSurface &image, bool top_down);
    void draw_linear_gradient(Surface &s, const Gradient &g, Point start, Point end, u32 options);
    void draw_radial_gradient(Surface &s, const Gradient &g, Point c0, double r0, Point c1,
                              double r1, u32 options);
    // One glyph's coverage bitmap (`cov`, gw x gh, one byte per pixel) whose pixel (0, 0) sits
    // at offset (x0, y0) device pixels from the pen at user point `pen`, in UIKit's y-down
    // orientation, at `device_scale` device pixels per user unit; `embolden` widens it by
    // that many pixels (synthetic bold).
    void draw_glyph(Surface &s, Point pen, const u8 *cov, int gw, int gh, int x0, int y0,
                    double device_scale, int embolden);
    // The device scale of the CTM: device pixels per user unit (sqrt |det|).
    double device_scale() const;

  private:
    u32 width_, height_;
    GState gs_;
    std::vector<GState> saved_;
    Path path_; // raster space
};

// --- guest objects (objects.cpp) ------------------------------------------------------------
// Every CG object is a guest __NSCFType instance (retain count 1 when made) whose contents are
// host state; CGxxxRelease(NULL) is allowed, CFRelease/CFRetain work on all of them.

// A CGColorSpace (only device RGB exists).
u32 color_space_create(mt_cpu *cpu);
bool is_color_space(mt_cpu *cpu, u32 obj);

// A CGColor: its components, and a guest copy of them for CGColorGetComponents.
u32 color_create(mt_cpu *cpu, const Color &c);
bool color_value(mt_cpu *cpu, u32 obj, Color *out);

// A CGDataProvider over a CFData (retained) or over host bytes.
u32 provider_create_data(mt_cpu *cpu, u32 cfdata);
u32 provider_create_bytes(mt_cpu *cpu, std::shared_ptr<const std::vector<u8>> bytes);
// The provider's bytes (a host pointer into guest memory for a CFData), or false.
bool provider_bytes(mt_cpu *cpu, u32 provider, const u8 **bytes, std::size_t *size);

// A CGImage. Its pixels are its provider's bytes.
struct ImageInfo {
    u32 width = 0, height = 0, bpc = 8, bpp = 32, bytes_per_row = 0;
    u32 bitmap_info = 0, intent = 0;
    bool interpolate = true;
    u32 space = 0;    // guest CGColorSpace, retained
    u32 provider = 0; // guest CGDataProvider, retained
};
// CGImageCreate: retains `space` and `provider`; 0 if the format is unsupported or the
// provider has too few bytes.
u32 image_create(mt_cpu *cpu, const ImageInfo &info);
// A new image (retain count 1) over host pixels in `format_info`/`bpp` layout.
u32 image_create_host(mt_cpu *cpu, u32 width, u32 height, u32 bpp, u32 bitmap_info,
                      std::shared_ptr<const std::vector<u8>> pixels);
const ImageInfo *image_info(mt_cpu *cpu, u32 image);
// The pixels of a CGImage, or false.
bool image_surface(mt_cpu *cpu, u32 image, ConstSurface *out);
// The image as straight (not premultiplied) RGBA, 4 bytes per pixel, top row first.
bool image_rgba(mt_cpu *cpu, u32 image, std::vector<u8> &rgba);

// A CGGradient.
u32 gradient_create(mt_cpu *cpu, Gradient g);
const Gradient *gradient_of(mt_cpu *cpu, u32 obj);

// A CGPath (immutable).
u32 path_create(mt_cpu *cpu, Path p);
const Path *path_of(mt_cpu *cpu, u32 obj);

// A CGBitmapContext. `data` 0: the context owns host memory (cleared), as for
// UIGraphicsBeginImageContext. 0 if the arguments are invalid (logged, like CG's own
// "CGBitmapContextCreate: unsupported parameter combination").
u32 context_create(mt_cpu *cpu, u32 data, u32 width, u32 height, u32 bpc, u32 bytes_per_row,
                   u32 space, u32 bitmap_info);
// The canvas and pixel memory of a context, or false if `obj` is not one.
bool context_canvas(mt_cpu *cpu, u32 obj, Canvas **canvas, Surface *surface);
// Bitmap info of a context (for snapshots).
u32 context_bitmap_info(mt_cpu *cpu, u32 obj);

// Logs one line "cg: ..." through the ObjC runtime's log sink.
void log(mt_cpu *cpu, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
// The same, but only the first time for each `key` (a string literal) in this runtime.
void log_once(mt_cpu *cpu, const char *key, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// The runtime the module's state belongs to: when the current ObjC runtime changes (a new
// process, a test's new runtime), the UIGraphics stack and the image cache are forgotten.
mt_objc *runtime(mt_cpu *cpu);
void uikit_forget(); // uikit.cpp
void text_forget();  // text.cpp

// --- PNG (codec.cpp) ---------------------------------------------------------------------------

// A decoded image: straight RGBA (alpha info kCGImageAlphaLast) when the file has any alpha
// (an alpha channel or tRNS), else RGBX with 0xFF in the skip byte
// (kCGImageAlphaNoneSkipLast), 8 bits per component, 32 bits per pixel, rows of 4 * width
// bytes: what ImageIO gave the game's texture loaders (FUN_00009534, FUN_0000d6d0) on iOS 4.
// Apple CgBI files are converted (BGRA -> RGBA, un-premultiplied), bit-identical to ImageIO.
struct Decoded {
    u32 width = 0, height = 0;
    bool has_alpha = false;
    std::shared_ptr<const std::vector<u8>> pixels;
};
bool png_decode(const u8 *data, std::size_t size, Decoded *out, std::string *error);
// A PNG of straight RGBA (4 channels) or RGB (3 channels) pixels, rows of channels * width.
bool png_encode(const u8 *pixels, u32 width, u32 height, u32 channels, std::vector<u8> &out);

// A new CGImage (retain count 1) from encoded PNG bytes, or 0 (with *error set).
u32 image_from_png(mt_cpu *cpu, const u8 *data, std::size_t size, std::string *error);

// --- text (text.cpp) ---------------------------------------------------------------------------

// A UIFont's face and size. The bundled font stands in for every face (ADR 0036); `bold` is
// synthesized.
struct Font {
    bool bold = false;
    float size = 12;
};
struct FontMetrics {
    float ascender = 0, descender = 0, line_height = 0; // points; descender < 0
};
FontMetrics font_metrics(const Font &f);
// The advance width of a single line (kerned), unrounded.
float text_width(const std::u16string &text, const Font &f);
// -sizeWithFont: (one line): the width and line height, rounded up to whole points as UIKit
// did on iOS 4.
void text_size(const std::u16string &text, const Font &f, float *width, float *height);
// -drawAtPoint:withFont: on a canvas: one line, baseline at y + ascender, in the state's fill
// colour, clip and blend mode. Returns the text's size (as text_size).
void draw_text(Canvas &canvas, Surface &s, const std::u16string &text, const Font &f, Point at,
               float *width, float *height);
// Whether the bundled font has a glyph for code point `cp` (tests: Latin and Hangul coverage).
bool font_has_glyph(int cp);
// The UIFont of a guest object, or false.
bool font_of(mt_cpu *cpu, u32 uifont, Font *out);

// --- UIKit (uikit.cpp) ---------------------------------------------------------------------

// The context on top of UIGraphics' stack, or 0.
u32 current_context(mt_cpu *cpu);
// The CGImage of a UIImage (not retained), or 0.
u32 uiimage_cgimage(mt_cpu *cpu, u32 uiimage);
// A new autoreleased UIImage over `cgimage` (retained by the UIImage), or 0.
u32 uiimage_create(mt_cpu *cpu, u32 cgimage);

// --- host classes (runtime/src/objc/classes.cpp lists them) --------------------------------

extern const mt_objc_host_class k_uiimage;
extern const mt_objc_host_class k_uifont;
extern const mt_objc_host_class k_uicolor;
extern const mt_objc_host_class k_uibezierpath;
// NSString's UIKit drawing selectors, a host category on #49's NSString.
extern const mt_objc_host_category k_nsstring_drawing;

} // namespace mt_cg

#endif // MAPLETHIEF_RUNTIME_CG_CG_OBJECTS_H
