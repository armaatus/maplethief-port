/* UIKit (#54, ADR 0037): UIApplicationMain, MainWindow.nib, the view tree, touches and the
 * compositor, as the platform layer (#117) and the GL renderer (#58) use them.
 *
 * The shell runs recompiled on host UIKit classes (ADR 0026). UIApplicationMain is the boot's
 * app hook (maplethief/boot.h): it creates the UIApplication, loads MainWindow.nib (whose
 * EAGLView starts the engine in -initWithCoder:), sends the app delegate its launch callback
 * with the nib's window, and then runs the main run loop for ever, handing control to the
 * host after every turn:
 *
 *     static int turn(void *user, mt_cpu *cpu, double now, double next) {
 *         ... poll input: mt_uikit_touch(finger, MT_UIKIT_TOUCH_BEGAN, x, y) ...
 *         mt_uikit_composite(cpu, now, MT_UIKIT_COMPOSITE_OVERLAY, NULL, pixels, 480, 320, 1920);
 *         ... present: the GL frame in mt_uikit_gl_rect(), the overlay on top ...
 *         ... wait until `next` (CFAbsoluteTime) or input ...
 *         return quit ? 1 : 0;
 *     }
 *     mt_uikit_config ui = mt_uikit_config_default();
 *     ui.turn = turn;
 *     boot_cfg.app_main = mt_uikit_app_main;
 *     boot_cfg.app_user = &ui;
 *
 * Coordinates. The game is an iPhone OS 3 app: a 320x480-point portrait window at scale 1,
 * whose EAGLView turns itself into a 480x320 landscape view (-[EAGLView createGLes]). The
 * screen the host sees is that landscape view upright: 480x320 points, origin top left, in
 * the orientation of Info.plist's UIInterfaceOrientation (LandscapeLeft: the window's top edge
 * on the right). Touches and the composited image use these screen points.
 *
 * Single guest thread (ADR 0033): every function runs on the guest's main thread, between run
 * loop turns (from the turn hook) unless it says otherwise. C17 that also compiles as C++20;
 * the implementation is runtime/src/uikit/. */
#ifndef MAPLETHIEF_UIKIT_H
#define MAPLETHIEF_UIKIT_H

#include <stdint.h>

#include "maplethief/boot.h"
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The screen in points (landscape). */
#define MT_UIKIT_SCREEN_WIDTH 480u
#define MT_UIKIT_SCREEN_HEIGHT 320u

typedef struct mt_uikit_config {
    /* The clock of UIKit's animations, scrolling and touch timestamps, in CFAbsoluteTime
     * seconds. NULL: mt_cf_absolute_time (the run loop's clock). */
    double (*clock)(void *user);
    /* After each turn of the main run loop: `now` is the turn's time, `next` when the loop
     * next has work (mt_runloop_next_wake). The host presents, feeds input and waits. Return 0
     * to go on; anything else ends the run: UIApplicationMain stops the guest
     * (MT_PROCESS_STOPPED, "UIApplicationMain: the host ended the run loop"). NULL: the run
     * stops after the launch (before the first turn). */
    int (*turn)(void *user, mt_cpu *cpu, double now, double next);
    /* -[UIApplication openURL:]: return 1 if the URL was opened. NULL: logged, not opened. */
    int (*open_url)(void *user, const char *url);
    void *user;
} mt_uikit_config;

mt_uikit_config mt_uikit_config_default(void) MT_NOEXCEPT;

/* UIApplicationMain (an mt_app_main_fn): `user` is an mt_uikit_config (NULL: the default).
 * It never returns: the run ends through the turn hook, exit or a trap. A nib that cannot be
 * loaded stops the run with the reason. */
int32_t mt_uikit_app_main(mt_process *p, mt_cpu *cpu, const mt_app_main_args *args,
                          void *user) MT_NOEXCEPT;

/* --- input --------------------------------------------------------------------------------- */

enum mt_uikit_touch_phase {
    MT_UIKIT_TOUCH_BEGAN = 0,
    MT_UIKIT_TOUCH_MOVED = 1,
    MT_UIKIT_TOUCH_ENDED = 2,
    MT_UIKIT_TOUCH_CANCELLED = 3,
};

