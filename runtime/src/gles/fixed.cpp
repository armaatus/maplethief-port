// GL ES 1.1's fixed-function pipeline on mt_gfx (#56, ADR 0040, maplethief/gles.h): executes the
// commands the state tracker (gles.cpp) accepted.
//
//   matrices   three stacks with values (modelview 16 deep, projection 2, texture 2; the
//              tracker keeps the depths). glLoadIdentity, glPushMatrix (copies the top),
//              glOrthox, glTranslatex, glRotatex and glScalef multiply the top of the current
//              stack on the right, as GL ES 1.1 section 2.10.2 defines them. GLfixed arguments
//              are value / 65536. glRotatex's sine and cosine come from an exact reduction of
//              the angle in 16.16 degrees and a series in double: the same bits on every CPU
//              (ADR 0039's determinism; no libm on the way to a pixel), exact at multiples of
//              90 degrees.
//   arrays     glVertexPointer / glTexCoordPointer (size 2-4, GL_BYTE, GL_SHORT, GL_FIXED,
//              GL_FLOAT, stride, guest pointer) and the client states. At glDrawArrays the
//              elements first .. first + count - 1 are read from guest memory, after checking
//              that the whole range is mapped (else a logged misuse, the draw is skipped).
//   draw       positions by projection x modelview to clip space, texture coordinates by the
//              texture matrix (divided by q), then one mt_gfx_draw with the constant colour, the
//              bound texture (if GL_TEXTURE_2D is on and the texture is complete), the blend
//              state and the viewport, into the target of the bound framebuffer's colour
//              attachment.
//   state      glEnable/glDisable of every GL ES 1.1 cap (GL_TEXTURE_2D and GL_BLEND executed,
//              the rest kept; a draw with one that would change pixels here is logged once:
//              GL_CULL_FACE, GL_LIGHTING, GL_FOG, and GL_LINE_SMOOTH for a line),
//              glColor4x (clamped, (v * 255 + 32768) >> 16), glBlendFunc (GL ES 1.1's source
//              and destination lists), glTexParameteri and level-0 glTexImage2D onto the
//              texture's mt_gfx texture.
//
// Compiled without floating-point contraction (maplethief_strict_fp), like mt_gfx.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gles/gles_internal.h"
#include "maplethief/memory.h"

namespace mt_gles {
namespace {

// The host's draw filter (mt_gles_set_draw_filter, #159): guest thread only.
mt_gles_draw_filter g_draw_filter = nullptr;
void *g_draw_filter_user = nullptr;
// The host's clip-space x transform (mt_gles_set_host_x_transform, widescreen #86): identity.
float g_host_x_scale = 1.0f, g_host_x_offset = 0.0f;

using i32 = std::int32_t;
using u64 = std::uint64_t;

enum : u32 {
    k_gl_byte = 0x1400,
    k_gl_unsigned_byte = 0x1401,
    k_gl_short = 0x1402,
    k_gl_float = 0x1406,
    k_gl_fixed = 0x140C,
    k_gl_rgba = 0x1908,
    // caps
    k_gl_blend = 0x0BE2,
    k_gl_cull_face = 0x0B44,
    k_gl_lighting = 0x0B50,
    k_gl_fog = 0x0B60,
    k_gl_line_smooth = 0x0B20,
    k_gl_dither = 0x0BD0,
    k_gl_multisample = 0x809D,
    k_gl_clip_plane0 = 0x3000,
    k_gl_light0 = 0x4000,
    // client states
    k_gl_vertex_array = 0x8074,
    k_gl_normal_array = 0x8075,
    k_gl_color_array = 0x8076,
    k_gl_texture_coord_array = 0x8078,
    k_gl_point_size_array = 0x8B9C,
    // texture parameters
    k_gl_nearest = 0x2600,
    k_gl_linear = 0x2601,
    k_gl_nearest_mipmap_nearest = 0x2700,
    k_gl_linear_mipmap_linear = 0x2703,
    k_gl_mag_filter = 0x2800,
    k_gl_min_filter = 0x2801,
    k_gl_wrap_s = 0x2802,
    k_gl_wrap_t = 0x2803,
    k_gl_repeat = 0x2901,
    k_gl_clamp_to_edge = 0x812F,
    k_gl_generate_mipmap = 0x8191,
};

// More vertices in one draw than any 480 x 320 frame needs: a misuse, not an allocation.
constexpr u32 k_max_draw_vertices = 1u << 20;

// --- matrices -------------------------------------------------------------------------------

Matrix identity() { return Matrix{}; }

bool is_identity(const Matrix &m) {
    static const Matrix k_identity{};
    for (int i = 0; i < 16; i++) {
        if (m.m[i] != k_identity.m[i]) {
            return false;
        }
    }
    return true;
}

// a x b, each element summed in the order k = 0..3.
Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = a.m[0 * 4 + row] * b.m[col * 4 + 0];
            sum = sum + a.m[1 * 4 + row] * b.m[col * 4 + 1];
            sum = sum + a.m[2 * 4 + row] * b.m[col * 4 + 2];
            sum = sum + a.m[3 * 4 + row] * b.m[col * 4 + 3];
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

// A matrix from rows of doubles, each element rounded once to float.
Matrix from_rows(const double rows[4][4]) {
    Matrix r;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            r.m[col * 4 + row] = static_cast<float>(rows[row][col]);
        }
    }
    return r;
}

