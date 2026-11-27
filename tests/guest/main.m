/* The guest program (#385, ADR 0213): an armv7 iOS app of our own that takes every layer of the
 * port end to end in CI, where the game cannot run. It boots like the game (crt1's start, main,
 * UIApplicationMain), sends Objective-C messages through a class hierarchy of its own, draws one
 * GLES1 frame on a CAEAGLLayer from a CFRunLoop timer (the game's frame loop), plays a generated
 * sound through OpenAL and takes a C++ exception through the SjLj unwinder. Each step prints a
 * "guest:" line that the test platform.guest_program checks. Built by scripts/guest-image. */
#include "guest_objc.h"

/* --- a class hierarchy of its own: messaging, super, a category, a class method ----------------
 */

@interface Shape : NSObject {
    int _corners;
}
+ (int)made;
- (id)initWithCorners:(int)corners;
- (int)corners;
- (int)weight;
@end

static int g_made;

@implementation Shape
+ (int)made {
    return g_made;
}
- (id)initWithCorners:(int)corners {
    self = [super init];
    if (self != nil) {
        _corners = corners;
        g_made++;
    }
    return self;
}
- (int)corners {
    return _corners;
}
- (int)weight {
    return 10;
}
@end

@interface Triangle : Shape
- (id)init;
@end

@implementation Triangle
- (id)init {
    return [super initWithCorners:3];
}
- (int)weight {
    return [super weight] + [self corners]; /* 13 */
}
@end

@interface Shape (Doubling)
- (int)doubledCorners;
@end

@implementation Shape (Doubling)
- (int)doubledCorners {
    return 2 * [self corners];
}
@end

/* --- the sound: a WAV from the bundle (the game's CFBundle read), played by OpenAL -------------
 */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void play_sound(void) {
    CFStringRef name =
        CFStringCreateWithCString(kCFAllocatorDefault, "tone", kCFStringEncodingASCII);
    CFStringRef type =
        CFStringCreateWithCString(kCFAllocatorDefault, "wav", kCFStringEncodingASCII);
    CFURLRef url =
        CFBundleCopyResourceURLForLocalization(CFBundleGetMainBundle(), name, type, 0, 0);
    CFDataRef data = 0;
    SInt32 error = 0;
    if (url == 0 ||
        !CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, url, &data, 0, 0, &error)) {
        puts("guest: sound: tone.wav is not in the bundle");
        return;
    }
    const uint8_t *bytes = CFDataGetBytePtr(data);
    CFIndex size = CFDataGetLength(data), at = 12;
    const uint8_t *samples = 0;
    uint32_t sample_bytes = 0, rate = 0;
    while (at + 8 <= size) {
        uint32_t chunk = rd32(bytes + at + 4);
        if (memcmp(bytes + at, "fmt ", 4) == 0) {
            rate = rd32(bytes + at + 12);
        } else if (memcmp(bytes + at, "data", 4) == 0) {
            samples = bytes + at + 8;
            sample_bytes = chunk;
        }
        at += 8 + ((chunk + 1u) & ~1u);
    }
    ALCdevice *device = alcOpenDevice(0);
    alcMakeContextCurrent(alcCreateContext(device, 0));
    ALuint buffer = 0, source = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, samples, (ALsizei)sample_bytes, (ALsizei)rate);
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, (ALint)buffer);
    alSourcePlay(source);
    printf("guest: sound: playing %u samples at %u Hz, al error %d\n", sample_bytes / 2u, rate,
           alGetError());
    CFRelease(data);
    CFRelease(url);
    CFRelease(type);
    CFRelease(name);
}

/* --- the view: a CAEAGLLayer with an ES 1.1 context ------------------------------------------- */

@interface GuestView : UIView {
    EAGLContext *_context;
    GLuint _framebuffer, _renderbuffer;
    GLint _width, _height;
    int _frames;
}
- (void)drawFrame;
@end

