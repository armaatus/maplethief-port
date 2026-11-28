// Text: the bundled font, UIFont, and NSString's UIKit drawing selectors (#51, ADR 0036).
//
// iOS drew the game's text with MarkerFelt-Thin (TextCache_DrawText 0x9031, the engine's text
// textures, and its measurement FUN_000072f0), Helvetica and Helvetica-Bold (the shell's
// labels), falling back to AppleGothic for Hangul. None of these may be shipped. One
// open-licensed font, NanumGothic Regular (SIL OFL 1.1; Latin, Hangul and the KS X 1001
// symbols the game's strings use, e.g. the arrows of "CDataMgr.drawControlInfo"), stands in for
// all of them, embedded into the runtime by cmake/Dependencies.cmake. Bold faces are
// synthesized by widening the glyph coverage. Glyph outlines are rasterized by stb_truetype
// (pinned with stb, ADR 0003), then composited by the Canvas like any other coverage.
//
// A character NanumGothic has no glyph for (none of the game's own: tests/test_hle_cg_real.py)
// is drawn with the fallback font, Fira Sans Regular (OFL 1.1; Latin-1, Latin Extended, Greek,
// Cyrillic), at the same size on the same baseline: a text pack's "Pötion" (#189, ADR 0108).
// The line's metrics stay NanumGothic's, and kerning applies only between two characters of
// the same font, so text without such characters is drawn exactly as before.
//
// UIKit's single-line text API as the game uses it:
//   -[NSString drawAtPoint:withFont:]  draws at the current UIGraphics context (TextCache
//                                      pushes its bitmap context), baseline at y + ascender,
//                                      in the context's fill colour; returns the size
//   -[NSString sizeWithFont:]          the same size without drawing
// Sizes are rounded up to whole points, as UIKit's were on iOS 4.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_objects.h"
#include "cg/cg_snapshot.h"
#include "maplethief/cg.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/textpack.h"
#include "textpack/textpack_internal.h"

#define STBTT_STATIC
// clang-tidy sees the declarations only: the third-party bodies are not ours to check.
#if !defined(__clang_analyzer__)
#define STB_TRUETYPE_IMPLEMENTATION
#endif
#include <stb_truetype.h>

// The font files' bytes, generated from the pinned downloads (cmake/Dependencies.cmake).
extern const unsigned char mt_cg_font_data[];
extern const std::size_t mt_cg_font_size;
extern const unsigned char mt_cg_fallback_font_data[];
extern const std::size_t mt_cg_fallback_font_size;

