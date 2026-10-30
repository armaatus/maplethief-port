// Presentation (#58, ADR 0042); see present.h.
#include "present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "focus.h"
#include "maplethief/gfx.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

// --- Screen -----------------------------------------------------------------------------------

Screen::Screen(std::uint32_t render_scale) {
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
    gl_width_ = mt_gfx_target_pixel_width(target);
    gl_height_ = mt_gfx_target_pixel_height(target);
    const std::size_t n = static_cast<std::size_t>(gl_width_) * gl_height_;
    gl_.resize(n * 4);
    const std::uint8_t *src = mt_gfx_target_cpixels(target);
    // The CAEAGLLayer is opaque: the device shows the renderbuffer's colour and ignores its
    // alpha (which starts at 0 and is whatever the blending left).
    std::memcpy(gl_.data(), src, n * 4);
    for (std::size_t i = 0; i < n; i++) {
        gl_[i * 4 + 3] = 255;
    }
    gl_index_ = frame.index;
    if (first_lit_gl_ == 0 && !is_black(gl_.data(), gl_width_, gl_height_, gl_width_ * 4)) {
        first_lit_gl_ = frame.index;
    }
    return true;
}

bool Screen::compose(mt_cpu *cpu, double now) {
    mt_uikit_gl_frame gl{};
    const bool have_gl = !gl_.empty();
    if (have_gl) {
        gl.pixels = gl_.data();
        gl.width = gl_width_;
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

bool Presenter::start(SDL_Renderer *renderer, std::uint32_t image_w, std::uint32_t image_h) {
    stop();
    renderer_ = renderer;
    width_ = image_w;
    height_ = image_h;
    // ABGR8888 is R, G, B, A in memory: mt_gfx's and the compositor's byte order. An opaque
    // copy, nearest scaling.
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
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
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    renderer_ = nullptr;
}

void Presenter::set_scaling(ScaleMode mode, Color border) {
    mode_ = mode;
    border_ = border;
}

bool Presenter::upload(const std::uint8_t *rgba, std::uint32_t stride) {
    if (texture_ == nullptr) {
        return false;
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
    placement_ = place(mode_, ow, oh, static_cast<int>(width_), static_cast<int>(height_));
    SDL_SetTextureScaleMode(texture_, placement_.filter == Filter::kLinear ? SDL_SCALEMODE_LINEAR
                                      : placement_.filter == Filter::kPixelArt
                                          ? SDL_SCALEMODE_PIXELART
                                          : SDL_SCALEMODE_NEAREST);
    SDL_SetRenderDrawColor(renderer_, border_.r, border_.g, border_.b, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_);
    if (placement_.rect.w <= 0 || placement_.rect.h <= 0) {
        return true; // a minimised window: nothing to draw into
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
    const bool drawn = SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    if (state != nullptr) {
        SDL_SetGPURenderState(renderer_, nullptr);
    }
    if (!drawn) {
        SDL_Log("present: SDL_RenderTexture failed: %s", SDL_GetError());
        return false;
    }
    return true;
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
                        ok = got[0] == e[0] && got[1] == e[1] && got[2] == e[2];
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
                    ok = ok && got[c] + 2 >= lo[c] && got[c] <= hi[c] + 2;
                }
            }
            if (!ok) {
                const std::uint8_t *e = texel((tx0 + tx1) / 2, (ty0 + ty1) / 2);
                note(static_cast<int>(ox), static_cast<int>(oy),
                     exact ? "image" : "image (blended)", got, e[0], e[1], e[2]);
            }
        }
    }
    // The bars: every output pixel outside the placement has the border colour.
    const int x0 = static_cast<int>(r.x);
    const int x1 = static_cast<int>(r.x + r.w);
    for (int oy = 0; oy < sh; oy++) {
        const float fy = static_cast<float>(oy);
        const bool row_in = r.w > 0 && fy >= r.y && fy < r.y + r.h;
        for (int ox = 0; ox < sw; ox++) {
            if (row_in && ox >= x0 && ox < x1) {
                ox = x1 - 1; // the image's part of the row
                continue;
            }
            const std::uint8_t *got = pixel(ox, oy);
            if (got[0] != border_.r || got[1] != border_.g || got[2] != border_.b) {
                note(ox, oy, "bar", got, border_.r, border_.g, border_.b);
            }
        }
    }
    if (report != nullptr) {
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "%dx%d output, %s%s: the %ux%u image at (%g, %g, %g, %g), bars "
                      "#%02X%02X%02X: %ld mismatched%s%s",
                      sw, sh, scale_mode_name(placement_.mode),
                      check == PassCheck::kBlend      ? " (a pass: blended)"
                      : check == PassCheck::kBarsOnly ? " (a pass: only the bars checked)"
                                                      : "",
                      width_, height_, static_cast<double>(r.x), static_cast<double>(r.y),
                      static_cast<double>(r.w), static_cast<double>(r.h), border_.r, border_.g,
                      border_.b, mismatches, mismatches != 0 ? ", " : "", first);
        *report = buf;
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
