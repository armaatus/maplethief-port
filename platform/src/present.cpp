// Presentation (#58, ADR 0042); see present.h.
#include "present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "focus.h"
#include "maplethief/gfx.h"
#include "maplethief/uikit.h"
#include "maplethief/widescreen.h"

namespace maplethief::frontend {

// --- Screen -----------------------------------------------------------------------------------

Screen::Screen(std::uint32_t render_scale) { set_scale(render_scale); }

void Screen::set_scale(std::uint32_t render_scale) {
    const std::uint32_t scale = std::clamp<std::uint32_t>(render_scale, 1, MT_GFX_MAX_SCALE);
    width_ = MT_UIKIT_SCREEN_WIDTH * scale;
    height_ = MT_UIKIT_SCREEN_HEIGHT * scale;
    screen_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
}

bool Screen::take_gl_frame(const mt_gles_frame &frame) {
    const mt_gfx_target *target = frame.target;
    if (target == nullptr) {
        return false;
    }
    set_gl_frame(frame.index, mt_gfx_target_cpixels(target), mt_gfx_target_pixel_width(target),
                 mt_gfx_target_pixel_height(target));
    return true;
}

void Screen::set_gl_frame(std::uint32_t index, const std::uint8_t *rgba, std::uint32_t width,
                          std::uint32_t height) {
    gl_width_ = width;
    gl_height_ = height;
    const std::size_t n = static_cast<std::size_t>(gl_width_) * gl_height_;
    gl_.resize(n * 4);
    // The CAEAGLLayer is opaque: the device shows the renderbuffer's colour and ignores its
    // alpha (which starts at 0 and is whatever the blending left).
    std::memcpy(gl_.data(), rgba, n * 4);
    for (std::size_t i = 0; i < n; i++) {
        gl_[i * 4 + 3] = 255;
    }
    gl_index_ = index;
    gl_view_ = mt_widescreen_view();
    if (first_lit_gl_ == 0 && !is_black(gl_.data(), gl_width_, gl_height_, gl_width_ * 4)) {
        first_lit_gl_ = index;
    }
}

GlColumns gl_screen_columns(std::uint32_t gl_width, std::uint32_t gl_height, std::uint32_t view) {
    const std::uint32_t scale = gl_height / MT_UIKIT_SCREEN_HEIGHT;
    const std::uint32_t narrow = MT_UIKIT_SCREEN_WIDTH * scale;
    if (scale == 0 || gl_height % MT_UIKIT_SCREEN_HEIGHT != 0 || gl_width <= narrow) {
        return GlColumns{0, gl_width}; // a 480-point frame (or another shape): whole
    }
    const std::uint32_t x =
        view > MT_UIKIT_SCREEN_WIDTH ? (view - MT_UIKIT_SCREEN_WIDTH) / 2 * scale : 0;
    return x + narrow <= gl_width ? GlColumns{x, narrow} : GlColumns{0, narrow};
}

bool Screen::compose(mt_cpu *cpu, double now) {
    mt_uikit_gl_frame gl{};
    const bool have_gl = !gl_.empty();
    if (have_gl) {
        // Widescreen (#86): the GL view shows the centre 480 points of what the engine drew.
        const GlColumns cols = gl_screen_columns(gl_width_, gl_height_, gl_view_);
        gl.pixels = gl_.data() + static_cast<std::size_t>(cols.x) * 4;
        gl.width = cols.width;
        gl.height = gl_height_;
        gl.stride = gl_width_ * 4;
    }
    return mt_uikit_composite(cpu, now, MT_UIKIT_COMPOSITE_FULL, have_gl ? &gl : nullptr,
                              screen_.data(), width_, height_, width_ * 4) == 0;
}

bool is_black(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
              std::uint32_t stride) {
    std::size_t lit = 0;
    for (std::uint32_t y = 0; y < height; y++) {
        const std::uint8_t *p = rgba + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < width; x++, p += 4) {
            lit += p[0] + p[1] + p[2] > 24 ? 1u : 0u;
        }
    }
    return lit * 100 <= static_cast<std::size_t>(width) * height;
}

// --- Presenter --------------------------------------------------------------------------------

