// GPU presentation (#130, ADR 0068); see gpu_screen.h.
#include "gpu_screen.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "maplethief/gfx.h"
#include "maplethief/gfx_gpu.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

// UIKit's views, premultiplied (the compositor's pixels), "over" what is under them.
SDL_Texture *make_overlay(SDL_Renderer *renderer, std::uint32_t width, std::uint32_t height) {
    SDL_Texture *overlay =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                          static_cast<int>(width), static_cast<int>(height));
    if (overlay == nullptr) {
        SDL_Log("present: no %ux%u overlay texture: %s", width, height, SDL_GetError());
        return nullptr;
    }
    SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_NEAREST);
    if (!SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND_PREMULTIPLIED)) {
        SDL_Log("present: the overlay cannot blend premultiplied: %s", SDL_GetError());
        SDL_DestroyTexture(overlay);
        return nullptr;
    }
    return overlay;
}

} // namespace

bool GpuScreen::start(SDL_Renderer *renderer, SDL_Texture *screen, std::uint32_t width,
                      std::uint32_t height) {
    stop();
    SDL_GPUDevice *device = renderer != nullptr ? SDL_GetGPURendererDevice(renderer) : nullptr;
    if (device == nullptr || screen == nullptr || width == 0 || height == 0) {
        SDL_Log("present: GPU presentation needs SDL's GPU renderer and a screen texture");
        return false;
    }
    SDL_Texture *overlay = make_overlay(renderer, width, height);
    if (overlay == nullptr) {
        return false;
    }
    renderer_ = renderer;
    device_ = device;
    screen_ = screen;
    overlay_tex_ = overlay;
    width_ = width;
    height_ = height;
    scratch_.assign(static_cast<std::size_t>(width) * height * 4, 0);
    uploaded_.assign(scratch_.size(), 0);
    uploaded_valid_ = false;
    return true;
}

bool GpuScreen::resize(SDL_Texture *screen, std::uint32_t width, std::uint32_t height) {
    if (renderer_ == nullptr || screen == nullptr || width == 0 || height == 0) {
        return false;
    }
    SDL_Texture *overlay = make_overlay(renderer_, width, height);
    if (overlay == nullptr) {
        return false;
    }
    SDL_DestroyTexture(overlay_tex_);
    overlay_tex_ = overlay;
    screen_ = screen;
    width_ = width;
    height_ = height;
    scratch_.assign(static_cast<std::size_t>(width) * height * 4, 0);
    uploaded_.assign(scratch_.size(), 0);
    uploaded_valid_ = false;
    base_.clear();
    base_in_uploaded_ = false;
    composed_ = false;
    has_rect_ = false;
    return true;
}

void GpuScreen::stop() {
    if (gl_ != nullptr) {
        SDL_DestroyTexture(gl_);
        gl_ = nullptr;
    }
    if (overlay_tex_ != nullptr) {
        SDL_DestroyTexture(overlay_tex_);
        overlay_tex_ = nullptr;
    }
    gl_gpu_ = nullptr;
    screen_ = nullptr;
    device_ = nullptr;
    renderer_ = nullptr;
    have_gl_ = false;
    uploaded_valid_ = false;
    base_in_uploaded_ = false;
    composed_ = false;
    has_rect_ = false;
}

