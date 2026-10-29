// UIKit's views with content (#54, ADR 0037): UIImageView, UILabel, UIControl and UIButton,
// UIScrollView, UITextView, UIPageControl and UIActivityIndicatorView. Their state is the
// View's (uikit_internal.h); the compositor draws them (compositor.cpp), touch.cpp drives
// scrolling and the controls' tracking.
//
// What the game does with them (docs/re/host-surface.md):
//   UIImageView        initWithImage: for nearly every panel and decoration (63 sends),
//                      setImage:, setFrame:, setAlpha:
//   UILabel            setText:/setFont:/setTextColor:/setTextAlignment:/setNumberOfLines:,
//                      mostly on a clear background (-[EAGLView addLoadingView] "Please Wait...")
//   UIButton           buttonWithType:0 with images for the normal and highlighted states,
//                      image edge insets and actions for touch down / touch up inside
//                      (-[MapKeyPadView addButtonWithImageName:...] 0xAE1F5, 47 sends)
//   UIScrollView       the main menu's and help's pages (paging, -scrollViewDidScroll:)
//   UITextView         read-only text panels (setEditable:NO)
//   UIPageControl      the help pages' dots
//   UIActivityIndicatorView  the "Please Wait..." spinner
#include <algorithm>
#include <cmath>

#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "uikit/uikit_internal.h"

namespace mt_ui {

namespace {

View *self_view(mt_cpu *cpu, const char *what) {
    const u32 self = mt_objc_self(cpu);
    View *v = view_state(cpu, self);
    if (v == nullptr && self != 0) {
        log_once(cpu, what, "%s: 0x%08X is no UIView: ignored", what, static_cast<unsigned>(self));
    }
    return v;
}

bool read_bool(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    return (mt_arg_u32(&a) & 0xFFu) != 0;
}

u32 read_u32(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    return mt_arg_u32(&a);
}

// [[cls alloc] initWithFrame:frame] (autoreleased if `autorelease`).
u32 new_view(mt_cpu *cpu, u32 cls, const Rect &f, bool autorelease) {
    u32 obj = mt_objc_send0(cpu, cls, "alloc");
    if (obj == 0) {
        return 0;
    }
    const u32 frame[4] = {
        mt_f32_bits(static_cast<float>(f.x)), mt_f32_bits(static_cast<float>(f.y)),
        mt_f32_bits(static_cast<float>(f.w)), mt_f32_bits(static_cast<float>(f.h))};
    obj = mt_objc_msg(cpu, obj, mt_objc_sel(runtime(cpu), "initWithFrame:"), frame, 4);
    return autorelease ? mt_objc_autorelease(cpu, obj) : obj;
}

// The text of a -setText: argument; nil is "".
void set_text(mt_cpu *cpu, const char *what) {
    const u32 str = read_u32(cpu);
    const std::u16string text = string_text(cpu, str);
    if (View *v = self_view(cpu, what)) {
        v->text = text;
        if (v->kind == Kind::text_view) {
            text_view_layout(*v);
        }
    }
}

void set_font(mt_cpu *cpu, const char *what) {
    const u32 font = read_u32(cpu);
    View *v = self_view(cpu, what);
    if (v == nullptr) {
        return;
    }
    Font f = default_font(v->kind);
    if (font != 0 && !mt_cg::font_of(cpu, font, &f)) {
        log_once(cpu, what, "%s: 0x%08X is no UIFont: the default font", what,
                 static_cast<unsigned>(font));
    }
    v->font_value = f;
    assign(cpu, &v->font, font);
    if (v->kind == Kind::text_view) {
        text_view_layout(*v);
    }
}

// --- UIImageView ---------------------------------------------------------------------------------

// -[UIImageView initWithImage:(UIImage *)image]: the frame is the image's size at (0, 0).
void image_view_init_with_image(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    const u32 image = read_u32(cpu);
    double w = 0, h = 0;
    if (image != 0 && !mt_cg::uiimage_size(cpu, image, &w, &h)) {
        log_once(cpu, "initWithImage.noimage",
                 "-[UIImageView initWithImage:]: 0x%08X is no UIImage: no image",
                 static_cast<unsigned>(image));
    }
    const u32 frame[4] = {0, 0, mt_f32_bits(static_cast<float>(w)),
                          mt_f32_bits(static_cast<float>(h))};
    const u32 obj = mt_objc_msg(cpu, self, mt_objc_sel(runtime(cpu), "initWithFrame:"), frame, 4);
    if (View *v = view_state(cpu, obj)) {
        assign(cpu, &v->image, w > 0 || h > 0 ? image : 0u);
    }
    mt_ret_u32(cpu, obj);
}

// -[UIImageView setImage:]: the frame stays.
void image_view_set_image(mt_cpu *cpu) {
    const u32 image = read_u32(cpu);
    if (View *v = self_view(cpu, "-[UIImageView setImage:]")) {
        assign(cpu, &v->image, image);
    }
}

void image_view_image(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIImageView image]");
    mt_ret_u32(cpu, v != nullptr ? v->image : 0u);
}

// --- UILabel -------------------------------------------------------------------------------------

void label_set_text(mt_cpu *cpu) { set_text(cpu, "-[UILabel setText:]"); }
void label_set_font(mt_cpu *cpu) { set_font(cpu, "-[UILabel setFont:]"); }

void label_text(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UILabel text]");
    std::string utf8;
    if (v != nullptr) {
        for (std::size_t i = 0; i < v->text.size(); i++) {
            u32 c = v->text[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < v->text.size()) {
                c = 0x10000 + ((c - 0xD800) << 10) + (v->text[++i] - 0xDC00u);
            }
            if (c < 0x80) {
                utf8 += static_cast<char>(c);
            } else if (c < 0x800) {
                utf8 += static_cast<char>(0xC0 | (c >> 6));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                utf8 += static_cast<char>(0xE0 | (c >> 12));
                utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                utf8 += static_cast<char>(0xF0 | (c >> 18));
                utf8 += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
    }
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, mt_ns::string_from_utf8(cpu, utf8)));
}