bool Presenter::start(SDL_Renderer *renderer, std::uint32_t image_w, std::uint32_t image_h,
                      bool target) {
    // A new size on the same renderer (render_scale = auto, #152) keeps the border's art; the
    // blur's levels follow the texture's size at the next draw.
    if (renderer != renderer_) {
        border_.release();
        color_pass_.close();
        color_pass_failed_ = false;
    }
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (graded_ != nullptr) {
        SDL_DestroyTexture(graded_);
        graded_ = nullptr;
    }
    uploaded_ = ColorblindFilter::kOff;
    renderer_ = renderer;
    width_ = image_w;
    height_ = image_h;
    // ABGR8888 is R, G, B, A in memory: mt_gfx's and the compositor's byte order. An opaque
    // copy, nearest scaling.
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                 target ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING,
                                 static_cast<int>(image_w), static_cast<int>(image_h));
    if (texture_ == nullptr) {
        SDL_Log("present: no %ux%u texture: %s", image_w, image_h, SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    return true;
}

void Presenter::stop() {
    border_.release();
    color_pass_.close();
    color_pass_failed_ = false;
    if (graded_ != nullptr) {
        SDL_DestroyTexture(graded_);
        graded_ = nullptr;
    }
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (wings_own_ != nullptr) {
        SDL_DestroyTexture(wings_own_);
        wings_own_ = nullptr;
    }
    wings_ = nullptr;
    wings_view_ = 0;
    if (pad_wings_ != nullptr) {
        SDL_DestroyTexture(pad_wings_);
        pad_wings_ = nullptr;
    }
    pad_wings_on_ = false;
    renderer_ = nullptr;
}

Rect wide_part(const Rect &wide, std::uint32_t wide_points, std::uint32_t points) {
    if (wide_points == 0 || points >= wide_points) {
        return wide;
    }
    const double per_point = static_cast<double>(wide.w) / wide_points;
    const double left = std::round(wide.x + per_point * ((wide_points - points) / 2.0));
    const double right = std::round(wide.x + per_point * ((wide_points + points) / 2.0));
    return Rect{static_cast<float>(left), wide.y, static_cast<float>(right - left), wide.h};
}

void Presenter::set_wings(SDL_Texture *gl, std::uint32_t view, std::uint32_t scale) {
    wings_ = gl;
    wings_view_ = gl != nullptr ? view : 0;
    wings_scale_ = std::max<std::uint32_t>(scale, 1);
}

bool Presenter::upload_wings(const std::uint8_t *rgba, std::uint32_t gl_w, std::uint32_t gl_h,
                             std::uint32_t view) {
    if (renderer_ == nullptr || rgba == nullptr || view <= MT_UIKIT_SCREEN_WIDTH ||
        gl_h < MT_UIKIT_SCREEN_HEIGHT) {
        set_wings(nullptr, 0, 1);
        return true;
    }
    float tw = 0;
    float th = 0;
    if (wings_own_ != nullptr &&
        (!SDL_GetTextureSize(wings_own_, &tw, &th) || tw != static_cast<float>(gl_w) ||
         th != static_cast<float>(gl_h))) {
        SDL_DestroyTexture(wings_own_);
        wings_own_ = nullptr;
    }
    if (wings_own_ == nullptr) {
        wings_own_ =
            SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              static_cast<int>(gl_w), static_cast<int>(gl_h));
        if (wings_own_ == nullptr) {
            SDL_Log("present: no %ux%u texture for the wide view: %s", gl_w, gl_h, SDL_GetError());
            set_wings(nullptr, 0, 1);
            return false;
        }
        SDL_SetTextureBlendMode(wings_own_, SDL_BLENDMODE_NONE);
    }
    if (!SDL_UpdateTexture(wings_own_, nullptr, rgba, static_cast<int>(gl_w * 4))) {
        SDL_Log("present: the wide view's upload failed: %s", SDL_GetError());
        set_wings(nullptr, 0, 1);
        return false;
    }
    set_wings(wings_own_, view, gl_h / MT_UIKIT_SCREEN_HEIGHT);
    return true;
}

