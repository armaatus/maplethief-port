// UIResponder, UIView, UIWindow, CALayer and CAEAGLLayer (#54, ADR 0037): the view tree, its
// geometry, UIView's begin/commit animations and host-driven -drawRect:.
//
// What the game does with them (docs/re/objc-shell.md, docs/re/host-surface.md):
//   MainWindow.nib          a UIWindow (320x480) holding the EAGLView, both by -initWithCoder:
//   -[EAGLView createGLes]  self.layer (a CAEAGLLayer: +layerClass), layer.opaque, the 90° turn
//                           setTransform:(0, -1, 1, 0) and setBounds:(0, 0, 480, 320)
//   every panel and menu    initWithFrame:, addSubview:, setFrame:, setAlpha: (TouchKeyPadView
//                           -enable:/-disable: show and hide its 11 panels with alpha 1/0),
//                           setBackgroundColor:, setTag:, subviews, removeFromSuperview
//   MainMenuView, ChooseCharacterView  +beginAnimations:context: ... +commitAnimations with
//                           delays, durations and a curve; -drawRect: builds MainMenuView's UI
#include <algorithm>
#include <cmath>
#include <cstring>

#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

// --- host state ----------------------------------------------------------------------------------

void view_destroy(mt_cpu *cpu, u32 obj, void *block);
const mt_objc_state_type k_view_state = {"UIView", sizeof(View *), view_destroy};

void release_all(mt_cpu *cpu, View *v) {
    mt_objc_release(cpu, v->background);
    mt_objc_release(cpu, v->layer);
    mt_objc_release(cpu, v->image);
    mt_objc_release(cpu, v->font);
    mt_objc_release(cpu, v->text_color_obj);
    for (u32 &img : v->images) {
        mt_objc_release(cpu, img);
    }
}

void view_destroy(mt_cpu *cpu, u32 obj, void *block) {
    View *v = *static_cast<View **>(block);
    if (v == nullptr) {
        return;
    }
    *static_cast<View **>(block) = nullptr;
    touch_forget_view(obj);
    State &st = mt_ui::state(cpu);
    for (Group &g : st.groups) {
        g.pending.erase(std::remove_if(g.pending.begin(), g.pending.end(),
                                       [obj](const Group::Pending &p) { return p.view == obj; }),
                        g.pending.end());
    }
    // Over-released while still in a superview: unlink it without another release.
    if (View *parent = view_of(cpu, v->superview)) {
        auto &s = parent->subviews;
        s.erase(std::remove(s.begin(), s.end(), obj), s.end());
    }
    std::vector<u32> subviews = std::move(v->subviews);
    for (u32 sub : subviews) {
        if (View *sv = view_of(cpu, sub)) {
            sv->superview = 0;
        }
    }
    release_all(cpu, v);
    delete v;
    for (u32 sub : subviews) {
        mt_objc_release(cpu, sub);
    }
}

Kind kind_of(mt_cpu *cpu, u32 obj) {
    static const struct {
        const char *name;
        Kind kind;
    } k_kinds[] = {
        {"UIWindow", Kind::window},
        {"UITextView", Kind::text_view},
        {"UIScrollView", Kind::scroll_view},
        {"UIImageView", Kind::image_view},
        {"UILabel", Kind::label},
        {"UIButton", Kind::button},
        {"UIPageControl", Kind::page_control},
        {"UIActivityIndicatorView", Kind::activity},
        {"UIControl", Kind::control},
    };
    for (const auto &k : k_kinds) {
        if (is_kind(cpu, obj, k.name)) {
            return k.kind;
        }
    }
    return Kind::view;
}

// --- arguments and results -----------------------------------------------------------------------

Rect read_rect(mt_args *a) {
    Rect r;
    r.x = mt_arg_f32(a);
    r.y = mt_arg_f32(a);
    r.w = mt_arg_f32(a);
    r.h = mt_arg_f32(a);
    return r;
}

void ret_rect(mt_cpu *cpu, u32 result, const Rect &r) {
    const float f[4] = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                        static_cast<float>(r.h)};
    mt_ret_struct(cpu, result, f, sizeof f);
}

void ret_point(mt_cpu *cpu, u32 result, Point p) {
    const float f[2] = {static_cast<float>(p.x), static_cast<float>(p.y)};
    mt_ret_struct(cpu, result, f, sizeof f);
}

// The View of self for a host method; logs once if self is none.
View *self_view(mt_cpu *cpu, u32 self, const char *what) {
    View *v = view_state(cpu, self);
    if (v == nullptr && self != 0) {
        log_once(cpu, what, "%s: 0x%08X is no UIView: ignored", what, static_cast<unsigned>(self));
    }
    return v;
}

// --- animations ----------------------------------------------------------------------------------

void values_of(const Geometry &g, Prop p, double *out) {
    switch (p) {
    case k_prop_center:
        out[0] = g.center.x;
        out[1] = g.center.y;
        break;
    case k_prop_bounds:
        out[0] = g.bounds.x;
        out[1] = g.bounds.y;
        out[2] = g.bounds.w;
        out[3] = g.bounds.h;
        break;
    case k_prop_alpha:
        out[0] = g.alpha;
        break;
    case k_prop_transform:
        out[0] = g.transform.a;
        out[1] = g.transform.b;
        out[2] = g.transform.c;
        out[3] = g.transform.d;
        out[4] = g.transform.tx;
        out[5] = g.transform.ty;
        break;
    }
}

