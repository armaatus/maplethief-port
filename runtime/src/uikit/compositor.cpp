// The compositor (#54, ADR 0037): the view tree -> premultiplied RGBA with #51's rasterizer,
// and the public compositing API of maplethief/uikit.h.
//
// Each visible window is drawn back to front: a view's background colour, its own content
// (an image view's image, a label's text, a button's image, a text view's text, a page
// control's dots, an activity indicator's spokes), what its -drawRect: drew, then its
// subviews, clipped to its bounds if it clips. Geometry and opacity are presentation values at
// the requested time (running UIView animations). A view with opacity below 1 and subviews is
// drawn as a group: its subtree goes into a scratch buffer, which is blended in with its
// opacity, as Core Animation does.
//
// The host can fade a view out without the guest knowing (mt_uikit_set_host_opacity, #71): its
// opacity is multiplied into the view's, and a view at host opacity 0 is not drawn. The view's
// alpha and hidden flag, which the game reads and hit testing uses, stay as the game set them.
//
// The host can also move a view (mt_uikit_set_host_transform, #159): an affine map of screen
// points applied after the view's own geometry, to it and its subviews; and draw a view the
// game has switched off (mt_uikit_set_host_reveal). Neither is seen by the guest.
//
// The view with a CAEAGLLayer is the GL slot: in FULL mode the GL frame (or black) fills its
// bounds; in OVERLAY mode its pixels are cleared, so the host's GL frame shows through and
// only what lies above it (its subviews and later views) remains.
//
// Text is drawn axis-aligned in device space (draw_text): right for every view the game has,
// which all sit in the EAGLView, whose turn cancels the window's landscape turn.
//
// The trace channel `composite` (MAPLETHIEF_TRACE=composite, #134) logs each drawn view (class,
// kind, device box, opacity) with the time of its own drawing, indented by depth, then the
// composite's size, mode and total time.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "maplethief/diag.h"
#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

constexpr double k_page_dot = 7;    // UIPageControl dot diameter (points)
constexpr double k_page_pitch = 16; // distance between dot centres

struct Target {
    mt_cg::Surface surface;
    mt_cg::Canvas *canvas;
};

struct Frame {
    mt_cpu *cpu;
    double now;
    u32 mode;
    const mt_uikit_gl_frame *gl;
    u32 width, height;
    bool trace = false; // the trace channel "composite" is on
};

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A canvas whose user space is mapped to device pixels (y down) by `device`.
void set_device_transform(mt_cg::Canvas &c, const Affine &device) {
    c.state().ctm = device.then(Affine::scale(1, -1)).then(Affine::translate(0, c.height()));
}

void fill(Target &t, const Rect &r, const Color &color, u32 blend = UINT32_MAX) {
    t.canvas->state().fill = color;
    t.canvas->fill_rect(t.surface, r, blend);
}

void draw_cgimage(Frame &f, Target &t, u32 cgimage, const Rect &r) {
    mt_cg::ConstSurface img;
    if (cgimage != 0 && r.w > 0 && r.h > 0 && mt_cg::image_surface(f.cpu, cgimage, &img)) {
        t.canvas->draw_image(t.surface, r, img, true);
    }
}

void draw_uiimage(Frame &f, Target &t, u32 uiimage, const Rect &r) {
    draw_cgimage(f, t, uiimage != 0 ? mt_cg::uiimage_cgimage(f.cpu, uiimage) : 0u, r);
}

void draw_lines(Target &t, const std::vector<std::u16string> &lines, const Font &font,
                const Color &color, const Rect &box, i32 alignment, double top, double clip_top,
                double clip_bottom) {
    const mt_cg::FontMetrics m = mt_cg::font_metrics(font);
    t.canvas->state().fill = color;
    double y = top;
    for (const std::u16string &line : lines) {
        if (y + m.line_height >= clip_top && y <= clip_bottom && !line.empty()) {
            const double w = mt_cg::text_width(line, font);
            double x = box.x;
            if (alignment == 1) {
                x = box.x + (box.w - w) / 2;
            } else if (alignment == 2) {
                x = box.x + box.w - w;
            }
            float dw = 0, dh = 0;
            mt_cg::draw_text(*t.canvas, t.surface, line, font, Point{x, y}, &dw, &dh);
        }
        y += m.line_height;
    }
}