bool Presenter::upload_pad_wings(const std::uint8_t *rgba, std::uint32_t w, std::uint32_t h) {
    pad_wings_on_ = false;
    if (renderer_ == nullptr || rgba == nullptr || w < 2 || h == 0) {
        return true;
    }
    if (pad_wings_ != nullptr && (pad_w_ != w || pad_h_ != h)) {
        SDL_DestroyTexture(pad_wings_);
        pad_wings_ = nullptr;
    }
    if (pad_wings_ == nullptr) {
        pad_wings_ =
            SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              static_cast<int>(w), static_cast<int>(h));
        if (pad_wings_ == nullptr) {
            SDL_Log("present: no %ux%u texture for the key pad's wings: %s", w, h, SDL_GetError());
            return false;
        }
        // The compositor's pixels are premultiplied.
        SDL_SetTextureBlendMode(pad_wings_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        pad_w_ = w;
        pad_h_ = h;
    }
    if (!SDL_UpdateTexture(pad_wings_, nullptr, rgba, static_cast<int>(w * 4))) {
        SDL_Log("present: the key pad's wings' upload failed: %s", SDL_GetError());
        return false;
    }
    pad_wings_on_ = true;
    return true;
}

void Presenter::set_scaling(ScaleMode mode, Color border) {
    mode_ = mode;
    color_ = border;
}

void Presenter::set_border(BorderMode mode, const std::string &image) { border_.set(mode, image); }

bool Presenter::color_on_gpu() {
    if (renderer_ == nullptr) {
        return false;
    }
    if (color_pass_.state() != nullptr) {
        return true;
    }
    if (color_pass_failed_) {
        return false;
    }
    color_pass_failed_ = !color_pass_.open(renderer_);
    SDL_Log("present: colour-blind filter on the %s",
            color_pass_failed_ ? "CPU, before the upload (the renderer runs no shaders of ours)"
                               : "GPU (present_colorblind.frag into an image-sized target)");
    return !color_pass_failed_;
}

