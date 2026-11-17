// The OpenGL ES 1.1 imports (#117 ADR 0038, #56 ADR 0040, maplethief/gles.h): each HLE function
// decodes its guest arguments into an mt_gles_cmd, the state tracker applies it and answers the
// game's queries, the fixed-function pipeline (fixed.cpp) executes what the tracker accepted,
// then the command hook (an observer) sees it.
//
// The tracker keeps what the game reads back or what the pipeline needs:
//   - names: textures, framebuffers and renderbuffers made by glGen* or by binding an unused
//     name (GL ES 1.1 creates the object then); glDeleteTextures frees them
//   - bindings: the framebuffer, the renderbuffer, the 2D texture
//   - each texture's level-0 size and format (glTexImage2D), each renderbuffer's storage
//     (EAGL), each framebuffer's colour attachment
//   - the matrix mode and the depth of each matrix stack, the viewport, the frame counters
// and the GL objects' mt_gfx counterparts: a texture object's mt_gfx texture (made when the
// name is first bound, freed by glDeleteTextures), a renderbuffer's mt_gfx target (made at its
// storage).
// glIsTexture answers as GL ES 1.1 does: a name is a texture once it has been bound, until it
// is deleted. Calls the device would reject with a GL error are counted and logged (once per
// kind), set GL's error flag, and change nothing, as on the device.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cf/cf_objects.h"
#include "gles/gles_internal.h"
#include "maplethief/boot.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "objc/objc_internal.h"

