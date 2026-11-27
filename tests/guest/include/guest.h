/* The guest program's own declarations (#385, ADR 0213): the few iOS 5 APIs it calls, written
 * here from the public ABI so that no SDK header is needed. Only what the game itself imports
 * (docs/re/imports.md): the runtime implements nothing else. C and Objective-C. */
#ifndef GUEST_H
#define GUEST_H

typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef unsigned long size_t;
typedef signed char BOOL;

#ifdef __cplusplus
#define GUEST_C extern "C"
#else
#define GUEST_C extern
#endif

/* --- libSystem ---------------------------------------------------------------------------- */
GUEST_C int puts(const char *s);
GUEST_C int printf(const char *format, ...);
GUEST_C void *malloc(size_t n);
GUEST_C void free(void *p);
GUEST_C void *memcpy(void *dst, const void *src, size_t n);
GUEST_C int memcmp(const void *a, const void *b, size_t n);
GUEST_C void exit(int status) __attribute__((noreturn));

/* --- CoreFoundation ----------------------------------------------------------------------- */
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef struct __CFBundle *CFBundleRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef double CFAbsoluteTime;
typedef double CFTimeInterval;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef int32_t SInt32;
typedef uint32_t CFOptionFlags;
typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef timer, void *info);
typedef struct {
    CFIndex version;
    void *info;
    const void *(*retain)(const void *info);
    void (*release)(const void *info);
    CFStringRef (*copyDescription)(const void *info);
} CFRunLoopTimerContext;
#define kCFStringEncodingASCII 0x0600u

GUEST_C const CFAllocatorRef kCFAllocatorDefault;
GUEST_C const CFStringRef kCFRunLoopDefaultMode;
GUEST_C CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
GUEST_C CFRunLoopRef CFRunLoopGetCurrent(void);
GUEST_C CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, CFAbsoluteTime fireDate,
                                               CFTimeInterval interval, CFOptionFlags flags,
                                               CFIndex order, CFRunLoopTimerCallBack callout,
                                               CFRunLoopTimerContext *context);
GUEST_C void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer, CFStringRef mode);
GUEST_C CFBundleRef CFBundleGetMainBundle(void);
GUEST_C CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr,
                                              uint32_t encoding);
GUEST_C CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef bundle, CFStringRef name,
                                                        CFStringRef type, CFStringRef subDir,
                                                        CFStringRef localizationName);
GUEST_C Boolean CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef alloc, CFURLRef url,
                                                         CFDataRef *resourceData,
                                                         CFDictionaryRef *properties,
                                                         CFArrayRef desiredProperties,
                                                         SInt32 *errorCode);
GUEST_C const uint8_t *CFDataGetBytePtr(CFDataRef data);
GUEST_C CFIndex CFDataGetLength(CFDataRef data);
GUEST_C void CFRelease(CFTypeRef cf);

/* --- OpenGL ES 1.1 (the game's imports) ----------------------------------------------------- */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef int GLfixed;
typedef short GLshort;
#define GL_TRIANGLES 0x0004u
#define GL_TRIANGLE_STRIP 0x0005u
#define GL_SHORT 0x1402u
#define GL_MODELVIEW 0x1700u
#define GL_PROJECTION 0x1701u
#define GL_TEXTURE_2D 0x0DE1u
#define GL_BLEND 0x0BE2u
#define GL_VERTEX_ARRAY 0x8074u
#define GL_FRAMEBUFFER_OES 0x8D40u
#define GL_RENDERBUFFER_OES 0x8D41u
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42u
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43u
#define GL_COLOR_ATTACHMENT0_OES 0x8CE0u
#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5u

GUEST_C void glGenFramebuffersOES(GLsizei n, GLuint *ids);
GUEST_C void glGenRenderbuffersOES(GLsizei n, GLuint *ids);
GUEST_C void glBindFramebufferOES(GLenum target, GLuint id);
GUEST_C void glBindRenderbufferOES(GLenum target, GLuint id);
GUEST_C void glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum rbtarget,
                                          GLuint rb);
GUEST_C void glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *out);
GUEST_C GLenum glCheckFramebufferStatusOES(GLenum target);
GUEST_C void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
GUEST_C void glMatrixMode(GLenum mode);
GUEST_C void glLoadIdentity(void);
GUEST_C void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
GUEST_C void glDisable(GLenum cap);
GUEST_C void glEnableClientState(GLenum array);
GUEST_C void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
GUEST_C void glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a);
GUEST_C void glDrawArrays(GLenum mode, GLint first, GLsizei count);

/* --- OpenAL --------------------------------------------------------------------------------- */
typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;
typedef int ALint;
typedef unsigned int ALuint;
typedef int ALenum;
typedef int ALsizei;
#define AL_NO_ERROR 0
#define AL_BUFFER 0x1009
#define AL_FORMAT_MONO16 0x1101

GUEST_C ALCdevice *alcOpenDevice(const char *name);
GUEST_C ALCcontext *alcCreateContext(ALCdevice *device, const int *attrs);
GUEST_C unsigned char alcMakeContextCurrent(ALCcontext *context);
GUEST_C void alGenBuffers(ALsizei n, ALuint *ids);
GUEST_C void alBufferData(ALuint buffer, ALenum format, const void *data, ALsizei size,
                          ALsizei freq);
GUEST_C void alGenSources(ALsizei n, ALuint *ids);
GUEST_C void alSourcei(ALuint source, ALenum param, ALint value);
GUEST_C void alSourcePlay(ALuint source);
GUEST_C ALenum alGetError(void);

/* --- UIKit ---------------------------------------------------------------------------------- */
GUEST_C int UIApplicationMain(int argc, char *argv[], const void *principal, const void *delegate);

/* --- the C++ part (cxx.cpp) ----------------------------------------------------------------- */
GUEST_C int guest_constructed(void);  /* the static constructor's value, 0 if it never ran */
GUEST_C int guest_throw_and_catch(int code); /* throws code, returns what the catch saw */

#endif