@implementation GuestView
+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) {
        return nil;
    }
    CAEAGLLayer *layer = [self layer];
    [layer setOpaque:YES];
    [layer
        setDrawableProperties:[NSDictionary
                                  dictionaryWithObjectsAndKeys:[NSNumber numberWithBool:NO],
                                                               kEAGLDrawablePropertyRetainedBacking,
                                                               kEAGLColorFormatRGBA8,
                                                               kEAGLDrawablePropertyColorFormat,
                                                               nil]];
    _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1];
    [EAGLContext setCurrentContext:_context];
    glGenFramebuffersOES(1, &_framebuffer);
    glGenRenderbuffersOES(1, &_renderbuffer);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_context renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:layer];
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES,
                                 _renderbuffer);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &_width);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &_height);
    printf("guest: gl: renderbuffer %dx%d, framebuffer %s\n", _width, _height,
           glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES
               ? "complete"
               : "incomplete");
    return self;
}

/* The frame: the whole renderbuffer in one colour (the game imports no glClear, so a quad is
 * the clear), then a triangle in another, then presentRenderbuffer:. The renderbuffer is the
 * portrait window's 320x480, which the screen shows turned, as the game's. */
- (void)drawFrame {
    static const GLshort k_quad[8] = {0, 0, 320, 0, 0, 480, 320, 480};
    static const GLshort k_triangle[6] = {160, 60, 280, 400, 40, 330};
    [EAGLContext setCurrentContext:_context];
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glViewport(0, 0, _width, _height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthox(0, _width << 16, _height << 16, 0, -(1 << 16), 1 << 16);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glEnableClientState(GL_VERTEX_ARRAY);
    glColor4x(0x2000, 0x4000, 0x8000, 0x10000); /* dark blue */
    glVertexPointer(2, GL_SHORT, 0, k_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glColor4x(0x10000, 0xC000, 0x0000, 0x10000); /* orange */
    glVertexPointer(2, GL_SHORT, 0, k_triangle);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_context presentRenderbuffer:GL_RENDERBUFFER_OES];
    if (++_frames == 1) {
        puts("guest: gl: frame 1 presented");
    }
}
@end

/* --- the frame loop: a repeating CFRunLoop timer, as the game's RunLoopTimer_Arm -----------------
 */

static void on_timer(CFRunLoopTimerRef timer, void *info) {
    static int ticks;
    (void)timer;
    if (++ticks == 1) {
        puts("guest: timer: first tick");
    }
    [(GuestView *)info drawFrame];
}

/* --- the app delegate ------------------------------------------------------------------------- */

@interface GuestAppDelegate : UIResponder {
    UIWindow *_window;
    GuestView *_view;
}
@end

@implementation GuestAppDelegate
- (BOOL)application:(id)application didFinishLaunchingWithOptions:(id)options {
    (void)application;
    (void)options;
    puts("guest: delegate: didFinishLaunching");

    Triangle *t = [[Triangle alloc] init];
    printf("guest: objc: corners %d, doubled %d, weight %d, made %d, class %s\n", [t corners],
           [t doubledCorners], [t weight], [Shape made],
           [t class] == [Triangle class] ? "Triangle" : "?");
    [t release];

    printf("guest: c++: static constructor %d\n", guest_constructed());
    printf("guest: c++: caught %d\n", guest_throw_and_catch(7));

    CGRect frame = {{0.0f, 0.0f}, {320.0f, 480.0f}}; /* the iPhone's portrait window */
    _window = [[UIWindow alloc] initWithFrame:frame];
    _view = [[GuestView alloc] initWithFrame:frame];
    [_window addSubview:_view];
    [_window makeKeyAndVisible];

    play_sound();

    CFRunLoopTimerContext context = {0, _view, 0, 0, 0};
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(),
                                                   1.0 / 30.0, 0, 0, on_timer, &context);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    return YES;
}
@end

int main(int argc, char *argv[]) {
    puts("guest: main");
    return UIApplicationMain(argc, argv, nil, @"GuestAppDelegate");
}