// -[UILabel setTextColor:]: nil is black (UIKit's default).
void label_set_text_color(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu); // -CGColor below clobbers r0
    const bool is_view = self_view(cpu, "-[UILabel setTextColor:]") != nullptr;
    const u32 color = read_u32(cpu);
    Color c{0, 0, 0, 1};
    if (color != 0 && !uicolor_value(cpu, color, &c)) {
        log_once(cpu, "setTextColor.nocolor", "-[UILabel setTextColor:]: 0x%08X is no UIColor",
                 static_cast<unsigned>(color));
    }
    if (View *v = is_view ? view_of(cpu, self) : nullptr) {
        v->text_color = c;
        assign(cpu, &v->text_color_obj, color);
    }
}

// -[UILabel setTextAlignment:(UITextAlignment)a]: left, center, right.
void label_set_alignment(mt_cpu *cpu) {
    const i32 a = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UILabel setTextAlignment:]")) {
        v->alignment = a >= 0 && a <= 2 ? a : 0;
    }
}

void label_set_lines(mt_cpu *cpu) {
    const i32 n = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UILabel setNumberOfLines:]")) {
        v->lines = std::max(0, n);
    }
}

// -[UILabel setBaselineAdjustment:]: only matters with adjustsFontSizeToFitWidth, which the
// game does not set; kept.
void label_set_baseline(mt_cpu *cpu) {
    const i32 n = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UILabel setBaselineAdjustment:]")) {
        v->baseline_adjustment = n;
    }
}

// --- UIControl, UIButton -------------------------------------------------------------------------

// -[UIControl addTarget:(id)target action:(SEL)action forControlEvents:(UIControlEvents)e]
void control_add_target(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    TargetAction ta;
    ta.target = mt_arg_u32(&a);
    ta.action = mt_arg_u32(&a);
    ta.events = mt_arg_u32(&a);
    View *v = self_view(cpu, "-[UIControl addTarget:action:forControlEvents:]");
    if (v == nullptr || ta.action == 0) {
        return;
    }
    for (const TargetAction &old : v->actions) {
        if (old.target == ta.target && old.action == ta.action && old.events == ta.events) {
            return;
        }
    }
    v->actions.push_back(ta);
}

