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
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "cg/cg_objects.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/textpack.h"
#include "textpack/textpack_internal.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
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
const FontFile &face_for(const FontFile &primary, int cp) {
    if (stbtt_FindGlyphIndex(&primary.info, cp) != 0) {
        return primary;
    }
    const FontFile *fb = load(g_fallback, mt_cg_fallback_font_data, mt_cg_fallback_font_size);
    return fb != nullptr && stbtt_FindGlyphIndex(&fb->info, cp) != 0 ? *fb : primary;
}

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
const mt_objc_state_type k_font_state = {"UIFont", sizeof(FontState), nullptr};

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
            mt_textpack::note_overflow(mt_cf::to_utf8(text), static_cast<float>(at.x) + w,
                                       static_cast<float>(s.width / ds));
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

} // namespace mt_cg