bool Presenter::upload(const std::uint8_t *rgba, std::uint32_t stride) {
    if (texture_ == nullptr) {
        return false;
    }
    // The colour-blind filter's CPU way (#158): where the renderer has no pass for it.
    uploaded_ = ColorblindFilter::kOff;
    if (colorblind_ != ColorblindFilter::kOff && !color_on_gpu()) {
        if (color_cpu_.filter() != colorblind_) {
            color_cpu_.set(colorblind_);
        }
        graded_px_.resize(std::size_t{width_} * height_ * 4);
        color_cpu_.apply(rgba, stride, graded_px_.data(), width_ * 4, width_, height_);
        rgba = graded_px_.data();
        stride = width_ * 4;
        uploaded_ = colorblind_;
    }
    if (!SDL_UpdateTexture(texture_, nullptr, rgba, static_cast<int>(stride))) {
        SDL_Log("present: SDL_UpdateTexture failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Presenter::draw() {
    if (texture_ == nullptr) {
        return false;
    }
    int ow = 0;
    int oh = 0;
    if (!SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh)) {
        SDL_Log("present: SDL_GetCurrentRenderOutputSize failed: %s", SDL_GetError());
        return false;
    }
    SDL_Window *window = SDL_GetRenderWindow(renderer_);
    insets_ = forced_ ? *forced_ : window_insets(window);
    float dx = 1;
    float dy = 1;
    window_density(window, &dx, &dy);
    const bool wide = wide_ > MT_UIKIT_SCREEN_WIDTH;
    if (wide) {
        // Widescreen (#86): the wide picture placed, the screen image its centre 480 points.
        const std::uint32_t texels_w = width_ / MT_UIKIT_SCREEN_WIDTH * wide_;
        placement_ = place(mode_, ow, oh, static_cast<int>(texels_w), static_cast<int>(height_),
                           insets_, dx, dy, static_cast<int>(wide_));
        wide_rect_ = placement_.rect;
        placement_.rect = wide_part(wide_rect_, wide_, MT_UIKIT_SCREEN_WIDTH);
    } else {
        placement_ = place(mode_, ow, oh, static_cast<int>(width_), static_cast<int>(height_),
                           insets_, dx, dy);
        wide_rect_ = placement_.rect;
    }
    // 0. The colour-blind filter (#158, color_filter.h): the image the steps below read. On the
    //    CPU it is in the uploaded pixels already; on SDL's GPU renderer the screen texture is
    //    drawn through its pass into `graded_` here. With widescreen it grades the 3:2 screen
    //    image only: the wings are the GL frame's, not graded yet (#170).
    SDL_Texture *src = texture_;
    color_path_ = ColorPath::kNone;
    color_drawn_ = ColorblindFilter::kOff;
    if (uploaded_ != ColorblindFilter::kOff) {
        color_path_ = ColorPath::kCpu;
        color_drawn_ = uploaded_;
    } else if (colorblind_ != ColorblindFilter::kOff && color_on_gpu()) {
        if (!grade()) {
            return false;
        }
        src = graded_;
        color_path_ = ColorPath::kGpu;
        color_drawn_ = colorblind_;
    }
    const SDL_ScaleMode scale_mode = placement_.filter == Filter::kLinear ? SDL_SCALEMODE_LINEAR
                                     : placement_.filter == Filter::kPixelArt
                                         ? SDL_SCALEMODE_PIXELART
                                         : SDL_SCALEMODE_NEAREST;
    SDL_SetTextureScaleMode(src, scale_mode);
    // 1. The border colour (all of the bars for border = color).
    SDL_SetRenderDrawColor(renderer_, color_.r, color_.g, color_.b, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_);
    if (placement_.rect.w <= 0 || placement_.rect.h <= 0) {
        border_.draw(renderer_, nullptr, 0, 0, placement_.rect, ow, oh); // drawn: color
        return true; // a minimised window: nothing to draw into
    }
    // 2. The border's step: the blur or the art over the whole drawable (border.h). With
    // widescreen the blur is still stretched about the screen image's rectangle: it shares the
    // wide picture's centre, so the blur covers the drawable around the wide picture as well,
    // in the screen texture's own aspect; the wings and the image go over it next.
    if (!border_.draw(renderer_, src, static_cast<int>(width_), static_cast<int>(height_),
                      placement_.rect, ow, oh)) {
        return false;
    }
    // 3. The picture. Widescreen's wings first (#86, ADR 0090): the engine's first wings_view_
    // points of the GL frame, centred in the wide picture; the screen image covers their middle
    // 480 next (through the pass, if any: the wings take only the scale mode's filter, #170).
    if (wide && wings_ != nullptr && wings_view_ > MT_UIKIT_SCREEN_WIDTH) {
        const std::uint32_t view = std::min(wings_view_, wide_);
        const Rect r = wide_part(wide_rect_, wide_, view);
        const SDL_FRect wsrc{0, 0, static_cast<float>(view * wings_scale_),
                             static_cast<float>(MT_UIKIT_SCREEN_HEIGHT * wings_scale_)};
        const SDL_FRect wdst{r.x, r.y, r.w, r.h};
        SDL_SetTextureScaleMode(wings_, scale_mode);
        if (!SDL_RenderTexture(renderer_, wings_, &wsrc, &wdst)) {
            SDL_Log("present: the wide view's draw failed: %s", SDL_GetError());
        }
    }
    const SDL_FRect dst{placement_.rect.x, placement_.rect.y, placement_.rect.w, placement_.rect.h};
    SDL_GPURenderState *state = stage_.state;
    if (state != nullptr && stage_.sizes) {
        const float sw = static_cast<float>(width_);
        const float sh = static_cast<float>(height_);
        const PresentSizes sizes{{sw, sh, 1.0f / sw, 1.0f / sh},
                                 {dst.w, dst.h, 1.0f / dst.w, 1.0f / dst.h}};
        if (!SDL_SetGPURenderStateFragmentUniforms(state, 0, &sizes, sizeof sizes)) {
            SDL_Log("present: SDL_SetGPURenderStateFragmentUniforms failed: %s", SDL_GetError());
            return false;
        }
    }
    if (state != nullptr && !SDL_SetGPURenderState(renderer_, state)) {
        SDL_Log("present: SDL_SetGPURenderState failed: %s", SDL_GetError());
        return false;
    }
    const bool drawn = SDL_RenderTexture(renderer_, src, nullptr, &dst);
    if (state != nullptr) {
        SDL_SetGPURenderState(renderer_, nullptr);
    }
    if (!drawn) {
        SDL_Log("present: SDL_RenderTexture failed: %s", SDL_GetError());
        return false;
    }
    // 4. Widescreen with the key pad (#168): the game pad's keys moved into the wings, over
    // them, beside the screen image (each strip is a wing's (wide - 480) / 2 points).
    if (wide && pad_wings_on_ && pad_wings_ != nullptr) {
        const float half = static_cast<float>(pad_w_ / 2);
        const float left = placement_.rect.x - wide_rect_.x;
        const float right_x = placement_.rect.x + placement_.rect.w;
        const float right = wide_rect_.x + wide_rect_.w - right_x;
        SDL_SetTextureScaleMode(pad_wings_, scale_mode);
        const SDL_FRect src_l{0, 0, half, static_cast<float>(pad_h_)};
        const SDL_FRect src_r{half, 0, half, static_cast<float>(pad_h_)};
        const SDL_FRect dst_l{wide_rect_.x, placement_.rect.y, left, placement_.rect.h};
        const SDL_FRect dst_r{right_x, placement_.rect.y, right, placement_.rect.h};
        if ((left > 0 && !SDL_RenderTexture(renderer_, pad_wings_, &src_l, &dst_l)) ||
            (right > 0 && !SDL_RenderTexture(renderer_, pad_wings_, &src_r, &dst_r))) {
            SDL_Log("present: the key pad's wings' draw failed: %s", SDL_GetError());
        }
    }
    return true;
}

bool Presenter::grade() {
    float gw = 0;
    float gh = 0;
    if (graded_ != nullptr &&
        (!SDL_GetTextureSize(graded_, &gw, &gh) || gw != static_cast<float>(width_) ||
         gh != static_cast<float>(height_))) {
        SDL_DestroyTexture(graded_);
        graded_ = nullptr;
    }
    if (graded_ == nullptr) {
        graded_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                    static_cast<int>(width_), static_cast<int>(height_));
        if (graded_ == nullptr) {
            SDL_Log("present: colour-blind filter: no %ux%u render target: %s", width_, height_,
                    SDL_GetError());
            return false;
        }
        SDL_SetTextureBlendMode(graded_, SDL_BLENDMODE_NONE);
    }
    SDL_Texture *before = SDL_GetRenderTarget(renderer_);
    SDL_GPURenderState *state = color_pass_.state();
    bool ok = color_pass_.set_uniforms(colorblind_, width_, height_) &&
              SDL_SetRenderTarget(renderer_, graded_) && SDL_SetGPURenderState(renderer_, state);
    ok = ok && SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
    SDL_SetGPURenderState(renderer_, nullptr);
    SDL_SetRenderTarget(renderer_, before);
    if (!ok) {
        SDL_Log("present: colour-blind filter: %s", SDL_GetError());
    }
    return ok;
}