// A path transformed by `m`.
mt_cg::Path transformed(const mt_cg::Path &p, const Affine &m) {
    mt_cg::Path out = p;
    for (mt_cg::Subpath &s : out.subpaths) {
        for (Point &q : s.points) {
            q = m.apply(q);
        }
    }
    return out;
}

void draw_activity(Frame &f, Target &t, const View &v, const Rect &b) {
    const double side = std::min(b.w, b.h);
    if (!(side > 0)) {
        return;
    }
    const double r = side / 2, cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    const Color base = v.style == 2 ? Color{0.5f, 0.5f, 0.5f, 1} : Color{1, 1, 1, 1};
    const int step = static_cast<int>(std::floor(f.now * 12)) % 12;
    const double width = std::max(1.0, r * 0.2);
    for (int i = 0; i < 12; i++) {
        mt_cg::Path p;
        p.add_rounded_rect(Rect{-width / 2, -r, width, r * 0.5}, width / 2);
        const double angle = i * 3.14159265358979323846 / 6;
        const Affine m =
            Affine{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), cx, cy};
        const int age = ((step - i) % 12 + 12) % 12;
        Color c = base;
        c.a = static_cast<float>(1.0 - 0.8 * age / 11.0);
        t.canvas->state().fill = c;
        t.canvas->fill_path(t.surface, transformed(p, m));
    }
}

void draw_page_dots(Target &t, const View &v, const Rect &b) {
    if (v.pages <= 0) {
        return;
    }
    const double cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    const double x0 = cx - (v.pages - 1) * k_page_pitch / 2;
    for (i32 i = 0; i < v.pages; i++) {
        mt_cg::Path p;
        p.add_rounded_rect(Rect{x0 + i * k_page_pitch - k_page_dot / 2, cy - k_page_dot / 2,
                                k_page_dot, k_page_dot},
                           k_page_dot / 2);
        t.canvas->state().fill = i == v.page ? Color{1, 1, 1, 1} : Color{1, 1, 1, 0.35f};
        t.canvas->fill_path(t.surface, p);
    }
}

// The content a view of each kind draws itself (after its background).
void draw_own_content(Frame &f, Target &t, const View &v, const Rect &b) {
    switch (v.kind) {
    case Kind::image_view:
        draw_uiimage(f, t, v.image, b);
        break;
    case Kind::label: {
        if (v.text.empty() || !(b.w > 0)) {
            break;
        }
        const std::vector<std::u16string> lines = wrap_text(v.text, v.font_value, b.w, v.lines);
        const double lh = mt_cg::font_metrics(v.font_value).line_height;
        const double total = static_cast<double>(lines.size()) * lh;
        draw_lines(t, lines, v.font_value, v.text_color, b, v.alignment, b.y + (b.h - total) / 2,
                   -1e300, 1e300);
        break;
    }
    case Kind::text_view: {
        if (v.text.empty()) {
            break;
        }
        const double width = v.model.bounds.w - 2 * k_text_view_inset;
        const std::vector<std::u16string> lines = wrap_text(v.text, v.font_value, width, 0);
        draw_lines(t, lines, v.font_value, v.text_color, Rect{k_text_view_inset, 0, width, 0},
                   v.alignment, k_text_view_inset, b.y, b.y + b.h);
        break;
    }
    case Kind::button:
        if (const u32 image = button_image(v)) {
            draw_uiimage(f, t, image, button_image_rect(f.cpu, v, image));
        }
        break;
    case Kind::page_control:
        draw_page_dots(t, v, b);
        break;
    case Kind::activity:
        if (v.animating || !v.hides_when_stopped) {
            draw_activity(f, t, v, b);
        }
        break;
    case Kind::view:
    case Kind::window:
    case Kind::control:
    case Kind::scroll_view:
        break;
    }
}

