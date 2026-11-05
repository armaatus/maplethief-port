// UITouch, UIEvent, hit testing and touch delivery; UIControl tracking; UIScrollView dragging,
// deceleration and paging (#54, ADR 0037).
//
// The host queues touches (mt_uikit_touch, any thread); mt_uikit_turn delivers them on the
// guest thread. Each finger is one UITouch for its whole life. A touch belongs to the view the
// hit test finds when it begins (the frontmost visible, interactive view whose bounds hold
// the point); it gets touchesBegan:/Moved:/Ended:/Cancelled:withEvent: with an NSSet of its
// touches. A view that is not multipleTouchEnabled gets only the first of simultaneous
// touches, as in UIKit.
//
// Scroll views (UIKit's delaysContentTouches and canCancelContentTouches): a touch that begins
// inside a scroll view that can scroll is held back from the content for 150 ms; if it moves
// 10 points first, the scroll view drags and the content never sees it. Otherwise the content
// gets it, and a later 10-point move still starts a drag (the content's touch is cancelled)
// if the scroll view can cancel content touches. When the drag ends, a paging scroll view
// settles on the nearest page (or the next one in the flick's direction), another one
// decelerates; both bounce back inside the content.
//
// Taps (#65, ADR 0047): a touch that begins within k_multi_tap_interval of the previous tap's
// end and within k_multi_tap_distance of it counts one more (-[UITouch tapCount]: 2 for a
// double tap); a touch that moves k_drag_threshold or is cancelled is no tap and ends the run.
// Moves of one finger queued in one turn are merged into the last one, as UIKit merges the
// moves within a frame: previousLocationInView: is where the last delivered move was.
//
// The game's touch code (docs/re/objc-shell.md): TouchKeyPadView and the key pads override
// touches* and read [[touches allObjects] objectAtIndex:i], [touch view],
// [touch locationInView:self] and previousLocationInView:; buttons send their touch-down /
// touch-up-inside actions.
#include <algorithm>
#include <cmath>
#include <mutex>

#include "foundation/foundation.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

constexpr double k_drag_threshold = 10;       // points before a touch drags a scroll view
constexpr double k_content_delay = 0.15;      // seconds content touches are held back
constexpr double k_control_margin = 70;       // UIControl's touch-inside margin (points)
constexpr double k_settle_duration = 0.3;     // paging and bounce-back animations (seconds)
constexpr double k_deceleration = 0.998;      // velocity factor per millisecond (normal rate)
constexpr double k_multi_tap_interval = 0.35; // seconds from a tap's end to the next one's start
constexpr double k_multi_tap_distance = 25;   // points between a tap and the next one

// --- the queue -----------------------------------------------------------------------------------

struct Queued {
    u32 finger;
    u32 phase;
    float x, y; // landscape screen points
};

std::mutex g_queue_mutex;
std::vector<Queued> g_queue;

// --- touches -------------------------------------------------------------------------------------

struct Touch {
    u32 finger = 0;
    u32 obj = 0;    // the UITouch, retained
    u32 view = 0;   // the view it belongs to (0: none), not retained (forgotten on dealloc)
    u32 scroll = 0; // the scroll view that may take it, not retained
    Point start, now, previous; // portrait window points
    double began = 0;           // time of touchesBegan
    bool delivered = false;     // the view has had touchesBegan
    bool dragging = false;      // the scroll view took it
    u32 phase = MT_UIKIT_TOUCH_BEGAN;
    double timestamp = 0;
    u32 tap_count = 1;
    bool moved_far = false; // moved k_drag_threshold from its start: not a tap
    // the scroll view's drag
    double drag_offset_x = 0, drag_offset_y = 0; // content offset when the drag began
    Point drag_start;                            // window point when the drag began
    double last_time = 0;
    Point last_point;
    // The host touch area's x shift (mt_uikit_set_host_touch_area, #168), in landscape screen
    // points, fixed when the touch begins: its points above are the shifted ones.
    double shift = 0;
};

std::vector<Touch> g_touches;

// The host's touch area (mt_uikit_set_host_touch_area): one view, its shift and the margins
// beside the screen whose touches it takes. Guest thread (set between turns).
struct HostTouchArea {
    u32 view = 0;
    double shift = 0, left = 0, right = 0;
};
HostTouchArea g_host_area;
std::vector<u32> g_scrolling; // scroll views decelerating or settling (not retained)

// The last tap (a touch that ended without moving far), for the next touch's tapCount.
struct LastTap {
    bool valid = false;
    Point at;       // where it ended (portrait window points)
    double end = 0; // when
    u32 count = 0;  // its tapCount
};
LastTap g_last_tap;

// The touch being sent to its view (mt_uikit_touch_delivering, #194).
struct Delivering {
    bool on = false;
    u32 finger = 0, phase = 0;
};
Delivering g_delivering;