void Presenter::draw_highlight(float x, float y, float w, float h) {
    if (renderer_ == nullptr) {
        return;
    }
    const float width = focus_highlight_width(placement_);
    const Rect outer = focus_highlight(placement_, x, y, w, h, width);
    if (outer.w <= 0 || outer.h <= 0) {
        return;
    }
    // A dark line outside a bright one: visible over light and dark art alike.
    const float dark = std::max(1.0f, std::floor(width / 3));
    const auto frame = [this](const Rect &r, float t) {
        const SDL_FRect sides[4] = {{r.x, r.y, r.w, t},
                                    {r.x, r.y + r.h - t, r.w, t},
                                    {r.x, r.y + t, t, r.h - 2 * t},
                                    {r.x + r.w - t, r.y + t, t, r.h - 2 * t}};
        SDL_RenderFillRects(renderer_, sides, 4);
    };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0x20, 0x18, 0x00, SDL_ALPHA_OPAQUE);
    frame(outer, dark);
    SDL_SetRenderDrawColor(renderer_, 0xFF, 0xD4, 0x00, SDL_ALPHA_OPAQUE);
    if (outer.w > 2 * dark && outer.h > 2 * dark) {
        frame(Rect{outer.x + dark, outer.y + dark, outer.w - 2 * dark, outer.h - 2 * dark},
              std::max(1.0f, width - dark));
    }
}