bool GpuScreen::ensure_gl_texture(std::uint32_t w, std::uint32_t h) {
    if (gl_ != nullptr && gl_width_ == w && gl_height_ == h) {
        return true;
    }
    if (gl_ != nullptr) {
        SDL_DestroyTexture(gl_);
        gl_ = nullptr;
        gl_gpu_ = nullptr;
    }
    have_gl_ = false;
    // ABGR8888 is R8G8B8A8_UNORM on SDL's GPU renderer: mt_gfx's target format, so the frame is
    // copied texel for texel.
    gl_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                            static_cast<int>(w), static_cast<int>(h));
    if (gl_ == nullptr) {
        SDL_Log("present: no %ux%u texture for the GL frame: %s", w, h, SDL_GetError());
        return false;
    }
    gl_gpu_ = static_cast<SDL_GPUTexture *>(SDL_GetPointerProperty(
        SDL_GetTextureProperties(gl_), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    SDL_SetTextureScaleMode(gl_, SDL_SCALEMODE_NEAREST);
    // The opaque CAEAGLLayer: the frame's colour replaces what is under it, its alpha (whatever
    // the blending left) is ignored, so the screen's alpha stays the clear's 255.
    const SDL_BlendMode colour_only = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    if (gl_gpu_ == nullptr || !SDL_SetTextureBlendMode(gl_, colour_only)) {
        SDL_Log("present: the GL frame's texture is not usable on the GPU: %s", SDL_GetError());
        SDL_DestroyTexture(gl_);
        gl_ = nullptr;
        gl_gpu_ = nullptr;
        return false;
    }
    gl_width_ = w;
    gl_height_ = h;
    return true;
}

bool GpuScreen::take_gl_frame(const mt_gles_frame &frame) {
    const mt_gfx_target *target = frame.target;
    if (renderer_ == nullptr || target == nullptr) {
        return false;
    }
    SDL_GPUTexture *src = mt_gfx_gpu_target_texture(target);
    const std::uint32_t w = mt_gfx_target_pixel_width(target);
    const std::uint32_t h = mt_gfx_target_pixel_height(target);
    if (src == nullptr || !ensure_gl_texture(w, h)) {
        return false;
    }
    // A copy of its own (as Screen::take_gl_frame copies the pixels): the game's next draws go
    // into the same target. Submitted after mt_gfx_gpu_target_texture's flush and before the
    // renderer's command buffer (at SDL_RenderPresent), so it runs between the two.
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        SDL_Log("present: no command buffer for the GL frame: %s", SDL_GetError());
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation from{};
    from.texture = src;
    SDL_GPUTextureLocation to{};
    to.texture = gl_gpu_;
    // cycle: a frame the renderer still reads from keeps its memory.
    SDL_CopyGPUTextureToTexture(copy, &from, &to, w, h, 1, true);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        SDL_Log("present: the GL frame's copy failed: %s", SDL_GetError());
        return false;
    }
    have_gl_ = true;
    gl_index_ = frame.index;
    stats_.frames_taken++;
    return true;
}

bool GpuScreen::compose_uikit(mt_cpu *cpu, double now) {
    if (renderer_ == nullptr ||
        mt_uikit_composite(cpu, now, MT_UIKIT_COMPOSITE_OVERLAY, nullptr, scratch_.data(), width_,
                           height_, width_ * 4) != 0) {
        return false;
    }
    float r[4] = {};
    if (mt_uikit_gl_rect(r, nullptr, nullptr) != 1) {
        return compose(scratch_.data(), width_ * 4, nullptr);
    }
    // Screen points to the screen texture's pixels.
    const float sx = static_cast<float>(width_) / static_cast<float>(MT_UIKIT_SCREEN_WIDTH);
    const float sy = static_cast<float>(height_) / static_cast<float>(MT_UIKIT_SCREEN_HEIGHT);
    const Rect rect{r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy};
    return compose(scratch_.data(), width_ * 4, &rect);
}

bool GpuScreen::compose(const std::uint8_t *overlay, std::uint32_t stride, const Rect *gl_rect) {
    if (renderer_ == nullptr || overlay == nullptr || stride < width_ * 4) {
        return false;
    }
    if (!upload_overlay(overlay, stride)) {
        return false;
    }
    // compose_over's base: this overlay (now in uploaded_) and this rectangle.
    base_in_uploaded_ = true;
    composed_ = true;
    has_rect_ = gl_rect != nullptr;
    rect_ = has_rect_ ? *gl_rect : Rect{};
    return draw_layers(gl_rect);
}

bool GpuScreen::compose_over(const std::function<void(std::uint8_t *rgba)> &draw) {
    if (renderer_ == nullptr || !composed_) {
        return false;
    }
    if (base_in_uploaded_) {
        // Keep UIKit's overlay as the base; uploaded_ then no longer says what the texture has.
        std::swap(base_, uploaded_);
        base_in_uploaded_ = false;
        uploaded_valid_ = false;
    }
    scratch_.assign(base_.begin(), base_.end());
    draw(scratch_.data());
    return upload_overlay(scratch_.data(), width_ * 4) && draw_layers(has_rect_ ? &rect_ : nullptr);
}

bool GpuScreen::upload_overlay(const std::uint8_t *overlay, std::uint32_t stride) {
    const std::size_t row = static_cast<std::size_t>(width_) * 4;
    uploaded_.resize(row * height_);
    bool same = uploaded_valid_;
    for (std::uint32_t y = 0; same && y < height_; y++) {
        same = std::memcmp(overlay + static_cast<std::size_t>(y) * stride,
                           uploaded_.data() + y * row, row) == 0;
    }
    if (!same) {
        if (!SDL_UpdateTexture(overlay_tex_, nullptr, overlay, static_cast<int>(stride))) {
            SDL_Log("present: the overlay's upload failed: %s", SDL_GetError());
            return false;
        }
        if (overlay == scratch_.data() && stride == row) {
            std::swap(scratch_, uploaded_); // the next composite overwrites every pixel
        } else {
            for (std::uint32_t y = 0; y < height_; y++) {
                std::memcpy(uploaded_.data() + y * row,
                            overlay + static_cast<std::size_t>(y) * stride, row);
            }
        }
        uploaded_valid_ = true;
        stats_.overlay_uploads++;
    }
    return true;
}