bool visible(const View &v, const Geometry &g) { return !v.hidden && g.alpha >= 0.01; }

void render(Frame &f, Target &t, u32 obj, const Affine &parent, int depth, bool as_group = false);

// The view's subtree into a scratch buffer, blended in with `alpha`.
void render_group(Frame &f, Target &t, u32 obj, const Affine &parent, double alpha, int depth) {
    std::vector<u8> pixels(static_cast<std::size_t>(f.width) * f.height * 4, 0);
    mt_cg::Canvas canvas(f.width, f.height);
    canvas.state().clip = t.canvas->state().clip;
    Target scratch{t.surface, &canvas};
    scratch.surface.pixels = pixels.data();
    scratch.surface.bytes_per_row = f.width * 4;
    scratch.surface.format = mt_cg::Format{};
    render(f, scratch, obj, parent, depth, true);
    const u32 a = static_cast<u32>(std::lround(std::max(0.0, std::min(1.0, alpha)) * 255));
    for (u32 y = 0; y < f.height; y++) {
        const u8 *src = pixels.data() + static_cast<std::size_t>(y) * f.width * 4;
        u8 *dst = t.surface.pixels + static_cast<std::size_t>(y) * t.surface.bytes_per_row;
        for (u32 x = 0; x < f.width; x++, src += 4, dst += 4) {
            if (src[3] == 0) {
                continue;
            }
            const u32 sa = (src[3] * a + 127) / 255;
            for (int c = 0; c < 4; c++) {
                const u32 s = (src[c] * a + 127) / 255;
                dst[c] = static_cast<u8>(std::min(255u, s + (dst[c] * (255 - sa) + 127) / 255));
            }
        }
    }
}