struct TouchState {
    Point now, previous;
    u32 view;
    u32 phase;
    double timestamp;
    u32 tap_count;
};
const mt_objc_state_type k_touch_state = {"UITouch", sizeof(TouchState), nullptr};

TouchState *touch_state(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && obj != 0
               ? static_cast<TouchState *>(mt_objc_find_state(rt, obj, &k_touch_state))
               : nullptr;
}

// Landscape screen -> portrait window (the inverse of screen_from_window: x' = y, y' = 480 - x).
Point window_point(float x, float y) { return Point{y, 480.0 - x}; }

// Portrait window -> a view's local space (model values), or false.
bool to_local(mt_cpu *cpu, u32 view, Point p, Point *out) {
    const View *v = view_of(cpu, view);
    if (v == nullptr) {
        return false;
    }
    Affine inv;
    if (!local_to_screen(cpu, *v).invert(&inv)) {
        return false;
    }
    *out = inv.apply(p);
    return true;
}

bool inside(const Rect &b, Point p) {
    return p.x >= b.x && p.y >= b.y && p.x < b.x + b.w && p.y < b.y + b.h;
}

// -[UIView hitTest:withEvent:]: the frontmost view under `p` (portrait window point).
u32 hit_test(mt_cpu *cpu, u32 obj, Point p, int depth) {
    const View *v = view_of(cpu, obj);
    if (v == nullptr || depth > 64 || v->hidden || !v->interaction || v->model.alpha < 0.01) {
        return 0;
    }
    Point local;
    if (!to_local(cpu, obj, p, &local) || !inside(v->model.bounds, local)) {
        return 0;
    }
    const std::vector<u32> subs = v->subviews;
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
        if (const u32 hit = hit_test(cpu, *it, p, depth + 1)) {
            return hit;
        }
    }
    return obj;
}

// The innermost scroll view at or above `view` that can scroll, or 0.
u32 scroll_ancestor(mt_cpu *cpu, u32 view) {
    const View *v = view_of(cpu, view);
    for (int depth = 0; v != nullptr && depth < 256; depth++) {
        if ((v->kind == Kind::scroll_view || v->kind == Kind::text_view) && v->scroll_enabled) {
            double mx = 0, my = 0;
            max_content_offset(*v, &mx, &my);
            if (mx > 0 || my > 0 || v->paging) {
                return v->obj;
            }
        }
        v = view_of(cpu, v->superview);
    }
    return 0;
}

} // namespace

u32 hit_test_screen(mt_cpu *cpu, Point window_point) {
    const State &st = state(cpu);
    const std::vector<u32> windows = st.windows;
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        if (const u32 hit = hit_test(cpu, *it, window_point, 0)) {
            return hit;
        }
    }
    return 0;
}

