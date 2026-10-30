// The controls a tap can reach (#69, ADR 0051): maplethief/uikit.h mt_uikit_controls.
//
// The frontend's controller navigation moves a focus over these and taps the focused one
// through the touch path (Input::touch), so the list follows the touch rules exactly: a view
// counts where the hit test (touch.cpp) would find it or one of its subviews. The walk mirrors
// the hit test: a hidden, transparent (alpha under 0.01) or non-interactive view hides its
// subtree, and a view is only hit inside the bounds of every superview.
//
// Single guest thread; sends no messages (it reads the host state of the views only).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

// Landscape screen rectangles (x0, y0, x1, y1).
struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return !(x1 - x0 >= 0.5 && y1 - y0 >= 0.5); }
};

Box intersect(const Box &a, const Box &b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

// A view's bounds on the landscape screen (the bounding box: the game turns views only by
// right angles).
Box screen_box(mt_cpu *cpu, const View &v) {
    const Affine m = local_to_screen(cpu, v).then(screen_from_window());
    const Rect &b = v.model.bounds;
    Box out{1e300, 1e300, -1e300, -1e300};
    for (const Point p : {Point{b.x, b.y}, Point{b.x + b.w, b.y}, Point{b.x, b.y + b.h},
                          Point{b.x + b.w, b.y + b.h}}) {
        const Point q = m.apply(p);
        out.x0 = std::min(out.x0, q.x);
        out.y0 = std::min(out.y0, q.y);
        out.x1 = std::max(out.x1, q.x);
        out.y1 = std::max(out.y1, q.y);
    }
    return out;
}

// Whether the hit test at landscape point (x, y) finds `view` or a subview of it.
bool reaches(mt_cpu *cpu, u32 view, double x, double y) {
    // Landscape -> portrait window (touch.cpp's window_point).
    u32 hit = hit_test_screen(cpu, Point{y, 480.0 - x});
    for (int depth = 0; hit != 0 && depth < 256; depth++) {
        if (hit == view) {
            return true;
        }
        const View *h = view_of(cpu, hit);
        hit = h != nullptr ? h->superview : 0u;
    }
    return false;
}

void copy_name(char *dst, const char *src) {
    std::snprintf(dst, MT_UIKIT_CONTROL_NAME_MAX, "%s", src != nullptr ? src : "");
}

struct Walk {
    mt_cpu *cpu;
    mt_uikit_control *out;
    u32 max;
    u32 count = 0;
    u32 order = 0;
};

void consider(Walk &w, const View &v, const Box &visible) {
    const bool control =
        v.kind == Kind::button || v.kind == Kind::control || v.kind == Kind::page_control;
    const bool scroll = v.kind == Kind::scroll_view || v.kind == Kind::text_view;
    if (!control && !scroll) {
        return;
    }
    double mx = 0, my = 0;
    const TargetAction *action = nullptr;
    if (control) {
        for (const TargetAction &ta : v.actions) {
            if (ta.target == 0 || ta.action == 0) {
                continue;
            }
            if (action == nullptr || ((ta.events & k_event_touch_up_inside) != 0 &&
                                      (action->events & k_event_touch_up_inside) == 0)) {
                action = &ta;
            }
        }
        if (action == nullptr) {
            return; // nothing a tap would do
        }
    } else {
        if (!v.scroll_enabled) {
            return;
        }
        max_content_offset(v, &mx, &my);
        const bool pages = v.paging && (v.content_width > v.model.bounds.w + 0.5 ||
                                        v.content_height > v.model.bounds.h + 0.5);
        if (!(mx > 0.5 || my > 0.5 || pages)) {
            return;
        }
    }
    // Where a touch reaches it: the centre, else a 3x3 grid.
    const double cx = (visible.x0 + visible.x1) / 2, cy = (visible.y0 + visible.y1) / 2;
    double px = cx, py = cy;
    bool found = reaches(w.cpu, v.obj, px, py);
    for (int i = 0; i < 9 && !found; i++) {
        px = visible.x0 + (visible.x1 - visible.x0) * (1 + 2 * (i % 3)) / 6.0;
        py = visible.y0 + (visible.y1 - visible.y0) * (1 + 2 * (i / 3)) / 6.0;
        found = reaches(w.cpu, v.obj, px, py);
    }
    if (!found) {
        return; // covered
    }
    if (w.count < w.max && w.out != nullptr) {
        mt_uikit_control &c = w.out[w.count];
        std::memset(&c, 0, sizeof c);
        c.object = v.obj;
        c.kind = scroll                   ? MT_UIKIT_CONTROL_SCROLL
                 : v.kind == Kind::button ? MT_UIKIT_CONTROL_BUTTON
                                          : MT_UIKIT_CONTROL_OTHER;
        c.order = w.order;
        c.tag = v.tag;
        c.rect[0] = static_cast<float>(visible.x0);
        c.rect[1] = static_cast<float>(visible.y0);
        c.rect[2] = static_cast<float>(visible.x1 - visible.x0);
        c.rect[3] = static_cast<float>(visible.y1 - visible.y0);
        c.point[0] = static_cast<float>(px);
        c.point[1] = static_cast<float>(py);
        mt_objc *rt = runtime(w.cpu);
        copy_name(c.view_class, mt_objc_class_name(rt, mt_objc_class_of(rt, v.obj)));
        if (action != nullptr) {
            copy_name(c.action, mt_objc_sel_name(rt, action->action));
            copy_name(c.target, mt_objc_class_name(rt, mt_objc_class_of(rt, action->target)));
        } else {
            c.offset[0] = static_cast<float>(v.model.bounds.x);
            c.offset[1] = static_cast<float>(v.model.bounds.y);
            c.max_offset[0] = static_cast<float>(mx);
            c.max_offset[1] = static_cast<float>(my);
            if (v.paging) {
                c.page[0] = static_cast<float>(v.model.bounds.w);
                c.page[1] = static_cast<float>(v.model.bounds.h);
            }
        }
    }
    w.count++;
}

void walk(Walk &w, u32 obj, const Box &clip, int depth) {
    const View *v = view_of(w.cpu, obj);
    if (v == nullptr || depth > 64 || v->hidden || !v->interaction || v->model.alpha < 0.01) {
        return;
    }
    const Box visible = intersect(clip, screen_box(w.cpu, *v));
    w.order++;
    if (!visible.empty()) {
        consider(w, *v, visible);
    }
    // Subviews are hit only inside this view's bounds (touch.cpp's hit test), so its box
    // clips them; an empty box leaves nothing reachable below.
    if (visible.empty()) {
        return;
    }
    const std::vector<u32> subs = v->subviews;
    for (const u32 sub : subs) {
        walk(w, sub, visible, depth + 1);
    }
}

} // namespace

} // namespace mt_ui

uint32_t mt_uikit_controls(mt_cpu *cpu, mt_uikit_control *out, uint32_t max) noexcept {
    using namespace mt_ui;
    if (cpu == nullptr || runtime(cpu) == nullptr) {
        return 0;
    }
    Walk w{cpu, out, out != nullptr ? max : 0};
    const Box screen{0, 0, MT_UIKIT_SCREEN_WIDTH, MT_UIKIT_SCREEN_HEIGHT};
    const std::vector<u32> windows = state(cpu).windows;
    for (const u32 window : windows) {
        walk(w, window, screen, 0);
    }
    return w.count;
}