// `as_group`: the view is drawn into its group's scratch buffer (at opacity 1).
void render(Frame &f, Target &t, u32 obj, const Affine &parent, int depth, bool as_group) {
    const View *vp = view_of(f.cpu, obj);
    if (vp == nullptr || depth > 64) {
        return;
    }
    const View &v = *vp;
    Geometry g = presentation(v, f.now);
    // The host's reveal (#159): drawn as if the game had it shown; its subviews keep theirs.
    if (v.host_reveal) {
        g.alpha = 1;
    } else if (!visible(v, g)) {
        return;
    }
    // The host's opacity (#71, ADR 0052): the compositor's alone, the guest's alpha is untouched.
    if (v.host_opacity < 1) {
        g.alpha *= std::max(0.0f, v.host_opacity);
        if (g.alpha < 0.01) {
            return;
        }
    }
    Affine device = local_to_super(g).then(parent);
    // The host's transform (#159, ADR 0086): in landscape screen points, after the view's own
    // geometry: device pixels -> screen points, the transform, back to device pixels.
    if (v.has_host_transform) {
        const double sx = f.width / static_cast<double>(MT_UIKIT_SCREEN_WIDTH);
        const double sy = f.height / static_cast<double>(MT_UIKIT_SCREEN_HEIGHT);
        device = device.then(Affine::scale(1 / sx, 1 / sy))
                     .then(v.host_transform)
                     .then(Affine::scale(sx, sy));
    }
    // Group opacity for a translucent view with subviews.
    if (!as_group && g.alpha < 0.999 && !v.subviews.empty()) {
        render_group(f, t, obj, parent, g.alpha, depth);
        return;
    }
    if (as_group) {
        g.alpha = 1;
    }
    const std::int64_t t0 = f.trace ? now_ns() : 0;
    mt_cg::Canvas &c = *t.canvas;
    c.save();
    set_device_transform(c, device);
    c.state().alpha = static_cast<float>(c.state().alpha * std::min(1.0, g.alpha));
    const Rect &b = g.bounds;
    if (v.gl) {
        if (f.mode == MT_UIKIT_COMPOSITE_OVERLAY) {
            fill(t, b, Color{0, 0, 0, 0}, mt_cg::k_blend_clear);
        } else if (f.gl != nullptr && f.gl->pixels != nullptr && f.gl->width > 0 &&
                   f.gl->height > 0 && f.gl->stride >= f.gl->width * 4) {
            mt_cg::ConstSurface frame;
            frame.pixels = f.gl->pixels;
            frame.width = f.gl->width;
            frame.height = f.gl->height;
            frame.bytes_per_row = f.gl->stride;
            frame.format = mt_cg::Format{};
            fill(t, b, Color{0, 0, 0, 1});
            c.draw_image(t.surface, b, frame, true);
        } else {
            fill(t, b, Color{0, 0, 0, 1});
        }
    } else {
        if (v.background_color.a > 0) {
            fill(t, b, v.background_color);
        }
        draw_own_content(f, t, v, b);
        if (!v.contents.empty() && !v.contents_blank) {
            mt_cg::ConstSurface img;
            img.pixels = v.contents.data();
            img.width = v.contents_width;
            img.height = v.contents_height;
            img.bytes_per_row = v.contents_width * 4;
            img.format = mt_cg::Format{};
            c.draw_image(t.surface,
                         Rect{b.x, b.y, static_cast<double>(v.contents_width),
                              static_cast<double>(v.contents_height)},
                         img, true);
        }
    }
    if (v.clips) {
        mt_cg::Path p;
        p.move_to({b.x, b.y});
        p.line_to({b.x + b.w, b.y});
        p.line_to({b.x + b.w, b.y + b.h});
        p.line_to({b.x, b.y + b.h});
        p.close();
        c.clear_path();
        c.add_path(p);
        c.clip_to_path();
        c.clear_path();
    }
    if (f.trace) {
        // The view's own drawing (not its subviews'), its device box and its kind.
        const Rect &bb = b;
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const Point p : {Point{bb.x, bb.y}, Point{bb.x + bb.w, bb.y}, Point{bb.x, bb.y + bb.h},
                              Point{bb.x + bb.w, bb.y + bb.h}}) {
            const Point q = device.apply(p);
            x0 = std::min(x0, q.x);
            y0 = std::min(y0, q.y);
            x1 = std::max(x1, q.x);
            y1 = std::max(y1, q.y);
        }
        mt_objc *rt = mt_objc_current();
        mt_tracef("composite", "%*s%s 0x%08X kind %d (%.0f,%.0f %.0fx%.0f) alpha %.3f%s%s %.3f ms",
                  depth * 2, "", mt_objc_class_name(rt, mt_objc_class_of(rt, obj)),
                  static_cast<unsigned>(obj), static_cast<int>(v.kind), x0, y0, x1 - x0, y1 - y0,
                  g.alpha, as_group ? " (in a group)" : "",
                  !v.contents.empty() && !v.contents_blank ? " contents" : "",
                  static_cast<double>(now_ns() - t0) / 1e6);
    }
    const std::vector<u32> subs = v.subviews;
    for (const u32 sub : subs) {
        render(f, t, sub, device, depth + 1);
        set_device_transform(c, device);
    }
    c.restore();
}

// The first view with a CAEAGLLayer in the windows (depth first, back to front), or nullptr.
const View *find_gl_view(mt_cpu *cpu) {
    const State &st = state(cpu);
    std::vector<u32> stack(st.windows.rbegin(), st.windows.rend());
    while (!stack.empty() && stack.size() < 100000) {
        const View *v = view_of(cpu, stack.back());
        stack.pop_back();
        if (v == nullptr) {
            continue;
        }
        if (v->gl) {
            return v;
        }
        stack.insert(stack.end(), v->subviews.rbegin(), v->subviews.rend());
    }
    return nullptr;
}

} // namespace

} // namespace mt_ui

using namespace mt_ui;

