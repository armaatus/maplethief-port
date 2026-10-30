// UIKit (#54, ADR 0037): what the files of runtime/src/uikit/ share.
//
//   nib.cpp         the NIBArchive reader (nib.h): untrusted bytes -> tables
//   app.cpp         UIApplication, UIApplicationMain, MainWindow.nib -> objects (UINibDecoder),
//                   UIDevice, UIViewController, the module state and the clock
//   view.cpp        UIResponder, UIView, UIWindow, CALayer/CAEAGLLayer: geometry, the tree,
//                   UIView animations (model vs presentation values), drawRect: display
//   controls.cpp    UIImageView, UILabel, UIControl/UIButton, UIScrollView, UITextView,
//                   UIPageControl, UIActivityIndicatorView
//   touch.cpp       UITouch, UIEvent, hit testing, touch delivery, scroll-view dragging
//   compositor.cpp  the view tree -> RGBA with #51's rasterizer; the public compositing API
//   focus.cpp       the controls a tap can reach, for controller navigation (#69, ADR 0051)
//
// Host state. Every view's state is one View (a C++ object behind a pointer in the object's
// side-table block, maplethief/objc/host.h), whatever its class: the fields of the subclasses
// (label text, button images, scroll state) live in the same struct, used by the kind the
// view was created as. Views keep model values (what the game set); the compositor asks for
// presentation values (model values under running UIView animations, at a time).
//
// Coordinates. A view's local space is its bounds (origin = the bounds origin, y down); its
// superview places it with center, bounds size and transform as UIKit does. The window lies
// on the 320x480-point portrait screen; the host's screen is the landscape view of it
// (maplethief/uikit.h): screen_from_window() maps one to the other.
//
// Single guest thread (ADR 0033): no locks. Functions that send messages say so.
#ifndef MAPLETHIEF_RUNTIME_UIKIT_UIKIT_INTERNAL_H
#define MAPLETHIEF_RUNTIME_UIKIT_UIKIT_INTERNAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "cg/cg_objects.h"
#include "maplethief/objc/host.h"
#include "maplethief/uikit.h"

namespace mt_ui {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using mt_cg::Affine;
using mt_cg::Color;
using mt_cg::Font;
using mt_cg::Point;
using mt_cg::Rect;

// --- views ---------------------------------------------------------------------------------------

// The host class a view was created as (its nearest UIKit ancestor).
enum class Kind : u8 {
    view,
    window,
    image_view,
    label,
    button,
    control, // a UIControl that is not one of the above (UIPageControl is page_control)
    scroll_view,
    text_view, // a UIScrollView subclass, as in UIKit
    page_control,
    activity,
};

// Animatable properties and their value layouts (doubles).
enum Prop : u8 {
    k_prop_center = 0,    // x, y
    k_prop_bounds = 1,    // x, y, w, h
    k_prop_alpha = 2,     // alpha
    k_prop_transform = 3, // a, b, c, d, tx, ty
};
constexpr int k_prop_count = 4;

// UIViewAnimationCurve
enum : u32 {
    k_curve_ease_in_out = 0,
    k_curve_ease_in = 1,
    k_curve_ease_out = 2,
    k_curve_linear = 3
};

struct Anim {
    Prop prop = k_prop_center;
    double from[6] = {}, to[6] = {};
    double start = 0, duration = 0;
    u32 curve = k_curve_ease_in_out;
};

// The model values a compositor or hit test needs.
struct Geometry {
    Point center;
    Rect bounds;
    Affine transform;
    double alpha = 1;
};

// UIControlEvents the controls send.
enum : u32 {
    k_event_touch_down = 1u << 0,
    k_event_touch_drag_inside = 1u << 2,
    k_event_touch_drag_outside = 1u << 3,
    k_event_touch_drag_enter = 1u << 4,
    k_event_touch_drag_exit = 1u << 5,
    k_event_touch_up_inside = 1u << 6,
    k_event_touch_up_outside = 1u << 7,
    k_event_touch_cancel = 1u << 8,
    k_event_value_changed = 1u << 12,
};

struct TargetAction {
    u32 target = 0; // not retained (UIKit does not retain targets)
    u32 action = 0; // SEL
    u32 events = 0;
};

struct View {
    u32 obj = 0;
    Kind kind = Kind::view;

