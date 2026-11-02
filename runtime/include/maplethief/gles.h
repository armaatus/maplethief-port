/* OpenGL ES 1.1 and EAGL (#117 ADR 0038, #56 ADR 0040): the GL imports and EAGLContext accept
 * every call the game makes, keep the state the game reads back, and execute GL ES 1.1's
 * fixed-function pipeline on the rendering backend mt_gfx (maplethief/gfx.h, ADR 0039).
 * Presentation of the frames in a window is the host's (#58), behind the present hook.
 *
 * Four layers, one per job:
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
 *            glGetRenderbufferParameterivOES) from that state, truthfully. A call the device
 *            would reject with a GL error changes nothing, as on the device.
 *   execute  the fixed-function pipeline (runtime/src/gles/fixed.cpp), always on: the matrix
 *            stacks with their values (GLfixed is value / 65536), the client arrays, the
 *            enables, the constant colour, the blend function and the textures (each texture
 *            object is an mt_gfx texture, each renderbuffer with storage an mt_gfx target).
 *            Every glDrawArrays reads its client arrays from guest memory (bounds-checked),
 *            transforms them to clip space and submits one mt_gfx_draw to the target of the
 *            bound framebuffer's colour attachment.
 *   observe  then mt_gles_config.command sees the command, inside the guest's call, while
 *            the guest memory it points at (client arrays, pixels) is valid (traces, tests).
 *
 * Errors. The game does not import glGetError, but GL's error flag is kept: the first error
 * sticks until mt_gles_get_error reads it. Every call the device would reject is a "misuse":
 * counted (mt_gles_stats.misuses) and logged once per kind ("gl: glPushMatrix: stack
 * overflow ..."), then again at 10, 100, 1000, ... occurrences; never fatal. A client array
 * outside mapped guest memory is a misuse too (the draw is skipped). What the device does and
 * the emulation does not (yet): a texture format #57 adds, an enable that would change pixels,
 * is logged once.
 *
 * EAGL. EAGLContext is a host class (runtime/src/gles/eagl.cpp): -initWithAPI: accepts
 * kEAGLRenderingAPIOpenGLES1 (boot milestone "eagl-context"), +setCurrentContext: keeps the
 * current context, -renderbufferStorage:fromDrawable: gives the bound renderbuffer the size of
 * the CAEAGLLayer's bounds and the colour format of its drawableProperties, and makes its
 * mt_gfx target (that logical size, at mt_gles_config.render_scale), and
 * -presentRenderbuffer: ends a frame: mt_gfx_end_frame, then mt_gles_config.present gets the
 * frame's summary and its target. The first present after at least one draw call is the boot
 * milestone "first-frame".
 *
 * Trace: MAPLETHIEF_TRACE=gl logs every GL call with its decoded arguments and every present,
 * e.g. "gl: glBindTexture(GL_TEXTURE_2D, 3)", "gl: glColor4x(1, 1, 1, 0.5)",
 * "gl: glDrawArrays(GL_TRIANGLE_STRIP, 0, 4): 4 vertices, drawn",
 * "gl: present frame 1: 212 calls, 40 draws, 3 uploads". A glTexImage2D line ends with its
 * call site (" from 0x987E": lr - 4, the BL/BLX before the return address).
 * The GL trace file: MAPLETHIEF_GL_TRACE_FILE=<path> (or mt_gles_config.trace_file) writes the
 * same lines without the "gl: " prefix to a text file, for offline debugging (local only: it
 * grows by megabytes a minute). It is opened (truncated) at the first GL call after
 * mt_gles_configure, kept open across processes (a new process's state starts with a
 * "# new GL state" line) and flushed at every present.
 *
 * The state is per process: a new current ObjC runtime (a new process) starts from scratch,
 * and the old one's mt_gfx textures and targets are freed. Single guest thread (ADR 0033).
 * C17 that also compiles as C++20. */
#ifndef MAPLETHIEF_GLES_H
#define MAPLETHIEF_GLES_H

#include <stdint.h>

#include "maplethief/gfx.h"
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
    /* The presented renderbuffer's mt_gfx target: its logical size is width x height, its
     * pixels (RGBA8, top row first) that times mt_gfx_target_scale. Valid until that
     * renderbuffer gets new storage, mt_gles_reset or a new process. */
    const mt_gfx_target *target;
} mt_gles_frame;