double fixed_to_double(u32 v) { return static_cast<double>(static_cast<i32>(v)) / 65536.0; }

float float_bits(u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

Matrix &top(State &s) {
    Fixed &f = s.fixed;
    switch (s.matrix_mode) {
    case k_gl_projection:
        return f.projection[s.depth[1] - 1];
    case k_gl_texture:
        return f.texture[s.depth[2] - 1];
    default:
        return f.modelview[s.depth[0] - 1];
    }
}

void mult(State &s, const Matrix &m) {
    Matrix &t = top(s);
    t = multiply(t, m);
}

// sin and cos of x in [0, pi/4] by their Taylor series (to x^17 and x^16: below double's
// precision there), evaluated by Horner's rule in a fixed order.
void sincos_octant(double x, double *sin_out, double *cos_out) {
    const double x2 = x * x;
    double s = 1.0 / 355687428096000.0; // 1/17!
    s = 1.0 / 1307674368000.0 - x2 * s; // 1/15!
    s = 1.0 / 6227020800.0 - x2 * s;    // 1/13!
    s = 1.0 / 39916800.0 - x2 * s;      // 1/11!
    s = 1.0 / 362880.0 - x2 * s;        // 1/9!
    s = 1.0 / 5040.0 - x2 * s;          // 1/7!
    s = 1.0 / 120.0 - x2 * s;           // 1/5!
    s = 1.0 / 6.0 - x2 * s;             // 1/3!
    s = 1.0 - x2 * s;
    double c = 1.0 / 20922789888000.0; // 1/16!
    c = 1.0 / 87178291200.0 - x2 * c;  // 1/14!
    c = 1.0 / 479001600.0 - x2 * c;    // 1/12!
    c = 1.0 / 3628800.0 - x2 * c;      // 1/10!
    c = 1.0 / 40320.0 - x2 * c;        // 1/8!
    c = 1.0 / 720.0 - x2 * c;          // 1/6!
    c = 1.0 / 24.0 - x2 * c;           // 1/4!
    c = 1.0 / 2.0 - x2 * c;            // 1/2!
    c = 1.0 - x2 * c;
    *sin_out = x * s;
    *cos_out = c;
}

} // namespace

float fixed_to_float(u32 v) { return static_cast<float>(fixed_to_double(v)); }

// sin and cos of an angle in GLfixed degrees: the angle is reduced exactly (integers) to a
// quadrant and an offset of at most 45 degrees, so multiples of 90 degrees are exact.
void sincos_fixed_degrees(u32 angle, double *sin_out, double *cos_out) {
    constexpr std::int64_t k_90 = 90LL << 16, k_360 = 360LL << 16;
    std::int64_t a = static_cast<std::int64_t>(static_cast<i32>(angle)) % k_360;
    if (a < 0) {
        a += k_360;
    }
    const int quadrant = static_cast<int>(a / k_90);
    const std::int64_t r = a % k_90; // [0, 90 degrees)
    const bool complement = r > k_90 / 2;
    constexpr double k_deg = 3.14159265358979323846 / 180.0 / 65536.0; // radians per 1/65536 deg
    double s = 0, c = 0;
    sincos_octant(static_cast<double>(complement ? k_90 - r : r) * k_deg, &s, &c);
    if (complement) {
        const double t = s;
        s = c;
        c = t;
    }
    switch (quadrant) {
    case 1:
        *sin_out = c;
        *cos_out = -s;
        break;
    case 2:
        *sin_out = -s;
        *cos_out = -c;
        break;
    case 3:
        *sin_out = -c;
        *cos_out = s;
        break;
    default:
        *sin_out = s;
        *cos_out = c;
        break;
    }
}