void apply_values(Geometry &g, Prop p, const double *v) {
    switch (p) {
    case k_prop_center:
        g.center = {v[0], v[1]};
        break;
    case k_prop_bounds:
        g.bounds = {v[0], v[1], v[2], v[3]};
        break;
    case k_prop_alpha:
        g.alpha = v[0];
        break;
    case k_prop_transform:
        g.transform = {v[0], v[1], v[2], v[3], v[4], v[5]};
        break;
    }
}

constexpr int k_value_count[k_prop_count] = {2, 4, 1, 6};

// CAMediaTimingFunction's cubic Bézier curves through (0, 0), (x1, y1), (x2, y2), (1, 1):
// the progress at time fraction t.
double timing(u32 curve, double t) {
    double x1 = 0.42, y1 = 0, x2 = 0.58, y2 = 1; // ease in, ease out
    switch (curve) {
    case k_curve_ease_in:
        x2 = 1;
        y2 = 1;
        break;
    case k_curve_ease_out:
        x1 = 0;
        y1 = 0;
        break;
    case k_curve_linear:
        return t;
    default:
        break;
    }
    auto bezier = [](double p1, double p2, double s) {
        const double u = 1 - s;
        return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
    };
    double lo = 0, hi = 1, s = t;
    for (int i = 0; i < 40; i++) { // bisection: x(s) is increasing
        s = (lo + hi) / 2;
        if (bezier(x1, x2, s) < t) {
            lo = s;
        } else {
            hi = s;
        }
    }
    return bezier(y1, y2, s);
}

// Records a property change: an animation inside an animation block, else drops a running
// animation of that property.
void changed(mt_cpu *cpu, View &v, Prop p, const double *from) {
    State &st = state(cpu);
    if (st.groups.empty()) {
        v.anims.erase(std::remove_if(v.anims.begin(), v.anims.end(),
                                     [p](const Anim &a) { return a.prop == p; }),
                      v.anims.end());
        return;
    }
    Group::Pending pending{};
    pending.view = v.obj;
    pending.prop = p;
    std::copy(from, from + 6, pending.from);
    values_of(v.model, p, pending.to);
    Group &g = st.groups.back();
    for (Group::Pending &old : g.pending) {
        if (old.view == v.obj && old.prop == p) {
            std::copy(pending.to, pending.to + 6, old.to); // keep the first from value
            return;
        }
    }
    g.pending.push_back(pending);
}

// Runs before a property changes: its presentation value now, the animation's from value.
void before_change(mt_cpu *cpu, const View &v, Prop p, double *from) {
    const Geometry g = presentation(v, now(cpu));
    values_of(g, p, from);
}

// +[UIView beginAnimations:(NSString *)animationID context:(void *)context]
void begin_animations(mt_cpu *cpu) {
    State &st = state(cpu);
    Group g;
    if (!st.groups.empty()) {
        // A nested block starts with the enclosing block's settings.
        g.delay = st.groups.back().delay;
        g.duration = st.groups.back().duration;
        g.curve = st.groups.back().curve;
    }
    st.groups.push_back(std::move(g));
}

// The host's animation speed (mt_uikit_set_animation_scale, #90): delays and durations times
// this at commit. The guest's thread only.
double g_animation_scale = 1.0;

// +[UIView commitAnimations]: starts the block's animations at the current time, with the
// block's delay, duration and curve (set at any point inside the block), each delay and
// duration times the host's animation scale.
void commit_animations(mt_cpu *cpu) {
    State &st = state(cpu);
    if (st.groups.empty()) {
        log_once(cpu, "commitAnimations.unbalanced",
                 "+[UIView commitAnimations] without +beginAnimations:context: (called from "
                 "0x%08X): ignored",
                 static_cast<unsigned>(cpu->r[MT_LR]));
        return;
    }
    Group g = std::move(st.groups.back());
    st.groups.pop_back();
    const double t = now(cpu);
    for (const Group::Pending &p : g.pending) {
        View *v = view_of(cpu, p.view);
        if (v == nullptr) {
            continue;
        }
        v->anims.erase(std::remove_if(v->anims.begin(), v->anims.end(),
                                      [&p](const Anim &a) { return a.prop == p.prop; }),
                       v->anims.end());
        const double duration = g.duration * g_animation_scale;
        if (!(duration > 0)) {
            continue;
        }
        Anim a;
        a.prop = p.prop;
        std::copy(p.from, p.from + 6, a.from);
        std::copy(p.to, p.to + 6, a.to);
        a.start = t + std::max(0.0, g.delay) * g_animation_scale;
        a.duration = duration;
        a.curve = g.curve;
        v->anims.push_back(a);
    }
}

Group *current_group(mt_cpu *cpu, const char *what) {
    State &st = state(cpu);
    if (st.groups.empty()) {
        log_once(cpu, what, "%s outside an animation block (called from 0x%08X): ignored", what,
                 static_cast<unsigned>(cpu->r[MT_LR]));
        return nullptr;
    }
    return &st.groups.back();
}

