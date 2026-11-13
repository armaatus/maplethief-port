// The border (#157, ADR 0085); see border.h.
#include "border.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <utility>

// The pinned stb_image (ADR 0003), a private copy (static linkage) for the player's border art:
// PNG only, from memory, no larger than the limits allow.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace maplethief::frontend {

static_assert(kMaxBorderImageSide == STBI_MAX_DIMENSIONS);

BlurChain blur_chain(int source_w, int source_h) {
    BlurChain c;
    if (source_w <= 0 || source_h <= 0) {
        return c;
    }
    int w = source_w;
    int h = source_h;
    do {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        c.levels.push_back(BlurLevel{w, h});
    } while (w > kBlurBaseWidth && (w > 1 || h > 1));
    c.top = c.levels.size() - 1;
    for (std::size_t i = 0; i < c.levels.size(); i++) {
        if (c.levels[i].w <= kBlurTopWidth) {
            c.top = i;
            break;
        }
    }
    return c;
}

bool decode_border_image(const std::uint8_t *data, std::size_t size, BorderImage *out,
                         std::string *why) {
    auto fail = [why](std::string what) {
        if (why != nullptr) {
            *why = std::move(what);
        }
        return false;
    };
    if (data == nullptr || size == 0) {
        return fail("the file is empty");
    }
    if (size > kMaxBorderImageBytes) {
        return fail("the file is over " + std::to_string(kMaxBorderImageBytes >> 20) + " MiB");
    }
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < sizeof kSignature || std::memcmp(data, kSignature, sizeof kSignature) != 0) {
        return fail("not a PNG");
    }
    // The header first: nothing is decoded unless its size is within the limits.
    int w = 0;
    int h = 0;
    int comp = 0;
    const int len = static_cast<int>(size);
    if (stbi_info_from_memory(data, len, &w, &h, &comp) == 0) {
        return fail(std::string("not a PNG stb_image can read (") + stbi_failure_reason() + ")");
    }
    if (w <= 0 || h <= 0 || static_cast<std::uint32_t>(w) > kMaxBorderImageSide ||
        static_cast<std::uint32_t>(h) > kMaxBorderImageSide ||
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxBorderImagePixels) {
        return fail(std::to_string(w) + "x" + std::to_string(h) + " is larger than " +
                    std::to_string(kMaxBorderImageSide) + " a side or 7680x4320 in all");
    }
    stbi_uc *pixels = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (pixels == nullptr) {
        return fail(std::string("the PNG is broken (") + stbi_failure_reason() + ")");
    }
    out->width = static_cast<std::uint32_t>(w);
    out->height = static_cast<std::uint32_t>(h);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    out->rgba.assign(pixels, pixels + n);
    stbi_image_free(pixels);
    return true;
}

bool load_border_image(const std::string &path, BorderImage *out, std::string *why) {
    auto fail = [why](std::string what) {
        if (why != nullptr) {
            *why = std::move(what);
        }
        return false;
    };
    if (path.empty()) {
        return fail("border_image is empty");
    }
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (io == nullptr) {
        return fail(std::string("cannot open it (") + SDL_GetError() + ")");
    }
    const Sint64 size = SDL_GetIOSize(io);
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxBorderImageBytes) {
        SDL_CloseIO(io);
        return fail(size < 0 ? std::string("cannot tell its size (") + SDL_GetError() + ")"
                             : "the file is over " + std::to_string(kMaxBorderImageBytes >> 20) +
                                   " MiB");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const std::size_t got = bytes.empty() ? 0 : SDL_ReadIO(io, bytes.data(), bytes.size());
    SDL_CloseIO(io);
    if (got != bytes.size()) {
        return fail("cannot read it");
    }
    return decode_border_image(bytes.data(), bytes.size(), out, why);
}

// --- Border -----------------------------------------------------------------------------------