void control_set_enabled(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIControl setEnabled:]")) {
        v->interaction = b; // a disabled control takes no touches (drawn the same)
    }
}

void control_is_highlighted(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIControl isHighlighted]");
    mt_ret_u32(cpu, v != nullptr && v->highlighted ? 1u : 0u);
}

// The touches* of UIControl: tracking (touch.cpp's control_touches), not forwarded.
void control_touches_began(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 touches = mt_arg_u32(&a), event = mt_arg_u32(&a);
    if (View *v = view_of(cpu, mt_objc_self(cpu))) {
        control_touches(cpu, *v, touches, event, MT_UIKIT_TOUCH_BEGAN);
    }
}
void control_touches_moved(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 touches = mt_arg_u32(&a), event = mt_arg_u32(&a);
    if (View *v = view_of(cpu, mt_objc_self(cpu))) {
        control_touches(cpu, *v, touches, event, MT_UIKIT_TOUCH_MOVED);
    }
}
void control_touches_ended(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 touches = mt_arg_u32(&a), event = mt_arg_u32(&a);
    if (View *v = view_of(cpu, mt_objc_self(cpu))) {
        control_touches(cpu, *v, touches, event, MT_UIKIT_TOUCH_ENDED);
    }
}
void control_touches_cancelled(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 touches = mt_arg_u32(&a), event = mt_arg_u32(&a);
    if (View *v = view_of(cpu, mt_objc_self(cpu))) {
        control_touches(cpu, *v, touches, event, MT_UIKIT_TOUCH_CANCELLED);
    }
}

// +[UIButton buttonWithType:(UIButtonType)type]: an autoreleased button with a zero frame;
// every type draws as UIButtonTypeCustom (0, the only one the game makes).
void button_with_type(mt_cpu *cpu) {
    const u32 cls = mt_objc_self(cpu);
    const u32 type = read_u32(cpu);
    if (type != 0) {
        log_once(cpu, "buttonWithType", "+[UIButton buttonWithType:%u]: drawn as a custom button",
                 static_cast<unsigned>(type));
    }
    mt_ret_u32(cpu, new_view(cpu, cls, Rect{}, true));
}

// -[UIButton setImage:(UIImage *)image forState:(UIControlState)state]
void button_set_image(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 image = mt_arg_u32(&a);
    const u32 control_state = mt_arg_u32(&a);
    View *v = self_view(cpu, "-[UIButton setImage:forState:]");
    if (v == nullptr) {
        return;
    }
    if (control_state > 2) {
        log_once(cpu, "setImage:forState:",
                 "-[UIButton setImage:forState:%u]: only normal, highlighted and disabled are "
                 "drawn",
                 static_cast<unsigned>(control_state));
        return;
    }
    assign(cpu, &v->images[control_state], image);
}

// -[UIButton setImageEdgeInsets:(UIEdgeInsets)insets]: top, left, bottom, right.
void button_set_image_insets(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    float insets[4] = {};
    for (float &f : insets) {
        f = mt_arg_f32(&a);
    }
    if (View *v = self_view(cpu, "-[UIButton setImageEdgeInsets:]")) {
        std::copy(insets, insets + 4, v->image_insets);
    }
}

// --- UIScrollView
// ----------------------------------------------------------------------------------

// -[UIScrollView contentOffset] -> CGPoint (stret): the bounds origin.
void scroll_content_offset(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const View *v = view_state(cpu, self);
    const float f[2] = {v != nullptr ? static_cast<float>(v->model.bounds.x) : 0.0f,
                        v != nullptr ? static_cast<float>(v->model.bounds.y) : 0.0f};
    mt_ret_struct(cpu, result, f, sizeof f);
}

// -[UIScrollView setContentOffset:(CGPoint)offset] (not clamped, as in UIKit)
void scroll_set_content_offset(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double x = mt_arg_f32(&a), y = mt_arg_f32(&a);
    if (View *v = self_view(cpu, "-[UIScrollView setContentOffset:]")) {
        v->decelerating = false;
        set_content_offset(cpu, *v, x, y);
    }
}

