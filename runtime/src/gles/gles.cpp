// The OpenGL ES 1.1 imports, recorded (#117, ADR 0038, maplethief/gles.h): each HLE function
// decodes its guest arguments into an mt_gles_cmd, the state tracker applies it and answers
// the game's queries, then the command hook (#56's backend; none for the recorder) sees it.
//
// The tracker keeps what the game reads back or what a renderer will need at a present:
//   - names: textures, framebuffers and renderbuffers made by glGen* or by binding an unused
//     name (GL ES 1.1 creates the object then); glDeleteTextures frees them
//   - bindings: the framebuffer, the renderbuffer, the 2D texture
//   - each texture's level-0 size and format (glTexImage2D), each renderbuffer's storage
//     (EAGL), each framebuffer's colour attachment
//   - the matrix mode and the depth of each matrix stack, the viewport, the frame counters
// glIsTexture answers as GL ES 1.1 does: a name is a texture once it has been bound, until it
// is deleted. Calls the device would reject with a GL error are counted and logged, and change
// nothing, as on the device.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

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
    k_gl_modelview = 0x1700,
    k_gl_projection = 0x1701,
    k_gl_texture = 0x1702,
    k_gl_alpha = 0x1906,
    k_gl_rgb = 0x1907,
    k_gl_rgba = 0x1908,
    k_gl_luminance = 0x1909,
    k_gl_luminance_alpha = 0x190A,
    k_gl_unsigned_byte = 0x1401,
};

constexpr u32 k_max_names_per_call = 4096;

struct Texture {
    bool bound = false; // bound at least once: a texture object (glIsTexture)
    u32 width = 0, height = 0, internal_format = 0, format = 0, type = 0, bytes = 0;
};
struct Renderbuffer {
    u32 width = 0, height = 0, format = 0;
    bool retained = false;
};
struct Framebuffer {
    u32 color = 0, depth = 0, stencil = 0; // renderbuffer names
};

struct State {
    mt_objc *rt = nullptr; // the process (its ObjC runtime) the state belongs to
    u32 binding = 0;       // and CF's binding to it (a new runtime at the same address)
    std::unordered_map<u32, Texture> textures;
    std::unordered_map<u32, Renderbuffer> renderbuffers;
    std::unordered_map<u32, Framebuffer> framebuffers;
    u32 next_texture = 1, next_renderbuffer = 1, next_framebuffer = 1;
    u32 framebuffer = 0, renderbuffer = 0, texture = 0;
    u32 matrix_mode = k_gl_modelview;
    u32 depth[3] = {1, 1, 1}; // modelview, projection, texture
    u32 viewport[4] = {0, 0, 0, 0};
    u32 texture_bytes = 0;
    // counters
    mt_gles_stats stats{};
    u32 frame_calls = 0, frame_draws = 0, frame_uploads = 0;
    u32 current_context = 0;
};

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

void reset_state() {
    mt_objc *rt = g.rt;
    const u32 binding = g.binding;
    g = State{};
    g.rt = rt;
    g.binding = binding;
}

// The state of the current process: a new current ObjC runtime (also one at the old one's host
// address: CF rebinds to it) starts from scratch.
State &state() {
    mt_objc *rt = mt_objc_current();
    if (rt != g.rt || mt_cf::binding() != g.binding) {
        g.rt = rt;
        g.binding = mt_cf::binding();
        reset_state();
    }
    return g;
}

void misuse(State &s, const mt_gles_cmd &c, const char *what) {
    s.stats.misuses++;
    log("%s: %s (the device sets a GL error and ignores the call)", k_names[c.op], what);
}

u32 rd32(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, mt_g2h(cpu, addr, 4), 4);
    return v;
}
void wr32(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(mt_g2h(cpu, addr, 4), &v, 4); }

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