void Border::set(BorderMode mode, const std::string &image_path) {
    mode_ = mode;
    if (image_path != path_) {
        path_ = image_path;
        free_art();
        art_failed_ = false;
    }
    if (mode_ != BorderMode::kBlur) {
        free_chain(); // its textures are only for the blur
    }
}

void Border::free_chain() {
    for (SDL_Texture *t : levels_) {
        SDL_DestroyTexture(t);
    }
    levels_.clear();
    chain_ = {};
    chain_w_ = 0;
    chain_h_ = 0;
}

void Border::free_art() {
    if (art_ != nullptr) {
        SDL_DestroyTexture(art_);
        art_ = nullptr;
    }
    art_w_ = 0;
    art_h_ = 0;
}

void Border::release() {
    free_chain();
    free_art();
    renderer_ = nullptr;
    blur_failed_ = false; // a new renderer may have render targets
    drawn_ = BorderMode::kColor;
}

bool Border::ensure_chain(SDL_Renderer *renderer, int screen_w, int screen_h) {
    if (!levels_.empty() && chain_w_ == screen_w && chain_h_ == screen_h) {
        return true;
    }
    free_chain();
    if (blur_failed_) {
        return false;
    }
    BlurChain chain = blur_chain(screen_w, screen_h);
    for (const BlurLevel &l : chain.levels) {
        SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_TARGET, l.w, l.h);
        if (t == nullptr) {
            SDL_Log("present: border blur: no %dx%d render target (%s): the bars are the border "
                    "colour",
                    l.w, l.h, SDL_GetError());
            blur_failed_ = true;
            free_chain();
            return false;
        }
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        levels_.push_back(t);
    }
    chain_ = std::move(chain);
    chain_w_ = screen_w;
    chain_h_ = screen_h;
    if (!blur_logged_) {
        const BlurLevel &base = chain_.levels.back();
        const BlurLevel &top = chain_.levels[chain_.top];
        SDL_Log("present: border blur on the %s renderer: the %dx%d frame down to %dx%d in %zu "
                "halvings, up to %dx%d, stretched over the window at %u/255",
                SDL_GetRendererName(renderer), screen_w, screen_h, base.w, base.h,
                chain_.levels.size(), top.w, top.h, static_cast<unsigned>(kBlurDim));
        blur_logged_ = true;
    }
    return true;
}

bool Border::ensure_art(SDL_Renderer *renderer) {
    if (art_ != nullptr) {
        return true;
    }
    if (art_failed_) {
        return false;
    }
    art_failed_ = true; // until it is made
    BorderImage img;
    std::string why;
    if (!load_border_image(path_, &img, &why)) {
        SDL_Log("present: border image '%s': %s: the bars are the border colour", path_.c_str(),
                why.c_str());
        return false;
    }
    const Sint64 max = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                             SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
    if (max > 0 && (img.width > static_cast<std::uint64_t>(max) ||
                    img.height > static_cast<std::uint64_t>(max))) {
        SDL_Log("present: border image '%s': %ux%u is larger than the %s renderer's largest "
                "texture (%lld): the bars are the border colour",
                path_.c_str(), img.width, img.height, SDL_GetRendererName(renderer),
                static_cast<long long>(max));
        return false;
    }
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    SDL_Texture *t =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t == nullptr || !SDL_UpdateTexture(t, nullptr, img.rgba.data(), w * 4)) {
        SDL_Log("present: border image '%s': no %dx%d texture (%s): the bars are the border "
                "colour",
                path_.c_str(), w, h, SDL_GetError());
        SDL_DestroyTexture(t);
        return false;
    }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); // straight alpha, over the border colour
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    art_ = t;
    art_w_ = w;
    art_h_ = h;
    art_failed_ = false;
    SDL_Log("present: border image '%s': %dx%d, scaled to cover the window", path_.c_str(), w, h);
    return true;
}