// -[UIScrollView contentSize] -> CGSize (stret)
void scroll_content_size(mt_cpu *cpu) {
    u32 result, self;
    mt_args a = mt_objc_args_stret(cpu, &result, &self);
    (void)a;
    const View *v = view_state(cpu, self);
    const float f[2] = {v != nullptr ? static_cast<float>(v->content_width) : 0.0f,
                        v != nullptr ? static_cast<float>(v->content_height) : 0.0f};
    mt_ret_struct(cpu, result, f, sizeof f);
}

// -[UIScrollView setContentSize:(CGSize)size]
void scroll_set_content_size(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const double w = mt_arg_f32(&a), h = mt_arg_f32(&a);
    if (View *v = self_view(cpu, "-[UIScrollView setContentSize:]")) {
        v->content_width = std::max(0.0, w);
        v->content_height = std::max(0.0, h);
    }
}

void scroll_set_paging(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setPagingEnabled:]")) {
        v->paging = b;
    }
}

void scroll_set_enabled(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setScrollEnabled:]")) {
        v->scroll_enabled = b;
    }
}

void scroll_set_can_cancel(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setCanCancelContentTouches:]")) {
        v->can_cancel_content_touches = b;
    }
}

void scroll_set_shows_horizontal(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setShowsHorizontalScrollIndicator:]")) {
        v->shows_horizontal = b;
    }
}

void scroll_set_shows_vertical(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setShowsVerticalScrollIndicator:]")) {
        v->shows_vertical = b;
    }
}

void scroll_set_indicator_style(mt_cpu *cpu) {
    const i32 s = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UIScrollView setIndicatorStyle:]")) {
        v->indicator_style = s;
    }
}

// -[UIScrollView setDelegate:]: not retained.
void scroll_set_delegate(mt_cpu *cpu) {
    const u32 d = read_u32(cpu);
    if (View *v = self_view(cpu, "-[UIScrollView setDelegate:]")) {
        v->delegate = d;
    }
}

void scroll_delegate(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIScrollView delegate]");
    mt_ret_u32(cpu, v != nullptr ? v->delegate : 0u);
}

// --- UITextView ----------------------------------------------------------------------------------

void text_view_set_text(mt_cpu *cpu) { set_text(cpu, "-[UITextView setText:]"); }
void text_view_set_font(mt_cpu *cpu) { set_font(cpu, "-[UITextView setFont:]"); }

// -[UITextView setEditable:]: the text is never edited (no keyboard; the game sets NO).
void text_view_set_editable(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UITextView setEditable:]")) {
        v->editable = b;
        if (b) {
            log_once(cpu, "setEditable", "-[UITextView setEditable:YES]: no text input: read-only");
        }
    }
}

void text_view_set_text_color(mt_cpu *cpu) { label_set_text_color(cpu); }

// --- UIPageControl -------------------------------------------------------------------------------

void page_set_count(mt_cpu *cpu) {
    const i32 n = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UIPageControl setNumberOfPages:]")) {
        v->pages = std::max(0, n);
        v->page = std::min(v->page, std::max(0, v->pages - 1));
    }
}

void page_set_current(mt_cpu *cpu) {
    const i32 n = static_cast<i32>(read_u32(cpu));
    if (View *v = self_view(cpu, "-[UIPageControl setCurrentPage:]")) {
        v->page = std::max(0, std::min(n, v->pages - 1));
    }
}

void page_current(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIPageControl currentPage]");
    mt_ret_u32(cpu, v != nullptr ? static_cast<u32>(v->page) : 0u);
}

void page_count(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIPageControl numberOfPages]");
    mt_ret_u32(cpu, v != nullptr ? static_cast<u32>(v->pages) : 0u);
}

// --- UIActivityIndicatorView --------------------------------------------------------------------

// The side of an indicator of a UIActivityIndicatorViewStyle: WhiteLarge 37, White and Gray 20.
double activity_side(i32 style) { return style == 0 ? 37 : 20; }

