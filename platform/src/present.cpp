// Presentation (#58, ADR 0042); see present.h.
#include "present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "maplethief/gfx.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

Rect fit_rect(int output_w, int output_h, int image_w, int image_h) {
    if (output_w <= 0 || output_h <= 0 || image_w <= 0 || image_h <= 0) {
        return {};
    }
    // Compare output_w / output_h with image_w / image_h in integers: the image fills the width
    // if the output is relatively taller, else the height.
    const long long ow = output_w, oh = output_h, iw = image_w, ih = image_h;
    Rect r;
    if (ow * ih <= oh * iw) {
        r.w = static_cast<float>(ow);
        r.h = static_cast<float>(static_cast<double>(ow) * static_cast<double>(ih) /
                                 static_cast<double>(iw));
    } else {
        r.h = static_cast<float>(oh);
        r.w = static_cast<float>(static_cast<double>(oh) * static_cast<double>(iw) /
                                 static_cast<double>(ih));
    }
    // Whole pixels for the bars, so the image's edges do not straddle a pixel.
    r.w = std::round(r.w);
    r.h = std::round(r.h);
    r.x = std::floor((static_cast<float>(ow) - r.w) / 2);
    r.y = std::floor((static_cast<float>(oh) - r.h) / 2);
    return r;
}

Placement present_rect(int output_w, int output_h, int image_w, int image_h) {
    Placement p;
    p.rect = fit_rect(output_w, output_h, image_w, image_h);
    p.nearest = true;
    return p;
}

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
    placement_ = present_rect(ow, oh, static_cast<int>(width_), static_cast<int>(height_));
    SDL_SetTextureScaleMode(texture_,
                            placement_.nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_);
    if (placement_.rect.w <= 0 || placement_.rect.h <= 0) {
        return true; // a minimised window: nothing to draw into
    }
    const SDL_FRect dst{placement_.rect.x, placement_.rect.y, placement_.rect.w, placement_.rect.h};
    if (!SDL_RenderTexture(renderer_, texture_, nullptr, &dst)) {
        SDL_Log("present: SDL_RenderTexture failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

long Presenter::check_readback(const std::uint8_t *rgba, std::uint32_t stride,
                               std::string *report) {
    SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr);
    if (surface == nullptr) {
        if (report != nullptr) {
            *report = std::string("SDL_RenderReadPixels failed: ") + SDL_GetError();
        }
        return -1;
    }
    const Rect &r = placement_.rect;
    long mismatches = 0;
    char first[160] = "";
    auto note = [&](int ox, int oy, const char *what, unsigned er, unsigned eg, unsigned eb,
                    unsigned gr, unsigned gg, unsigned gb) {
        if (mismatches++ == 0) {
            std::snprintf(first, sizeof first,
                          "first at output (%d, %d), %s: #%02X%02X%02X, expected #%02X%02X%02X", ox,
                          oy, what, gr, gg, gb, er, eg, eb);
        }
    };
    // The image: the output pixel under each image pixel's centre.
    for (std::uint32_t y = 0; y < height_ && r.h > 0; y++) {
        const int oy = static_cast<int>(std::floor(r.y + (y + 0.5) * r.h / height_));
        for (std::uint32_t x = 0; x < width_ && r.w > 0; x++) {
            const int ox = static_cast<int>(std::floor(r.x + (x + 0.5) * r.w / width_));
            Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
            const std::uint8_t *e = rgba + static_cast<std::size_t>(y) * stride + x * 4;
            if (!SDL_ReadSurfacePixel(surface, ox, oy, &cr, &cg, &cb, &ca) || cr != e[0] ||
                cg != e[1] || cb != e[2]) {
                note(ox, oy, "image", e[0], e[1], e[2], cr, cg, cb);
            }
        }
    }
    // The bars: black. Every output pixel outside the placement, on a 4-pixel grid.
    for (int oy = 0; oy < surface->h; oy += 4) {
        for (int ox = 0; ox < surface->w; ox += 4) {
            const float fx = static_cast<float>(ox), fy = static_cast<float>(oy);
            if (fx >= r.x && fx < r.x + r.w && fy >= r.y && fy < r.y + r.h) {
                continue;
            }
            Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
            if (!SDL_ReadSurfacePixel(surface, ox, oy, &cr, &cg, &cb, &ca) || cr != 0 || cg != 0 ||
                cb != 0) {
                note(ox, oy, "bar", 0, 0, 0, cr, cg, cb);
            }
        }
    }
    if (report != nullptr) {
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "%dx%d output, the %ux%u image at (%g, %g, %g, %g): %ld mismatched%s%s",
                      surface->w, surface->h, width_, height_, static_cast<double>(r.x),
                      static_cast<double>(r.y), static_cast<double>(r.w), static_cast<double>(r.h),
                      mismatches, mismatches != 0 ? ", " : "", first);
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