namespace {

u32 new_touch_object(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    return mt_objc_create_instance(rt, mt_objc_class_named(rt, "UITouch"));
}

void sync_touch(mt_cpu *cpu, const Touch &t) {
    if (TouchState *s = static_cast<TouchState *>(
            t.obj != 0 ? mt_objc_state(runtime(cpu), t.obj, &k_touch_state) : nullptr)) {
        s->now = t.now;
        s->previous = t.previous;
        s->view = t.view;
        s->phase = t.phase;
        s->timestamp = t.timestamp;
        s->tap_count = t.tap_count;
    }
}

u32 g_event = 0; // the UIEvent (one for the run, as in UIKit), retained

u32 event_object(mt_cpu *cpu) {
    if (g_event == 0) {
        mt_objc *rt = runtime(cpu);
        g_event = mt_objc_create_instance(rt, mt_objc_class_named(rt, "UIEvent"));
    }
    return g_event;
}

const char *phase_selector(u32 phase) {
    switch (phase) {
    case MT_UIKIT_TOUCH_BEGAN:
        return "touchesBegan:withEvent:";
    case MT_UIKIT_TOUCH_MOVED:
        return "touchesMoved:withEvent:";
    case MT_UIKIT_TOUCH_ENDED:
        return "touchesEnded:withEvent:";
    default:
        return "touchesCancelled:withEvent:";
    }
}

// [view touchesX:{touch} withEvent:event].
void send_touch(mt_cpu *cpu, u32 view, u32 touch, u32 phase) {
    if (view == 0 || view_of(cpu, view) == nullptr) {
        return;
    }
    if (mt_trace_enabled("uikit")) {
        mt_objc *rt = runtime(cpu);
        mt_tracef("uikit", "-[%s 0x%08X %s] (touch 0x%08X)",
                  mt_objc_class_name(rt, mt_objc_class_of(rt, view)), static_cast<unsigned>(view),
                  phase_selector(phase), static_cast<unsigned>(touch));
    }
    const u32 set = mt_ns::set_create(cpu, &touch, 1);
    mt_objc_send2(cpu, view, phase_selector(phase), mt_objc_autorelease(cpu, set),
                  event_object(cpu));
}

// --- scrolling -----------------------------------------------------------------------------------

// The drag vector (window points) in the scroll view's local space.
Point local_vector(mt_cpu *cpu, const View &v, Point d) {
    Affine inv;
    if (!local_to_screen(cpu, v).invert(&inv)) {
        return d;
    }
    return inv.apply_vector(d);
}

// Rubber band: past the edges a drag moves the content half as far.
double rubber(double offset, double max) {
    if (offset < 0) {
        return offset / 2;
    }
    if (offset > max) {
        return max + (offset - max) / 2;
    }
    return offset;
}

void begin_drag(mt_cpu *cpu, Touch &t, double now) {
    View *s = view_of(cpu, t.scroll);
    if (s == nullptr) {
        t.scroll = 0;
        return;
    }
    t.dragging = true;
    t.drag_offset_x = s->model.bounds.x;
    t.drag_offset_y = s->model.bounds.y;
    t.drag_start = t.now;
    t.last_time = now;
    t.last_point = t.now;
    s->dragging = true;
    s->decelerating = false;
    s->velocity_x = s->velocity_y = 0;
    g_scrolling.erase(std::remove(g_scrolling.begin(), g_scrolling.end(), t.scroll),
                      g_scrolling.end());
    const u32 scroll = t.scroll, delegate = s->delegate;
    if (delegate != 0 && responds(cpu, delegate, "scrollViewWillBeginDragging:")) {
        mt_objc_send1(cpu, delegate, "scrollViewWillBeginDragging:", scroll);
    }
}

void drag(mt_cpu *cpu, Touch &t, double now) {
    View *s = view_of(cpu, t.scroll);
    if (s == nullptr) {
        return;
    }
    const Point d =
        local_vector(cpu, *s, Point{t.now.x - t.drag_start.x, t.now.y - t.drag_start.y});
    double mx = 0, my = 0;
    max_content_offset(*s, &mx, &my);
    const bool horizontal = mx > 0 || (s->paging && s->content_width > s->model.bounds.w);
    const bool vertical = my > 0 || (s->paging && s->content_height > s->model.bounds.h);
    const double x = horizontal ? rubber(t.drag_offset_x - d.x, mx) : s->model.bounds.x;
    const double y = vertical ? rubber(t.drag_offset_y - d.y, my) : s->model.bounds.y;
    const double dt = now - t.last_time;
    if (dt > 1e-3) {
        const Point v =
            local_vector(cpu, *s, Point{t.now.x - t.last_point.x, t.now.y - t.last_point.y});
        // Smoothed, in content-offset points per second.
        s->velocity_x = 0.8 * (-v.x / dt) + 0.2 * s->velocity_x;
        s->velocity_y = 0.8 * (-v.y / dt) + 0.2 * s->velocity_y;
        t.last_time = now;
        t.last_point = t.now;
    }
    set_content_offset(cpu, *s, x, y);
}

// An animation of the content offset to (x, y) over k_settle_duration.
void settle(View &s, double now, double x, double y) {
    s.decelerating = true;
    s.scroll_from_x = s.model.bounds.x;
    s.scroll_from_y = s.model.bounds.y;
    s.scroll_to_x = x;
    s.scroll_to_y = y;
    s.scroll_start = now;
    s.scroll_last = -1; // a settle, not a deceleration
    s.velocity_x = s.velocity_y = 0;
    if (std::find(g_scrolling.begin(), g_scrolling.end(), s.obj) == g_scrolling.end()) {
        g_scrolling.push_back(s.obj);
    }
}

void end_drag(mt_cpu *cpu, Touch &t, double now) {
    View *s = view_of(cpu, t.scroll);
    t.dragging = false;
    if (s == nullptr) {
        return;
    }
    s->dragging = false;
    double mx = 0, my = 0;
    max_content_offset(*s, &mx, &my);
    if (now - t.last_time > 0.1) {
        s->velocity_x = s->velocity_y = 0; // held still before lifting
    }
    const double x = s->model.bounds.x, y = s->model.bounds.y;
    if (s->paging) {
        auto page_target = [](double offset, double size, double velocity, double max) {
            if (!(size > 0)) {
                return 0.0;
            }
            double page = std::floor(offset / size + 0.5);
            if (velocity > 300) {
                page = std::floor(offset / size) + 1;
            } else if (velocity < -300) {
                page = std::ceil(offset / size) - 1;
            }
            return std::max(0.0, std::min(page * size, max));
        };
        const double tx = page_target(x, s->model.bounds.w, s->velocity_x, mx);
        const double ty = page_target(y, s->model.bounds.h, s->velocity_y, my);
        if (mt_trace_enabled("uikit")) {
            // Horizontal pages if it scrolls sideways, else vertical ones.
            const bool sideways = mx > 0 && s->model.bounds.w > 0;
            const double size = sideways ? s->model.bounds.w : s->model.bounds.h;
            const long page = size > 0 ? std::lround((sideways ? tx : ty) / size) : 0;
            mt_tracef("uikit", "UIScrollView 0x%08X: drag ended, pages to page %ld (offset %g, %g)",
                      static_cast<unsigned>(s->obj), page, tx, ty);
        }
        settle(*s, now, tx, ty);
    } else if (x < 0 || y < 0 || x > mx || y > my) {
        settle(*s, now, std::max(0.0, std::min(x, mx)), std::max(0.0, std::min(y, my)));
    } else if (std::fabs(s->velocity_x) > 20 || std::fabs(s->velocity_y) > 20) {
        s->decelerating = true;
        s->scroll_last = now;
        if (std::find(g_scrolling.begin(), g_scrolling.end(), s->obj) == g_scrolling.end()) {
            g_scrolling.push_back(s->obj);
        }
    }
    const u32 scroll = t.scroll, delegate = s->delegate;
    const bool decelerate = s->decelerating;
    if (delegate != 0 && responds(cpu, delegate, "scrollViewDidEndDragging:willDecelerate:")) {
        mt_objc_send2(cpu, delegate, "scrollViewDidEndDragging:willDecelerate:", scroll,
                      decelerate ? 1u : 0u);
    }
}

// --- delivery ------------------------------------------------------------------------------------

Touch *find_touch(u32 finger) {
    for (Touch &t : g_touches) {
        if (t.finger == finger) {
            return &t;
        }
    }
    return nullptr;
}

// Whether another live touch already belongs to `view`.
bool view_has_touch(u32 view, const Touch *except) {
    for (const Touch &t : g_touches) {
        if (&t != except && t.view == view && t.view != 0) {
            return true;
        }
    }
    return false;
}

void deliver(mt_cpu *cpu, Touch &t, u32 phase) {
    if (!t.delivered && phase != MT_UIKIT_TOUCH_BEGAN) {
        return;
    }
    t.phase = phase;
    sync_touch(cpu, t);
    if (phase == MT_UIKIT_TOUCH_BEGAN) {
        t.delivered = true;
    }
    const Delivering outer = g_delivering;
    g_delivering = Delivering{true, t.finger, phase};
    send_touch(cpu, t.view, t.obj, phase);
    g_delivering = outer;
}

// A portrait window point moved `shift` landscape screen points along x (window y = 480 - x).
Point shifted(Point p, double shift) { return Point{p.x, p.y - shift}; }

// Whether `view` and its superviews are shown and take touches (hit_test's rules, without the
// bounds).
bool takes_touches(mt_cpu *cpu, u32 view) {
    const View *v = view_of(cpu, view);
    for (int depth = 0; v != nullptr && depth < 256; depth++) {
        if (v->hidden || !v->interaction || v->model.alpha < 0.01) {
            return false;
        }
        if (v->superview == 0) {
            return v->kind == Kind::window;
        }
        v = view_of(cpu, v->superview);
    }
    return false;
}

void begin(mt_cpu *cpu, u32 finger, Point p, double now) {
    State &st = state(cpu);
    Touch t;
    t.finger = finger;
    t.start = t.now = t.previous = p;
    t.began = t.timestamp = now;
    if (g_last_tap.valid && now - g_last_tap.end <= k_multi_tap_interval &&
        std::hypot(p.x - g_last_tap.at.x, p.y - g_last_tap.at.y) <= k_multi_tap_distance) {
        t.tap_count = g_last_tap.count + 1;
    }
    g_last_tap.valid = false;
    for (auto it = st.windows.rbegin(); it != st.windows.rend() && t.view == 0; ++it) {
        t.view = hit_test(cpu, *it, p, 0);
    }
    // The host's touch area (#168): beside the screen, its view; on it, its points shifted.
    if (g_host_area.view != 0) {
        const double sx = 480.0 - p.y, sy = p.x; // landscape screen point
        if (t.view == 0 && sy >= 0 && sy < 320 &&
            ((sx < 0 && sx >= -g_host_area.left) || (sx >= 480 && sx < 480 + g_host_area.right)) &&
            takes_touches(cpu, g_host_area.view)) {
            t.view = g_host_area.view;
        }
        if (t.view == g_host_area.view && g_host_area.shift != 0) {
            t.shift = g_host_area.shift;
            t.start = t.now = t.previous = shifted(p, t.shift);
        }
    }
    if (t.view != 0) {
        const View *v = view_of(cpu, t.view);
        if (v != nullptr && !v->multitouch && view_has_touch(t.view, nullptr)) {
            t.view = 0; // UIKit: a single-touch view ignores the second finger
        }
    }
    t.scroll = t.view != 0 ? scroll_ancestor(cpu, t.view) : 0u;
    if (View *s = view_of(cpu, t.scroll)) {
        // A touch on a moving scroll view stops it (and does not reach the content).
        if (s->decelerating) {
            s->decelerating = false;
            g_scrolling.erase(std::remove(g_scrolling.begin(), g_scrolling.end(), t.scroll),
                              g_scrolling.end());
            t.view = t.scroll;
        }
    }
    t.obj = new_touch_object(cpu);
    g_touches.push_back(t);
    Touch &added = g_touches.back();
    sync_touch(cpu, added);
    if (added.scroll == 0 || added.view == added.scroll) {
        // Nothing to hold back: the scroll view itself (or no scroll view) gets it now.
        deliver(cpu, added, MT_UIKIT_TOUCH_BEGAN);
    }
}

void move(mt_cpu *cpu, Touch &t, Point p, double now) {
    t.previous = t.now;
    t.now = p;
    t.timestamp = now;
    if (std::hypot(p.x - t.start.x, p.y - t.start.y) >= k_drag_threshold) {
        t.moved_far = true;
    }
    if (t.dragging) {
        drag(cpu, t, now);
        return;
    }
    const double dx = p.x - t.start.x, dy = p.y - t.start.y;
    if (t.scroll != 0 && std::sqrt(dx * dx + dy * dy) >= k_drag_threshold) {
        const View *s = view_of(cpu, t.scroll);
        const bool may_cancel =
            s != nullptr && (s->can_cancel_content_touches || !t.delivered || t.view == t.scroll);
        if (may_cancel) {
            if (t.delivered && t.view != t.scroll) {
                deliver(cpu, t, MT_UIKIT_TOUCH_CANCELLED);
            }
            if (t.view == t.scroll) {
                deliver(cpu, t, MT_UIKIT_TOUCH_MOVED);
            }
            t.delivered = t.view == t.scroll && t.delivered;
            begin_drag(cpu, t, now);
            Touch *again = find_touch(t.finger); // messages may have changed the list
            if (again != nullptr) {
                drag(cpu, *again, now);
            }
            return;
        }
        t.scroll = 0; // the content keeps the touch
    }
    if (!t.delivered && t.scroll != 0 && now - t.began >= k_content_delay) {
        deliver(cpu, t, MT_UIKIT_TOUCH_BEGAN);
    }
    deliver(cpu, t, MT_UIKIT_TOUCH_MOVED);
}

void finish(mt_cpu *cpu, u32 finger, u32 phase, Point p, double now) {
    Touch *t = find_touch(finger);
    if (t == nullptr) {
        return;
    }
    if (t->now.x != p.x || t->now.y != p.y) {
        t->previous = t->now;
        t->now = p;
    }
    t->timestamp = now;
    if (std::hypot(p.x - t->start.x, p.y - t->start.y) >= k_drag_threshold) {
        t->moved_far = true;
    }
    if (phase == MT_UIKIT_TOUCH_ENDED && !t->moved_far) {
        g_last_tap = LastTap{true, p, now, t->tap_count};
    } else {
        g_last_tap.valid = false;
    }
    if (t->dragging) {
        if (t->view == t->scroll) {
            deliver(cpu, *t, phase);
        }
        t = find_touch(finger);
        if (t != nullptr) {
            end_drag(cpu, *t, now);
        }
    } else {
        if (!t->delivered && phase == MT_UIKIT_TOUCH_ENDED) {
            deliver(cpu, *t, MT_UIKIT_TOUCH_BEGAN); // a tap shorter than the delay
            t = find_touch(finger);
        }
        if (t != nullptr) {
            deliver(cpu, *t, phase);
        }
    }
    t = find_touch(finger);
    if (t != nullptr) {
        const u32 obj = t->obj;
        g_touches.erase(g_touches.begin() + (t - g_touches.data()));
        mt_objc_release(cpu, obj);
    }
}

} // namespace