typedef struct mt_gles_config {
    /* After the state tracker and the pipeline executed a command, inside the guest's call
     * (its pointers are valid only now): an observer (traces, tests). NULL: none. */
    void (*command)(void *user, mt_cpu *cpu, const mt_gles_cmd *cmd);
    /* -[EAGLContext presentRenderbuffer:]. Return 0 to go on; anything else ends the run there
     * (mt_process_stop, "presentRenderbuffer: the host ended the run"). NULL: go on. */
    int (*present)(void *user, mt_cpu *cpu, const mt_gles_frame *frame);
    void *user;
    /* The internal scale of the renderbuffers' targets (1 to MT_GFX_MAX_SCALE; 0 means 1),
     * applied when a renderbuffer gets storage. GL and EAGL keep seeing the logical size. */
    uint32_t render_scale;
    /* The GL trace file's path. NULL: MAPLETHIEF_GL_TRACE_FILE from the environment; "": none. */
    const char *trace_file;
} mt_gles_config;

/* Installs the hooks and settings (NULL: no hooks, scale 1, the trace file from the
 * environment). Kept across processes. */
void mt_gles_configure(const mt_gles_config *cfg) MT_NOEXCEPT;

/* Changes mt_gles_config.render_scale while the game runs (render_scale = auto, #152, ADR
 * 0080): every renderbuffer with storage gets a new mt_gfx target of the same logical size at
 * `scale` (1 to MT_GFX_MAX_SCALE; 0 means 1), its pixels resampled from the old one (nearest:
 * each new pixel takes the old pixel under its centre), since the game never clears and expects
 * a retained frame; later storage uses it too. GL and EAGL see no change. Call it between guest
 * GL calls (a host hook), never from inside one. A mt_gles_frame's target is invalid after it.
 * Returns how many targets were remade (0 also when the scale was already `scale`). */
uint32_t mt_gles_set_render_scale(uint32_t scale) MT_NOEXCEPT;

/* A host transform of the draws' clip-space x, after the guest's matrices (widescreen #86, ADR
 * 0090: the backdrop stretched to the view's width): x' = scale * x + offset * w, until it is set
 * back to (1, 0), the default. The guest never sees it. Guest thread, between GL calls. */
void mt_gles_set_host_x_transform(float scale, float offset) MT_NOEXCEPT;
/* The transform in effect: its scale and offset (tests). */
void mt_gles_get_host_x_transform(float *scale, float *offset) MT_NOEXCEPT;

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
    uint32_t misuses;       /* calls the device would have rejected with a GL error (logged) */
    uint32_t error;         /* GL's error flag now (mt_gles_get_error reads and clears it) */
    uint32_t draws_drawn;   /* glDrawArrays submitted to mt_gfx and drawn */
    uint32_t draws_skipped; /* glDrawArrays that drew nothing: no vertex array, count 0, no
                               complete framebuffer, an array outside guest memory,
                               GL_POINTS (mt_gfx does not draw them; lines it does) */
    uint32_t vertices;      /* vertices of the submitted draws */
} mt_gles_stats;

/* This process's numbers (zeros before the first GL call). */
mt_gles_stats mt_gles_get_stats(void) MT_NOEXCEPT;

/* glGetError: GL's error flag (GL_NO_ERROR 0, GL_INVALID_ENUM 0x0500, GL_INVALID_VALUE 0x0501,
 * GL_INVALID_OPERATION 0x0502, GL_STACK_OVERFLOW 0x0503, GL_STACK_UNDERFLOW 0x0504,
 * GL_INVALID_FRAMEBUFFER_OPERATION_OES 0x0506), then clears it. */
uint32_t mt_gles_get_error(void) MT_NOEXCEPT;

/* "glBindTexture" for MT_GLES_BindTexture; NULL for an unknown op. */
const char *mt_gles_op_name(uint32_t op) MT_NOEXCEPT;

/* Forgets every GL object (freeing its mt_gfx texture or target) and the counters (a new
 * process does this by itself). */
void mt_gles_reset(void) MT_NOEXCEPT;

/* A host filter of the draws (#159, ADR 0086). While one is installed it is called for every
 * glDrawArrays that reaches mt_gfx, with the bounding box of its vertices in normalised device
 * coordinates, box = {x0, y0, x1, y1} (after the projection: x to the right, y up, -1 to 1 over
 * the viewport). Returning 1 with m = {a, b, c, d, tx, ty} moves the draw's vertices by
 * x' = a x + c y + tx, y' = b x + d y + ty in those coordinates; returning 0 draws it as it is.
 * The platform installs one only around a hooked guest function (the field HUD's quick slots,
 * moved with a custom key pad layout), so every other draw, and every draw without a filter,
 * is unchanged. Guest thread only. */
typedef int (*mt_gles_draw_filter)(void *user, const float box[4], float m[6]);
/* Installs `filter` with `user` (NULL: none). */
void mt_gles_set_draw_filter(mt_gles_draw_filter filter, void *user) MT_NOEXCEPT;
/* The filter installed (NULL: none), and its `user` if `user` is not NULL. */
mt_gles_draw_filter mt_gles_get_draw_filter(void **user) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_GLES_H */