// -[UIActivityIndicatorView initWithActivityIndicatorStyle:(UIActivityIndicatorViewStyle)s]
void activity_init_with_style(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    const i32 style = static_cast<i32>(read_u32(cpu));
    const double side = activity_side(style);
    const u32 frame[4] = {0, 0, mt_f32_bits(static_cast<float>(side)),
                          mt_f32_bits(static_cast<float>(side))};
    const u32 obj = mt_objc_msg(cpu, self, mt_objc_sel(runtime(cpu), "initWithFrame:"), frame, 4);
    if (View *v = view_state(cpu, obj)) {
        v->style = style >= 0 && style <= 2 ? style : 0;
    }
    mt_ret_u32(cpu, obj);
}

void activity_set_hides(mt_cpu *cpu) {
    const bool b = read_bool(cpu);
    if (View *v = self_view(cpu, "-[UIActivityIndicatorView setHidesWhenStopped:]")) {
        v->hides_when_stopped = b;
    }
}

void activity_start(mt_cpu *cpu) {
    if (View *v = self_view(cpu, "-[UIActivityIndicatorView startAnimating]")) {
        v->animating = true;
    }
}

void activity_stop(mt_cpu *cpu) {
    if (View *v = self_view(cpu, "-[UIActivityIndicatorView stopAnimating]")) {
        v->animating = false;
    }
}

void activity_is_animating(mt_cpu *cpu) {
    const View *v = self_view(cpu, "-[UIActivityIndicatorView isAnimating]");
    mt_ret_u32(cpu, v != nullptr && v->animating ? 1u : 0u);
}

// --- tables --------------------------------------------------------------------------------------

const mt_objc_host_method k_image_view_methods[] = {
    {"initWithImage:", image_view_init_with_image},
    {"setImage:", image_view_set_image},
    {"image", image_view_image},
};
const mt_objc_host_method k_label_methods[] = {
    {"setText:", label_set_text},
    {"text", label_text},
    {"setFont:", label_set_font},
    {"setTextColor:", label_set_text_color},
    {"setTextAlignment:", label_set_alignment},
    {"setNumberOfLines:", label_set_lines},
    {"setBaselineAdjustment:", label_set_baseline},
};
const mt_objc_host_method k_control_methods[] = {
    {"addTarget:action:forControlEvents:", control_add_target},
    {"setEnabled:", control_set_enabled},
    {"isHighlighted", control_is_highlighted},
    {"touchesBegan:withEvent:", control_touches_began},
    {"touchesMoved:withEvent:", control_touches_moved},
    {"touchesEnded:withEvent:", control_touches_ended},
    {"touchesCancelled:withEvent:", control_touches_cancelled},
};
const mt_objc_host_method k_button_class_methods[] = {
    {"buttonWithType:", button_with_type},
};
const mt_objc_host_method k_button_methods[] = {
    {"setImage:forState:", button_set_image},
    {"setImageEdgeInsets:", button_set_image_insets},
};
const mt_objc_host_method k_scroll_methods[] = {
    {"contentOffset", scroll_content_offset},
    {"setContentOffset:", scroll_set_content_offset},
    {"contentSize", scroll_content_size},
    {"setContentSize:", scroll_set_content_size},
    {"setPagingEnabled:", scroll_set_paging},
    {"setScrollEnabled:", scroll_set_enabled},
    {"setCanCancelContentTouches:", scroll_set_can_cancel},
    {"setShowsHorizontalScrollIndicator:", scroll_set_shows_horizontal},
    {"setShowsVerticalScrollIndicator:", scroll_set_shows_vertical},
    {"setIndicatorStyle:", scroll_set_indicator_style},
    {"setDelegate:", scroll_set_delegate},
    {"delegate", scroll_delegate},
};
const mt_objc_host_method k_text_view_methods[] = {
    {"setText:", text_view_set_text},         {"text", label_text},
    {"setFont:", text_view_set_font},         {"setTextColor:", text_view_set_text_color},
    {"setEditable:", text_view_set_editable},
};
const mt_objc_host_method k_page_methods[] = {
    {"setNumberOfPages:", page_set_count},
    {"numberOfPages", page_count},
    {"setCurrentPage:", page_set_current},
    {"currentPage", page_current},
};
const mt_objc_host_method k_activity_methods[] = {
    {"initWithActivityIndicatorStyle:", activity_init_with_style},
    {"setHidesWhenStopped:", activity_set_hides},
    {"startAnimating", activity_start},
    {"stopAnimating", activity_stop},
    {"isAnimating", activity_is_animating},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

// UTF-16 helpers for wrapping.
bool is_space(char16_t c) { return c == u' ' || c == u'\t'; }

} // namespace