// --- shared ------------------------------------------------------------------------------------

void deliver_touches(mt_cpu *cpu, double now) {
    std::vector<Queued> queue;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        queue.swap(g_queue);
    }
    for (const Queued &q : queue) {
        Point p = window_point(q.x, q.y);
        Touch *t = find_touch(q.finger);
        if (t != nullptr && q.phase != MT_UIKIT_TOUCH_BEGAN && t->shift != 0) {
            p = shifted(p, t->shift); // a touch of the host's touch area (#168)
        }
        switch (q.phase) {
        case MT_UIKIT_TOUCH_BEGAN:
            if (t != nullptr) {
                finish(cpu, q.finger, MT_UIKIT_TOUCH_CANCELLED, t->now, now);
            }
            begin(cpu, q.finger, p, now);
            break;
        case MT_UIKIT_TOUCH_MOVED:
            if (t != nullptr) {
                move(cpu, *t, p, now);
            }
            break;
        default:
            finish(cpu, q.finger, q.phase, p, now);
            break;
        }
    }
    // Held-back content touches whose delay ran out without a drag.
    for (std::size_t i = 0; i < g_touches.size(); i++) {
        Touch &t = g_touches[i];
        if (!t.delivered && !t.dragging && t.scroll != 0 && t.view != 0 &&
            now - t.began >= k_content_delay) {
            deliver(cpu, t, MT_UIKIT_TOUCH_BEGAN);
        }
    }
}