extern "C" int mt_uikit_composite(mt_cpu *cpu, double now, std::uint32_t mode,
                                  const mt_uikit_gl_frame *gl, std::uint8_t *pixels,
                                  std::uint32_t width, std::uint32_t height,
                                  std::uint32_t stride) noexcept {
    if (cpu == nullptr || pixels == nullptr || width == 0 || height == 0 || width > 0x4000 ||
        height > 0x4000 || stride < width * 4 || mode > MT_UIKIT_COMPOSITE_OVERLAY) {
        return -1;
    }
    State &st = state(cpu);
    if (st.app == 0 || st.windows.empty()) {
        return -1;
    }
    for (std::uint32_t y = 0; y < height; y++) {
        u8 *row = pixels + static_cast<std::size_t>(y) * stride;
        if (mode == MT_UIKIT_COMPOSITE_FULL) {
            for (std::uint32_t x = 0; x < width; x++) {
                row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = 0;
                row[4 * x + 3] = 255;
            }
        } else {
            std::memset(row, 0, static_cast<std::size_t>(width) * 4);
        }
    }
    try {
        mt_cg::Canvas canvas(width, height);
        Target t{mt_cg::Surface{}, &canvas};
        t.surface.pixels = pixels;
        t.surface.width = width;
        t.surface.height = height;
        t.surface.bytes_per_row = stride;
        t.surface.format = mt_cg::Format{};
        Frame f{cpu, now, mode, gl, width, height, mt_trace_enabled("composite") != 0};
        const std::int64_t t0 = f.trace ? now_ns() : 0;
        // Window space (portrait points) -> the landscape screen -> device pixels.
        const Affine screen = screen_from_window().then(
            Affine::scale(width / static_cast<double>(MT_UIKIT_SCREEN_WIDTH),
                          height / static_cast<double>(MT_UIKIT_SCREEN_HEIGHT)));
        const std::vector<u32> windows = st.windows;
        for (const u32 w : windows) {
            render(f, t, w, screen, 0);
        }
        if (f.trace) {
            mt_tracef("composite", "%ux%u %s at %.3f s: %.3f ms", width, height,
                      mode == MT_UIKIT_COMPOSITE_FULL ? "full" : "overlay", now,
                      static_cast<double>(now_ns() - t0) / 1e6);
        }
    } catch (...) {
        return -1;
    }
    return 0;
}

extern "C" int mt_uikit_gl_rect(float rect[4], std::uint32_t *drawable_width,
                                std::uint32_t *drawable_height) noexcept {
    mt_cpu *cpu = nullptr; // the lookups below need no cpu
    if (mt_objc_current() == nullptr) {
        return 0;
    }
    const View *v = find_gl_view(cpu);
    if (v == nullptr) {
        return 0;
    }
    const Affine m = local_to_screen(cpu, *v).then(screen_from_window());
    const Rect &b = v->model.bounds;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Point p : {Point{b.x, b.y}, Point{b.x + b.w, b.y}, Point{b.x, b.y + b.h},
                          Point{b.x + b.w, b.y + b.h}}) {
        const Point q = m.apply(p);
        x0 = std::min(x0, q.x);
        y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x);
        y1 = std::max(y1, q.y);
    }
    if (rect != nullptr) {
        rect[0] = static_cast<float>(x0);
        rect[1] = static_cast<float>(y0);
        rect[2] = static_cast<float>(x1 - x0);
        rect[3] = static_cast<float>(y1 - y0);
    }
    if (drawable_width != nullptr) {
        *drawable_width = static_cast<std::uint32_t>(std::lround(std::max(0.0, b.w)));
    }
    if (drawable_height != nullptr) {
        *drawable_height = static_cast<std::uint32_t>(std::lround(std::max(0.0, b.h)));
    }
    return 1;
}

extern "C" int mt_uikit_set_host_opacity(std::uint32_t view, float opacity) noexcept {
    if (mt_objc_current() == nullptr || view == 0) {
        return 0;
    }
    View *v = view_of(nullptr, view);
    if (v == nullptr) {
        return 0;
    }
    v->host_opacity = std::isnan(opacity) ? 1.0f : std::max(0.0f, std::min(1.0f, opacity));
    return 1;
}

extern "C" float mt_uikit_host_opacity(std::uint32_t view) noexcept {
    if (mt_objc_current() == nullptr || view == 0) {
        return 1;
    }
    const View *v = view_of(nullptr, view);
    return v != nullptr ? v->host_opacity : 1.0f;
}