/* Queues a touch of finger `finger` (any id the host keeps for one finger while it is down) at
 * screen point (x, y); it is delivered at the start of the next turn (UIApplicationMain runs
 * mt_uikit_turn before each). Returns 0, or -1 if the phase is invalid. Any thread. */
int mt_uikit_touch(uint32_t finger, uint32_t phase, float x, float y) MT_NOEXCEPT;

/* App lifecycle events the platform reports to the app delegate. */
enum mt_uikit_lifecycle {
    MT_UIKIT_WILL_RESIGN_ACTIVE = 0, /* applicationWillResignActive: (Clet_Pause) */
    MT_UIKIT_DID_BECOME_ACTIVE = 1,  /* applicationDidBecomeActive: (Clet_Resume) */
    MT_UIKIT_WILL_TERMINATE = 2,     /* applicationWillTerminate: */
};
/* Sends the delegate the event's message if it implements it. Returns 1 if it was sent. */
int mt_uikit_lifecycle_event(mt_cpu *cpu, uint32_t event) MT_NOEXCEPT;

/* One UIKit turn at time `now`: delivers the queued touches, advances scrolling, tells
 * presented view controllers they appeared, and lets views whose class overrides drawRect:
 * draw (in a UIGraphics context, before anything is composited), inside an autorelease pool.
 * UIApplicationMain runs it before each run-loop turn; hosts and tests that drive the loop
 * themselves call it. */
void mt_uikit_turn(mt_cpu *cpu, double now) MT_NOEXCEPT;

/* --- compositing ----------------------------------------------------------------------------- */

/* A frame of the GL view (#58): premultiplied RGBA8, top row first, upright (as the screen
 * shows it), any size: it is scaled to the GL view's rectangle. */
typedef struct mt_uikit_gl_frame {
    const uint8_t *pixels;
    uint32_t width, height, stride; /* stride in bytes */
} mt_uikit_gl_frame;

enum mt_uikit_composite_mode {
    /* Everything: the windows and their views, with `gl` (or, if NULL, opaque black as a
     * placeholder) in the GL view's place. */
    MT_UIKIT_COMPOSITE_FULL = 0,
    /* What the host draws over its own GL frame: the views above the GL view, on transparent
     * pixels inside the GL view's rectangle (what lies under the opaque GL view is not
     * visible), and whatever is outside it. `gl` is ignored. */
    MT_UIKIT_COMPOSITE_OVERLAY = 1,
};

/* Renders the screen at time `now` (animations show their values at that time) into `pixels`:
 * premultiplied RGBA8, top row first, `width` x `height` pixels covering the 480x320-point
 * screen (2x: 960x640; the aspect ratio should be 3:2), `stride` bytes per row. Views draw
 * with the CoreGraphics rasterizer of #51 (images, text, colours, group opacity, clipping).
 * Returns 0, or -1 if there is no application, no window or the arguments are invalid. */
int mt_uikit_composite(mt_cpu *cpu, double now, uint32_t mode, const mt_uikit_gl_frame *gl,
                       uint8_t *pixels, uint32_t width, uint32_t height,
                       uint32_t stride) MT_NOEXCEPT;

/* The GL view's rectangle in screen points (x, y, width, height; the bounding box of its
 * frame) and the size of its drawable in pixels (its bounds: 480x320). Returns 1, or 0 if no
 * view in a window has a CAEAGLLayer. */
int mt_uikit_gl_rect(float rect[4], uint32_t *drawable_width,
                     uint32_t *drawable_height) MT_NOEXCEPT;

/* The host's opacity for `view` (a UIView), 0 to 1 (#71, ADR 0052): the compositor multiplies
 * it into the view's own opacity and does not draw the view and its subviews at 0. Nothing the
 * guest can see changes: not its alpha, not its hidden flag, not hit testing. The platform
 * fades the game's on-screen key pad with it. 1 by default. Returns 1, or 0 if `view` is not a
 * view. */
int mt_uikit_set_host_opacity(uint32_t view, float opacity) MT_NOEXCEPT;
/* The host's opacity for `view`; 1 for anything that is not a view. */
float mt_uikit_host_opacity(uint32_t view) MT_NOEXCEPT;

/* The number of UIView animations still running or waiting for their delay at `now`. */
uint32_t mt_uikit_animation_count(double now) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_UIKIT_H */