// +[UIView setAnimationDuration:(NSTimeInterval)d]
void set_animation_duration(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double d = mt_arg_f64(&a);
    if (Group *g = current_group(cpu, "+[UIView setAnimationDuration:]")) {
        g->duration = d;
    }
}

// +[UIView setAnimationDelay:(NSTimeInterval)d]
void set_animation_delay(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double d = mt_arg_f64(&a);
    if (Group *g = current_group(cpu, "+[UIView setAnimationDelay:]")) {
        g->delay = d;
    }
}

// +[UIView setAnimationCurve:(UIViewAnimationCurve)c]
void set_animation_curve(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 c = mt_arg_u32(&a);
    if (Group *g = current_group(cpu, "+[UIView setAnimationCurve:]")) {
        g->curve = c <= k_curve_linear ? c : k_curve_ease_in_out;
    }
}

// --- UIView: life ------------------------------------------------------------------------------

// +[UIView layerClass]
void view_layer_class(mt_cpu *cpu) {
    mt_ret_u32(cpu, mt_objc_class_named(runtime(cpu), "CALayer"));
}

// -[UIView initWithFrame:(CGRect)frame]
void view_init_with_frame(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const Rect f = read_rect(&a);
    View *v = self_view(cpu, self, "-[UIView initWithFrame:]");
    if (v != nullptr) {
        v->model.bounds = {0, 0, f.w, f.h};
        v->model.center = {f.x + f.w / 2, f.y + f.h / 2};
    }
    mt_ret_u32(cpu, self);
}

// -[UIView init]: [self initWithFrame:CGRectZero] (subclasses' initWithFrame: run, as in UIKit).
void view_init(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    const u32 zero[4] = {0, 0, 0, 0};
    mt_ret_u32(cpu, mt_objc_msg(cpu, self, mt_objc_sel(runtime(cpu), "initWithFrame:"), zero, 4));
}

// The nib keys -[UIView initWithCoder:] reads (others are logged once per key).
const char *const k_view_keys[] = {
    "UIBounds",
    "UICenter",
    "UITransform",
    "UIBackgroundColor",
    "UIOpaque",
    "UIHidden",
    "UIAlpha",
    "UITag",
    "UIClipsToBounds",
    "UIAutoresizeSubviews",
    "UIAutoresizingMask",
    "UIClearsContextBeforeDrawing",
    "UIMultipleTouchEnabled",
    "UIUserInteractionDisabled",
    "UISubviews",
    "UIClassName",
    "UIOriginalClassName",
    "UIContentStretch",
    "UIContentMode",
    "UIViewDoesNotTranslateAutoresizingMaskIntoConstraints",
};

// -[UIView initWithCoder:(NSCoder *)coder]: the view's nib values (UINibDecoder, app.cpp), its
// subviews decoded and added in order.
void view_init_with_coder(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 coder = mt_arg_u32(&a);
    View *v = self_view(cpu, self, "-[UIView initWithCoder:]");
    if (v == nullptr) {
        mt_ret_u32(cpu, self);
        return;
    }
    float f[6] = {};
    if (coder_floats(cpu, coder, "UIBounds", f, 4)) {
        v->model.bounds = {f[0], f[1], f[2], f[3]};
    }
    if (coder_floats(cpu, coder, "UICenter", f, 2)) {
        v->model.center = {f[0], f[1]};
    }
    if (coder_floats(cpu, coder, "UITransform", f, 6)) {
        v->model.transform = {f[0], f[1], f[2], f[3], f[4], f[5]};
    }
    bool b = false;
    double n = 0;
    if (coder_bool(cpu, coder, "UIOpaque", &b)) {
        v->opaque = b;
    }
    if (coder_bool(cpu, coder, "UIHidden", &b)) {
        v->hidden = b;
    }
    if (coder_bool(cpu, coder, "UIClipsToBounds", &b)) {
        v->clips = b;
    }
    if (coder_bool(cpu, coder, "UIAutoresizeSubviews", &b)) {
        v->autoresizes = b;
    }
    if (coder_bool(cpu, coder, "UIClearsContextBeforeDrawing", &b)) {
        v->clears_context = b;
    }
    if (coder_bool(cpu, coder, "UIMultipleTouchEnabled", &b)) {
        v->multitouch = b;
    }
    if (coder_bool(cpu, coder, "UIUserInteractionDisabled", &b)) {
        v->interaction = !b;
    }
    if (coder_number(cpu, coder, "UIAlpha", &n)) {
        v->model.alpha = n;
    }
    if (coder_number(cpu, coder, "UITag", &n)) {
        v->tag = static_cast<i32>(n);
    }
    if (coder_number(cpu, coder, "UIAutoresizingMask", &n)) {
        v->autoresizing = static_cast<u32>(n);
    }
    const u32 color = coder_object(cpu, coder, "UIBackgroundColor");
    if (color != 0) {
        Color c;
        if (uicolor_value(cpu, color, &c)) {
            v = view_of(cpu, self); // the message may have run anything
            if (v == nullptr) {
                mt_ret_u32(cpu, self);
                return;
            }
            v->background_color = c;
            assign(cpu, &v->background, color);
        }
    }
    for (const std::string &key : coder_keys(cpu, coder)) {
        bool known = false;
        for (const char *k : k_view_keys) {
            known |= key == k;
        }
        if (!known) {
            log(cpu, "-[UIView initWithCoder:]: nib key %s of 0x%08X is not read", key.c_str(),
                static_cast<unsigned>(self));
        }
    }
    // The subviews last: decoding them runs their -initWithCoder: (and the EAGLView's starts
    // the engine).
    for (u32 sub : coder_objects(cpu, coder, "UISubviews")) {
        View *parent = view_of(cpu, self);
        if (parent != nullptr && view_state(cpu, sub) != nullptr) {
            add_subview(cpu, *parent, sub);
        }
    }
    mt_ret_u32(cpu, self);
}