bool GpuScreen::draw_layers(const Rect *gl_rect) {
    SDL_Texture *before = SDL_GetRenderTarget(renderer_);
    if (!SDL_SetRenderTarget(renderer_, screen_)) {
        SDL_Log("present: cannot draw into the screen texture: %s", SDL_GetError());
        return false;
    }
    // Opaque black, as the FULL composite starts (and as the GL view shows without a frame).
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    bool ok = SDL_RenderClear(renderer_);
    if (ok && have_gl_ && gl_rect != nullptr && gl_rect->w > 0 && gl_rect->h > 0) {
        const SDL_FRect dst{gl_rect->x, gl_rect->y, gl_rect->w, gl_rect->h};
        ok = SDL_RenderTexture(renderer_, gl_, nullptr, &dst);
    }
    ok = ok && SDL_RenderTexture(renderer_, overlay_tex_, nullptr, nullptr);
    if (!ok) {
        SDL_Log("present: composing the screen texture failed: %s", SDL_GetError());
    }
    SDL_SetRenderTarget(renderer_, before);
    stats_.composes += ok ? 1 : 0;
    return ok;
}

bool GpuScreen::read_gl(std::vector<std::uint8_t> *rgba) {
    if (renderer_ == nullptr || !have_gl_) {
        SDL_Log("present: no GL frame to read back");
        return false;
    }
    const std::uint32_t bytes = gl_width_ * gl_height_ * 4;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device_, &info);
    SDL_GPUCommandBuffer *cmd = tb != nullptr ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if (cmd == nullptr) {
        SDL_Log("present: cannot read the GL frame back: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = gl_gpu_;
    region.w = gl_width_;
    region.h = gl_height_;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    dst.pixels_per_row = gl_width_;
    dst.rows_per_layer = gl_height_;
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    bool ok = fence != nullptr && SDL_WaitForGPUFences(device_, true, &fence, 1);
    if (fence != nullptr) {
        SDL_ReleaseGPUFence(device_, fence);
    }
    const void *mem = ok ? SDL_MapGPUTransferBuffer(device_, tb, false) : nullptr;
    if (mem != nullptr) {
        rgba->resize(bytes);
        std::memcpy(rgba->data(), mem, bytes);
        SDL_UnmapGPUTransferBuffer(device_, tb);
        for (std::size_t i = 3; i < rgba->size(); i += 4) {
            (*rgba)[i] = 255; // the opaque layer, as Screen::take_gl_frame
        }
        stats_.downloads++;
        stats_.download_bytes += bytes;
    } else {
        SDL_Log("present: the GL frame's read-back failed: %s", SDL_GetError());
        ok = false;
    }
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    return ok;
}

bool GpuScreen::read_screen(std::vector<std::uint8_t> *rgba) {
    if (renderer_ == nullptr) {
        return false;
    }
    SDL_Texture *before = SDL_GetRenderTarget(renderer_);
    SDL_Surface *read = SDL_SetRenderTarget(renderer_, screen_)
                            ? SDL_RenderReadPixels(renderer_, nullptr)
                            : nullptr;
    SDL_SetRenderTarget(renderer_, before);
    SDL_Surface *s = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    SDL_DestroySurface(read);
    if (s == nullptr || static_cast<std::uint32_t>(s->w) != width_ ||
        static_cast<std::uint32_t>(s->h) != height_) {
        SDL_Log("present: the screen texture's read-back failed: %s", SDL_GetError());
        SDL_DestroySurface(s);
        return false;
    }
    const std::size_t row = static_cast<std::size_t>(width_) * 4;
    rgba->resize(row * height_);
    for (std::uint32_t y = 0; y < height_; y++) {
        std::memcpy(rgba->data() + y * row,
                    static_cast<const std::uint8_t *>(s->pixels) +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(s->pitch),
                    row);
    }
    SDL_DestroySurface(s);
    stats_.downloads++;
    stats_.download_bytes += row * height_;
    return true;
}

ScreenDiff diff_screens(const std::uint8_t *a, const std::uint8_t *b, std::uint32_t width,
                        std::uint32_t height) {
    ScreenDiff d;
    d.pixels = static_cast<std::uint64_t>(width) * height;
    for (std::uint64_t i = 0; i < d.pixels; i++) {
        int delta = 0;
        for (int c = 0; c < 3; c++) {
            delta = std::max(delta, std::abs(static_cast<int>(a[i * 4 + c]) - b[i * 4 + c]));
        }
        d.max_delta = std::max(d.max_delta, delta);
        d.beyond += delta > ScreenDiff::kChannel ? 1 : 0;
    }
    return d;
}

} // namespace maplethief::frontend