    // UIView
    Geometry model;
    bool hidden = false, opaque = true, clips = false, interaction = true, multitouch = false;
    bool autoresizes = true, clears_context = true;
    i32 tag = 0;
    u32 autoresizing = 0;
    u32 background = 0; // UIColor, retained; 0: none (transparent)
    Color background_color{0, 0, 0, 0};
    u32 superview = 0;         // not retained
    std::vector<u32> subviews; // retained, back to front
    u32 layer = 0;             // retained, made on first -layer
    bool gl = false;           // the layer is a CAEAGLLayer (the GL slot)
    // The host's opacity for this view's layer (mt_uikit_set_host_opacity, #71): multiplied into
    // what the compositor draws, never seen by the guest (alpha, hidden) or by hit testing.
    float host_opacity = 1;
    std::vector<Anim> anims;   // running animations (presentation values)
    bool needs_display = true; // -drawRect: has to run (only for classes that override it)
    std::vector<u8> contents;  // what -drawRect: drew: premultiplied RGBA, top row first
    u32 contents_width = 0, contents_height = 0;
    bool contents_blank = true; // every pixel of `contents` is transparent

    // UIImageView
    u32 image = 0; // UIImage, retained

    // UILabel, UITextView, UIButton title (unused)
    std::u16string text;
    u32 font = 0; // UIFont, retained; 0: the default font of the kind
    Font font_value;
    Color text_color{0, 0, 0, 1};
    u32 text_color_obj = 0; // UIColor, retained
    i32 alignment = 0;      // UITextAlignment: left, center, right
    i32 lines = 1;          // numberOfLines (0: any)
    i32 baseline_adjustment = 0;
    bool editable = true;

    // UIControl, UIButton
    std::vector<TargetAction> actions;
    bool highlighted = false, tracking = false, touch_inside = false;
    u32 images[3] = {0, 0, 0};            // UIControlState normal, highlighted, disabled; retained
    float image_insets[4] = {0, 0, 0, 0}; // top, left, bottom, right

    // UIScrollView, UITextView
    double content_width = 0, content_height = 0;
    bool paging = false, scroll_enabled = true, can_cancel_content_touches = true;
    bool shows_horizontal = true, shows_vertical = true;
    i32 indicator_style = 0;
    u32 delegate = 0; // not retained
    // scrolling the host runs (touch.cpp): a drag's velocity, then deceleration or paging
    bool dragging = false, decelerating = false;
    double velocity_x = 0, velocity_y = 0; // points per second of the content offset
    double scroll_from_x = 0, scroll_from_y = 0, scroll_to_x = 0, scroll_to_y = 0;
    double scroll_start = 0, scroll_last = 0;

    // UIPageControl
    i32 pages = 0, page = 0;