void advance_scrolling(mt_cpu *cpu, double now) {
    const std::vector<u32> scrolling = g_scrolling;
    for (const u32 obj : scrolling) {
        View *s = view_of(cpu, obj);
        if (s == nullptr || !s->decelerating) {
            g_scrolling.erase(std::remove(g_scrolling.begin(), g_scrolling.end(), obj),
                              g_scrolling.end());
            continue;
        }
        double mx = 0, my = 0;
        max_content_offset(*s, &mx, &my);
        double x = s->model.bounds.x, y = s->model.bounds.y;
        bool done = false;
        if (s->scroll_last < 0) {
            // A settle: ease out to the target.
            const double p = std::min(1.0, (now - s->scroll_start) / k_settle_duration);
            const double e = 1 - (1 - p) * (1 - p) * (1 - p);
            x = s->scroll_from_x + (s->scroll_to_x - s->scroll_from_x) * e;
            y = s->scroll_from_y + (s->scroll_to_y - s->scroll_from_y) * e;
            done = p >= 1;
        } else {
            const double dt = std::max(0.0, now - s->scroll_last);
            s->scroll_last = now;
            const double f = std::pow(k_deceleration, dt * 1000);
            // Distance travelled: the integral of v f^t over dt.
            const double k = std::log(k_deceleration) * 1000;
            const double travelled = std::fabs(k) > 0 ? (f - 1) / k : dt;
            x += s->velocity_x * travelled;
            y += s->velocity_y * travelled;
            s->velocity_x *= f;
            s->velocity_y *= f;
            if (x < 0 || y < 0 || x > mx || y > my) {
                x = std::max(-40.0, std::min(x, mx + 40));
                y = std::max(-40.0, std::min(y, my + 40));
                s->model.bounds.x = x; // overshoot a little, then bounce back
                s->model.bounds.y = y;
                settle(*s, now, std::max(0.0, std::min(x, mx)), std::max(0.0, std::min(y, my)));
                continue;
            }
            done = std::fabs(s->velocity_x) < 10 && std::fabs(s->velocity_y) < 10;
        }
        set_content_offset(cpu, *s, x, y);
        s = view_of(cpu, obj);
        if (done && s != nullptr) {
            s->decelerating = false;
            g_scrolling.erase(std::remove(g_scrolling.begin(), g_scrolling.end(), obj),
                              g_scrolling.end());
            const u32 delegate = s->delegate;
            if (delegate != 0 && responds(cpu, delegate, "scrollViewDidEndDecelerating:")) {
                mt_objc_send1(cpu, delegate, "scrollViewDidEndDecelerating:", obj);
            }
        }
    }
}