long Presenter::check_readback(const std::uint8_t *rgba, std::uint32_t stride,
                               std::string *report) {
    SDL_Surface *read = SDL_RenderReadPixels(renderer_, nullptr);
    if (read == nullptr) {
        if (report != nullptr) {
            *report = std::string("SDL_RenderReadPixels failed: ") + SDL_GetError();
        }
        return -1;
    }
    // R, G, B, A bytes, whatever the renderer's format, so that pixels are indexed directly.
    SDL_Surface *surface = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(read);
    if (surface == nullptr) {
        if (report != nullptr) {
            *report = std::string("SDL_ConvertSurface failed: ") + SDL_GetError();
        }
        return -1;
    }
    const auto *out = static_cast<const std::uint8_t *>(surface->pixels);
    const int sw = surface->w;
    const int sh = surface->h;
    auto pixel = [&](int ox, int oy) {
        return out + static_cast<std::size_t>(oy) * static_cast<std::size_t>(surface->pitch) +
               static_cast<std::size_t>(ox) * 4;
    };
    const Rect &r = placement_.rect;
    // Under the colour-blind filter (#158) the image is the filtered one: the CPU way's exactly,
    // the GPU way's within 2 per channel of it (float against the CPU's integer tables).
    std::vector<std::uint8_t> filtered;
    int tolerance = 0;
    if (color_drawn_ != ColorblindFilter::kOff) {
        ColorTransform t;
        t.set(color_drawn_);
        filtered.resize(std::size_t{width_} * height_ * 4);
        t.apply(rgba, stride, filtered.data(), width_ * 4, width_, height_);
        rgba = filtered.data();
        stride = width_ * 4;
        tolerance = color_path_ == ColorPath::kGpu ? 2 : 0;
    }
    // Under a pass, its PassCheck decides how the image is judged.
    const PassCheck check = stage_.state != nullptr ? stage_.check : PassCheck::kScaleMode;
    const bool exact = check == PassCheck::kScaleMode && placement_.filter == Filter::kNearest;
    const bool image = check != PassCheck::kBarsOnly;
    long mismatches = 0;
    char first[200] = "";
    auto note = [&](int ox, int oy, const char *what, const std::uint8_t *got, unsigned er,
                    unsigned eg, unsigned eb) {
        if (mismatches++ == 0) {
            std::snprintf(first, sizeof first,
                          "first at output (%d, %d), %s: #%02X%02X%02X, expected #%02X%02X%02X", ox,
                          oy, what, got[0], got[1], got[2], er, eg, eb);
        }
    };
    // The image, every output pixel of the placement (whole pixels: place() rounds them). The
    // texels an output pixel covers, per axis: lo = floor(o * n / size) to
    // hi = floor(((o + 1) * n - 1) / size), in integers (o from the image's edge, n texels).
    const long long rx = static_cast<long long>(r.x);
    const long long ry = static_cast<long long>(r.y);
    const long long rw = static_cast<long long>(r.w);
    const long long rh = static_cast<long long>(r.h);
    const long long iw = width_;
    const long long ih = height_;
    auto texel = [&](long long tx, long long ty) {
        return rgba + static_cast<std::size_t>(ty) * stride + static_cast<std::size_t>(tx) * 4;
    };
    // Blending: per texel, the range of each channel over it and its 8 neighbours (low R, G, B,
    // then high R, G, B). A blend of the texels a pixel covers lies within their ranges.
    std::vector<std::uint8_t> range;
    if (image && !exact && rw > 0 && rh > 0) {
        range.resize(static_cast<std::size_t>(iw * ih) * 6);
        for (long long ty = 0; ty < ih; ty++) {
            for (long long tx = 0; tx < iw; tx++) {
                std::uint8_t *q = &range[static_cast<std::size_t>(ty * iw + tx) * 6];
                q[0] = q[1] = q[2] = 255;
                q[3] = q[4] = q[5] = 0;
                for (long long ny = ty > 0 ? ty - 1 : 0; ny <= ty + 1 && ny < ih; ny++) {
                    for (long long nx = tx > 0 ? tx - 1 : 0; nx <= tx + 1 && nx < iw; nx++) {
                        const std::uint8_t *e = texel(nx, ny);
                        for (int c = 0; c < 3; c++) {
                            q[c] = e[c] < q[c] ? e[c] : q[c];
                            q[3 + c] = e[c] > q[3 + c] ? e[c] : q[3 + c];
                        }
                    }
                }
            }
        }
    }
    const long long oy_end = std::min(ry + rh, static_cast<long long>(sh));
    const long long ox_end = std::min(rx + rw, static_cast<long long>(sw));
    for (long long oy = std::max(ry, 0LL); image && rw > 0 && oy < oy_end; oy++) {
        const long long ty0 = (oy - ry) * ih / rh;
        const long long ty1 = ((oy - ry + 1) * ih - 1) / rh;
        for (long long ox = std::max(rx, 0LL); ox < ox_end; ox++) {
            const long long tx0 = (ox - rx) * iw / rw;
            const long long tx1 = ((ox - rx + 1) * iw - 1) / rw;
            const std::uint8_t *got = pixel(static_cast<int>(ox), static_cast<int>(oy));
            bool ok = false;
            if (exact) {
                // Nearest: one of the texels the pixel covers (at a fractional scale the
                // renderer may round a centre near a texel edge either way).
                for (long long ty = ty0; ty <= ty1 && !ok; ty++) {
                    for (long long tx = tx0; tx <= tx1 && !ok; tx++) {
                        const std::uint8_t *e = texel(tx, ty);
                        ok = std::abs(got[0] - e[0]) <= tolerance &&
                             std::abs(got[1] - e[1]) <= tolerance &&
                             std::abs(got[2] - e[2]) <= tolerance;
                    }
                }
            } else {
                int lo[3] = {255, 255, 255};
                int hi[3] = {0, 0, 0};
                for (long long ty = ty0; ty <= ty1; ty++) {
                    for (long long tx = tx0; tx <= tx1; tx++) {
                        const std::uint8_t *q = &range[static_cast<std::size_t>(ty * iw + tx) * 6];
                        for (int c = 0; c < 3; c++) {
                            lo[c] = q[c] < lo[c] ? q[c] : lo[c];
                            hi[c] = q[3 + c] > hi[c] ? q[3 + c] : hi[c];
                        }
                    }
                }
                ok = true;
                for (int c = 0; c < 3; c++) {
                    ok = ok && got[c] + 2 + tolerance >= lo[c] && got[c] <= hi[c] + 2 + tolerance;
                }
            }
            if (!ok) {
                const std::uint8_t *e = texel((tx0 + tx1) / 2, (ty0 + ty1) / 2);
                note(static_cast<int>(ox), static_cast<int>(oy),
                     exact ? "image" : "image (blended)", got, e[0], e[1], e[2]);
            }
        }
    }
    // The bars: every output pixel outside the placement has the border colour (unless the
    // border drew the blur or the art there: border.h; widescreen's wings, the GL frame beside
    // the image, are never a bar).
    const BorderMode bars = border_.drawn();
    const int x0 = static_cast<int>(r.x);
    const int x1 = static_cast<int>(r.x + r.w);
    const bool wings_drawn =
        wide_ > MT_UIKIT_SCREEN_WIDTH && wings_ != nullptr && wings_view_ > MT_UIKIT_SCREEN_WIDTH;
    const Rect wr = wide_part(wide_rect_, wide_, std::min(wings_view_, wide_));
    const int wx0 = static_cast<int>(wr.x);
    const int wx1 = static_cast<int>(wr.x + wr.w);
    for (int oy = 0; bars == BorderMode::kColor && oy < sh; oy++) {
        const float fy = static_cast<float>(oy);
        const bool row_in = r.w > 0 && fy >= r.y && fy < r.y + r.h;
        for (int ox = 0; ox < sw; ox++) {
            if (row_in && ox >= x0 && ox < x1) {
                ox = x1 - 1; // the image's part of the row
                continue;
            }
            if (row_in && wings_drawn && ox >= wx0 && ox < wx1) {
                continue; // widescreen's wings: the GL frame, not a bar
            }
            const std::uint8_t *got = pixel(ox, oy);
            if (got[0] != color_.r || got[1] != color_.g || got[2] != color_.b) {
                note(ox, oy, "bar", got, color_.r, color_.g, color_.b);
            }
        }
    }
    if (report != nullptr) {
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "%dx%d output, %s%s: the %ux%u image at (%g, %g, %g, %g), bars "
                      "#%02X%02X%02X%s: %ld mismatched%s%s",
                      sw, sh, scale_mode_name(placement_.mode),
                      check == PassCheck::kBlend      ? " (a pass: blended)"
                      : check == PassCheck::kBarsOnly ? " (a pass: only the bars checked)"
                                                      : "",
                      width_, height_, static_cast<double>(r.x), static_cast<double>(r.y),
                      static_cast<double>(r.w), static_cast<double>(r.h), color_.r, color_.g,
                      color_.b,
                      bars == BorderMode::kBlur    ? " under the border blur (not checked)"
                      : bars == BorderMode::kImage ? " under the border image (not checked)"
                                                   : "",
                      mismatches, mismatches != 0 ? ", " : "", first);
        *report = buf;
        if (color_drawn_ != ColorblindFilter::kOff) {
            *report += std::string(" (colour-blind filter ") +
                       colorblind_filter_name(color_drawn_) +
                       (color_path_ == ColorPath::kGpu ? " on the GPU, +-2)" : " on the CPU)");
        }
    }
    SDL_DestroySurface(surface);
    return mismatches;
}