// glGen*(n, names): fresh names from `next`, skipping names in use.
template <typename Map> void gen(mt_cpu *cpu, State &s, const mt_gles_cmd &c, Map &map, u32 &next) {
    const auto n = static_cast<std::int32_t>(c.args[0]);
    if (n < 0 || static_cast<u32>(n) > k_max_names_per_call) {
        misuse(s, c, "n is negative or implausibly large");
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
// GL ES 1.1's minimum stack depths (the iPhone's): modelview 16, projection and texture 2.
u32 stack_max(const State &s) { return s.matrix_mode == k_gl_modelview ? 16u : 2u; }

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

// Applies `c` to the state; sets c.result.
void apply(mt_cpu *cpu, State &s, mt_gles_cmd &c) {
    const u32 *a = c.args;
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
            misuse(s, c, "n is negative or implausibly large");
            break;
        }
        for (u32 i = 0; i < static_cast<u32>(n); i++) {
            const u32 name = rd32(cpu, a[1] + 4u * i);
            const auto t = s.textures.find(name);
            if (name == 0 || t == s.textures.end()) {
                continue; // unused names and 0 are silently ignored
            }
            s.texture_bytes -= t->second.bytes;
            s.textures.erase(t);
            if (s.texture == name) {
                s.texture = 0;
            }
        }
        break;
    }
    case MT_GLES_BindTexture:
        if (a[0] != k_gl_texture_2d) {
            misuse(s, c, "target is not GL_TEXTURE_2D");
            break;
        }
        s.texture = a[1];
        if (a[1] != 0) {
            s.textures[a[1]].bound = true;
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
            misuse(s, c, "target is not GL_TEXTURE_2D");
            break;
        }
        const auto w = static_cast<std::int32_t>(a[3]), h = static_cast<std::int32_t>(a[4]);
        if (w < 0 || h < 0 || w > 2048 || h > 2048 || a[5] != 0) {
            misuse(s, c, "size out of range or border not 0");
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
            misuse(s, c, "target is not GL_FRAMEBUFFER_OES");
            break;
        }
        s.framebuffer = a[1];
        if (a[1] != 0) {
            s.framebuffers[a[1]];
        }
        break;
    case MT_GLES_BindRenderbufferOES:
        if (a[0] != k_gl_renderbuffer) {
            misuse(s, c, "target is not GL_RENDERBUFFER_OES");
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
            misuse(s, c, "no framebuffer is bound");
            break;
        }
        if (a[3] != 0 && (a[2] != k_gl_renderbuffer || s.renderbuffers.count(a[3]) == 0)) {
            misuse(s, c, "no such renderbuffer");
            break;
        }
        if (a[1] == k_gl_color_attachment0) {
            fb->second.color = a[3];
        } else if (a[1] == k_gl_depth_attachment) {
            fb->second.depth = a[3];
        } else if (a[1] == k_gl_stencil_attachment) {
            fb->second.stencil = a[3];
        } else {
            misuse(s, c, "unknown attachment");
        }
        break;
    }
    case MT_GLES_CheckFramebufferStatusOES:
        if (a[0] != k_gl_framebuffer) {
            misuse(s, c, "target is not GL_FRAMEBUFFER_OES");
            break;
        }
        c.result = framebuffer_status(s);
        break;
    case MT_GLES_GetRenderbufferParameterivOES: {
        // (target, pname, params)
        const auto rb = s.renderbuffers.find(s.renderbuffer);
        if (a[0] != k_gl_renderbuffer || s.renderbuffer == 0 || rb == s.renderbuffers.end()) {
            misuse(s, c, "no renderbuffer is bound");
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
            misuse(s, c, "unknown parameter");
            return;
        }
        wr32(cpu, a[2], v);
        break;
    }
    case MT_GLES_MatrixMode:
        if (a[0] != k_gl_modelview && a[0] != k_gl_projection && a[0] != k_gl_texture) {
            misuse(s, c, "unknown matrix mode");
            break;
        }
        s.matrix_mode = a[0];
        break;
    case MT_GLES_PushMatrix: {
        u32 *d = stack_depth(s);
        if (*d >= stack_max(s)) {
            misuse(s, c, "stack overflow");
            break;
        }
        (*d)++;
        break;
    }
    case MT_GLES_PopMatrix: {
        u32 *d = stack_depth(s);
        if (*d <= 1) {
            misuse(s, c, "stack underflow");
            break;
        }
        (*d)--;
        break;
    }
    case MT_GLES_Viewport:
        if (static_cast<std::int32_t>(a[2]) < 0 || static_cast<std::int32_t>(a[3]) < 0) {
            misuse(s, c, "negative size");
            break;
        }
        std::memcpy(s.viewport, a, sizeof s.viewport);
        break;
    case MT_GLES_DrawArrays:
        if (static_cast<std::int32_t>(a[2]) < 0) {
            misuse(s, c, "negative count");
            break;
        }
        s.frame_draws++;
        break;
    // State a renderer needs, not read back by the game: the command hook sees it.
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