extern "C" int mt_uikit_set_host_transform(std::uint32_t view, const float m[6]) noexcept {
    if (mt_objc_current() == nullptr || view == 0) {
        return 0;
    }
    View *v = view_of(nullptr, view);
    if (v == nullptr) {
        return 0;
    }
    if (m == nullptr) {
        v->host_transform = Affine{};
        v->has_host_transform = false;
        return 1;
    }
    for (int i = 0; i < 6; i++) {
        if (!std::isfinite(m[i])) {
            return 0;
        }
    }
    v->host_transform = Affine{m[0], m[1], m[2], m[3], m[4], m[5]};
    // The identity draws exactly as no transform: the compositor skips it.
    v->has_host_transform =
        !(m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0);
    return 1;
}

extern "C" int mt_uikit_host_transform(std::uint32_t view, float m[6]) noexcept {
    const View *v = mt_objc_current() != nullptr && view != 0 ? view_of(nullptr, view) : nullptr;
    const Affine t = v != nullptr && v->has_host_transform ? v->host_transform : Affine{};
    if (m != nullptr) {
        const double in[6] = {t.a, t.b, t.c, t.d, t.tx, t.ty};
        for (int i = 0; i < 6; i++) {
            m[i] = static_cast<float>(in[i]);
        }
    }
    return v != nullptr && v->has_host_transform ? 1 : 0;
}

extern "C" int mt_uikit_set_host_reveal(std::uint32_t view, int reveal) noexcept {
    if (mt_objc_current() == nullptr || view == 0) {
        return 0;
    }
    View *v = view_of(nullptr, view);
    if (v == nullptr) {
        return 0;
    }
    v->host_reveal = reveal != 0;
    return 1;
}

extern "C" std::uint32_t mt_uikit_subviews(std::uint32_t view, std::uint32_t *out,
                                           std::uint32_t max) noexcept {
    const View *v = mt_objc_current() != nullptr && view != 0 ? view_of(nullptr, view) : nullptr;
    if (v == nullptr) {
        return 0;
    }
    const std::size_t n = v->subviews.size();
    for (std::size_t i = 0; out != nullptr && i < n && i < max; i++) {
        out[i] = v->subviews[i];
    }
    return static_cast<std::uint32_t>(n);
}

extern "C" int mt_uikit_screen_frame(std::uint32_t view, float rect[4]) noexcept {
    mt_cpu *cpu = nullptr; // the lookups below need no cpu
    const View *v = mt_objc_current() != nullptr && view != 0 ? view_of(cpu, view) : nullptr;
    if (v == nullptr) {
        return 0;
    }
    const Affine m = local_to_screen(cpu, *v).then(screen_from_window());
    const Rect &b = v->model.bounds;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Point p : {Point{b.x, b.y}, Point{b.x + b.w, b.y}, Point{b.x, b.y + b.h},
                          Point{b.x + b.w, b.y + b.h}}) {
        const Point q = m.apply(p);
        x0 = std::min(x0, q.x);
        y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x);
        y1 = std::max(y1, q.y);
    }
    if (rect != nullptr) {
        rect[0] = static_cast<float>(x0);
        rect[1] = static_cast<float>(y0);
        rect[2] = static_cast<float>(x1 - x0);
        rect[3] = static_cast<float>(y1 - y0);
    }
    return 1;
}

extern "C" std::uint32_t mt_uikit_animation_count(double now) noexcept {
    mt_cpu *cpu = nullptr;
    if (mt_objc_current() == nullptr) {
        return 0;
    }
    const State &st = state(cpu);
    std::uint32_t count = 0;
    std::vector<u32> stack(st.windows.begin(), st.windows.end());
    while (!stack.empty() && stack.size() < 100000) {
        const View *v = view_of(cpu, stack.back());
        stack.pop_back();
        if (v == nullptr) {
            continue;
        }
        for (const Anim &a : v->anims) {
            count += now < a.start + a.duration ? 1u : 0u;
        }
        stack.insert(stack.end(), v->subviews.begin(), v->subviews.end());
    }
    return count;
}