    // UIActivityIndicatorView
    i32 style = 0;
    bool animating = false, hides_when_stopped = true;
};

// The View of a guest object if it has one (it is a view that host code has seen), else
// nullptr.
View *view_of(mt_cpu *cpu, u32 obj);
// The View of a guest UIView, created (with the defaults of its kind) on first use; nullptr if
// `obj` is no UIView or out of memory.
View *view_state(mt_cpu *cpu, u32 obj);
// Whether `obj` is an instance of the UIKit class `name` or a subclass (0: no such class).
bool is_kind(mt_cpu *cpu, u32 obj, const char *name);

// The local -> superview map of a geometry: bounds origin and centre to (0, 0), transform,
// then to the centre in the superview.
Affine local_to_super(const Geometry &g);
// The frame of a geometry: the bounding box of its bounds in the superview.
Rect frame_of(const Geometry &g);
// A view's local -> portrait screen map from model values (every superview's, then the
// window's place on the screen; a view in no window: up to its root).
Affine local_to_screen(mt_cpu *cpu, const View &v);
// The window that holds `v` (itself for a window), or 0.
u32 window_of(mt_cpu *cpu, const View &v);
// Portrait screen points -> the host's landscape screen points (maplethief/uikit.h).
Affine screen_from_window();

// The presentation values of `v` at `now`.
Geometry presentation(const View &v, double now);
// Drops the animations that ended before `now`.
void prune_animations(View &v, double now);

// Sets a view property the way UIKit does: the model value at once and, inside a
// +beginAnimations:context: block, an animation from the presentation value (view.cpp).
void set_center(mt_cpu *cpu, View &v, Point c);
void set_bounds(mt_cpu *cpu, View &v, const Rect &b);
void set_frame(mt_cpu *cpu, View &v, const Rect &f);
void set_alpha(mt_cpu *cpu, View &v, double alpha);
void set_transform(mt_cpu *cpu, View &v, const Affine &t);

// [superview addSubview:sub] / [sub removeFromSuperview] without a message (sub retained by
// the superview / released, which may deallocate it).
void add_subview(mt_cpu *cpu, View &superview, u32 sub);
void remove_from_superview(mt_cpu *cpu, View &v);

// Runs -drawRect: for every view in a window that needs display and whose class overrides it
// (view.cpp; sends messages).
void display_views(mt_cpu *cpu);

// A CALayer's view (its delegate, set by -[UIView layer]), or 0.
void layer_set_view(mt_cpu *cpu, u32 layer, u32 view);
u32 layer_view(mt_cpu *cpu, u32 layer);

// --- controls (controls.cpp) ---------------------------------------------------------------------

// Defaults of a view of kind `k` (called when its View is created).
void control_defaults(mt_cpu *cpu, View &v);
// The image a button shows now (not retained), or 0.
u32 button_image(const View &v);
// The image rectangle of a button in its local space.
Rect button_image_rect(mt_cpu *cpu, const View &v, u32 image);
// Text laid out in lines no wider than `width` (UILabel/UITextView word wrapping; words wider
// than a line are broken between characters); at most `max_lines` lines (0: any).
std::vector<std::u16string> wrap_text(const std::u16string &text, const Font &f, double width,
                                      int max_lines);
// The default fonts (UIKit's system font is Helvetica 17; UITextView's is 12).
Font default_font(Kind k);
// UITextView's text inset (points) on every side.
constexpr double k_text_view_inset = 8;
// UIScrollView: sets the content offset (bounds origin), clamped or not, and tells the
// delegate (-scrollViewDidScroll:, sends a message) if it changed.
void set_content_offset(mt_cpu *cpu, View &v, double x, double y);
// The largest content offset of a scroll view on each axis (0 if it cannot scroll that way).
void max_content_offset(const View &v, double *x, double *y);
// A UITextView's content size from its text (when the text, font or width change).
void text_view_layout(View &v);

// --- touches (touch.cpp) -------------------------------------------------------------------------

// Delivers the queued touches (maplethief/uikit.h mt_uikit_touch); sends messages.
void deliver_touches(mt_cpu *cpu, double now);
// Advances the scroll views' deceleration and paging animations; sends -scrollViewDidScroll:.
void advance_scrolling(mt_cpu *cpu, double now);
// A view is going away: forget it in the touch and scroll state.
void touch_forget_view(u32 view);
// UIControl's tracking from its touches* host methods.
void control_touches(mt_cpu *cpu, View &v, u32 touches, u32 event, int phase);
// -[UIView hitTest:withEvent:] over the windows, front to back, at a portrait window point: the
// view a touch beginning there belongs to, or 0.
u32 hit_test_screen(mt_cpu *cpu, Point window_point);
// The screen point (portrait) of a UITouch, and its previous one; false if `touch` is none.
bool touch_point(mt_cpu *cpu, u32 touch, Point *now, Point *previous);
// Forgets every touch (a new runtime).
void touch_reset();

// --- the module (app.cpp) ------------------------------------------------------------------------

// Per-runtime state; reset when the current ObjC runtime changes.
struct Group {
    double delay = 0, duration = 0.2;
    u32 curve = k_curve_ease_in_out;
    struct Pending {
        u32 view;
        Prop prop;
        double from[6], to[6];
    };
    std::vector<Pending> pending;
};

struct State {
    mt_objc *rt = nullptr;
    mt_uikit_config config{};
    bool launched = false;
    u32 app = 0;                  // the UIApplication, retained
    u32 delegate = 0;             // its delegate (not retained, as in UIKit; the nib keeps it)
    std::vector<u32> windows;     // visible windows, back to front, retained
    std::vector<u32> nib_objects; // the main nib's top-level objects, retained
    std::vector<Group> groups;    // open +beginAnimations:context: blocks
    std::vector<u32> presented;   // view controllers to tell -viewDidAppear:, retained
    u32 device = 0;               // +[UIDevice currentDevice], retained
    u32 badge = 0;
    double last_turn = 0; // the time of the last mt_uikit_turn
};

State &state(mt_cpu *cpu);
// UIApplicationMain's launch (app.cpp): the UIApplication, the main nib (Info.plist's
// NSMainNibFile, else MainWindow) with it as File's Owner, the delegate's launch callback and
// -applicationDidBecomeActive:. Returns why it failed (logged), or nullptr. Sends messages;
// mt_uikit_app_main runs it before the run loop, tests on their own.
const char *launch(mt_cpu *cpu, const mt_app_main_args *args);
// The UIKit clock (mt_uikit_config::clock).
double now(mt_cpu *cpu);
mt_objc *runtime(mt_cpu *cpu);
// Whether `obj` responds to `selector` (lookup only, no message).
bool responds(mt_cpu *cpu, u32 obj, const char *selector);
// Whether the class of `obj` overrides the instance method `selector` of the host class `base`
// (a subclass of it, an image class or a category, implements it).
bool overrides(mt_cpu *cpu, u32 obj, const char *selector, const char *base);
// The text of an NSString as UTF-16 ("" for nil).
std::u16string string_text(mt_cpu *cpu, u32 str);
// The colour of a UIColor (through -CGColor, a message); false for nil or no colour.
bool uicolor_value(mt_cpu *cpu, u32 color, Color *out);
// Replaces a retained object reference: retains `value`, releases the old one.
void assign(mt_cpu *cpu, u32 *slot, u32 value);

// Logs "uikit: ..." through the runtime's log sink; log_once only the first time for `key`.
void log(mt_cpu *cpu, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void log_once(mt_cpu *cpu, const char *key, const char *fmt, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// The decoder of -initWithCoder: (app.cpp, a UINibDecoder): the nib values of the object it is
// decoding. These read them for UIView's -initWithCoder: (false / 0: no such key, or `coder`
// is no UINibDecoder).
bool coder_bool(mt_cpu *cpu, u32 coder, const char *key, bool *out);
bool coder_number(mt_cpu *cpu, u32 coder, const char *key, double *out);
bool coder_floats(mt_cpu *cpu, u32 coder, const char *key, float *out, u32 count);
// The object a key refers to, decoded (not retained: the decoder keeps it), or 0.
u32 coder_object(mt_cpu *cpu, u32 coder, const char *key);
// The objects of an array a key refers to, decoded.
std::vector<u32> coder_objects(mt_cpu *cpu, u32 coder, const char *key);
// The keys of the object being decoded (for logging the ones a class does not read).
std::vector<std::string> coder_keys(mt_cpu *cpu, u32 coder);

// --- host classes (runtime/src/objc/classes.cpp lists them, superclasses first) ------------------

extern const mt_objc_host_class k_uiresponder;
extern const mt_objc_host_class k_uiview;
extern const mt_objc_host_class k_uiwindow;
extern const mt_objc_host_class k_uiimageview;
extern const mt_objc_host_class k_uilabel;
extern const mt_objc_host_class k_uicontrol;
extern const mt_objc_host_class k_uibutton;
extern const mt_objc_host_class k_uipagecontrol;
extern const mt_objc_host_class k_uiscrollview;
extern const mt_objc_host_class k_uitextview;
extern const mt_objc_host_class k_uiactivityindicatorview;
extern const mt_objc_host_class k_calayer;
extern const mt_objc_host_class k_caeagllayer;
extern const mt_objc_host_class k_uiapplication;
extern const mt_objc_host_class k_uiviewcontroller;
extern const mt_objc_host_class k_uidevice;
extern const mt_objc_host_class k_uitouch;
extern const mt_objc_host_class k_uievent;
extern const mt_objc_host_class k_nscoder;
extern const mt_objc_host_class k_uinibdecoder;

} // namespace mt_ui

#endif // MAPLETHIEF_RUNTIME_UIKIT_UIKIT_INTERNAL_H