void touch_forget_view(u32 view) {
    if (g_host_area.view == view) {
        g_host_area = HostTouchArea();
    }
    for (Touch &t : g_touches) {
        if (t.view == view) {
            t.view = 0;
        }
        if (t.scroll == view) {
            t.scroll = 0;
            t.dragging = false;
        }
    }
    g_scrolling.erase(std::remove(g_scrolling.begin(), g_scrolling.end(), view), g_scrolling.end());
}

void touch_reset() {
    g_touches.clear(); // the objects went with the old runtime
    g_host_area = HostTouchArea();
    g_scrolling.clear();
    g_event = 0;
    g_last_tap = LastTap();
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.clear();
}

bool touch_point(mt_cpu *cpu, u32 touch, Point *now_point, Point *previous) {
    const TouchState *s = touch_state(cpu, touch);
    if (s == nullptr) {
        return false;
    }
    *now_point = s->now;
    *previous = s->previous;
    return true;
}

// UIControl's tracking: highlighted while the touch is inside the bounds grown by 70 points;
// touch down, drag enter/exit/inside/outside, touch up inside/outside and cancel actions.
void control_touches(mt_cpu *cpu, View &v, u32 touches, u32 event, int phase) {
    const std::vector<u32> *items = mt_ns::set_items(cpu, touches);
    if (items == nullptr || items->empty()) {
        return;
    }
    Point p, prev;
    if (!touch_point(cpu, items->front(), &p, &prev)) {
        return;
    }
    const u32 self = v.obj;
    Point local;
    if (!to_local(cpu, self, p, &local)) {
        return;
    }
    const Rect &b = v.model.bounds;
    const bool strictly_inside = inside(b, local);
    const bool near = inside(Rect{b.x - k_control_margin, b.y - k_control_margin,
                                  b.w + 2 * k_control_margin, b.h + 2 * k_control_margin},
                             local);
    u32 events = 0;
    switch (phase) {
    case MT_UIKIT_TOUCH_BEGAN:
        v.tracking = true;
        v.touch_inside = strictly_inside;
        v.highlighted = strictly_inside;
        events = k_event_touch_down;
        break;
    case MT_UIKIT_TOUCH_MOVED:
        if (!v.tracking) {
            return;
        }
        events = near ? k_event_touch_drag_inside : k_event_touch_drag_outside;
        if (near != v.touch_inside) {
            events |= near ? k_event_touch_drag_enter : k_event_touch_drag_exit;
        }
        v.touch_inside = near;
        v.highlighted = near;
        break;
    case MT_UIKIT_TOUCH_ENDED:
        if (!v.tracking) {
            return;
        }
        v.tracking = false;
        v.highlighted = false;
        events = near ? k_event_touch_up_inside : k_event_touch_up_outside;
        if (near && v.kind == Kind::page_control && v.pages > 1) {
            // UIPageControl: a tap right of the current dot goes forward, left goes back.
            const i32 page = local.x >= b.x + b.w / 2 ? std::min(v.page + 1, v.pages - 1)
                                                      : std::max(v.page - 1, 0);
            if (page != v.page) {
                v.page = page;
                events |= k_event_value_changed;
            }
        }
        break;
    default:
        if (!v.tracking) {
            return;
        }
        v.tracking = false;
        v.highlighted = false;
        events = k_event_touch_cancel;
        break;
    }
    const std::vector<TargetAction> actions = v.actions;
    for (const TargetAction &ta : actions) {
        if ((ta.events & events) == 0 || ta.target == 0 || view_of(cpu, self) == nullptr) {
            continue;
        }
        // -sendAction:to:forEvent: : 0, 1 (sender) or 2 (sender, event) arguments by the
        // selector's colons.
        const char *name = mt_objc_sel_name(runtime(cpu), ta.action);
        int colons = 0;
        for (const char *c = name; c != nullptr && *c != 0; c++) {
            colons += *c == ':';
        }
        const u32 args[2] = {self, event};
        if (mt_trace_enabled("uikit")) {
            mt_objc *rt = runtime(cpu);
            mt_tracef("uikit", "action -[%s %s] from 0x%08X (events 0x%X)",
                      mt_objc_class_name(rt, mt_objc_class_of(rt, ta.target)), name,
                      static_cast<unsigned>(self), static_cast<unsigned>(events));
        }
        mt_objc_msg(cpu, ta.target, ta.action, args, static_cast<u32>(std::min(colons, 2)));
    }
}

} // namespace mt_ui

