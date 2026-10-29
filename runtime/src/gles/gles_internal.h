// OpenGL ES 1.1 and EAGL, recorded (#117, ADR 0038, maplethief/gles.h): what gles.cpp (the GL
// imports and the state tracker) and eagl.cpp (EAGLContext) share. The HLE functions are
// listed by import name in runtime/src/hle/registry.cpp, EAGLContext in
// runtime/src/objc/classes.cpp.
#ifndef MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H
#define MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H

#include <cstdint>

#include "maplethief/gles.h"
#include "maplethief/objc/host.h"

extern "C" {
// One HLE function per GL import: mt_hle_glBindTexture, ...
#define MT_GLES_HLE_DECL(name, argc) void mt_hle_gl##name(mt_cpu *cpu, std::uint32_t id) noexcept;
MT_GLES_OPS(MT_GLES_HLE_DECL)
#undef MT_GLES_HLE_DECL
}

namespace mt_gles {

using u32 = std::uint32_t;

// GL enums the tracker and EAGL use.
enum : u32 {
    k_gl_texture_2d = 0x0DE1,
    k_gl_framebuffer = 0x8D40,  // GL_FRAMEBUFFER_OES
    k_gl_renderbuffer = 0x8D41, // GL_RENDERBUFFER_OES
    k_gl_rgba8 = 0x8058,        // GL_RGBA8_OES
    k_gl_rgb565 = 0x8D62,       // GL_RGB565_OES
};

// Logs "gl: <message>" through the ObjC runtime's log sink (always, not a trace channel):
// misuse and EAGL events.
void log(const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// -[EAGLContext initWithAPI:] made a context; -setCurrentContext: changed the current one.
void context_created();
void set_current_context(u32 context);

// -[EAGLContext renderbufferStorage:fromDrawable:]: storage of width x height pixels in
// `format` for the bound renderbuffer. False (and logged) if `target` is not
// GL_RENDERBUFFER_OES or no renderbuffer is bound.
bool renderbuffer_storage(u32 target, u32 width, u32 height, u32 format, bool retained);

// -[EAGLContext presentRenderbuffer:]: ends the frame. False if `target` is not
// GL_RENDERBUFFER_OES or the bound renderbuffer has no storage. Records the first frame with
// draws as the boot milestone "first-frame"; calls the present hook, and stops the process
// if it asks to.
bool present(mt_cpu *cpu, u32 target);

// EAGLContext (eagl.cpp).
extern const mt_objc_host_class k_eaglcontext;

} // namespace mt_gles

#endif // MAPLETHIEF_RUNTIME_GLES_GLES_INTERNAL_H