namespace {

void ortho(State &s, const u32 *a) {
    const double l = fixed_to_double(a[0]), r = fixed_to_double(a[1]);
    const double b = fixed_to_double(a[2]), t = fixed_to_double(a[3]);
    const double n = fixed_to_double(a[4]), f = fixed_to_double(a[5]);
    if (l == r || b == t || n == f) {
        misuse(s, "glOrthox", k_gl_invalid_value, "left = right, bottom = top or near = far");
        return;
    }
    const double rows[4][4] = {
        {2.0 / (r - l), 0, 0, -(r + l) / (r - l)},
        {0, 2.0 / (t - b), 0, -(t + b) / (t - b)},
        {0, 0, -2.0 / (f - n), -(f + n) / (f - n)},
        {0, 0, 0, 1},
    };
    mult(s, from_rows(rows));
}

void translate(State &s, const u32 *a) {
    Matrix m;
    m.m[12] = fixed_to_float(a[0]);
    m.m[13] = fixed_to_float(a[1]);
    m.m[14] = fixed_to_float(a[2]);
    mult(s, m);
}

void scale(State &s, const u32 *a) {
    Matrix m;
    m.m[0] = float_bits(a[0]);
    m.m[5] = float_bits(a[1]);
    m.m[10] = float_bits(a[2]);
    mult(s, m);
}

void rotate(State &s, const u32 *a) {
    double x = fixed_to_double(a[1]), y = fixed_to_double(a[2]), z = fixed_to_double(a[3]);
    const double len2 = x * x + y * y + z * z;
    if (len2 == 0) {
        note_once(s, "rotate zero axis",
                  "glRotatex with the axis (0, 0, 0): GL ES leaves it undefined; the matrix is "
                  "left as it is");
        return;
    }
    if (len2 != 1.0) {
        const double len = std::sqrt(len2); // IEEE: correctly rounded, the same everywhere
        x = x / len;
        y = y / len;
        z = z / len;
    }
    double sn = 0, c = 0;
    sincos_fixed_degrees(a[0], &sn, &c);
    const double k = 1.0 - c;
    const double rows[4][4] = {
        {x * x * k + c, x * y * k - z * sn, x * z * k + y * sn, 0},
        {y * x * k + z * sn, y * y * k + c, y * z * k - x * sn, 0},
        {x * z * k - y * sn, y * z * k + x * sn, z * z * k + c, 0},
        {0, 0, 0, 1},
    };
    mult(s, from_rows(rows));
}

// --- enables --------------------------------------------------------------------------------

// The GL ES 1.1 caps of glEnable / glDisable (section 6.1.2 table 6.x), with their initial
// values: only GL_DITHER and GL_MULTISAMPLE start enabled.
bool known_cap(u32 cap) {
    switch (cap) {
    case 0x0B10: // GL_POINT_SMOOTH
    case k_gl_line_smooth:
    case k_gl_cull_face:
    case k_gl_lighting:
    case 0x0B57: // GL_COLOR_MATERIAL
    case k_gl_fog:
    case 0x0B71: // GL_DEPTH_TEST
    case 0x0B90: // GL_STENCIL_TEST
    case 0x0BA1: // GL_NORMALIZE
    case 0x0BC0: // GL_ALPHA_TEST
    case k_gl_dither:
    case k_gl_blend:
    case 0x0BF2: // GL_COLOR_LOGIC_OP
    case 0x0C11: // GL_SCISSOR_TEST
    case k_gl_texture_2d:
    case 0x8037: // GL_POLYGON_OFFSET_FILL
    case 0x803A: // GL_RESCALE_NORMAL
    case k_gl_multisample:
    case 0x809E: // GL_SAMPLE_ALPHA_TO_COVERAGE
    case 0x809F: // GL_SAMPLE_ALPHA_TO_ONE
    case 0x80A0: // GL_SAMPLE_COVERAGE
    case 0x8861: // GL_POINT_SPRITE_OES
        return true;
    default:
        return (cap >= k_gl_clip_plane0 && cap < k_gl_clip_plane0 + 6) ||
               (cap >= k_gl_light0 && cap < k_gl_light0 + 8);
    }
}

void enable(State &s, u32 cap, bool on, const char *who) {
    Fixed &f = s.fixed;
    if (cap == k_gl_texture_2d) {
        f.texture_2d = on;
    } else if (cap == k_gl_blend) {
        f.blend = on;
    } else if (known_cap(cap)) {
        f.caps[cap] = on;
    } else {
        char detail[32];
        std::snprintf(detail, sizeof detail, "0x%04X", static_cast<unsigned>(cap));
        misuse(s, who, k_gl_invalid_enum, "unknown capability", detail);
    }
}

void client_state(State &s, u32 array, bool on, const char *who) {
    Fixed &f = s.fixed;
    switch (array) {
    case k_gl_vertex_array:
        f.vertex.enabled = on;
        break;
    case k_gl_texture_coord_array:
        f.texcoord.enabled = on;
        break;
    case k_gl_normal_array:
        f.normal_array = on;
        break;
    case k_gl_color_array:
        f.color_array = on;
        break;
    case k_gl_point_size_array:
        f.point_size_array = on;
        break;
    default: {
        char detail[32];
        std::snprintf(detail, sizeof detail, "0x%04X", static_cast<unsigned>(array));
        misuse(s, who, k_gl_invalid_enum, "unknown array", detail);
    }
    }
}

// The caps that would change the game's pixels on the device and that the emulation does not
// execute. The others have no effect here: no depth or stencil buffer (depth, stencil, polygon
// offset), no multisample buffer, no points or lines drawn, glAlphaFunc / glScissor /
// glLogicOp / glClipPlane are not imported (their initial values pass everything), lighting's
// helpers act only with GL_LIGHTING, and dithering an RGBA8 target changes nothing.
void note_caps(State &s, u32 mode) {
    const bool line = mode >= 1 && mode <= 3; // GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP
    for (const auto &[cap, on] : s.fixed.caps) {
        if (on && (cap == k_gl_cull_face || cap == k_gl_lighting || cap == k_gl_fog ||
                   (line && cap == k_gl_line_smooth))) {
            note_once(s, "cap " + std::to_string(cap),
                      "glDrawArrays with cap 0x%04X enabled: not emulated, drawn without it",
                      static_cast<unsigned>(cap));
        }
    }
    if (s.fixed.color_array || s.fixed.normal_array || s.fixed.point_size_array) {
        note_once(s, "arrays",
                  "glDrawArrays with a colour, normal or point-size array enabled (the game "
                  "imports no pointer function for them): ignored");
    }
}

// --- client arrays --------------------------------------------------------------------------

u32 type_size(u32 type) {
    switch (type) {
    case k_gl_byte:
        return 1;
    case k_gl_short:
        return 2;
    case k_gl_fixed:
    case k_gl_float:
        return 4;
    default:
        return 0;
    }
}

void pointer(State &s, ClientArray &array, const u32 *a, const char *who) {
    // (size, type, stride, pointer)
    if (a[0] < 2 || a[0] > 4 || static_cast<i32>(a[2]) < 0) {
        misuse(s, who, k_gl_invalid_value, "size not 2, 3 or 4, or a negative stride");
        return;
    }
    if (type_size(a[1]) == 0) {
        misuse(s, who, k_gl_invalid_enum, "type not GL_BYTE, GL_SHORT, GL_FIXED or GL_FLOAT");
        return;
    }
    array.size = a[0];
    array.type = a[1];
    array.stride = a[2];
    array.pointer = a[3];
}

float component(const u8 *p, u32 type) {
    switch (type) {
    case k_gl_byte:
        return static_cast<float>(static_cast<std::int8_t>(p[0]));
    case k_gl_short: {
        std::int16_t v;
        std::memcpy(&v, p, 2);
        return static_cast<float>(v);
    }
    case k_gl_fixed: {
        u32 v;
        std::memcpy(&v, p, 4);
        return fixed_to_float(v);
    }
    default: {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    }
}

// The array's elements first .. first + count - 1 in guest memory, or nullptr (a misuse) if
// they are not all mapped. *step gets the stride in bytes.
const u8 *elements(mt_cpu *cpu, State &s, const ClientArray &array, u32 first, u32 count,
                   const char *what, u32 *step) {
    const u32 size = array.size * type_size(array.type);
    *step = array.stride != 0 ? array.stride : size;
    const u64 start = static_cast<u64>(array.pointer) + static_cast<u64>(first) * *step;
    const u64 bytes = static_cast<u64>(count - 1) * *step + size;
    if (start > 0xFFFFFFFFull || !guest_readable(cpu, static_cast<u32>(start), bytes)) {
        char detail[128];
        std::snprintf(detail, sizeof detail,
                      "pointer 0x%08X, elements %u..%u of %u bytes, stride %u",
                      static_cast<unsigned>(array.pointer), static_cast<unsigned>(first),
                      static_cast<unsigned>(first + count - 1), static_cast<unsigned>(size),
                      static_cast<unsigned>(*step));
        misuse(s, "glDrawArrays", k_gl_no_error, what, detail);
        return nullptr;
    }
    return cpu->mem + start;
}

// --- textures -------------------------------------------------------------------------------

bool mipmapped(u32 filter) {
    return filter >= k_gl_nearest_mipmap_nearest && filter <= k_gl_linear_mipmap_linear;
}

// GL ES 1.1 section 3.8.10: a texture whose minification filter needs mipmaps is complete only
// with its whole chain; the game uploads level 0 only, so that is a 1 x 1 image or
// GL_GENERATE_MIPMAP. Texturing with an incomplete texture is as if it were off.
bool complete(const Texture &t) {
    if (t.width == 0 || t.height == 0) {
        return false;
    }
    return !mipmapped(t.min_filter) || t.generate_mipmap || (t.width == 1 && t.height == 1);
}

void tex_parameter(State &s, const u32 *a) {
    // (target, pname, param)
    const char *who = "glTexParameteri";
    if (a[0] != k_gl_texture_2d) {
        misuse(s, who, k_gl_invalid_enum, "target is not GL_TEXTURE_2D");
        return;
    }
    const u32 v = a[2];
    bool ok = false;
    switch (a[1]) {
    case k_gl_min_filter:
        ok = v == k_gl_nearest || v == k_gl_linear || mipmapped(v);
        break;
    case k_gl_mag_filter:
        ok = v == k_gl_nearest || v == k_gl_linear;
        break;
    case k_gl_wrap_s:
    case k_gl_wrap_t:
        ok = v == k_gl_repeat || v == k_gl_clamp_to_edge;
        break;
    case k_gl_generate_mipmap:
        ok = v <= 1;
        break;
    default:
        misuse(s, who, k_gl_invalid_enum, "unknown parameter");
        return;
    }
    if (!ok) {
        misuse(s, who, k_gl_invalid_enum, "value not allowed for the parameter");
        return;
    }
    Texture &t = s.textures[s.texture];
    if (a[1] == k_gl_generate_mipmap) {
        t.generate_mipmap = v != 0;
        if (v != 0) {
            note_once(s, "generate mipmap",
                      "glTexParameteri(GL_GENERATE_MIPMAP, GL_TRUE): no mipmaps are made; "
                      "minification samples level 0");
        }
        return;
    }
    if (a[1] == k_gl_min_filter) {
        t.min_filter = v;
    }
    mt_gfx_texture_parameter(texture_handle(t), a[1], v);
}

void tex_image(mt_cpu *cpu, State &s, const u32 *a) {
    // (target, level, internalformat, width, height, border, format, type, pixels); the
    // tracker checked the arguments and recorded the size.
    if (a[1] != 0) {
        note_once(s, "mipmap level", "glTexImage2D level %u: mipmap levels are not kept",
                  static_cast<unsigned>(a[1]));
        return;
    }
    Texture &t = s.textures[s.texture];
    const u32 handle = texture_handle(t);
    const u32 w = a[3], h = a[4], format = a[6], type = a[7], addr = a[8];
    if ((w & (w - 1)) != 0 || (h & (h - 1)) != 0) {
        note_once(s, "npot",
                  "glTexImage2D: %ux%u is not a power of two (GL ES 1.1 core rejects it; "
                  "uploaded anyway, as APPLE_texture_2D_limited_npot devices do)",
                  static_cast<unsigned>(w), static_cast<unsigned>(h));
    }
    const void *pixels = nullptr;
    if (addr != 0 && w != 0 && h != 0) {
        const u32 bpp = t.bytes / (w * h); // the tracker's bytes per texel
        const u64 row = static_cast<u64>(w) * bpp;
        const u64 bytes = ((row + 3) & ~3ull) * (h - 1) + row;
        if (guest_readable(cpu, addr, bytes)) {
            pixels = cpu->mem + addr;
        } else {
            char detail[96];
            std::snprintf(detail, sizeof detail, "pixels 0x%08X, %llu bytes",
                          static_cast<unsigned>(addr), static_cast<unsigned long long>(bytes));
            misuse(s, "glTexImage2D", k_gl_no_error, "pixels outside mapped guest memory", detail);
        }
    }
    if (mt_gfx_texture_image(handle, w, h, format, type, pixels) != 0) {
        // mt_gfx converts every pair the tracker accepts (#57) at every size it accepts (2048),
        // so only running out of memory gets here.
        char key[48];
        std::snprintf(key, sizeof key, "format %X %X", static_cast<unsigned>(format),
                      static_cast<unsigned>(type));
        note_once(s, key,
                  "glTexImage2D: format 0x%04X, type 0x%04X, %ux%u: mt_gfx could not store the "
                  "image (out of memory): such textures stay empty (drawn untextured)",
                  static_cast<unsigned>(format), static_cast<unsigned>(type),
                  static_cast<unsigned>(w), static_cast<unsigned>(h));
        mt_gfx_texture_image(handle, 0, 0, k_gl_rgba, k_gl_unsigned_byte, nullptr);
    }
}

// --- the draw -------------------------------------------------------------------------------

void skip(State &s, const char *why) {
    s.stats.draws_skipped++;
    std::snprintf(s.draw_note, sizeof s.draw_note, "skipped: %s", why);
}

// Moves the vertices as the host's draw filter asks (mt_gles_set_draw_filter, #159), in
// normalised device coordinates: in clip space the translation is scaled by w.
void filter_draw(mt_gfx_vertex *v, u32 count) {
    if (g_draw_filter == nullptr || count == 0) {
        return;
    }
    float box[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < count; i++) {
        const float w = v[i].w != 0 ? v[i].w : 1.0f;
        box[0] = std::min(box[0], v[i].x / w);
        box[1] = std::min(box[1], v[i].y / w);
        box[2] = std::max(box[2], v[i].x / w);
        box[3] = std::max(box[3], v[i].y / w);
    }
    float m[6] = {1, 0, 0, 1, 0, 0};
    if (g_draw_filter(g_draw_filter_user, box, m) == 0) {
        return;
    }
    for (u32 i = 0; i < count; i++) {
        const float x = v[i].x, y = v[i].y, w = v[i].w;
        v[i].x = m[0] * x + m[2] * y + m[4] * w;
        v[i].y = m[1] * x + m[3] * y + m[5] * w;
    }
}

void draw(mt_cpu *cpu, State &s, const u32 *a) {
    Fixed &f = s.fixed;
    const u32 mode = a[0], first = a[1], count = a[2]; // checked by the tracker
    if (!f.vertex.enabled) {
        skip(s, "GL_VERTEX_ARRAY is disabled");
        return;
    }
    if (count == 0) {
        skip(s, "count 0");
        return;
    }
    if (count > k_max_draw_vertices) {
        misuse(s, "glDrawArrays", k_gl_no_error, "implausibly many vertices");
        skip(s, "too many vertices");
        return;
    }
    mt_gfx_target *target = draw_target(s);
    if (target == nullptr) {
        misuse(s, "glDrawArrays", k_gl_invalid_framebuffer_operation,
               "the bound framebuffer is not complete");
        skip(s, "no complete framebuffer");
        return;
    }
    u32 vstep = 0, tstep = 0;
    const u8 *vp = elements(cpu, s, f.vertex, first, count,
                            "the vertex array is outside mapped "
                            "guest memory",
                            &vstep);
    const u8 *tp = nullptr;
    if (vp != nullptr && f.texcoord.enabled) {
        tp = elements(cpu, s, f.texcoord, first, count,
                      "the texture coordinate array is outside mapped guest memory", &tstep);
    }
    if (vp == nullptr || (f.texcoord.enabled && tp == nullptr)) {
        skip(s, "an array is outside guest memory");
        return;
    }
    note_caps(s, mode);

    const Matrix pm = multiply(f.projection[s.depth[1] - 1], f.modelview[s.depth[0] - 1]);
    const Matrix &tm = f.texture[s.depth[2] - 1];
    const bool tm_identity = is_identity(tm);
    const u32 vsize = type_size(f.vertex.type), tsize = type_size(f.texcoord.type);
    f.vertices.resize(count);
    // Without a texture coordinate array every vertex has the current coordinates (0, 0, 0, 1):
    // the game imports no glMultiTexCoord.
    float in_t[4] = {0, 0, 0, 1};
    for (u32 i = 0; i < count; i++) {
        float v[4] = {0, 0, 0, 1};
        const u8 *e = vp + static_cast<std::size_t>(i) * vstep;
        for (u32 k = 0; k < f.vertex.size; k++) {
            v[k] = component(e + k * vsize, f.vertex.type);
        }
        mt_gfx_vertex &o = f.vertices[i];
        const float *m = pm.m;
        o.x = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
        o.y = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
        o.z = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
        o.w = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
        if (tp != nullptr) {
            const u8 *te = tp + static_cast<std::size_t>(i) * tstep;
            in_t[2] = 0;
            in_t[3] = 1;
            for (u32 k = 0; k < f.texcoord.size; k++) {
                in_t[k] = component(te + k * tsize, f.texcoord.type);
            }
        }
        if (tm_identity) {
            o.s = in_t[0];
            o.t = in_t[1];
            if (in_t[3] != 1.0f && in_t[3] != 0.0f) {
                o.s = o.s / in_t[3];
                o.t = o.t / in_t[3];
            }
        } else {
            const float *t = tm.m;
            const float ts = t[0] * in_t[0] + t[4] * in_t[1] + t[8] * in_t[2] + t[12] * in_t[3];
            const float tt = t[1] * in_t[0] + t[5] * in_t[1] + t[9] * in_t[2] + t[13] * in_t[3];
            const float tq = t[3] * in_t[0] + t[7] * in_t[1] + t[11] * in_t[2] + t[15] * in_t[3];
            o.s = tq != 1.0f && tq != 0.0f ? ts / tq : ts;
            o.t = tq != 1.0f && tq != 0.0f ? tt / tq : tt;
        }
    }

    if (g_host_x_scale != 1.0f || g_host_x_offset != 0.0f) {
        for (u32 i = 0; i < count; i++) {
            mt_gfx_vertex &o = f.vertices[i];
            o.x = g_host_x_scale * o.x + g_host_x_offset * o.w;
        }
    }
    filter_draw(f.vertices.data(), count);

    mt_gfx_draw d = mt_gfx_draw_default();
    d.primitive = mode;
    d.vertices = f.vertices.data();
    d.count = count;
    std::memcpy(d.color, f.color, 4);
    if (f.texture_2d) {
        const auto it = s.textures.find(s.texture);
        if (it != s.textures.end() && it->second.gfx != 0) {
            if (complete(it->second)) {
                d.texture = it->second.gfx;
            } else if (it->second.width != 0 && it->second.height != 0) {
                note_once(s, "incomplete",
                          "glDrawArrays with an incomplete texture (a mipmap minification "
                          "filter and level 0 only): drawn untextured, as on the device");
            }
        }
    }
    d.blend = f.blend ? 1u : 0u;
    d.src_factor = f.src_factor;
    d.dst_factor = f.dst_factor;
    for (int i = 0; i < 4; i++) {
        d.viewport[i] = static_cast<i32>(s.viewport[i]);
    }
    const int r = mt_gfx_submit(target, &d);
    s.stats.vertices += count;
    if (r == MT_GFX_DRAWN) {
        s.stats.draws_drawn++;
        std::snprintf(s.draw_note, sizeof s.draw_note, "%u vertices, drawn",
                      static_cast<unsigned>(count));
    } else {
        s.stats.draws_skipped++;
        std::snprintf(s.draw_note, sizeof s.draw_note, "%u vertices, %s",
                      static_cast<unsigned>(count),
                      r == MT_GFX_UNSUPPORTED ? "not drawn (points)" : "invalid");
    }
}

// GL ES 1.1 section 4.1.7: the factors each side takes.
bool valid_src_factor(u32 f) {
    return f == MT_GFX_ZERO || f == MT_GFX_ONE || f == MT_GFX_DST_COLOR ||
           f == MT_GFX_ONE_MINUS_DST_COLOR || f == MT_GFX_SRC_ALPHA ||
           f == MT_GFX_ONE_MINUS_SRC_ALPHA || f == MT_GFX_DST_ALPHA ||
           f == MT_GFX_ONE_MINUS_DST_ALPHA || f == MT_GFX_SRC_ALPHA_SATURATE;
}
bool valid_dst_factor(u32 f) {
    return f == MT_GFX_ZERO || f == MT_GFX_ONE || f == MT_GFX_SRC_COLOR ||
           f == MT_GFX_ONE_MINUS_SRC_COLOR || f == MT_GFX_SRC_ALPHA ||
           f == MT_GFX_ONE_MINUS_SRC_ALPHA || f == MT_GFX_DST_ALPHA ||
           f == MT_GFX_ONE_MINUS_DST_ALPHA;
}

// GLfixed colour to 8 bits: clamped to [0, 1], then round(v * 255).
u8 color_byte(u32 v) {
    const i32 c = static_cast<i32>(v);
    const u32 clamped = c <= 0 ? 0u : c >= 65536 ? 65536u : static_cast<u32>(c);
    return static_cast<u8>((clamped * 255u + 32768u) >> 16);
}

} // namespace

void fixed_execute(mt_cpu *cpu, State &s, const mt_gles_cmd &c) {
    const u32 *a = c.args;
    Fixed &f = s.fixed;
    switch (static_cast<mt_gles_op>(c.op)) {
    case MT_GLES_LoadIdentity:
        top(s) = identity();
        break;
    case MT_GLES_PushMatrix: {
        Matrix &t = top(s); // the tracker made the stack one deeper
        t = (&t)[-1];
        break;
    }
    case MT_GLES_Orthox:
        ortho(s, a);
        break;
    case MT_GLES_Translatex:
        translate(s, a);
        break;
    case MT_GLES_Rotatex:
        rotate(s, a);
        break;
    case MT_GLES_Scalef:
        scale(s, a);
        break;
    case MT_GLES_Enable:
        enable(s, a[0], true, "glEnable");
        break;
    case MT_GLES_Disable:
        enable(s, a[0], false, "glDisable");
        break;
    case MT_GLES_EnableClientState:
        client_state(s, a[0], true, "glEnableClientState");
        break;
    case MT_GLES_DisableClientState:
        client_state(s, a[0], false, "glDisableClientState");
        break;
    case MT_GLES_VertexPointer:
        pointer(s, f.vertex, a, "glVertexPointer");
        break;
    case MT_GLES_TexCoordPointer:
        pointer(s, f.texcoord, a, "glTexCoordPointer");
        break;
    case MT_GLES_Color4x:
        for (int i = 0; i < 4; i++) {
            f.color[i] = color_byte(a[i]);
        }
        break;
    case MT_GLES_BlendFunc:
        if (!valid_src_factor(a[0]) || !valid_dst_factor(a[1])) {
            misuse(s, "glBlendFunc", k_gl_invalid_enum, "factor not allowed on its side");
            break;
        }
        f.src_factor = a[0];
        f.dst_factor = a[1];
        break;
    case MT_GLES_TexParameteri:
        tex_parameter(s, a);
        break;
    case MT_GLES_TexImage2D:
        tex_image(cpu, s, a);
        break;
    case MT_GLES_DrawArrays:
        draw(cpu, s, a);
        break;
    // The tracker's own (objects, bindings, queries, the viewport, the matrix mode and depth).
    case MT_GLES_BindFramebufferOES:
    case MT_GLES_BindRenderbufferOES:
    case MT_GLES_BindTexture:
    case MT_GLES_CheckFramebufferStatusOES:
    case MT_GLES_DeleteTextures:
    case MT_GLES_FramebufferRenderbufferOES:
    case MT_GLES_GenFramebuffersOES:
    case MT_GLES_GenRenderbuffersOES:
    case MT_GLES_GenTextures:
    case MT_GLES_GetRenderbufferParameterivOES:
    case MT_GLES_IsTexture:
    case MT_GLES_MatrixMode:
    case MT_GLES_PopMatrix:
    case MT_GLES_Viewport:
    case MT_GLES_OP_COUNT:
        break;
    }
}

} // namespace mt_gles

extern "C" void mt_gles_set_draw_filter(mt_gles_draw_filter filter, void *user) noexcept {
    mt_gles::g_draw_filter = filter;
    mt_gles::g_draw_filter_user = filter != nullptr ? user : nullptr;
}

extern "C" void mt_gles_set_host_x_transform(float scale, float offset) noexcept {
    mt_gles::g_host_x_scale = scale;
    mt_gles::g_host_x_offset = offset;
}

extern "C" void mt_gles_get_host_x_transform(float *scale, float *offset) noexcept {
    if (scale != nullptr) {
        *scale = mt_gles::g_host_x_scale;
    }
    if (offset != nullptr) {
        *offset = mt_gles::g_host_x_offset;
    }
}

extern "C" mt_gles_draw_filter mt_gles_get_draw_filter(void **user) noexcept {
    if (user != nullptr) {
        *user = mt_gles::g_draw_filter_user;
    }
    return mt_gles::g_draw_filter;
}