// --- UITouch, UIEvent ----------------------------------------------------------------------------

namespace mt_ui {
namespace {

void ret_point(mt_cpu *cpu, u32 result, Point p) {
    const float f[2] = {static_cast<float>(p.x), static_cast<float>(p.y)};
    mt_ret_struct(cpu, result, f, sizeof f);
}

// -[UITouch locationInView:(UIView *)view] -> CGPoint (stret); nil: the window's space.
void touch_location_common(mt_cpu *cpu, bool previous) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    const u32 view = mt_arg_u32(&a);
    const TouchState *s = touch_state(cpu, self);
    Point p = s != nullptr ? (previous ? s->previous : s->now) : Point{};
    Point local = p;
    if (view != 0 && !to_local(cpu, view, p, &local)) {
        local = p;
    }
    ret_point(cpu, result, local);
}
void touch_location(mt_cpu *cpu) { touch_location_common(cpu, false); }
void touch_previous_location(mt_cpu *cpu) { touch_location_common(cpu, true); }

void touch_view(mt_cpu *cpu) {
    const TouchState *s = touch_state(cpu, mt_objc_self(cpu));
    mt_ret_u32(cpu, s != nullptr && view_of(cpu, s->view) != nullptr ? s->view : 0u);
}

void touch_window(mt_cpu *cpu) {
    const TouchState *s = touch_state(cpu, mt_objc_self(cpu));
    const View *v = s != nullptr ? view_of(cpu, s->view) : nullptr;
    mt_ret_u32(cpu, v != nullptr ? window_of(cpu, *v) : 0u);
}

// UITouchPhase: began 0, moved 1, stationary 2, ended 3, cancelled 4.
void touch_phase(mt_cpu *cpu) {
    const TouchState *s = touch_state(cpu, mt_objc_self(cpu));
    static const u32 k_phase[] = {0, 1, 3, 4};
    mt_ret_u32(cpu, s != nullptr && s->phase < 4 ? k_phase[s->phase] : 0u);
}

void touch_tap_count(mt_cpu *cpu) {
    const TouchState *s = touch_state(cpu, mt_objc_self(cpu));
    mt_ret_u32(cpu, s != nullptr ? s->tap_count : 1u);
}

void touch_timestamp(mt_cpu *cpu) {
    const TouchState *s = touch_state(cpu, mt_objc_self(cpu));
    const double t = s != nullptr ? s->timestamp : 0.0;
    mt_ret_f64(cpu, t);
}

// -[UIEvent allTouches]: the live touches.
void event_all_touches(mt_cpu *cpu) {
    std::vector<u32> items;
    for (const Touch &t : g_touches) {
        items.push_back(t.obj);
    }
    const u32 set = mt_ns::set_create(cpu, items.empty() ? nullptr : items.data(),
                                      static_cast<u32>(items.size()));
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, set));
}