namespace mt_gles {
namespace {

// GL enums of the tracker.
enum : u32 {
    k_gl_color_attachment0 = 0x8CE0,
    k_gl_depth_attachment = 0x8D00,
    k_gl_stencil_attachment = 0x8D20,
    k_gl_framebuffer_complete = 0x8CD5,
    k_gl_framebuffer_incomplete_attachment = 0x8CD6,
    k_gl_framebuffer_missing_attachment = 0x8CD7,
    k_gl_renderbuffer_width = 0x8D42,
    k_gl_renderbuffer_height = 0x8D43,
    k_gl_renderbuffer_internal_format = 0x8D44,
    k_gl_renderbuffer_red_size = 0x8D50,
    k_gl_renderbuffer_green_size = 0x8D51,
    k_gl_renderbuffer_blue_size = 0x8D52,
    k_gl_renderbuffer_alpha_size = 0x8D53,
    k_gl_renderbuffer_depth_size = 0x8D54,
    k_gl_renderbuffer_stencil_size = 0x8D55,
    k_gl_alpha = 0x1906,
    k_gl_rgb = 0x1907,
    k_gl_rgba = 0x1908,
    k_gl_luminance = 0x1909,
    k_gl_luminance_alpha = 0x190A,
    k_gl_unsigned_byte = 0x1401,
    k_gl_unsigned_short_4444 = 0x8033,
    k_gl_unsigned_short_5551 = 0x8034,
    k_gl_unsigned_short_565 = 0x8363,
};

constexpr u32 k_max_names_per_call = 4096;

State g;
mt_gles_config g_config{};

const char *const k_names[] = {
#define MT_GLES_OP_NAME(name, argc) "gl" #name,
    MT_GLES_OPS(MT_GLES_OP_NAME)
#undef MT_GLES_OP_NAME
};
const u32 k_argc[] = {
#define MT_GLES_OP_ARGC(name, argc) argc,
    MT_GLES_OPS(MT_GLES_OP_ARGC)
#undef MT_GLES_OP_ARGC
};

// --- the GL trace file ----------------------------------------------------------------------

struct TraceFile {
    bool resolved = false;   // the path was decided since the last mt_gles_configure
    bool configured = false; // mt_gles_config.trace_file was given (else the environment)
    std::string configured_path;
    std::string path; // of the open file
    std::FILE *f = nullptr;
};
TraceFile g_trace;

std::string env_trace_path() {
#if defined(_MSC_VER)
    char *v = nullptr;
    std::size_t len = 0;
    std::string path;
    if (_dupenv_s(&v, &len, "MAPLETHIEF_GL_TRACE_FILE") == 0 && v != nullptr) {
        path = v;
    }
    std::free(v);
    return path;
#else
    const char *v = std::getenv("MAPLETHIEF_GL_TRACE_FILE");
    return v != nullptr ? v : "";
#endif
}

std::FILE *trace_file() {
    if (!g_trace.resolved) {
        g_trace.resolved = true;
        const std::string path = g_trace.configured ? g_trace.configured_path : env_trace_path();
        if (path != g_trace.path || (g_trace.f == nullptr && !path.empty())) {
            if (g_trace.f != nullptr) {
                std::fclose(g_trace.f);
                g_trace.f = nullptr;
            }
            g_trace.path = path;
            if (!path.empty()) {
#if defined(_MSC_VER)
                // Shared, so the file can be read while it is written (fopen_s denies that).
                g_trace.f = _fsopen(path.c_str(), "w", _SH_DENYNO);
#else
                g_trace.f = std::fopen(path.c_str(), "w");
#endif
                if (g_trace.f == nullptr) {
                    log("the GL trace file %s cannot be written", path.c_str());
                }
            }
        }
    }
    return g_trace.f;
}

bool tracing() { return trace_file() != nullptr || mt_trace_enabled("gl") != 0; }

// --- the state ------------------------------------------------------------------------------

// The last runtime's renderbuffer targets: no runtime switch comes after it to free them, so
// they go at exit (#414). Registered after mt_gfx made its first target, so it runs before
// mt_gfx's registry of targets is destroyed.
void free_targets_at_exit() {
    for (auto &[name, rb] : g.renderbuffers) {
        mt_gfx_target_destroy(rb.target);
        rb.target = nullptr;
    }
}

mt_gfx_target *create_target(u32 width, u32 height, u32 scale) {
    mt_gfx_target *t = mt_gfx_target_create(width, height, scale);
    static bool registered = false;
    if (t != nullptr && !registered) {
        registered = std::atexit(free_targets_at_exit) == 0;
    }
    return t;
}

// Frees the state's mt_gfx objects and starts from scratch (the same process binding).
void reset_state() {
    for (auto &[name, t] : g.textures) {
        mt_gfx_texture_destroy(t.gfx);
    }
    for (auto &[name, rb] : g.renderbuffers) {
        mt_gfx_target_destroy(rb.target);
    }
    mt_objc *rt = g.rt;
    const u32 binding = g.binding;
    g = State{};
    g.rt = rt;
    g.binding = binding;
    if (std::FILE *f = trace_file()) {
        std::fputs("# new GL state\n", f);
    }
}

u32 rd32(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, mt_g2h(cpu, addr, 4), 4);
    return v;
}
void wr32(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(mt_g2h(cpu, addr, 4), &v, 4); }

// The bytes of one texel of a valid format/type pair.
u32 bytes_per_pixel(u32 format, u32 type) {
    if (type != k_gl_unsigned_byte) {
        return 2; // the packed 16-bit types: 5_6_5, 4_4_4_4, 5_5_5_1
    }
    switch (format) {
    case k_gl_alpha:
    case k_gl_luminance:
        return 1;
    case k_gl_luminance_alpha:
        return 2;
    case k_gl_rgb:
        return 3;
    case k_gl_rgba:
    default:
        return 4;
    }
}

bool valid_texture_format(u32 format) {
    return format == k_gl_alpha || format == k_gl_rgb || format == k_gl_rgba ||
           format == k_gl_luminance || format == k_gl_luminance_alpha;
}
bool valid_texture_type(u32 type) {
    return type == k_gl_unsigned_byte || type == k_gl_unsigned_short_4444 ||
           type == k_gl_unsigned_short_5551 || type == k_gl_unsigned_short_565;
}
// GL ES 1.1 table 3.4: the packed types belong to one format each.
bool valid_texture_pair(u32 format, u32 type) {
    switch (type) {
    case k_gl_unsigned_short_565:
        return format == k_gl_rgb;
    case k_gl_unsigned_short_4444:
    case k_gl_unsigned_short_5551:
        return format == k_gl_rgba;
    default:
        return true;
    }
}

// glGen*(n, names): fresh names from `next`, skipping names in use.
template <typename Map> void gen(mt_cpu *cpu, State &s, const mt_gles_cmd &c, Map &map, u32 &next) {
    const auto n = static_cast<std::int32_t>(c.args[0]);
    if (n < 0 || static_cast<u32>(n) > k_max_names_per_call) {
        misuse(s, k_names[c.op], k_gl_invalid_value, "n is negative or implausibly large");
        return;
    }
    for (u32 i = 0; i < static_cast<u32>(n); i++) {
        while (next == 0 || map.count(next) != 0) {
            next++;
        }
        map[next];
        wr32(cpu, c.args[1] + 4u * i, next);
        next++;
    }
}

u32 *stack_depth(State &s) {
    switch (s.matrix_mode) {
    case k_gl_projection:
        return &s.depth[1];
    case k_gl_texture:
        return &s.depth[2];
    default:
        return &s.depth[0];
    }
}
u32 stack_max(const State &s) {
    return s.matrix_mode == k_gl_modelview    ? k_modelview_depth
           : s.matrix_mode == k_gl_projection ? k_projection_depth
                                              : k_texture_depth;
}

u32 framebuffer_status(const State &s) {
    const auto fb = s.framebuffers.find(s.framebuffer);
    if (s.framebuffer == 0 || fb == s.framebuffers.end() || fb->second.color == 0) {
        return k_gl_framebuffer_missing_attachment;
    }
    const auto rb = s.renderbuffers.find(fb->second.color);
    return rb == s.renderbuffers.end() || rb->second.width == 0 || rb->second.height == 0
               ? k_gl_framebuffer_incomplete_attachment
               : k_gl_framebuffer_complete;
}

// Applies `c` to the state; sets c.result. A misuse leaves the state as it was.
void apply(mt_cpu *cpu, State &s, mt_gles_cmd &c) {
    const u32 *a = c.args;
    const char *who = k_names[c.op];
    switch (static_cast<mt_gles_op>(c.op)) {
    case MT_GLES_GenTextures:
        gen(cpu, s, c, s.textures, s.next_texture);
        break;
    case MT_GLES_GenFramebuffersOES:
        gen(cpu, s, c, s.framebuffers, s.next_framebuffer);
        break;
    case MT_GLES_GenRenderbuffersOES:
        gen(cpu, s, c, s.renderbuffers, s.next_renderbuffer);
        break;
    case MT_GLES_DeleteTextures: {
        const auto n = static_cast<std::int32_t>(a[0]);
        if (n < 0 || static_cast<u32>(n) > k_max_names_per_call) {
            misuse(s, who, k_gl_invalid_value, "n is negative or implausibly large");
            break;
        }
        for (u32 i = 0; i < static_cast<u32>(n); i++) {
            const u32 name = rd32(cpu, a[1] + 4u * i);
            const auto t = s.textures.find(name);
            if (name == 0 || t == s.textures.end()) {
                continue; // unused names and 0 are silently ignored
            }
            s.texture_bytes -= t->second.bytes;
            mt_gfx_texture_destroy(t->second.gfx);
            s.textures.erase(t);
            if (s.texture == name) {
                s.texture = 0;
            }
        }
        break;
    }
    case MT_GLES_BindTexture:
        if (a[0] != k_gl_texture_2d) {
            misuse(s, who, k_gl_invalid_enum, "target is not GL_TEXTURE_2D");
            break;
        }
        s.texture = a[1];
        if (a[1] != 0) {
            Texture &t = s.textures[a[1]];
            t.bound = true;
            texture_handle(t); // the name is a texture object now
        }
        break;
    case MT_GLES_IsTexture: {
        const auto t = s.textures.find(a[0]);
        c.result = a[0] != 0 && t != s.textures.end() && t->second.bound ? 1u : 0u;
        break;
    }
    case MT_GLES_TexImage2D: {
        // (target, level, internalformat, width, height, border, format, type, pixels)
        if (a[0] != k_gl_texture_2d) {
            misuse(s, who, k_gl_invalid_enum, "target is not GL_TEXTURE_2D");
            break;
        }
        if (!valid_texture_format(a[6]) || !valid_texture_type(a[7])) {
            misuse(s, who, k_gl_invalid_enum, "unknown format or type");
            break;
        }
        const auto w = static_cast<std::int32_t>(a[3]), h = static_cast<std::int32_t>(a[4]);
        if (static_cast<std::int32_t>(a[1]) < 0 || w < 0 || h < 0 || w > 2048 || h > 2048 ||
            a[5] != 0) {
            misuse(s, who, k_gl_invalid_value, "level or size out of range or border not 0");
            break;
        }
        if (a[2] != a[6] || !valid_texture_pair(a[6], a[7])) {
            misuse(s, who, k_gl_invalid_operation,
                   "internalformat is not format, or the type does not fit the format");
            break;
        }
        s.frame_uploads++;
        if (a[1] != 0) {
            break; // only level 0 is tracked (the game uploads no mipmaps)
        }
        Texture &t = s.textures[s.texture]; // texture 0: the default texture object
        s.texture_bytes -= t.bytes;
        t.width = static_cast<u32>(w);
        t.height = static_cast<u32>(h);
        t.internal_format = a[2];
        t.format = a[6];
        t.type = a[7];
        t.bytes = t.width * t.height * bytes_per_pixel(a[6], a[7]);
        s.texture_bytes += t.bytes;
        break;
    }
    case MT_GLES_BindFramebufferOES:
        if (a[0] != k_gl_framebuffer) {
            misuse(s, who, k_gl_invalid_enum, "target is not GL_FRAMEBUFFER_OES");
            break;
        }
        s.framebuffer = a[1];
        if (a[1] != 0) {
            s.framebuffers[a[1]];
        }
        break;
    case MT_GLES_BindRenderbufferOES:
        if (a[0] != k_gl_renderbuffer) {
            misuse(s, who, k_gl_invalid_enum, "target is not GL_RENDERBUFFER_OES");
            break;
        }
        s.renderbuffer = a[1];
        if (a[1] != 0) {
            s.renderbuffers[a[1]];
        }
        break;
    case MT_GLES_FramebufferRenderbufferOES: {
        // (target, attachment, renderbuffertarget, renderbuffer)
        const auto fb = s.framebuffers.find(s.framebuffer);
        if (a[0] != k_gl_framebuffer || s.framebuffer == 0 || fb == s.framebuffers.end()) {
            misuse(s, who, k_gl_invalid_operation, "no framebuffer is bound");
            break;
        }
        if (a[3] != 0 && (a[2] != k_gl_renderbuffer || s.renderbuffers.count(a[3]) == 0)) {
            misuse(s, who, k_gl_invalid_operation, "no such renderbuffer");
            break;
        }
        if (a[1] == k_gl_color_attachment0) {
            fb->second.color = a[3];
        } else if (a[1] == k_gl_depth_attachment) {
            fb->second.depth = a[3];
        } else if (a[1] == k_gl_stencil_attachment) {
            fb->second.stencil = a[3];
        } else {
            misuse(s, who, k_gl_invalid_enum, "unknown attachment");
        }
        break;
    }
    case MT_GLES_CheckFramebufferStatusOES:
        if (a[0] != k_gl_framebuffer) {
            misuse(s, who, k_gl_invalid_enum, "target is not GL_FRAMEBUFFER_OES");
            break;
        }
        c.result = framebuffer_status(s);
        break;
    case MT_GLES_GetRenderbufferParameterivOES: {
        // (target, pname, params)
        const auto rb = s.renderbuffers.find(s.renderbuffer);
        if (a[0] != k_gl_renderbuffer || s.renderbuffer == 0 || rb == s.renderbuffers.end()) {
            misuse(s, who, k_gl_invalid_operation, "no renderbuffer is bound");
            break;
        }
        const Renderbuffer &r = rb->second;
        const bool rgb565 = r.format == k_gl_rgb565, none = r.format == 0;
        u32 v = 0;
        switch (a[1]) {
        case k_gl_renderbuffer_width:
            v = r.width;
            break;
        case k_gl_renderbuffer_height:
            v = r.height;
            break;
        case k_gl_renderbuffer_internal_format:
            v = none ? 0x1908u /* GL_RGBA, the initial value */ : r.format;
            break;
        case k_gl_renderbuffer_red_size:
        case k_gl_renderbuffer_blue_size:
            v = none ? 0u : rgb565 ? 5u : 8u;
            break;
        case k_gl_renderbuffer_green_size:
            v = none ? 0u : rgb565 ? 6u : 8u;
            break;
        case k_gl_renderbuffer_alpha_size:
            v = none || rgb565 ? 0u : 8u;
            break;
        case k_gl_renderbuffer_depth_size:
        case k_gl_renderbuffer_stencil_size:
            v = 0;
            break;
        default:
            misuse(s, who, k_gl_invalid_enum, "unknown parameter");
            return;
        }
        wr32(cpu, a[2], v);
        break;
    }
    case MT_GLES_MatrixMode:
        if (a[0] != k_gl_modelview && a[0] != k_gl_projection && a[0] != k_gl_texture) {
            misuse(s, who, k_gl_invalid_enum, "unknown matrix mode");
            break;
        }
        s.matrix_mode = a[0];
        break;
    case MT_GLES_PushMatrix: {
        u32 *d = stack_depth(s);
        if (*d >= stack_max(s)) {
            misuse(s, who, k_gl_stack_overflow, "stack overflow");
            break;
        }
        (*d)++;
        break;
    }
    case MT_GLES_PopMatrix: {
        u32 *d = stack_depth(s);
        if (*d <= 1) {
            misuse(s, who, k_gl_stack_underflow, "stack underflow");
            break;
        }
        (*d)--;
        break;
    }
    case MT_GLES_Viewport:
        if (static_cast<std::int32_t>(a[2]) < 0 || static_cast<std::int32_t>(a[3]) < 0) {
            misuse(s, who, k_gl_invalid_value, "negative size");
            break;
        }
        std::memcpy(s.viewport, a, sizeof s.viewport);
        break;
    case MT_GLES_DrawArrays:
        if (a[0] > 6 /* GL_TRIANGLE_FAN */) {
            misuse(s, who, k_gl_invalid_enum, "unknown mode");
            break;
        }
        if (static_cast<std::int32_t>(a[2]) < 0 || static_cast<std::int32_t>(a[1]) < 0) {
            misuse(s, who, k_gl_invalid_value, "negative first or count");
            break;
        }
        s.frame_draws++;
        break;
    // The fixed-function state, not read back by the game: fixed.cpp checks and executes it.
    case MT_GLES_BlendFunc:
    case MT_GLES_Color4x:
    case MT_GLES_Disable:
    case MT_GLES_DisableClientState:
    case MT_GLES_Enable:
    case MT_GLES_EnableClientState:
    case MT_GLES_LoadIdentity:
    case MT_GLES_Orthox:
    case MT_GLES_Rotatex:
    case MT_GLES_Scalef:
    case MT_GLES_TexCoordPointer:
    case MT_GLES_TexParameteri:
    case MT_GLES_Translatex:
    case MT_GLES_VertexPointer:
    case MT_GLES_OP_COUNT:
        break;
    }
}

// --- the trace ------------------------------------------------------------------------------

struct EnumName {
    u32 value;
    const char *name;
};
// The GL ES 1.1 enums the game's calls take (values 0 and 1 are argument-specific: below).
const EnumName k_enums[] = {
    {0x0B44, "GL_CULL_FACE"},
    {0x0B50, "GL_LIGHTING"},
    {0x0B57, "GL_COLOR_MATERIAL"},
    {0x0B60, "GL_FOG"},
    {0x0B71, "GL_DEPTH_TEST"},
    {0x0B90, "GL_STENCIL_TEST"},
    {0x0BA1, "GL_NORMALIZE"},
    {0x0BC0, "GL_ALPHA_TEST"},
    {0x0BD0, "GL_DITHER"},
    {0x0BE2, "GL_BLEND"},
    {0x0BF2, "GL_COLOR_LOGIC_OP"},
    {0x0C11, "GL_SCISSOR_TEST"},
    {0x0B10, "GL_POINT_SMOOTH"},
    {0x0B20, "GL_LINE_SMOOTH"},
    {0x0DE1, "GL_TEXTURE_2D"},
    {0x1400, "GL_BYTE"},
    {0x1401, "GL_UNSIGNED_BYTE"},
    {0x1402, "GL_SHORT"},
    {0x1406, "GL_FLOAT"},
    {0x140C, "GL_FIXED"},
    {0x1700, "GL_MODELVIEW"},
    {0x1701, "GL_PROJECTION"},
    {0x1702, "GL_TEXTURE"},
    {0x1906, "GL_ALPHA"},
    {0x1907, "GL_RGB"},
    {0x1908, "GL_RGBA"},
    {0x1909, "GL_LUMINANCE"},
    {0x190A, "GL_LUMINANCE_ALPHA"},
    {0x2600, "GL_NEAREST"},
    {0x2601, "GL_LINEAR"},
    {0x2700, "GL_NEAREST_MIPMAP_NEAREST"},
    {0x2701, "GL_LINEAR_MIPMAP_NEAREST"},
    {0x2702, "GL_NEAREST_MIPMAP_LINEAR"},
    {0x2703, "GL_LINEAR_MIPMAP_LINEAR"},
    {0x2800, "GL_TEXTURE_MAG_FILTER"},
    {0x2801, "GL_TEXTURE_MIN_FILTER"},
    {0x2802, "GL_TEXTURE_WRAP_S"},
    {0x2803, "GL_TEXTURE_WRAP_T"},
    {0x2901, "GL_REPEAT"},
    {0x812F, "GL_CLAMP_TO_EDGE"},
    {0x8191, "GL_GENERATE_MIPMAP"},
    {0x8033, "GL_UNSIGNED_SHORT_4_4_4_4"},
    {0x8034, "GL_UNSIGNED_SHORT_5_5_5_1"},
    {0x8363, "GL_UNSIGNED_SHORT_5_6_5"},
    {0x8037, "GL_POLYGON_OFFSET_FILL"},
    {0x803A, "GL_RESCALE_NORMAL"},
    {0x809D, "GL_MULTISAMPLE"},
    {0x809E, "GL_SAMPLE_ALPHA_TO_COVERAGE"},
    {0x809F, "GL_SAMPLE_ALPHA_TO_ONE"},
    {0x80A0, "GL_SAMPLE_COVERAGE"},
    {0x8074, "GL_VERTEX_ARRAY"},
    {0x8075, "GL_NORMAL_ARRAY"},
    {0x8076, "GL_COLOR_ARRAY"},
    {0x8078, "GL_TEXTURE_COORD_ARRAY"},
    {0x8861, "GL_POINT_SPRITE_OES"},
    {0x8B9C, "GL_POINT_SIZE_ARRAY_OES"},
    {0x8058, "GL_RGBA8_OES"},
    {0x8D62, "GL_RGB565_OES"},
    {0x8CD5, "GL_FRAMEBUFFER_COMPLETE_OES"},
    {0x8CD6, "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_OES"},
    {0x8CD7, "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_OES"},
    {0x8CE0, "GL_COLOR_ATTACHMENT0_OES"},
    {0x8D00, "GL_DEPTH_ATTACHMENT_OES"},
    {0x8D20, "GL_STENCIL_ATTACHMENT_OES"},
    {0x8D40, "GL_FRAMEBUFFER_OES"},
    {0x8D41, "GL_RENDERBUFFER_OES"},
    {0x8D42, "GL_RENDERBUFFER_WIDTH_OES"},
    {0x8D43, "GL_RENDERBUFFER_HEIGHT_OES"},
    {0x8D44, "GL_RENDERBUFFER_INTERNAL_FORMAT_OES"},
    {0x8D50, "GL_RENDERBUFFER_RED_SIZE_OES"},
    {0x8D51, "GL_RENDERBUFFER_GREEN_SIZE_OES"},
    {0x8D52, "GL_RENDERBUFFER_BLUE_SIZE_OES"},
    {0x8D53, "GL_RENDERBUFFER_ALPHA_SIZE_OES"},
    {0x8D54, "GL_RENDERBUFFER_DEPTH_SIZE_OES"},
    {0x8D55, "GL_RENDERBUFFER_STENCIL_SIZE_OES"},
};
const char *const k_primitives[] = {"GL_POINTS",      "GL_LINES",     "GL_LINE_LOOP",
                                    "GL_LINE_STRIP",  "GL_TRIANGLES", "GL_TRIANGLE_STRIP",
                                    "GL_TRIANGLE_FAN"};
const EnumName k_factors[] = {
    {0x0000, "GL_ZERO"},
    {0x0001, "GL_ONE"},
    {0x0300, "GL_SRC_COLOR"},
    {0x0301, "GL_ONE_MINUS_SRC_COLOR"},
    {0x0302, "GL_SRC_ALPHA"},
    {0x0303, "GL_ONE_MINUS_SRC_ALPHA"},
    {0x0304, "GL_DST_ALPHA"},
    {0x0305, "GL_ONE_MINUS_DST_ALPHA"},
    {0x0306, "GL_DST_COLOR"},
    {0x0307, "GL_ONE_MINUS_DST_COLOR"},
    {0x0308, "GL_SRC_ALPHA_SATURATE"},
};

const char *find_name(const EnumName *table, std::size_t n, u32 v) {
    for (std::size_t i = 0; i < n; i++) {
        if (table[i].value == v) {
            return table[i].name;
        }
    }
    return nullptr;
}

// How each argument of an op is shown: e GL enum, p primitive, b blend factor, i signed
// integer, u name or unsigned, x GLfixed, f GLfloat, a guest address, t a texture parameter's
// value (an enum, or GL_TRUE / GL_FALSE).
const char *arg_kinds(u32 op) {
    switch (static_cast<mt_gles_op>(op)) {
    case MT_GLES_BindFramebufferOES:
    case MT_GLES_BindRenderbufferOES:
    case MT_GLES_BindTexture:
        return "eu";
    case MT_GLES_BlendFunc:
        return "bb";
    case MT_GLES_CheckFramebufferStatusOES:
    case MT_GLES_Disable:
    case MT_GLES_DisableClientState:
    case MT_GLES_Enable:
    case MT_GLES_EnableClientState:
    case MT_GLES_MatrixMode:
        return "e";
    case MT_GLES_Color4x:
    case MT_GLES_Rotatex:
        return "xxxx";
    case MT_GLES_DeleteTextures:
    case MT_GLES_GenFramebuffersOES:
    case MT_GLES_GenRenderbuffersOES:
    case MT_GLES_GenTextures:
        return "ia";
    case MT_GLES_DrawArrays:
        return "pii";
    case MT_GLES_FramebufferRenderbufferOES:
        return "eeeu";
    case MT_GLES_GetRenderbufferParameterivOES:
        return "eea";
    case MT_GLES_IsTexture:
        return "u";
    case MT_GLES_Orthox:
        return "xxxxxx";
    case MT_GLES_Scalef:
        return "fff";
    case MT_GLES_TexCoordPointer:
    case MT_GLES_VertexPointer:
        return "ieia";
    case MT_GLES_TexImage2D:
        return "eieiiieea";
    case MT_GLES_TexParameteri:
        return "eet";
    case MT_GLES_Translatex:
        return "xxx";
    case MT_GLES_Viewport:
        return "iiii";
    case MT_GLES_LoadIdentity:
    case MT_GLES_PopMatrix:
    case MT_GLES_PushMatrix:
    case MT_GLES_OP_COUNT:
        break;
    }
    return "";
}

// "glBindTexture(GL_TEXTURE_2D, 3)", "glOrthox(0, 480, 320, 0, -1, 1)".
void format_call(const mt_gles_cmd &c, char *text, std::size_t size) {
    int n = std::snprintf(text, size, "%s(", k_names[c.op]);
    const char *kinds = arg_kinds(c.op);
    for (u32 i = 0; i < c.argc && n > 0 && static_cast<std::size_t>(n) < size; i++) {
        const std::size_t left = size - static_cast<std::size_t>(n);
        const char *sep = i != 0 ? ", " : "";
        const u32 v = c.args[i];
        const char kind = *kinds != 0 ? *kinds++ : 'u'; // past the end of the kinds: 'u'
        const char *name = nullptr;
        if (kind == 'e' || kind == 't') {
            name = find_name(k_enums, sizeof k_enums / sizeof k_enums[0], v);
            if (name == nullptr && kind == 't' && v <= 1) {
                name = v != 0 ? "GL_TRUE" : "GL_FALSE";
            }
        } else if (kind == 'p') {
            name = v < 7 ? k_primitives[v] : nullptr;
        } else if (kind == 'b') {
            name = find_name(k_factors, sizeof k_factors / sizeof k_factors[0], v);
        }
        if (name != nullptr) {
            n += std::snprintf(text + n, left, "%s%s", sep, name);
        } else if (kind == 'e' || kind == 'p' || kind == 'b' || kind == 't') {
            n += std::snprintf(text + n, left, "%s0x%04X", sep, static_cast<unsigned>(v));
        } else if (kind == 'i') {
            n += std::snprintf(text + n, left, "%s%d", sep, static_cast<int>(v));
        } else if (kind == 'x') {
            n += std::snprintf(text + n, left, "%s%.9g", sep,
                               static_cast<double>(static_cast<std::int32_t>(v)) / 65536.0);
        } else if (kind == 'f') {
            float f;
            std::memcpy(&f, &v, 4);
            n += std::snprintf(text + n, left, "%s%.9g", sep, static_cast<double>(f));
        } else if (kind == 'a') {
            n += std::snprintf(text + n, left, "%s0x%08X", sep, static_cast<unsigned>(v));
        } else {
            n += std::snprintf(text + n, left, "%s%u", sep, static_cast<unsigned>(v));
        }
    }
    if (n > 0 && static_cast<std::size_t>(n) < size) {
        std::snprintf(text + n, size - static_cast<std::size_t>(n), ")");
    }
}

void trace_call(const mt_cpu *cpu, const State &s, const mt_gles_cmd &c) {
    if (!tracing()) {
        return;
    }
    char text[320];
    format_call(c, text, sizeof text);
    const u32 lr = cpu->r[MT_LR];
    if (c.op == MT_GLES_TexImage2D && lr >= 4u) {
        // The call site, for telling the game's uploads apart (#57): lr is the return address
        // of the 4-byte Thumb BL/BLX to the import's stub.
        trace_line("%s from 0x%X", text, static_cast<unsigned>((lr & ~1u) - 4u));
    } else if (c.op == MT_GLES_IsTexture) {
        trace_line("%s -> %s", text, c.result != 0 ? "GL_TRUE" : "GL_FALSE");
    } else if (c.op == MT_GLES_CheckFramebufferStatusOES) {
        const char *name = find_name(k_enums, sizeof k_enums / sizeof k_enums[0], c.result);
        trace_line("%s -> %s", text, name != nullptr ? name : "0");
    } else if (c.op == MT_GLES_DrawArrays && s.draw_note[0] != 0) {
        trace_line("%s: %s", text, s.draw_note);
    } else {
        trace_line("%s", text);
    }
}

// The body of every GL import.
void call(mt_cpu *cpu, mt_gles_op op) {
    State &s = state();
    mt_gles_cmd c{};
    c.op = op;
    c.argc = k_argc[op];
    mt_args args = mt_args_begin(cpu);
    for (u32 i = 0; i < c.argc; i++) {
        c.args[i] = mt_arg_u32(&args);
    }
    s.stats.calls++;
    s.stats.op_calls[op]++;
    s.frame_calls++;
    s.draw_note[0] = 0;
    const u32 misuses = s.stats.misuses;
    apply(cpu, s, c);
    if (s.stats.misuses == misuses) {
        fixed_execute(cpu, s, c);
    } else if (op == MT_GLES_DrawArrays) {
        std::snprintf(s.draw_note, sizeof s.draw_note, "rejected");
    }
    trace_call(cpu, s, c);
    if (g_config.command != nullptr) {
        g_config.command(g_config.user, cpu, &c);
    }
    mt_ret_u32(cpu, c.result);
}

const char *error_name(u32 error) {
    switch (error) {
    case k_gl_invalid_enum:
        return "GL_INVALID_ENUM";
    case k_gl_invalid_value:
        return "GL_INVALID_VALUE";
    case k_gl_invalid_operation:
        return "GL_INVALID_OPERATION";
    case k_gl_stack_overflow:
        return "GL_STACK_OVERFLOW";
    case k_gl_stack_underflow:
        return "GL_STACK_UNDERFLOW";
    case k_gl_invalid_framebuffer_operation:
        return "GL_INVALID_FRAMEBUFFER_OPERATION_OES";
    default:
        return "no GL error";
    }
}

// True for the occurrences that are logged: the 1st, 10th, 100th, ...
bool count_seen(State &s, const std::string &key, u32 *count) {
    u32 &n = s.seen[key];
    n++;
    *count = n;
    u32 p = 1;
    while (p < n && p <= 100000000u) {
        p *= 10;
    }
    return p == n;
}

} // namespace