bool Border::draw_blur(SDL_Renderer *renderer, SDL_Texture *screen, const Rect &image, int output_w,
                       int output_h) {
    SDL_Texture *before = SDL_GetRenderTarget(renderer);
    SDL_ScaleMode screen_mode = SDL_SCALEMODE_NEAREST;
    SDL_GetTextureScaleMode(screen, &screen_mode);
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_LINEAR);
    bool ok = true;
    // Down: each level the last one halved (the first from the screen texture).
    SDL_Texture *from = screen;
    for (std::size_t i = 0; ok && i < levels_.size(); i++) {
        ok = SDL_SetRenderTarget(renderer, levels_[i]) &&
             SDL_RenderTexture(renderer, from, nullptr, nullptr);
        from = levels_[i];
    }
    // Up: each level from the top's up to the base, the one below it doubled.
    for (std::size_t i = levels_.size() - 1; ok && i-- > chain_.top;) {
        ok = SDL_SetRenderTarget(renderer, levels_[i]) &&
             SDL_RenderTexture(renderer, levels_[i + 1], nullptr, nullptr);
    }
    const bool restored = SDL_SetRenderTarget(renderer, before);
    SDL_SetTextureScaleMode(screen, screen_mode);
    if (!ok || !restored) {
        SDL_Log("present: border blur failed: %s", SDL_GetError());
        return false;
    }
    const Rect c = cover_about(image, output_w, output_h);
    const SDL_FRect dst{c.x, c.y, c.w, c.h};
    SDL_Texture *top = levels_[chain_.top];
    SDL_SetTextureColorMod(top, kBlurDim, kBlurDim, kBlurDim);
    ok = SDL_RenderTexture(renderer, top, nullptr, &dst);
    SDL_SetTextureColorMod(top, 255, 255, 255); // it is a source of the next frame's chain
    if (!ok) {
        SDL_Log("present: border blur failed: %s", SDL_GetError());
    }
    return ok;
}

bool Border::draw(SDL_Renderer *renderer, SDL_Texture *screen, int screen_w, int screen_h,
                  const Rect &image, int output_w, int output_h) {
    drawn_ = BorderMode::kColor;
    if (mode_ == BorderMode::kColor || renderer == nullptr || screen == nullptr || image.w <= 0 ||
        image.h <= 0 || output_w <= 0 || output_h <= 0) {
        return true;
    }
    if (renderer != renderer_) {
        release();
        renderer_ = renderer;
    }
    if (mode_ == BorderMode::kBlur) {
        if (!ensure_chain(renderer, screen_w, screen_h)) {
            return true; // the border colour (logged)
        }
        if (!draw_blur(renderer, screen, image, output_w, output_h)) {
            return false;
        }
        drawn_ = BorderMode::kBlur;
        return true;
    }
    if (!ensure_art(renderer)) {
        return true; // the border colour (logged)
    }
    // Only the part of the art that covers the output, drawn over the output: a long, thin
    // image's cover rectangle can be a million pixels long, which SDL's software renderer
    // cannot blit (#413).
    const Rect c = cover_rect(output_w, output_h, art_w_, art_h_);
    if (c.w <= 0 || c.h <= 0) {
        drawn_ = BorderMode::kImage; // no output: nothing to cover
        return true;
    }
    const float sx = static_cast<float>(art_w_) / c.w;
    const float sy = static_cast<float>(art_h_) / c.h;
    const SDL_FRect src{-c.x * sx, -c.y * sy, static_cast<float>(output_w) * sx,
                        static_cast<float>(output_h) * sy};
    const SDL_FRect dst{0, 0, static_cast<float>(output_w), static_cast<float>(output_h)};
    if (!SDL_RenderTexture(renderer, art_, &src, &dst)) {
        SDL_Log("present: border image failed: %s", SDL_GetError());
        return false;
    }
    drawn_ = BorderMode::kImage;
    return true;
}

} // namespace maplethief::frontend