// "glScalef(0.5, 0.5, 1)", "glOrthox(0, 480, ...)" (GLfixed as numbers), else hex words.
void trace(const mt_gles_cmd &c) {
    if (mt_trace_enabled("gl") == 0) {
        return;
    }
    char text[256];
    int n = std::snprintf(text, sizeof text, "%s(", k_names[c.op]);
    const bool is_float = c.op == MT_GLES_Scalef;
    const bool is_fixed = c.op == MT_GLES_Color4x || c.op == MT_GLES_Orthox ||
                          c.op == MT_GLES_Rotatex || c.op == MT_GLES_Translatex;
    for (u32 i = 0; i < c.argc && n > 0 && static_cast<std::size_t>(n) < sizeof text; i++) {
        const std::size_t left = sizeof text - static_cast<std::size_t>(n);
        const char *sep = i != 0 ? ", " : "";
        if (is_float) {
            float f;
            std::memcpy(&f, &c.args[i], 4);
            n += std::snprintf(text + n, left, "%s%g", sep, static_cast<double>(f));
        } else if (is_fixed) {
            n += std::snprintf(text + n, left, "%s%g", sep,
                               static_cast<double>(static_cast<std::int32_t>(c.args[i])) / 65536.0);
        } else {
            n += std::snprintf(text + n, left, "%s0x%X", sep, static_cast<unsigned>(c.args[i]));
        }
    }
    const bool returns = c.op == MT_GLES_IsTexture || c.op == MT_GLES_CheckFramebufferStatusOES;
    if (returns) {
        mt_tracef("gl", "%s) -> 0x%X", text, static_cast<unsigned>(c.result));
    } else {
        mt_tracef("gl", "%s)", text);
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
    apply(cpu, s, c);
    trace(c);
    if (g_config.command != nullptr) {
        g_config.command(g_config.user, cpu, &c);
    }
    mt_ret_u32(cpu, c.result);
}

} // namespace

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
}

void context_created() { state().stats.contexts++; }

void set_current_context(u32 context) { state().current_context = context; }

bool renderbuffer_storage(u32 target, u32 width, u32 height, u32 format, bool retained) {
    State &s = state();
    const auto rb = s.renderbuffers.find(s.renderbuffer);
    if (target != k_gl_renderbuffer || s.renderbuffer == 0 || rb == s.renderbuffers.end()) {
        s.stats.misuses++;
        log("-[EAGLContext renderbufferStorage:fromDrawable:]: target 0x%X, renderbuffer %u: "
            "no renderbuffer is bound",
            static_cast<unsigned>(target), static_cast<unsigned>(s.renderbuffer));
        return false;
    }
    rb->second.width = width;
    rb->second.height = height;
    rb->second.format = format;
    rb->second.retained = retained;
    mt_tracef("gl", "renderbuffer %u: storage %ux%u, format 0x%X, retained backing %s",
              static_cast<unsigned>(s.renderbuffer), static_cast<unsigned>(width),
              static_cast<unsigned>(height), static_cast<unsigned>(format),
              retained ? "YES" : "NO");
    return true;
}

bool present(mt_cpu *cpu, u32 target) {
    State &s = state();
    const auto rb = s.renderbuffers.find(s.renderbuffer);
    if (target != k_gl_renderbuffer || rb == s.renderbuffers.end() || rb->second.width == 0) {
        s.stats.misuses++;
        log("-[EAGLContext presentRenderbuffer:0x%X]: renderbuffer %u has no storage",
            static_cast<unsigned>(target), static_cast<unsigned>(s.renderbuffer));
        return false;
    }
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
    s.frame_calls = s.frame_draws = s.frame_uploads = 0;
    mt_tracef("gl", "present frame %u: %u calls, %u draws, %u uploads",
              static_cast<unsigned>(f.index), static_cast<unsigned>(f.calls),
              static_cast<unsigned>(f.draws), static_cast<unsigned>(f.uploads));
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
    return st;
}

const char *mt_gles_op_name(uint32_t op) noexcept {
    return op < MT_GLES_OP_COUNT ? k_names[op] : nullptr;
}

void mt_gles_reset(void) noexcept {
    state();
    reset_state();
}

} // extern "C"