// --- UIView: geometry --------------------------------------------------------------------------

// -[UIView frame] -> CGRect (stret)
void view_frame(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const View *v = self_view(cpu, self, "-[UIView frame]");
    ret_rect(cpu, result, v != nullptr ? frame_of(v->model) : Rect{});
}

// -[UIView setFrame:(CGRect)frame]
void view_set_frame(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const Rect f = read_rect(&a);
    if (View *v = self_view(cpu, self, "-[UIView setFrame:]")) {
        set_frame(cpu, *v, f);
    }
}

// -[UIView bounds] -> CGRect (stret)
void view_bounds(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const View *v = self_view(cpu, self, "-[UIView bounds]");
    ret_rect(cpu, result, v != nullptr ? v->model.bounds : Rect{});
}

// -[UIView setBounds:(CGRect)bounds]
void view_set_bounds(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const Rect b = read_rect(&a);
    if (View *v = self_view(cpu, self, "-[UIView setBounds:]")) {
        set_bounds(cpu, *v, b);
    }
}

// -[UIView center] -> CGPoint (stret)
void view_center(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const View *v = self_view(cpu, self, "-[UIView center]");
    ret_point(cpu, result, v != nullptr ? v->model.center : Point{});
}

// -[UIView setCenter:(CGPoint)center]
void view_set_center(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    Point c;
    c.x = mt_arg_f32(&a);
    c.y = mt_arg_f32(&a);
    if (View *v = self_view(cpu, self, "-[UIView setCenter:]")) {
        set_center(cpu, *v, c);
    }
}

// -[UIView setTransform:(CGAffineTransform)t]
void view_set_transform(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    Affine t;
    t.a = mt_arg_f32(&a);
    t.b = mt_arg_f32(&a);
    t.c = mt_arg_f32(&a);
    t.d = mt_arg_f32(&a);
    t.tx = mt_arg_f32(&a);
    t.ty = mt_arg_f32(&a);
    if (View *v = self_view(cpu, self, "-[UIView setTransform:]")) {
        set_transform(cpu, *v, t);
    }
}

// -[UIView alpha] -> CGFloat
void view_alpha(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView alpha]");
    mt_ret_f32(cpu, v != nullptr ? static_cast<float>(v->model.alpha) : 0.0f);
}

// -[UIView setAlpha:(CGFloat)alpha]
void view_set_alpha(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const float alpha = mt_arg_f32(&a);
    if (View *v = self_view(cpu, self, "-[UIView setAlpha:]")) {
        set_alpha(cpu, *v, alpha);
    }
}

// --- UIView: flags and properties ----------------------------------------------------------------

// A BOOL setter: the low byte of the argument.
bool read_bool(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    return (mt_arg_u32(&a) & 0xFFu) != 0;
}

void view_set_hidden(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setHidden:]")) {
        v->hidden = b;
    }
}

void view_is_hidden(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView isHidden]");
    mt_ret_u32(cpu, v != nullptr && v->hidden ? 1u : 0u);
}

void view_set_opaque(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setOpaque:]")) {
        v->opaque = b;
    }
}

void view_set_clips(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setClipsToBounds:]")) {
        v->clips = b;
    }
}

void view_set_interaction(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setUserInteractionEnabled:]")) {
        v->interaction = b;
    }
}

void view_set_multitouch(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setMultipleTouchEnabled:]")) {
        v->multitouch = b;
    }
}

void view_tag(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView tag]");
    mt_ret_u32(cpu, v != nullptr ? static_cast<u32>(v->tag) : 0u);
}

void view_set_tag(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const i32 tag = mt_arg_i32(&a);
    if (View *v = self_view(cpu, self, "-[UIView setTag:]")) {
        v->tag = tag;
    }
}

// -[UIView setBackgroundColor:(UIColor *)color]: nil is transparent.
void view_set_background(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 color = mt_arg_u32(&a);
    Color c{0, 0, 0, 0};
    if (color != 0 && !uicolor_value(cpu, color, &c)) {
        log_once(cpu, "setBackgroundColor.nocolor",
                 "-[UIView setBackgroundColor:]: 0x%08X is no UIColor: transparent",
                 static_cast<unsigned>(color));
    }
    if (View *v = self_view(cpu, self, "-[UIView setBackgroundColor:]")) {
        v->background_color = c;
        assign(cpu, &v->background, color);
    }
}