bool Presenter::save_readback(const std::string &path) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr);
    if (surface == nullptr) {
        SDL_Log("present: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return false;
    }
    const bool ok = SDL_SavePNG(surface, path.c_str());
    if (!ok) {
        SDL_Log("present: cannot write %s: %s", path.c_str(), SDL_GetError());
    }
    SDL_DestroySurface(surface);
    return ok;
}

} // namespace maplethief::frontend

// --- window points ----------------------------------------------------------------------------

namespace maplethief::frontend {

void window_density(SDL_Window *window, float *density_x, float *density_y) {
    *density_x = 1;
    *density_y = 1;
    int w = 0;
    int h = 0;
    int pw = 0;
    int ph = 0;
    if (window == nullptr || !SDL_GetWindowSize(window, &w, &h) ||
        !SDL_GetWindowSizeInPixels(window, &pw, &ph) || w <= 0 || h <= 0 || pw <= 0 || ph <= 0) {
        return;
    }
    *density_x = static_cast<float>(pw) / static_cast<float>(w);
    *density_y = static_cast<float>(ph) / static_cast<float>(h);
}

Insets window_insets(SDL_Window *window) {
    int w = 0;
    int h = 0;
    SDL_Rect safe{};
    if (window == nullptr || !SDL_GetWindowSize(window, &w, &h) ||
        !SDL_GetWindowSafeArea(window, &safe) || w <= 0 || h <= 0 || safe.w <= 0 || safe.h <= 0) {
        return Insets{};
    }
    Insets in;
    in.left = static_cast<float>(std::max(0, safe.x));
    in.top = static_cast<float>(std::max(0, safe.y));
    in.right = static_cast<float>(std::max(0, w - safe.x - safe.w));
    in.bottom = static_cast<float>(std::max(0, h - safe.y - safe.h));
    return in;
}

ScreenPoint window_point_to_screen(SDL_Window *window, const Placement &placement, float x,
                                   float y) {
    float dx = 1;
    float dy = 1;
    window_density(window, &dx, &dy);
    return window_to_screen(placement, x, y, dx, dy);
}

void screen_to_window_point(SDL_Window *window, const Placement &placement, float sx, float sy,
                            float *x, float *y) {
    float dx = 1;
    float dy = 1;
    window_density(window, &dx, &dy);
    screen_to_window(placement, sx, sy, dx, dy, x, y);
}

} // namespace maplethief::frontend