// Whether `view` is `ancestor` or inside it.
bool descends(mt_cpu *cpu, u32 view, u32 ancestor) {
    const View *v = view_of(cpu, view);
    for (int depth = 0; v != nullptr && depth < 256; depth++) {
        if (v->obj == ancestor) {
            return true;
        }
        v = view_of(cpu, v->superview);
    }
    return false;
}

// -[UIEvent touchesForView:]: the live touches that belong to the view or to a view inside it;
// -touchesForWindow:: those in the window.
void touches_for_common(mt_cpu *cpu, bool window) {
    const u32 target = cpu->r[2];
    std::vector<u32> items;
    for (const Touch &t : g_touches) {
        const View *v = view_of(cpu, t.view);
        if (target != 0 && v != nullptr &&
            (window ? window_of(cpu, *v) == target : descends(cpu, t.view, target))) {
            items.push_back(t.obj);
        }
    }
    const u32 set = mt_ns::set_create(cpu, items.empty() ? nullptr : items.data(),
                                      static_cast<u32>(items.size()));
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, set));
}
void event_touches_for_view(mt_cpu *cpu) { touches_for_common(cpu, false); }
void event_touches_for_window(mt_cpu *cpu) { touches_for_common(cpu, true); }

void event_timestamp(mt_cpu *cpu) { mt_ret_f64(cpu, state(cpu).last_turn); }

const mt_objc_host_method k_touch_methods[] = {
    {"locationInView:", touch_location},
    {"previousLocationInView:", touch_previous_location},
    {"view", touch_view},
    {"window", touch_window},
    {"phase", touch_phase},
    {"tapCount", touch_tap_count},
    {"timestamp", touch_timestamp},
};
const mt_objc_host_method k_event_methods[] = {
    {"allTouches", event_all_touches},
    {"touchesForView:", event_touches_for_view},
    {"touchesForWindow:", event_touches_for_window},
    {"timestamp", event_timestamp},
};

} // namespace

const mt_objc_host_class k_uitouch = {
    "UITouch", "NSObject", 4, k_touch_methods, sizeof k_touch_methods / sizeof k_touch_methods[0],
    nullptr,   0};
const mt_objc_host_class k_uievent = {
    "UIEvent", "NSObject", 4, k_event_methods, sizeof k_event_methods / sizeof k_event_methods[0],
    nullptr,   0};

} // namespace mt_ui

extern "C" std::uint32_t mt_uikit_touches_active(void) noexcept {
    std::size_t queued;
    {
        std::lock_guard<std::mutex> lock(mt_ui::g_queue_mutex);
        queued = mt_ui::g_queue.size();
    }
    return static_cast<std::uint32_t>(mt_ui::g_touches.size() + queued);
}

extern "C" int mt_uikit_touch_delivering(std::uint32_t *finger, std::uint32_t *phase) noexcept {
    const mt_ui::Delivering d = mt_ui::g_delivering;
    if (!d.on) {
        return 0;
    }
    if (finger != nullptr) {
        *finger = d.finger;
    }
    if (phase != nullptr) {
        *phase = d.phase;
    }
    return 1;
}

extern "C" int mt_uikit_touch(std::uint32_t finger, std::uint32_t phase, float x,
                              float y) noexcept {
    if (phase > MT_UIKIT_TOUCH_CANCELLED || !std::isfinite(x) || !std::isfinite(y)) {
        return -1;
    }
    try {
        std::lock_guard<std::mutex> lock(mt_ui::g_queue_mutex);
        std::vector<mt_ui::Queued> &q = mt_ui::g_queue;
        // A move after a queued move of the same finger replaces it (UIKit merges a frame's
        // moves); any other event of the finger in between keeps both.
        if (phase == MT_UIKIT_TOUCH_MOVED) {
            for (auto it = q.rbegin(); it != q.rend(); ++it) {
                if (it->finger == finger) {
                    if (it->phase == MT_UIKIT_TOUCH_MOVED) {
                        it->x = x;
                        it->y = y;
                        return 0;
                    }
                    break;
                }
            }
        }
        q.push_back(mt_ui::Queued{finger, phase, x, y});
    } catch (...) {
        return -1;
    }
    return 0;
}

extern "C" int mt_uikit_set_host_touch_area(std::uint32_t view, float shift, float left,
                                            float right) noexcept {
    if (!std::isfinite(shift) || !std::isfinite(left) || !std::isfinite(right)) {
        return 0;
    }
    if (view == 0 || (shift == 0 && left <= 0 && right <= 0)) {
        mt_ui::g_host_area = mt_ui::HostTouchArea();
        return 1;
    }
    if (mt_objc_current() == nullptr || mt_ui::view_of(nullptr, view) == nullptr) {
        return 0;
    }
    mt_ui::g_host_area =
        mt_ui::HostTouchArea{view, shift, std::max(0.0f, left), std::max(0.0f, right)};
    return 1;
}