State &state() {
    mt_objc *rt = mt_objc_current();
    if (rt != g.rt || mt_cf::binding() != g.binding) {
        g.rt = rt;
        g.binding = mt_cf::binding();
        reset_state();
    }
    return g;
}

void misuse(State &s, const char *who, u32 error, const char *what, const char *detail) {
    s.stats.misuses++;
    if (s.error == k_gl_no_error) {
        s.error = error;
    }
    const std::string key = std::string(who) + ": " + what;
    u32 n = 0;
    if (!count_seen(s, key, &n)) {
        return;
    }
    const char *outcome = error != k_gl_no_error ? "the device ignores the call" : "skipped";
    if (n == 1) {
        log("%s%s%s (%s: %s)", key.c_str(), detail != nullptr ? ": " : "",
            detail != nullptr ? detail : "", error_name(error), outcome);
    } else {
        log("%s (%s: %s; %u times so far)", key.c_str(), error_name(error), outcome,
            static_cast<unsigned>(n));
    }
}

void note_once(State &s, const std::string &key, const char *fmt, ...) noexcept {
    u32 n = 0;
    if (!count_seen(s, "note: " + key, &n) || n != 1) {
        return;
    }
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log("%s", text);
}

void log(const char *fmt, ...) noexcept {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char line[540];
    std::snprintf(line, sizeof line, "gl: %s", text);
    mt_objc *rt = mt_objc_current();
    if (rt != nullptr) {
        mt_objc_impl::log_line(rt, line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
    if (std::FILE *f = trace_file()) {
        std::fprintf(f, "# %s\n", text);
    }
}

void trace_line(const char *fmt, ...) noexcept {
    std::FILE *f = trace_file();
    const bool channel = mt_trace_enabled("gl") != 0;
    if (f == nullptr && !channel) {
        return;
    }
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (channel) {
        mt_tracef("gl", "%s", text);
    }
    if (f != nullptr) {
        std::fputs(text, f);
        std::fputc('\n', f);
    }
}

mt_gfx_target *draw_target(State &s) {
    if (framebuffer_status(s) != k_gl_framebuffer_complete) {
        return nullptr;
    }
    return s.renderbuffers[s.framebuffers[s.framebuffer].color].target;
}

bool guest_readable(mt_cpu *cpu, u32 addr, std::uint64_t size) {
    if (size == 0) {
        return addr >= MT_GUEST_PAGE_ZERO_END && addr < cpu->mem_size;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(addr) + size; // exclusive
    if (addr < MT_GUEST_PAGE_ZERO_END || end > cpu->mem_size) {
        return false;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    // Every page of the range must be in a mapped region: the first byte, each page boundary
    // inside the range, the last byte.
    const auto mapped = [mem](std::uint64_t at) {
        const u32 r = mt_mem_describe(mem, static_cast<u32>(at), nullptr, 0);
        return r == MT_REGION_IMAGE || r == MT_REGION_HEAP || r == MT_REGION_STACK;
    };
    if (!mapped(addr) || !mapped(end - 1)) {
        return false;
    }
    for (std::uint64_t page = (static_cast<std::uint64_t>(addr) | 0xFFFu) + 1; page < end - 1;
         page += 0x1000) {
        if (!mapped(page)) {
            return false;
        }
    }
    return true;
}

u32 texture_handle(Texture &t) {
    if (t.gfx == 0) {
        t.gfx = mt_gfx_texture_create();
    }
    return t.gfx;
}

void context_created() { state().stats.contexts++; }

void set_current_context(u32 context) { state().current_context = context; }

bool renderbuffer_storage(u32 target, u32 width, u32 height, u32 format, bool retained) {
    State &s = state();
    const auto rb = s.renderbuffers.find(s.renderbuffer);
    if (target != k_gl_renderbuffer || s.renderbuffer == 0 || rb == s.renderbuffers.end()) {
        char detail[64];
        std::snprintf(detail, sizeof detail, "target 0x%X, renderbuffer %u",
                      static_cast<unsigned>(target), static_cast<unsigned>(s.renderbuffer));
        misuse(s, "-[EAGLContext renderbufferStorage:fromDrawable:]", k_gl_no_error,
               "no renderbuffer is bound", detail);
        return false;
    }
    u32 scale = g_config.render_scale != 0 ? g_config.render_scale : 1u;
    if (scale > MT_GFX_MAX_SCALE) {
        log("render scale %u is above %u: %u", static_cast<unsigned>(scale),
            static_cast<unsigned>(MT_GFX_MAX_SCALE), static_cast<unsigned>(MT_GFX_MAX_SCALE));
        scale = MT_GFX_MAX_SCALE;
    }
    Renderbuffer &r = rb->second;
    mt_gfx_target_destroy(r.target);
    r.target = create_target(width, height, scale);
    if (r.target == nullptr) {
        log("renderbuffer %u: no mt_gfx target of %ux%u at scale %u: nothing will be drawn",
            static_cast<unsigned>(s.renderbuffer), static_cast<unsigned>(width),
            static_cast<unsigned>(height), static_cast<unsigned>(scale));
    }
    r.width = width;
    r.height = height;
    r.format = format;
    r.retained = retained;
    trace_line("renderbuffer %u: storage %ux%u, format 0x%X, retained backing %s, target scale %u",
               static_cast<unsigned>(s.renderbuffer), static_cast<unsigned>(width),
               static_cast<unsigned>(height), static_cast<unsigned>(format),
               retained ? "YES" : "NO", static_cast<unsigned>(scale));
    return true;
}

bool present(mt_cpu *cpu, u32 target) {
    State &s = state();
    const auto rb = s.renderbuffers.find(s.renderbuffer);
    if (target != k_gl_renderbuffer || rb == s.renderbuffers.end() || rb->second.width == 0) {
        char detail[64];
        std::snprintf(detail, sizeof detail, "target 0x%X, renderbuffer %u",
                      static_cast<unsigned>(target), static_cast<unsigned>(s.renderbuffer));
        misuse(s, "-[EAGLContext presentRenderbuffer:]", k_gl_no_error,
               "the renderbuffer has no storage", detail);
        return false;
    }
    mt_gfx_end_frame();
    mt_gles_frame f{};
    f.index = ++s.stats.frames;
    f.renderbuffer = s.renderbuffer;
    f.width = rb->second.width;
    f.height = rb->second.height;
    f.format = rb->second.format;
    f.retained_backing = rb->second.retained ? 1u : 0u;
    f.calls = s.frame_calls;
    f.draws = s.frame_draws;
    f.uploads = s.frame_uploads;
    f.target = rb->second.target;
    s.frame_calls = s.frame_draws = s.frame_uploads = 0;
    trace_line("present frame %u: %u calls, %u draws, %u uploads", static_cast<unsigned>(f.index),
               static_cast<unsigned>(f.calls), static_cast<unsigned>(f.draws),
               static_cast<unsigned>(f.uploads));
    if (std::FILE *file = trace_file()) {
        std::fflush(file);
    }
    if (s.stats.first_frame == 0 && f.draws != 0) {
        s.stats.first_frame = f.index;
        char detail[128];
        std::snprintf(detail, sizeof detail,
                      "presentRenderbuffer: frame %u, %ux%u, %u GL calls, %u draws, %u uploads",
                      static_cast<unsigned>(f.index), static_cast<unsigned>(f.width),
                      static_cast<unsigned>(f.height), static_cast<unsigned>(f.calls),
                      static_cast<unsigned>(f.draws), static_cast<unsigned>(f.uploads));
        mt_process_milestone("first-frame", f.index, detail);
    }
    if (g_config.present != nullptr && g_config.present(g_config.user, cpu, &f) != 0) {
        mt_process_stop(cpu, "presentRenderbuffer: the host ended the run");
    }
    return true;
}

} // namespace mt_gles

using namespace mt_gles;

extern "C" {

#define MT_GLES_HLE_DEF(name, argc)                                                                \
    void mt_hle_gl##name(mt_cpu *cpu, std::uint32_t) noexcept { call(cpu, MT_GLES_##name); }
MT_GLES_OPS(MT_GLES_HLE_DEF)
#undef MT_GLES_HLE_DEF

void mt_gles_configure(const mt_gles_config *cfg) noexcept {
    g_config = cfg != nullptr ? *cfg : mt_gles_config{};
    g_trace.configured = g_config.trace_file != nullptr;
    g_trace.configured_path = g_trace.configured ? g_config.trace_file : "";
    g_config.trace_file = nullptr; // copied: the caller's string need not stay
    g_trace.resolved = false;
}

uint32_t mt_gles_set_render_scale(uint32_t scale) noexcept {
    scale = std::clamp<u32>(scale != 0 ? scale : 1u, 1u, MT_GFX_MAX_SCALE);
    g_config.render_scale = scale;
    State &s = state();
    u32 remade = 0;
    for (auto &[name, r] : s.renderbuffers) {
        mt_gfx_target *old = r.target;
        if (old == nullptr || mt_gfx_target_scale(old) == scale) {
            continue;
        }
        mt_gfx_target *next = create_target(r.width, r.height, scale);
        if (next == nullptr) {
            log("renderbuffer %u: no mt_gfx target of %ux%u at scale %u: it keeps scale %u",
                static_cast<unsigned>(name), static_cast<unsigned>(r.width),
                static_cast<unsigned>(r.height), static_cast<unsigned>(scale),
                static_cast<unsigned>(mt_gfx_target_scale(old)));
            continue;
        }
        // The retained frame, resampled: the pixel under each new pixel's centre.
        const u32 ow = mt_gfx_target_pixel_width(old), oh = mt_gfx_target_pixel_height(old);
        const u32 nw = mt_gfx_target_pixel_width(next), nh = mt_gfx_target_pixel_height(next);
        const std::uint8_t *src = mt_gfx_target_cpixels(old);
        std::uint8_t *dst = mt_gfx_target_pixels(next);
        if (src != nullptr && dst != nullptr && ow != 0 && oh != 0) {
            std::vector<u32> cols(nw);
            for (u32 x = 0; x < nw; x++) {
                cols[x] =
                    static_cast<u32>((std::uint64_t{x} * 2 + 1) * ow / (std::uint64_t{nw} * 2));
            }
            for (u32 y = 0; y < nh; y++) {
                const u32 sy =
                    static_cast<u32>((std::uint64_t{y} * 2 + 1) * oh / (std::uint64_t{nh} * 2));
                const std::uint8_t *row = src + static_cast<std::size_t>(sy) * ow * 4;
                std::uint8_t *out = dst + static_cast<std::size_t>(y) * nw * 4;
                for (u32 x = 0; x < nw; x++) {
                    std::memcpy(out + std::size_t{x} * 4, row + std::size_t{cols[x]} * 4, 4);
                }
            }
        }
        mt_gfx_target_destroy(old);
        r.target = next;
        remade++;
        trace_line("renderbuffer %u: target scale %u (render scale changed)",
                   static_cast<unsigned>(name), static_cast<unsigned>(scale));
    }
    return remade;
}

mt_gles_stats mt_gles_get_stats(void) noexcept {
    State &s = state();
    mt_gles_stats st = s.stats;
    st.current_context = s.current_context;
    st.textures = 0;
    for (const auto &t : s.textures) {
        st.textures += t.first != 0 ? 1u : 0u;
    }
    st.texture_bytes = s.texture_bytes;
    st.bound_framebuffer = s.framebuffer;
    st.bound_renderbuffer = s.renderbuffer;
    st.bound_texture = s.texture;
    std::memcpy(st.viewport, s.viewport, sizeof st.viewport);
    st.error = s.error;
    return st;
}

uint32_t mt_gles_get_error(void) noexcept {
    State &s = state();
    const u32 e = s.error;
    s.error = k_gl_no_error;
    return e;
}

const char *mt_gles_op_name(uint32_t op) noexcept {
    return op < MT_GLES_OP_COUNT ? k_names[op] : nullptr;
}

void mt_gles_reset(void) noexcept {
    state();
    reset_state();
}

} // extern "C"