namespace mt_cg {

namespace {

// --- the font ----------------------------------------------------------------------------------

struct FontFile {
    bool tried = false, ok = false;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, line_gap = 0;
};
FontFile g_font, g_fallback;

const FontFile *load(FontFile &f, const unsigned char *data, std::size_t size) {
    if (!f.tried) {
        f.tried = true;
        f.ok = size > 0 && stbtt_InitFont(&f.info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
        if (f.ok) {
            stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.line_gap);
        }
    }
    return f.ok ? &f : nullptr;
}

const FontFile *font_file() { return load(g_font, mt_cg_font_data, mt_cg_font_size); }

// The font that draws `cp`: NanumGothic if it has a glyph for it, else the fallback if that
// has one, else NanumGothic (its missing glyph: nothing is drawn, the advance is its own).
// Fonts are loaded once and live as long as the process, so the reference never dangles.
// NOLINTBEGIN(bugprone-return-const-ref-from-parameter)
const FontFile &face_for(const FontFile &primary, int cp) {
    if (stbtt_FindGlyphIndex(&primary.info, cp) != 0) {
        return primary;
    }
    const FontFile *fb = load(g_fallback, mt_cg_fallback_font_data, mt_cg_fallback_font_size);
    return fb != nullptr && stbtt_FindGlyphIndex(&fb->info, cp) != 0 ? *fb : primary;
}
// NOLINTEND(bugprone-return-const-ref-from-parameter)

// Font units -> points at size `size`.
float units_to_points(const FontFile &f, float size) {
    return stbtt_ScaleForMappingEmToPixels(&f.info, size);
}

// UTF-16 -> code points (an unpaired surrogate becomes U+FFFD).
std::vector<int> code_points(const std::u16string &s) {
    std::vector<int> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        const char16_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            out.push_back(0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Line breaks and other controls have no glyph and no advance in a single-line draw.
bool is_control(int cp) { return cp < 0x20 || (cp >= 0x7F && cp < 0xA0); }

// The pen advance of each code point at `size` (kerning with the next one included, when both
// are drawn with the same font), each in its own font (face_for).
std::vector<float> advances(const FontFile &f, const std::vector<int> &cps, float size) {
    std::vector<float> out(cps.size(), 0.0f);
    for (std::size_t i = 0; i < cps.size(); i++) {
        if (is_control(cps[i])) {
            continue;
        }
        const FontFile &face = face_for(f, cps[i]);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&face.info, cps[i], &adv, &lsb);
        int kern = 0;
        if (i + 1 < cps.size() && !is_control(cps[i + 1]) && &face_for(f, cps[i + 1]) == &face) {
            kern = stbtt_GetCodepointKernAdvance(&face.info, cps[i], cps[i + 1]);
        }
        out[i] = static_cast<float>(adv + kern) * units_to_points(face, size);
    }
    return out;
}

// --- UIFont ------------------------------------------------------------------------------------

struct FontState {
    Font font;
};
const mt_objc_state_type k_font_state = {"UIFont", sizeof(FontState), nullptr, nullptr};

// The iOS 4 font names the port knows (the game's three, the system fonts, Hangul's
// AppleGothic, and close relatives); `bold` faces are synthesized. +fontWithName:size: returns
// nil for any other name, as UIKit did for fonts the device lacked.
const struct {
    const char *name;
    bool bold;
} k_font_names[] = {
    {"MarkerFelt-Thin", false}, {"MarkerFelt-Wide", true},    {"Helvetica", false},
    {"Helvetica-Bold", true},   {"Helvetica-Oblique", false}, {"Helvetica-BoldOblique", true},
    {"HelveticaNeue", false},   {"HelveticaNeue-Bold", true}, {"AppleGothic", false},
    {"ArialMT", false},         {"Arial-BoldMT", true},
};

// A new autoreleased UIFont, or 0.
u32 font_create(mt_cpu *cpu, const Font &f) {
    mt_objc *rt = runtime(cpu);
    const u32 obj = mt_objc_create_instance(rt, mt_objc_class_named(rt, "UIFont"));
    auto *s = obj != 0 ? static_cast<FontState *>(mt_objc_state(rt, obj, &k_font_state)) : nullptr;
    if (s == nullptr) {
        mt_objc_release(cpu, obj);
        return 0;
    }
    s->font = f;
    return mt_objc_autorelease(cpu, obj);
}

// The UTF-8 text of an NSString (for names and logs).
std::string utf8_of(mt_cpu *cpu, u32 str) {
    const std::u16string *chars = mt_cf::string_chars(cpu, str);
    return chars != nullptr ? mt_cf::to_utf8(*chars) : std::string();
}

// +[UIFont fontWithName:(NSString *)name size:(CGFloat)size]
void font_with_name_size(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 name = mt_arg_u32(&a);
    const float size = mt_arg_f32(&a);
    const std::string n = utf8_of(cpu, name);
    for (const auto &k : k_font_names) {
        if (n == k.name) {
            mt_ret_u32(cpu, font_create(cpu, {k.bold, size}));
            return;
        }
    }
    log_once(cpu, "UIFont.fontWithName",
             "+[UIFont fontWithName:@\"%s\" size:%g]: no such font: nil", n.c_str(),
             static_cast<double>(size));
    mt_ret_u32(cpu, 0);
}

// +[UIFont boldSystemFontOfSize:(CGFloat)size]: Helvetica-Bold, iOS 4's bold system font.
void font_bold_system(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const float size = mt_arg_f32(&a);
    mt_ret_u32(cpu, font_create(cpu, {true, size}));
}

// -[UIFont fontWithSize:(CGFloat)size]
void font_with_size(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const float size = mt_arg_f32(&a);
    Font f;
    font_of(cpu, self, &f);
    f.size = size;
    mt_ret_u32(cpu, font_create(cpu, f));
}

const mt_objc_host_method k_font_class_methods[] = {
    {"fontWithName:size:", font_with_name_size},
    {"boldSystemFontOfSize:", font_bold_system},
};
const mt_objc_host_method k_font_methods[] = {
    {"fontWithSize:", font_with_size},
};

// --- NSString (UIStringDrawing) ------------------------------------------------------------------

std::u16string text_of(mt_cpu *cpu, u32 str) {
    if (const std::u16string *chars = mt_cf::string_chars(cpu, str)) {
        return *chars;
    }
    // A string of another class: its UTF-8 (Foundation, #49).
    const u32 utf8 = mt_objc_send0(cpu, str, "UTF8String");
    std::u16string out;
    if (utf8 != 0) {
        const char *s = mt_g2h_str(cpu, utf8);
        mt_cf::decode(reinterpret_cast<const std::uint8_t *>(s), std::strlen(s), mt_cf::k_enc_utf8,
                      out);
    }
    return out;
}

// The draw observer (mt_cg_set_text_observer).
mt_cg_text_observer_fn g_text_observer = nullptr;
void *g_text_observer_user = nullptr;

// Tells the observer about a text drawn at x `at_x`, `w` points wide, into a bitmap `limit`
// points wide.
void observe_text(const std::u16string &text, double at_x, float w, double limit) {
    const std::string utf8 = mt_cf::to_utf8(text);
    mt_cg_drawn_text t{};
    t.utf8 = utf8.c_str();
    t.right = static_cast<float>(at_x) + w;
    t.limit = static_cast<float>(limit);
    for (const int cp : code_points(text)) {
        if (cp == '\n') {
            t.newlines++;
        } else if (!is_control(cp) && (cp == 0xFFFD || !font_has_glyph(cp))) {
            t.missing++;
        }
    }
    g_text_observer(g_text_observer_user, &t);
}

void return_size(mt_cpu *cpu, u32 result, float w, float h) {
    const float size[2] = {w, h};
    mt_ret_struct(cpu, result, size, sizeof size);
}

// -[NSString drawAtPoint:(CGPoint)point withFont:(UIFont *)font] -> CGSize (stret)
void string_draw_at_point(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    const Point at{mt_arg_f32(&a), mt_arg_f32(&a)};
    const u32 uifont = mt_arg_u32(&a);
    Font f;
    if (!font_of(cpu, uifont, &f)) {
        log_once(cpu, "NSString.drawAtPoint.font",
                 "-[NSString drawAtPoint:withFont:]: no font (0x%08X): nothing drawn",
                 static_cast<unsigned>(uifont));
        return_size(cpu, result, 0, 0);
        return;
    }
    const std::u16string text = text_of(cpu, self);
    Canvas *canvas = nullptr;
    Surface s;
    float w = 0, h = 0;
    const u32 ctx = current_context(cpu);
    if (context_canvas(cpu, ctx, &canvas, &s)) {
        draw_context_text(cpu, ctx, *canvas, s, text, f, at, &w, &h);
        // A text pack's text that does not fit the bitmap (TextCache's 512 points) is logged.
        const double ds = canvas->device_scale();
        if (ds > 0 && mt_textpack_is_open() != 0) {
            const std::string utf8 = mt_cf::to_utf8(text);
            mt_textpack::note_drawn(utf8);
            mt_textpack::note_overflow(utf8, static_cast<float>(at.x) + w,
                                       static_cast<float>(s.width / ds));
        }
        if (ds > 0 && g_text_observer != nullptr) {
            observe_text(text, at.x, w, s.width / ds);
        }
    } else {
        log_once(cpu, "NSString.drawAtPoint.context",
                 "-[NSString drawAtPoint:withFont:]: no current context: nothing drawn");
        text_size(text, f, &w, &h);
    }
    return_size(cpu, result, w, h);
}

// -[NSString sizeWithFont:(UIFont *)font] -> CGSize (stret)
void string_size_with_font(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    const u32 uifont = mt_arg_u32(&a);
    Font f;
    float w = 0, h = 0;
    if (font_of(cpu, uifont, &f)) {
        text_size(text_of(cpu, self), f, &w, &h);
    }
    return_size(cpu, result, w, h);
}

const mt_objc_host_method k_string_drawing_methods[] = {
    {"drawAtPoint:withFont:", string_draw_at_point},
    {"sizeWithFont:", string_size_with_font},
};

} // namespace

void text_forget() {}

FontMetrics font_metrics(const Font &f) {
    FontMetrics m;
    const FontFile *ff = font_file();
    if (ff == nullptr) {
        return m;
    }
    const float scale = units_to_points(*ff, f.size);
    m.ascender = static_cast<float>(ff->ascent) * scale;
    m.descender = static_cast<float>(ff->descent) * scale;
    m.line_height = static_cast<float>(ff->ascent - ff->descent + ff->line_gap) * scale;
    return m;
}

float text_width(const std::u16string &text, const Font &f) {
    const FontFile *ff = font_file();
    if (ff == nullptr) {
        return 0;
    }
    float w = 0;
    for (const float adv : advances(*ff, code_points(text), f.size)) {
        w += adv;
    }
    return w;
}

void text_size(const std::u16string &text, const Font &f, float *width, float *height) {
    *width = std::ceil(text_width(text, f) - 1e-4f);
    *height = std::ceil(font_metrics(f).line_height - 1e-4f);
}

void draw_text(Canvas &canvas, Surface &s, const std::u16string &text, const Font &f, Point at,
               float *width, float *height) {
    text_size(text, f, width, height);
    const FontFile *ff = font_file();
    if (ff == nullptr || canvas.state().text_mode == k_text_invisible) {
        return;
    }
    const std::vector<int> cps = code_points(text);
    const std::vector<float> adv = advances(*ff, cps, f.size);
    const double ds = canvas.device_scale();
    const int embolden = f.bold ? std::max(1, static_cast<int>(std::lround(f.size * ds / 14))) : 0;
    const Affine m = canvas.user_to_raster();
    Affine inv;
    // On an axis-aligned CTM, glyphs are rendered at their subpixel x offset and placed on
    // whole device pixels (baselines rounded), so they are copied, not resampled.
    const bool aligned = std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9 && m.a > 0 &&
                         std::fabs(std::fabs(m.d) - m.a) < 1e-9 && m.invert(&inv);
    double pen = at.x;
    const double baseline = at.y + font_metrics(f).ascender;
    for (std::size_t i = 0; i < cps.size(); i++) {
        const int cp = cps[i];
        const FontFile &face = face_for(*ff, cp);
        const float scale = units_to_points(face, f.size) * static_cast<float>(ds);
        const int glyph = is_control(cp) ? 0 : stbtt_FindGlyphIndex(&face.info, cp);
        if (glyph != 0 && stbtt_IsGlyphEmpty(&face.info, glyph) == 0) {
            Point origin{pen, baseline};
            float shift = 0;
            if (aligned) {
                const Point r = m.apply(origin);
                const double rx = std::floor(r.x);
                shift = static_cast<float>(r.x - rx);
                origin = inv.apply({rx, std::round(r.y)});
            }
            int gw = 0, gh = 0, x0 = 0, y0 = 0;
            unsigned char *bitmap = stbtt_GetGlyphBitmapSubpixel(&face.info, scale, scale, shift,
                                                                 0.0f, glyph, &gw, &gh, &x0, &y0);
            if (bitmap != nullptr) {
                canvas.draw_glyph(s, origin, bitmap, gw, gh, x0, y0, ds, embolden);
                stbtt_FreeBitmap(bitmap, nullptr);
            }
        }
        pen += adv[i];
    }
}

bool font_has_glyph(int cp) {
    const FontFile *ff = font_file();
    return ff != nullptr && stbtt_FindGlyphIndex(&face_for(*ff, cp).info, cp) != 0;
}

bool font_of(mt_cpu *cpu, u32 uifont, Font *out) {
    mt_objc *rt = runtime(cpu);
    const auto *s =
        rt != nullptr && uifont != 0
            ? static_cast<const FontState *>(mt_objc_find_state(rt, uifont, &k_font_state))
            : nullptr;
    if (s != nullptr) {
        *out = s->font;
    }
    return s != nullptr;
}

const mt_objc_host_class k_uifont = {"UIFont",
                                     "NSObject",
                                     4,
                                     k_font_methods,
                                     sizeof k_font_methods / sizeof k_font_methods[0],
                                     k_font_class_methods,
                                     sizeof k_font_class_methods / sizeof k_font_class_methods[0]};

const mt_objc_host_category k_nsstring_drawing = {
    "NSString", k_string_drawing_methods,
    sizeof k_string_drawing_methods / sizeof k_string_drawing_methods[0], nullptr, 0};

// --- save states (#203, ADR 0105) ---------------------------------------------------------------

namespace {

bool save_font(mt_cpu *, u32, const void *state, mt_snap::Writer &w) {
    snap::put(w, static_cast<const FontState *>(state)->font);
    return true;
}
bool load_font(mt_cpu *, u32, void *state, mt_snap::Reader &r) {
    return snap::get(r, &static_cast<FontState *>(state)->font);
}

} // namespace

extern const mt_snap::StateOps k_snap_font;
const mt_snap::StateOps k_snap_font = {&k_font_state, "UIFont", save_font, load_font, nullptr};

} // namespace mt_cg

extern "C" void mt_cg_set_text_observer(mt_cg_text_observer_fn fn, void *user) noexcept {
    mt_cg::g_text_observer = fn;
    mt_cg::g_text_observer_user = user;
}

// The bundled fonts for the frontend's overlays (#372, ADR 0206): their own FontFiles, behind a
// lock, so the frontend's thread never touches the game's.
namespace {

std::mutex g_bundled_lock;
mt_cg::FontFile g_bundled_font, g_bundled_fallback;

// The face and glyph that draw `c`: NanumGothic, else the fallback; glyph 0 if neither has one.
// Under g_bundled_lock.
const mt_cg::FontFile *bundled_face(int c, int *glyph) {
    const mt_cg::FontFile *face = mt_cg::load(g_bundled_font, mt_cg_font_data, mt_cg_font_size);
    *glyph = face != nullptr ? stbtt_FindGlyphIndex(&face->info, c) : 0;
    if (*glyph == 0) {
        const mt_cg::FontFile *fb =
            mt_cg::load(g_bundled_fallback, mt_cg_fallback_font_data, mt_cg_fallback_font_size);
        const int fg = fb != nullptr ? stbtt_FindGlyphIndex(&fb->info, c) : 0;
        if (fg != 0) {
            *glyph = fg;
            return fb;
        }
    }
    return face;
}

// The next code point of `s` from `*i` (advanced past it): an invalid or truncated sequence, an
// overlong form or a surrogate is U+FFFD, one byte consumed.
int next_code_point(const unsigned char *s, std::size_t n, std::size_t *i) {
    const unsigned char b = s[*i];
    int len = 0, cp = 0, min = 0;
    if (b < 0x80) {
        *i += 1;
        return b;
    }
    if ((b & 0xE0) == 0xC0) {
        len = 2;
        cp = b & 0x1F;
        min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        len = 3;
        cp = b & 0x0F;
        min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        len = 4;
        cp = b & 0x07;
        min = 0x10000;
    }
    if (len == 0 || *i + static_cast<std::size_t>(len) > n) {
        *i += 1;
        return 0xFFFD;
    }
    for (int k = 1; k < len; k++) {
        const unsigned char c = s[*i + static_cast<std::size_t>(k)];
        if ((c & 0xC0) != 0x80) {
            *i += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
        *i += 1;
        return 0xFFFD;
    }
    *i += static_cast<std::size_t>(len);
    return cp;
}

} // namespace

extern "C" int mt_cg_bundled_glyph(uint32_t cp, int32_t em_pixels, int32_t max_width,
                                   mt_cg_glyph *g, uint8_t *coverage, size_t capacity) noexcept {
    *g = mt_cg_glyph{};
    const int c = static_cast<int>(std::min<uint32_t>(cp, 0x10FFFF));
    if (mt_cg::is_control(c) || em_pixels <= 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> hold(g_bundled_lock);
    int glyph = 0;
    const mt_cg::FontFile *face = bundled_face(c, &glyph);
    if (glyph == 0) {
        return 0;
    }
    const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, static_cast<float>(em_pixels));
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&face->info, glyph, &adv, &lsb);
    g->advance = static_cast<int32_t>(std::lround(static_cast<float>(adv) * scale));
    if (stbtt_IsGlyphEmpty(&face->info, glyph) != 0) {
        return 1;
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&face->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    float sx = scale;
    if (max_width > 0 && x1 - x0 > max_width) {
        sx = scale * static_cast<float>(max_width) / static_cast<float>(x1 - x0);
        stbtt_GetGlyphBitmapBox(&face->info, glyph, sx, scale, &x0, &y0, &x1, &y1);
    }
    *g = {x1 - x0, y1 - y0, x0, y0, g->advance};
    const std::size_t need =
        static_cast<std::size_t>(g->width) * static_cast<std::size_t>(g->height);
    if (coverage != nullptr && capacity >= need && need > 0) {
        stbtt_MakeGlyphBitmap(&face->info, coverage, g->width, g->height, g->width, sx, scale,
                              glyph);
    }
    return 1;
}

extern "C" int mt_cg_bundled_line(const char *utf8, size_t size, int32_t em_pixels,
                                  int32_t max_width, const uint8_t color[4], mt_cg_line *line,
                                  uint8_t *rgba, size_t capacity) noexcept {
    *line = mt_cg_line{};
    if (utf8 == nullptr || size == 0 || em_pixels < 1 || em_pixels > 256) {
        return 0;
    }
    const int32_t limit = std::clamp<int32_t>(max_width, 1, 4096);
    const std::lock_guard<std::mutex> hold(g_bundled_lock);
    const mt_cg::FontFile *primary = mt_cg::load(g_bundled_font, mt_cg_font_data, mt_cg_font_size);
    if (primary == nullptr) {
        return 0;
    }
    // The line's metrics are NanumGothic's, as the game's text (draw_text).
    const float em = static_cast<float>(em_pixels);
    const float ps = stbtt_ScaleForMappingEmToPixels(&primary->info, em);
    const int32_t ascent =
        static_cast<int32_t>(std::ceil(static_cast<float>(primary->ascent) * ps));
    const int32_t descent =
        static_cast<int32_t>(std::ceil(static_cast<float>(-primary->descent) * ps));
    struct Placed {
        const mt_cg::FontFile *face;
        int glyph;
        float scale;
        int32_t x;
    };
    std::vector<Placed> placed;
    const auto *s = reinterpret_cast<const unsigned char *>(utf8);
    float pen = 0;
    int32_t right = 0;
    for (std::size_t i = 0; i < size && pen < static_cast<float>(limit);) {
        const int cp = next_code_point(s, size, &i);
        if (mt_cg::is_control(cp)) {
            continue;
        }
        int glyph = 0;
        const mt_cg::FontFile *face = bundled_face(cp, &glyph);
        if (glyph == 0) {
            line->missing++;
            continue;
        }
        const float scale = stbtt_ScaleForMappingEmToPixels(&face->info, em);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&face->info, glyph, &adv, &lsb);
        const auto x = static_cast<int32_t>(std::lround(pen));
        if (stbtt_IsGlyphEmpty(&face->info, glyph) == 0) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&face->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
            right = std::max(right, x + x1);
            placed.push_back({face, glyph, scale, x});
        }
        pen += static_cast<float>(adv) * scale;
    }
    line->width = std::min(limit, std::max(right, static_cast<int32_t>(std::lround(pen))));
    line->height = ascent + descent;
    line->baseline = ascent;
    if (line->width <= 0 || line->height <= 0) {
        *line = mt_cg_line{};
        return 0;
    }
    const std::size_t w = static_cast<std::size_t>(line->width);
    const std::size_t h = static_cast<std::size_t>(line->height);
    if (rgba == nullptr || color == nullptr || capacity < w * h * 4) {
        return 1;
    }
    std::vector<uint8_t> cover(w * h, 0);
    std::vector<uint8_t> glyph_px;
    for (const Placed &p : placed) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&p.face->info, p.glyph, p.scale, p.scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) {
            continue;
        }
        glyph_px.assign(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0);
        stbtt_MakeGlyphBitmap(&p.face->info, glyph_px.data(), gw, gh, gw, p.scale, p.scale,
                              p.glyph);
        for (int gy = 0; gy < gh; gy++) {
            const int y = line->baseline + y0 + gy;
            if (y < 0 || y >= line->height) {
                continue;
            }
            for (int gx = 0; gx < gw; gx++) {
                const int x = p.x + x0 + gx;
                if (x < 0 || x >= line->width) {
                    continue;
                }
                uint8_t &c = cover[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
                c = std::max(c, glyph_px[static_cast<std::size_t>(gy) * gw + gx]);
            }
        }
    }
    for (std::size_t i = 0; i < w * h; i++) {
        uint8_t *px = rgba + i * 4;
        const bool ink = cover[i] != 0;
        px[0] = ink ? color[0] : 0;
        px[1] = ink ? color[1] : 0;
        px[2] = ink ? color[2] : 0;
        px[3] = static_cast<uint8_t>((cover[i] * color[3] + 127) / 255);
    }
    return 1;
}
