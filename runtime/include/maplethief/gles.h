/* OpenGL ES 1.1 and EAGL, recorded (#117, ADR 0038): the GL imports and EAGLContext accept
 * every call the game makes, keep the state the game reads back, and draw nothing. Real
 * rendering (#56 fixed-function emulation, #58 EAGL framebuffer and presentation) plugs in
 * behind the same entry points.
 *
 * Three layers, one per job:
 *
 *   decode   one HLE function per gl* import (runtime/src/gles/gles.cpp) reads the guest's
 *            arguments into an mt_gles_cmd: the op and its argument words exactly as the
 *            guest passed them (GLfixed and GLenum as integers, GLfloat as its bits, pointers
 *            as guest addresses).
 *   state    the runtime's state tracker applies the command: object names (textures,
 *            framebuffers, renderbuffers), the bound framebuffer, renderbuffer and 2D texture,
 *            each texture's level-0 size, each renderbuffer's storage, the colour attachment
 *            of each framebuffer, the matrix mode and stack depths, the viewport. It answers
 *            every query the game makes (glGen*, glIsTexture, glCheckFramebufferStatusOES,
 *            glGetRenderbufferParameterivOES) from that state, truthfully.
 *   sink     then mt_gles_config.command sees the command, inside the guest's call, while
 *            the guest memory it points at (client arrays, pixels) is valid. The recorder
 *            installs none; #56 executes the commands on its backend there.
 *
 * EAGL. EAGLContext is a host class (runtime/src/gles/eagl.cpp): -initWithAPI: accepts
 * kEAGLRenderingAPIOpenGLES1 (boot milestone "eagl-context"), +setCurrentContext: keeps the
 * current context, -renderbufferStorage:fromDrawable: gives the bound renderbuffer the size of
 * the CAEAGLLayer's bounds and the colour format of its drawableProperties, and
 * -presentRenderbuffer: ends a frame: mt_gles_config.present gets the frame's summary. The
 * first present after at least one draw call is the boot milestone "first-frame".
 *
 * Trace: MAPLETHIEF_TRACE=gl logs every GL call with its arguments and every present, e.g.
 * "gl: glBindTexture(0x0DE1, 3)", "gl: present frame 1: 212 calls, 40 draws".
 *
 * The state is per process: a new current ObjC runtime (a new process) starts from scratch.
 * Single guest thread (ADR 0033). C17 that also compiles as C++20. */
#ifndef MAPLETHIEF_GLES_H
#define MAPLETHIEF_GLES_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The GL functions the game imports (docs/re/imports.md), with their argument counts. */
#define MT_GLES_OPS(X)                                                                             \
    X(BindFramebufferOES, 2)                                                                       \
    X(BindRenderbufferOES, 2)                                                                      \
    X(BindTexture, 2)                                                                              \
    X(BlendFunc, 2)                                                                                \
    X(CheckFramebufferStatusOES, 1)                                                                \
    X(Color4x, 4)                                                                                  \
    X(DeleteTextures, 2)                                                                           \
    X(Disable, 1)                                                                                  \
    X(DisableClientState, 1)                                                                       \
    X(DrawArrays, 3)                                                                               \
    X(Enable, 1)                                                                                   \
    X(EnableClientState, 1)                                                                        \
    X(FramebufferRenderbufferOES, 4)                                                               \
    X(GenFramebuffersOES, 2)                                                                       \
    X(GenRenderbuffersOES, 2)                                                                      \
    X(GenTextures, 2)                                                                              \
    X(GetRenderbufferParameterivOES, 3)                                                            \
    X(IsTexture, 1)                                                                                \
    X(LoadIdentity, 0)                                                                             \
    X(MatrixMode, 1)                                                                               \
    X(Orthox, 6)                                                                                   \
    X(PopMatrix, 0)                                                                                \
    X(PushMatrix, 0)                                                                               \
    X(Rotatex, 4)                                                                                  \
    X(Scalef, 3)                                                                                   \
    X(TexCoordPointer, 4)                                                                          \
    X(TexImage2D, 9)                                                                               \
    X(TexParameteri, 3)                                                                            \
    X(Translatex, 3)                                                                               \
    X(VertexPointer, 4)                                                                            \
    X(Viewport, 4)

typedef enum mt_gles_op {
#define MT_GLES_OP_ENUM(name, argc) MT_GLES_##name,
    MT_GLES_OPS(MT_GLES_OP_ENUM)
#undef MT_GLES_OP_ENUM
        MT_GLES_OP_COUNT
} mt_gles_op;

#define MT_GLES_MAX_ARGS 9u

/* One GL call. `result` is what the call returns to the guest (queries, glCheck..., 0 for
 * void functions), already decided by the state tracker. */
typedef struct mt_gles_cmd {
    uint32_t op; /* enum mt_gles_op */
    uint32_t argc;
    uint32_t args[MT_GLES_MAX_ARGS];
    uint32_t result;
} mt_gles_cmd;

/* A frame: what -presentRenderbuffer: shows. */
typedef struct mt_gles_frame {
    uint32_t index;        /* 1 for the first present */
    uint32_t renderbuffer; /* the bound renderbuffer that is presented */
    uint32_t width, height;
    uint32_t format; /* GL_RGBA8_OES or GL_RGB565_OES */
    /* kEAGLDrawablePropertyRetainedBacking: 1 if the contents must survive the present. The
     * game says NO and never calls glClear: it redraws the whole screen every frame. */
    uint32_t retained_backing;
    uint32_t calls;   /* GL calls since the previous present */
    uint32_t draws;   /* glDrawArrays calls since the previous present */
    uint32_t uploads; /* glTexImage2D calls since the previous present */
} mt_gles_frame;

typedef struct mt_gles_config {
    /* After the state tracker applied a command, inside the guest's call (its pointers are
     * valid only now). NULL: nothing (the recorder). */
    void (*command)(void *user, mt_cpu *cpu, const mt_gles_cmd *cmd);
    /* -[EAGLContext presentRenderbuffer:]. Return 0 to go on; anything else ends the run there
     * (mt_process_stop, "presentRenderbuffer: the host ended the run"). NULL: go on. */
    int (*present)(void *user, mt_cpu *cpu, const mt_gles_frame *frame);
    void *user;
} mt_gles_config;

/* Installs the hooks (NULL: none). Kept across processes. */
void mt_gles_configure(const mt_gles_config *cfg) MT_NOEXCEPT;

typedef struct mt_gles_stats {
    uint32_t calls;                      /* every GL call of this process */
    uint32_t op_calls[MT_GLES_OP_COUNT]; /* by op */
    uint32_t frames;                     /* presents */
    uint32_t first_frame;                /* index of the first frame with draws, 0 if none yet */
    uint32_t contexts;                   /* EAGLContexts created */
    uint32_t current_context;            /* the current EAGLContext (guest object), 0 if none */
    uint32_t textures;                   /* texture names alive */
    uint32_t texture_bytes;              /* level-0 bytes of the alive textures */
    uint32_t bound_framebuffer, bound_renderbuffer, bound_texture;
    uint32_t viewport[4];
    uint32_t misuses; /* calls the device would have rejected with a GL error (logged) */
} mt_gles_stats;

/* This process's numbers (zeros before the first GL call). */
mt_gles_stats mt_gles_get_stats(void) MT_NOEXCEPT;

/* "glBindTexture" for MT_GLES_BindTexture; NULL for an unknown op. */
const char *mt_gles_op_name(uint32_t op) MT_NOEXCEPT;

/* Forgets every GL object and the counters (a new process does this by itself). */
void mt_gles_reset(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_GLES_H */