void view_background(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView backgroundColor]");
    mt_ret_u32(cpu, v != nullptr ? v->background : 0u);
}

// --- UIView: the tree --------------------------------------------------------------------------

// -[UIView addSubview:(UIView *)view]
void view_add_subview(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 sub = mt_arg_u32(&a);
    View *v = self_view(cpu, self, "-[UIView addSubview:]");
    if (v == nullptr || sub == 0 || sub == self) {
        return;
    }
    if (view_state(cpu, sub) == nullptr) {
        log(cpu, "-[UIView addSubview:]: 0x%08X is no UIView (called from 0x%08X): ignored",
            static_cast<unsigned>(sub), static_cast<unsigned>(cpu->r[MT_LR]));
        return;
    }
    add_subview(cpu, *v, sub);
}

// -[UIView removeFromSuperview]
void view_remove_from_superview(mt_cpu *cpu) {
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView removeFromSuperview]")) {
        remove_from_superview(cpu, *v);
    }
}

// -[UIView subviews]: an autoreleased NSArray, back to front.
void view_subviews(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView subviews]");
    const std::vector<u32> items = v != nullptr ? v->subviews : std::vector<u32>();
    const u32 array = mt_ns::array_create(cpu, items.empty() ? nullptr : items.data(),
                                          static_cast<u32>(items.size()), false);
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, array));
}

void view_superview(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView superview]");
    mt_ret_u32(cpu, v != nullptr ? v->superview : 0u);
}

void view_window(mt_cpu *cpu) {
    const View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView window]");
    mt_ret_u32(cpu, v != nullptr ? window_of(cpu, *v) : 0u);
}

// -[UIView layer]: made on first use from [[self class] layerClass], with the view as its
// delegate.
void view_layer(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    View *v = self_view(cpu, self, "-[UIView layer]");
    if (v == nullptr) {
        mt_ret_u32(cpu, 0);
        return;
    }
    if (v->layer == 0) {
        mt_objc *rt = runtime(cpu);
        const u32 cls = mt_objc_class_of(rt, self);
        const u32 layer_class = mt_objc_send0(cpu, cls, "layerClass");
        u32 layer = layer_class != 0 ? mt_objc_send0(cpu, layer_class, "alloc") : 0u;
        layer = layer != 0 ? mt_objc_send0(cpu, layer, "init") : 0u;
        v = view_of(cpu, self);
        if (v == nullptr) {
            mt_objc_release(cpu, layer);
            mt_ret_u32(cpu, 0);
            return;
        }
        v->layer = layer;
        v->gl = layer != 0 && is_kind(cpu, layer, "CAEAGLLayer");
        layer_set_view(cpu, layer, self);
    }
    mt_ret_u32(cpu, v->layer);
}

// -[UIView setNeedsDisplay]
void view_set_needs_display(mt_cpu *cpu) {
    if (View *v = self_view(cpu, mt_objc_self(cpu), "-[UIView setNeedsDisplay]")) {
        v->needs_display = true;
    }
}

// -[UIView drawRect:(CGRect)rect]: UIView draws nothing ([super drawRect:] in a subclass).
void view_draw_rect(mt_cpu *) {}

// UIResponder's touches*: the next responder gets them (a view's superview; a window ends the
// chain here, the application does nothing with them).
void forward_touches(mt_cpu *cpu, const char *selector) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 touches = mt_arg_u32(&a), event = mt_arg_u32(&a);
    const View *v = view_of(cpu, self);
    const u32 next = v != nullptr ? v->superview : 0u;
    if (next != 0) {
        mt_objc_send2(cpu, next, selector, touches, event);
    }
}
void responder_touches_began(mt_cpu *cpu) { forward_touches(cpu, "touchesBegan:withEvent:"); }
void responder_touches_moved(mt_cpu *cpu) { forward_touches(cpu, "touchesMoved:withEvent:"); }
void responder_touches_ended(mt_cpu *cpu) { forward_touches(cpu, "touchesEnded:withEvent:"); }
void responder_touches_cancelled(mt_cpu *cpu) {
    forward_touches(cpu, "touchesCancelled:withEvent:");
}

// --- CALayer, CAEAGLLayer ------------------------------------------------------------------------

struct LayerState {
    u32 view;                // its view (the delegate), not retained
    u32 drawable_properties; // retained
    bool opaque;
};

void layer_destroy(mt_cpu *cpu, u32, void *block) {
    mt_objc_release(cpu, static_cast<LayerState *>(block)->drawable_properties);
}
const mt_objc_state_type k_layer_state = {"CALayer", sizeof(LayerState), layer_destroy};

LayerState *layer_state(mt_cpu *cpu, u32 layer) {
    return layer != 0
               ? static_cast<LayerState *>(mt_objc_state(runtime(cpu), layer, &k_layer_state))
               : nullptr;
}

void layer_set_opaque(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (LayerState *s = layer_state(cpu, mt_objc_self(cpu))) {
        s->opaque = b;
    }
}

void layer_is_opaque(mt_cpu *cpu) {
    const LayerState *s = layer_state(cpu, mt_objc_self(cpu));
    mt_ret_u32(cpu, s != nullptr && s->opaque ? 1u : 0u);
}