// --- shared ------------------------------------------------------------------------------------

Font default_font(Kind k) {
    Font f;
    f.size = k == Kind::text_view ? 12.0f : 17.0f;
    return f;
}

void control_defaults(mt_cpu *, View &v) {
    v.font_value = default_font(v.kind);
    switch (v.kind) {
    case Kind::image_view:
        v.interaction = false;
        v.opaque = false;
        break;
    case Kind::label:
        // iOS 4's UILabel: opaque white, black 17-point system font, one line.
        v.interaction = false;
        v.background_color = Color{1, 1, 1, 1};
        break;
    case Kind::text_view:
        v.background_color = Color{1, 1, 1, 1};
        v.clips = true;
        break;
    case Kind::scroll_view:
        v.clips = true;
        break;
    case Kind::button:
    case Kind::control:
    case Kind::page_control:
        v.opaque = false;
        break;
    case Kind::activity:
        v.interaction = false;
        v.opaque = false;
        break;
    case Kind::view:
    case Kind::window:
        break;
    }
}

u32 button_image(const View &v) {
    if (v.highlighted && v.images[1] != 0) {
        return v.images[1];
    }
    if (!v.interaction && v.images[2] != 0) {
        return v.images[2];
    }
    return v.images[0];
}

// UIButtonTypeCustom: the image at its size (shrunk to fit, keeping its aspect ratio) centred
// in the bounds less the image edge insets.
Rect button_image_rect(mt_cpu *cpu, const View &v, u32 image) {
    const Rect &b = v.model.bounds;
    const Rect content{b.x + v.image_insets[1], b.y + v.image_insets[0],
                       b.w - v.image_insets[1] - v.image_insets[3],
                       b.h - v.image_insets[0] - v.image_insets[2]};
    double w = 0, h = 0;
    if (!mt_cg::uiimage_size(cpu, image, &w, &h) || !(w > 0 && h > 0)) {
        return Rect{content.x + content.w / 2, content.y + content.h / 2, 0, 0};
    }
    const double fit =
        std::min({1.0, content.w > 0 ? content.w / w : 0.0, content.h > 0 ? content.h / h : 0.0});
    w *= fit;
    h *= fit;
    return Rect{content.x + (content.w - w) / 2, content.y + (content.h - h) / 2, w, h};
}

