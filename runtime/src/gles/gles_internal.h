// OpenGL ES 1.1 and EAGL (#117 ADR 0038, #56 ADR 0040, maplethief/gles.h): what gles.cpp (the GL
// imports, the state tracker and the GL objects' mt_gfx counterparts), fixed.cpp (the
// fixed-function pipeline: matrices, client arrays, enables, colour, blending, draws) and
// eagl.cpp (EAGLContext) share. The HLE functions are listed by import name in
// runtime/src/hle/registry.cpp, EAGLContext in runtime/src/objc/classes.cpp.
#ifndef MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H
#define MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "maplethief/gfx.h"
#include "maplethief/gles.h"
#include "maplethief/objc/host.h"

extern "C" {
// One HLE function per GL import: mt_hle_glBindTexture, ...
#define MT_GLES_HLE_DECL(name, argc) void mt_hle_gl##name(mt_cpu *cpu, std::uint32_t id) noexcept;
MT_GLES_OPS(MT_GLES_HLE_DECL)
#undef MT_GLES_HLE_DECL
}

namespace mt_gles {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

// GL enums the tracker, the pipeline and EAGL use.
enum : u32 {
    k_gl_texture_2d = 0x0DE1,
    k_gl_framebuffer = 0x8D40,  // GL_FRAMEBUFFER_OES
    k_gl_renderbuffer = 0x8D41, // GL_RENDERBUFFER_OES
    k_gl_rgba8 = 0x8058,        // GL_RGBA8_OES
    k_gl_rgb565 = 0x8D62,       // GL_RGB565_OES
    k_gl_modelview = 0x1700,
    k_gl_projection = 0x1701,
    k_gl_texture = 0x1702,
    // glGetError's values
    k_gl_no_error = 0,
    k_gl_invalid_enum = 0x0500,
    k_gl_invalid_value = 0x0501,
    k_gl_invalid_operation = 0x0502,
    k_gl_stack_overflow = 0x0503,
    k_gl_stack_underflow = 0x0504,
    k_gl_invalid_framebuffer_operation = 0x0506, // GL_INVALID_FRAMEBUFFER_OPERATION_OES
};

// GL ES 1.1's minimum stack depths (the iPhone's): modelview 16, projection and texture 2.
constexpr u32 k_modelview_depth = 16, k_projection_depth = 2, k_texture_depth = 2;

// --- the tracker's objects ------------------------------------------------------------------

struct Texture {
    bool bound = false; // bound at least once: a texture object (glIsTexture)
    u32 width = 0, height = 0, internal_format = 0, format = 0, type = 0, bytes = 0;
    u32 gfx = 0;                  // its mt_gfx texture (0: none yet)
    u32 min_filter = 0x2702;      // GL_NEAREST_MIPMAP_LINEAR (for completeness)
    bool generate_mipmap = false; // GL_GENERATE_MIPMAP
};
struct Renderbuffer {
    u32 width = 0, height = 0, format = 0;
    bool retained = false;
    mt_gfx_target *target = nullptr; // made at storage (EAGL), the draws' destination
};
struct Framebuffer {
    u32 color = 0, depth = 0, stencil = 0; // renderbuffer names
};

// --- the fixed-function state (fixed.cpp) ---------------------------------------------------

// A 4 x 4 float matrix, column-major as GL stores it: m[column * 4 + row]. Identity at first.
struct Matrix {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// glVertexPointer / glTexCoordPointer and the client state's enable.
struct ClientArray {
    bool enabled = false;
    u32 size = 4, type = 0x1406 /* GL_FLOAT */, stride = 0, pointer = 0;
};

struct Fixed {
    Matrix modelview[k_modelview_depth];
    Matrix projection[k_projection_depth];
    Matrix texture[k_texture_depth];
    ClientArray vertex, texcoord;
    bool normal_array = false, color_array = false, point_size_array = false;
    std::unordered_map<u32, bool> caps; // glEnable/glDisable of the caps without a field below
    bool texture_2d = false, blend = false;
    u8 color[4] = {255, 255, 255, 255};
    u32 src_factor = 1 /* GL_ONE */, dst_factor = 0 /* GL_ZERO */;
    std::vector<mt_gfx_vertex> vertices; // the draw being assembled (reused)
};

// --- the state of one process ---------------------------------------------------------------

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
    u32 error = k_gl_no_error; // GL's error flag: the first error, until read
    Fixed fixed;
    // counters
    mt_gles_stats stats{};
    u32 frame_calls = 0, frame_draws = 0, frame_uploads = 0;
    u32 current_context = 0;
    // logging: how often each kind of misuse or note was seen (logged the first time, then at
    // 10, 100, 1000, ... times)
    std::unordered_map<std::string, u32> seen;
    // what the last glDrawArrays did, for the trace ("4 vertices, drawn")
    char draw_note[96] = {};
};

// The state of the current process (a new current ObjC runtime starts from scratch).
State &state();

// A call the device would reject with GL error `error` (0: an EAGL or memory misuse that sets
// no GL error): counted, the error flag set if it is clear, logged once per kind ("<who>:
// <what>", then again at 10, 100, ... occurrences), with `detail` appended the first time.
// `who` is the GL function ("glBindTexture") or the EAGL method.
void misuse(State &s, const char *who, u32 error, const char *what, const char *detail = nullptr);

// Something the emulation does not do (yet) that the device would: logged once per `key`.
void note_once(State &s, const std::string &key, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Logs "gl: <message>" through the ObjC runtime's log sink (always, not a trace channel):
// misuse and EAGL events.
void log(const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// A line for the trace channel "gl" and the GL trace file (MAPLETHIEF_GL_TRACE_FILE).
void trace_line(const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// The target the draws go to: that of the colour attachment of the bound framebuffer, or
// nullptr if the framebuffer is not complete.
mt_gfx_target *draw_target(State &s);

// True if `size` bytes at guest `addr` lie in guest memory that is mapped: one image segment,
// the heap below its top, or one guest stack (no fault on reading them).
bool guest_readable(mt_cpu *cpu, u32 addr, std::uint64_t size);

// The texture's mt_gfx texture, made the first time it is needed.
u32 texture_handle(Texture &t);

// --- fixed.cpp ------------------------------------------------------------------------------

// Executes a command the tracker accepted: the matrices, client arrays, enables, colour, blend
// function, texture parameters and uploads, and the draws.
void fixed_execute(mt_cpu *cpu, State &s, const mt_gles_cmd &c);

// GLfixed (16.16) as a float: value / 65536, rounded once.
float fixed_to_float(u32 v);

// sin and cos of `angle` GLfixed degrees, the same bits on every CPU (exact at multiples of 90).
void sincos_fixed_degrees(u32 angle, double *sin_out, double *cos_out);

// --- EAGL -----------------------------------------------------------------------------------

// -[EAGLContext initWithAPI:] made a context; -setCurrentContext: changed the current one.
void context_created();
void set_current_context(u32 context);

// -[EAGLContext renderbufferStorage:fromDrawable:]: storage of width x height pixels in
// `format` for the bound renderbuffer, and its mt_gfx target (that logical size at the
// configured render scale). False (and logged) if `target` is not GL_RENDERBUFFER_OES or no
// renderbuffer is bound.
bool renderbuffer_storage(u32 target, u32 width, u32 height, u32 format, bool retained);

// -[EAGLContext presentRenderbuffer:]: ends the frame (mt_gfx_end_frame). False if `target` is
// not GL_RENDERBUFFER_OES or the bound renderbuffer has no storage. Records the first frame
// with draws as the boot milestone "first-frame"; calls the present hook, and stops the process
// if it asks to.
bool present(mt_cpu *cpu, u32 target);

// EAGLContext (eagl.cpp).
extern const mt_objc_host_class k_eaglcontext;

// Save states (#203, ADR 0105, gles_snapshot.cpp): the current context EAGL holds (retained),
// and a restore's (no retain or release: the retain counts are the state's).
u32 eagl_current();
void eagl_restore_current(u32 context);

} // namespace mt_gles

#endif // MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H