// -[CAEAGLLayer setDrawableProperties:(NSDictionary *)properties]: kept for EAGL (#117).
void layer_set_drawable_properties(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    mt_args a = mt_objc_args(cpu);
    const u32 props = mt_arg_u32(&a);
    if (LayerState *s = layer_state(cpu, self)) {
        assign(cpu, &s->drawable_properties, props);
    }
}

void layer_drawable_properties(mt_cpu *cpu) {
    const LayerState *s = layer_state(cpu, mt_objc_self(cpu));
    mt_ret_u32(cpu, s != nullptr ? s->drawable_properties : 0u);
}

// -[CALayer bounds] -> CGRect (stret): its view's.
void layer_bounds(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const LayerState *s = layer_state(cpu, self);
    const View *v = s != nullptr ? view_of(cpu, s->view) : nullptr;
    ret_rect(cpu, result, v != nullptr ? v->model.bounds : Rect{});
}

const mt_objc_host_method k_responder_methods[] = {
    {"touchesBegan:withEvent:", responder_touches_began},
    {"touchesMoved:withEvent:", responder_touches_moved},
    {"touchesEnded:withEvent:", responder_touches_ended},
    {"touchesCancelled:withEvent:", responder_touches_cancelled},
};

const mt_objc_host_method k_view_class_methods[] = {
    {"layerClass", view_layer_class},
    {"beginAnimations:context:", begin_animations},
    {"commitAnimations", commit_animations},
    {"setAnimationDuration:", set_animation_duration},
    {"setAnimationDelay:", set_animation_delay},
    {"setAnimationCurve:", set_animation_curve},
};
const mt_objc_host_method k_view_methods[] = {
    {"init", view_init},
    {"initWithFrame:", view_init_with_frame},
    {"initWithCoder:", view_init_with_coder},
    {"frame", view_frame},
    {"setFrame:", view_set_frame},
    {"bounds", view_bounds},
    {"setBounds:", view_set_bounds},
    {"center", view_center},
    {"setCenter:", view_set_center},
    {"setTransform:", view_set_transform},
    {"alpha", view_alpha},
    {"setAlpha:", view_set_alpha},
    {"isHidden", view_is_hidden},
    {"setHidden:", view_set_hidden},
    {"setOpaque:", view_set_opaque},
    {"setClipsToBounds:", view_set_clips},
    {"setUserInteractionEnabled:", view_set_interaction},
    {"setMultipleTouchEnabled:", view_set_multitouch},
    {"tag", view_tag},
    {"setTag:", view_set_tag},
    {"backgroundColor", view_background},
    {"setBackgroundColor:", view_set_background},
    {"addSubview:", view_add_subview},
    {"removeFromSuperview", view_remove_from_superview},
    {"subviews", view_subviews},
    {"superview", view_superview},
    {"window", view_window},
    {"layer", view_layer},
    {"setNeedsDisplay", view_set_needs_display},
    {"drawRect:", view_draw_rect},
};
const mt_objc_host_method k_layer_methods[] = {
    {"setOpaque:", layer_set_opaque},
    {"isOpaque", layer_is_opaque},
    {"bounds", layer_bounds},
};
const mt_objc_host_method k_eagl_layer_methods[] = {
    {"setDrawableProperties:", layer_set_drawable_properties},
    {"drawableProperties", layer_drawable_properties},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

} // namespace

// --- shared ------------------------------------------------------------------------------------

View *view_of(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return nullptr;
    }
    void *s = mt_objc_find_state(rt, obj, &k_view_state);
    return s != nullptr ? *static_cast<View **>(s) : nullptr;
}

View *view_state(mt_cpu *cpu, u32 obj) {
    if (View *v = view_of(cpu, obj)) {
        return v;
    }
    if (obj == 0 || !is_kind(cpu, obj, "UIView")) {
        return nullptr;
    }
    void *s = mt_objc_state(runtime(cpu), obj, &k_view_state);
    if (s == nullptr) {
        return nullptr;
    }
    auto *v = new View();
    v->obj = obj;
    v->kind = kind_of(cpu, obj);
    *static_cast<View **>(s) = v;
    control_defaults(cpu, *v);
    return v;
}

bool is_kind(mt_cpu *cpu, u32 obj, const char *name) {
    mt_objc *rt = runtime(cpu);
    const u32 cls = rt != nullptr ? mt_objc_class_named(rt, name) : 0u;
    return cls != 0 && obj != 0 && mt_objc_is_kind_of(rt, obj, cls) != 0;
}

Affine local_to_super(const Geometry &g) {
    const Rect &b = g.bounds;
    return Affine::translate(-(b.x + b.w / 2), -(b.y + b.h / 2))
        .then(g.transform)
        .then(Affine::translate(g.center.x, g.center.y));
}