std::vector<std::u16string> wrap_text(const std::u16string &text, const Font &f, double width,
                                      int max_lines) {
    std::vector<std::u16string> lines;
    bool truncated = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(u'\n', start);
        if (end == std::u16string::npos) {
            end = text.size();
        }
        std::u16string para = text.substr(start, end - start);
        if (!para.empty() && para.back() == u'\r') {
            para.pop_back();
        }
        // Words: break after spaces; a word wider than the line breaks between characters.
        std::u16string line;
        std::size_t i = 0;
        bool emitted = false;
        while (i < para.size()) {
            std::size_t j = i;
            while (j < para.size() && !is_space(para[j])) {
                j++;
            }
            while (j < para.size() && is_space(para[j])) {
                j++;
            }
            const std::u16string word = para.substr(i, j - i);
            std::u16string trial = line + word;
            std::u16string trimmed = trial;
            while (!trimmed.empty() && is_space(trimmed.back())) {
                trimmed.pop_back();
            }
            if (width <= 0 || mt_cg::text_width(trimmed, f) <= width + 1e-3) {
                line = trial;
                i = j;
                continue;
            }
            if (!line.empty()) {
                while (!line.empty() && is_space(line.back())) {
                    line.pop_back();
                }
                lines.push_back(line);
                emitted = true;
                line.clear();
                continue;
            }
            // The word alone is too wide: as many characters as fit (at least one).
            std::size_t k = i + 1;
            while (k < j && mt_cg::text_width(para.substr(i, k + 1 - i), f) <= width + 1e-3) {
                k++;
            }
            lines.push_back(para.substr(i, k - i));
            emitted = true;
            i = k;
        }
        while (!line.empty() && is_space(line.back())) {
            line.pop_back();
        }
        if (!line.empty() || !emitted) {
            lines.push_back(line);
        }
        if (max_lines > 0 && static_cast<int>(lines.size()) > max_lines) {
            truncated = true;
            break;
        }
        start = end + 1;
        if (end == text.size()) {
            break;
        }
    }
    if (max_lines > 0 && static_cast<int>(lines.size()) >= max_lines &&
        (truncated || static_cast<int>(lines.size()) > max_lines)) {
        lines.resize(static_cast<std::size_t>(max_lines));
        // UILineBreakModeTailTruncation: the last line ends in "..." within the width.
        std::u16string &last = lines.back();
        const std::u16string dots = u"...";
        while (!last.empty() && width > 0 && mt_cg::text_width(last + dots, f) > width + 1e-3) {
            last.pop_back();
        }
        last += dots;
    }
    return lines;
}

void set_content_offset(mt_cpu *cpu, View &v, double x, double y) {
    if (v.model.bounds.x == x && v.model.bounds.y == y) {
        return;
    }
    v.model.bounds.x = x;
    v.model.bounds.y = y;
    const u32 self = v.obj, delegate = v.delegate;
    if (delegate != 0 && responds(cpu, delegate, "scrollViewDidScroll:")) {
        mt_objc_send1(cpu, delegate, "scrollViewDidScroll:", self);
    }
}

void max_content_offset(const View &v, double *x, double *y) {
    *x = std::max(0.0, v.content_width - v.model.bounds.w);
    *y = std::max(0.0, v.content_height - v.model.bounds.h);
}

void text_view_layout(View &v) {
    const double width = v.model.bounds.w - 2 * k_text_view_inset;
    const std::vector<std::u16string> lines = wrap_text(v.text, v.font_value, width, 0);
    const mt_cg::FontMetrics m = mt_cg::font_metrics(v.font_value);
    v.content_width = v.model.bounds.w;
    v.content_height = static_cast<double>(lines.size()) * m.line_height + 2 * k_text_view_inset;
}

// --- the classes ---------------------------------------------------------------------------------

const mt_objc_host_class k_uiimageview = {
    "UIImageView", "UIView", 46, k_image_view_methods, countof(k_image_view_methods), nullptr, 0};
const mt_objc_host_class k_uilabel = {
    "UILabel", "UIView", 46, k_label_methods, countof(k_label_methods), nullptr, 0};
const mt_objc_host_class k_uicontrol = {
    "UIControl", "UIView", 46, k_control_methods, countof(k_control_methods), nullptr, 0};
const mt_objc_host_class k_uibutton = {"UIButton",
                                       "UIControl",
                                       46,
                                       k_button_methods,
                                       countof(k_button_methods),
                                       k_button_class_methods,
                                       countof(k_button_class_methods)};
const mt_objc_host_class k_uipagecontrol = {
    "UIPageControl", "UIControl", 46, k_page_methods, countof(k_page_methods), nullptr, 0};
const mt_objc_host_class k_uiscrollview = {
    "UIScrollView", "UIView", 46, k_scroll_methods, countof(k_scroll_methods), nullptr, 0};
const mt_objc_host_class k_uitextview = {"UITextView",
                                         "UIScrollView",
                                         46,
                                         k_text_view_methods,
                                         countof(k_text_view_methods),
                                         nullptr,
                                         0};
const mt_objc_host_class k_uiactivityindicatorview = {
    "UIActivityIndicatorView",   "UIView", 46, k_activity_methods,
    countof(k_activity_methods), nullptr,  0};

} // namespace mt_ui