Rect frame_of(const Geometry &g) {
    const Affine &t = g.transform;
    if (t.a == 1 && t.b == 0 && t.c == 0 && t.d == 1 && t.tx == 0 && t.ty == 0) {
        return {g.center.x - g.bounds.w / 2, g.center.y - g.bounds.h / 2, g.bounds.w, g.bounds.h};
    }
    const Affine m = local_to_super(g);
    const Rect &b = g.bounds;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Point p : {Point{b.x, b.y}, Point{b.x + b.w, b.y}, Point{b.x, b.y + b.h},
                          Point{b.x + b.w, b.y + b.h}}) {
        const Point q = m.apply(p);
        x0 = std::min(x0, q.x);
        y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x);
        y1 = std::max(y1, q.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

Affine local_to_screen(mt_cpu *cpu, const View &v) {
    Affine m = local_to_super(v.model);
    const View *p = view_of(cpu, v.superview);
    for (int depth = 0; p != nullptr && depth < 256; depth++) {
        m = m.then(local_to_super(p->model));
        p = view_of(cpu, p->superview);
    }
    return m;
}

u32 window_of(mt_cpu *cpu, const View &v) {
    const View *p = &v;
    for (int depth = 0; p->superview != 0 && depth < 256; depth++) {
        const View *up = view_of(cpu, p->superview);
        if (up == nullptr) {
            return 0;
        }
        p = up;
    }
    return p->kind == Kind::window ? p->obj : 0u;
}

// UIInterfaceOrientationLandscapeLeft (Info.plist): the portrait window's top edge on the
// right, so window (0, 0) is the screen's top-right corner: (x, y) -> (480 - y, x).
Affine screen_from_window() { return Affine{0, 1, -1, 0, 480, 0}; }

Geometry presentation(const View &v, double now) {
    Geometry g = v.model;
    for (const Anim &a : v.anims) {
        double values[6] = {};
        const int n = k_value_count[a.prop];
        if (now < a.start) {
            std::copy(a.from, a.from + 6, values);
        } else if (now >= a.start + a.duration) {
            std::copy(a.to, a.to + 6, values);
        } else {
            const double p = timing(a.curve, (now - a.start) / a.duration);
            for (int i = 0; i < n; i++) {
                values[i] = a.from[i] + (a.to[i] - a.from[i]) * p;
            }
        }
        apply_values(g, a.prop, values);
    }
    return g;
}

void prune_animations(View &v, double now) {
    v.anims.erase(std::remove_if(v.anims.begin(), v.anims.end(),
                                 [now](const Anim &a) { return now >= a.start + a.duration; }),
                  v.anims.end());
}

void set_center(mt_cpu *cpu, View &v, Point c) {
    double from[6] = {};
    before_change(cpu, v, k_prop_center, from);
    v.model.center = c;
    changed(cpu, v, k_prop_center, from);
}

void set_bounds(mt_cpu *cpu, View &v, const Rect &b) {
    double from[6] = {};
    before_change(cpu, v, k_prop_bounds, from);
    v.model.bounds = b;
    changed(cpu, v, k_prop_bounds, from);
    if (v.kind == Kind::text_view) {
        text_view_layout(v);
    }
}

void set_frame(mt_cpu *cpu, View &v, const Rect &f) {
    Rect b = v.model.bounds;
    const Affine &t = v.model.transform;
    if (t.b == 0 && t.c == 0 && t.a != 0 && t.d != 0) {
        b.w = f.w / std::fabs(t.a);
        b.h = f.h / std::fabs(t.d);
    } else {
        // A turned view: the bounds whose box under the transform is the frame's size,
        // |a| w + |c| h = fw, |b| w + |d| h = fh.
        const double a = std::fabs(t.a), c = std::fabs(t.c), bb = std::fabs(t.b),
                     d = std::fabs(t.d);
        const double det = a * d - c * bb;
        if (std::fabs(det) > 1e-9) {
            b.w = (f.w * d - c * f.h) / det;
            b.h = (a * f.h - bb * f.w) / det;
        }
    }
    set_center(cpu, v, {f.x + f.w / 2, f.y + f.h / 2});
    set_bounds(cpu, v, b);
}

void set_alpha(mt_cpu *cpu, View &v, double alpha) {
    double from[6] = {};
    before_change(cpu, v, k_prop_alpha, from);
    v.model.alpha = alpha;
    changed(cpu, v, k_prop_alpha, from);
}

void set_transform(mt_cpu *cpu, View &v, const Affine &t) {
    double from[6] = {};
    before_change(cpu, v, k_prop_transform, from);
    v.model.transform = t;
    changed(cpu, v, k_prop_transform, from);
}

void add_subview(mt_cpu *cpu, View &superview, u32 sub) {
    const u32 parent_obj = superview.obj;
    mt_objc_retain(cpu, sub);
    View *s = view_of(cpu, sub);
    if (s == nullptr) {
        mt_objc_release(cpu, sub);
        return;
    }
    if (s->superview != 0) {
        remove_from_superview(cpu, *s); // still retained by us
    }
    View *p = view_of(cpu, parent_obj);
    s = view_of(cpu, sub);
    if (p == nullptr || s == nullptr) {
        mt_objc_release(cpu, sub);
        return;
    }
    s->superview = parent_obj;
    p->subviews.push_back(sub);
}

void remove_from_superview(mt_cpu *cpu, View &v) {
    View *p = view_of(cpu, v.superview);
    if (p == nullptr) {
        v.superview = 0;
        return;
    }
    const u32 obj = v.obj;
    auto &s = p->subviews;
    const auto it = std::find(s.begin(), s.end(), obj);
    v.superview = 0;
    if (it == s.end()) {
        return;
    }
    s.erase(it);
    touch_forget_view(obj);
    mt_objc_release(cpu, obj); // may deallocate it (and `v`)
}

void display_views(mt_cpu *cpu) {
    State &st = state(cpu);
    for (int pass = 0; pass < 8; pass++) {
        // The views in visible windows that need display, back to front, parents first.
        std::vector<u32> todo;
        std::vector<u32> stack(st.windows.rbegin(), st.windows.rend());
        while (!stack.empty() && stack.size() < 100000) {
            const u32 obj = stack.back();
            stack.pop_back();
            const View *v = view_of(cpu, obj);
            if (v == nullptr || v->hidden) {
                continue;
            }
            if (v->needs_display) {
                todo.push_back(obj);
            }
            stack.insert(stack.end(), v->subviews.rbegin(), v->subviews.rend());
        }
        bool drew = false;
        for (const u32 obj : todo) {
            View *v = view_of(cpu, obj);
            if (v == nullptr || !v->needs_display) {
                continue;
            }
            v->needs_display = false;
            if (!overrides(cpu, obj, "drawRect:", "UIView")) {
                continue;
            }
            const Rect b = v->model.bounds;
            const double pw = std::ceil(b.w - 1e-6), ph = std::ceil(b.h - 1e-6);
            if (!(pw >= 1 && ph >= 1 && pw <= 4096 && ph <= 4096)) {
                continue;
            }
            const u32 w = static_cast<u32>(pw), h = static_cast<u32>(ph);
            const u32 ctx = mt_cg::push_view_context(cpu, w, h, b.x, b.y);
            mt_objc_retain(cpu, ctx);
            const u32 rect[4] = {
                mt_f32_bits(static_cast<float>(b.x)), mt_f32_bits(static_cast<float>(b.y)),
                mt_f32_bits(static_cast<float>(b.w)), mt_f32_bits(static_cast<float>(b.h))};
            mt_objc_msg(cpu, obj, mt_objc_sel(runtime(cpu), "drawRect:"), rect, 4);
            mt_cg::pop_view_context(cpu, ctx);
            drew = true;
            v = view_of(cpu, obj);
            mt_cg::Canvas *canvas = nullptr;
            mt_cg::Surface s;
            if (v != nullptr && mt_cg::context_canvas(cpu, ctx, &canvas, &s)) {
                v->contents_width = s.width;
                v->contents_height = s.height;
                v->contents.assign(static_cast<std::size_t>(s.width) * s.height * 4, 0);
                bool blank = true;
                for (u32 y = 0; y < s.height; y++) {
                    const u8 *row = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row;
                    u8 *out = v->contents.data() + static_cast<std::size_t>(y) * s.width * 4;
                    for (u32 x = 0; x < s.width; x++) {
                        const mt_cg::Pixel p = mt_cg::load_pixel(s.format, row + x * 4);
                        out[4 * x] = p.r;
                        out[4 * x + 1] = p.g;
                        out[4 * x + 2] = p.b;
                        out[4 * x + 3] = p.a;
                        blank &= p.a == 0;
                    }
                }
                v->contents_blank = blank;
            }
            mt_objc_release(cpu, ctx);
        }
        if (!drew) {
            break;
        }
    }
}

// --- the classes ---------------------------------------------------------------------------------

const mt_objc_host_class k_uiresponder = {
    "UIResponder", "NSObject", 4, k_responder_methods, countof(k_responder_methods), nullptr, 0};
// UIView: the SDK's 46 bytes, which the image's 17 subclasses start their ivars after.
const mt_objc_host_class k_uiview = {"UIView",
                                     "UIResponder",
                                     46,
                                     k_view_methods,
                                     countof(k_view_methods),
                                     k_view_class_methods,
                                     countof(k_view_class_methods)};
const mt_objc_host_class k_uiwindow = {"UIWindow", "UIView", 46, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_calayer = {
    "CALayer", "NSObject", 4, k_layer_methods, countof(k_layer_methods), nullptr, 0};
const mt_objc_host_class k_caeagllayer = {
    "CAEAGLLayer", "CALayer", 4, k_eagl_layer_methods, countof(k_eagl_layer_methods), nullptr, 0};

void layer_set_view(mt_cpu *cpu, u32 layer, u32 view) {
    if (LayerState *s = layer_state(cpu, layer)) {
        s->view = view;
    }
}

u32 layer_view(mt_cpu *cpu, u32 layer) {
    mt_objc *rt = runtime(cpu);
    const auto *s =
        rt != nullptr && layer != 0
            ? static_cast<const LayerState *>(mt_objc_find_state(rt, layer, &k_layer_state))
            : nullptr;
    return s != nullptr ? s->view : 0u;
}

} // namespace mt_ui

extern "C" int mt_uikit_set_animation_scale(double scale) noexcept {
    if (!std::isfinite(scale) || scale < 0) {
        return 0;
    }
    mt_ui::g_animation_scale = std::min(scale, 1.0);
    return 1;
}

extern "C" double mt_uikit_animation_scale(void) noexcept { return mt_ui::g_animation_scale; }
