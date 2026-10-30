// Text at the output resolution (#88, ADR 0066, maplethief/cg.h): text shadows of bitmap
// contexts over guest memory, and the mt_gfx replacement hook that hands them to the game's
// text textures.
//
// TextCache_DrawText (0x9031) makes a context over its 512x32 (or 32x32) buffer, clears the
// buffer, draws one line with -drawAtPoint:withFont:, releases the context and uploads the
// buffer with glTexImage2D. With a text scale k > 1 the draw goes into the context's shadow
// too: the same text, the CTM scaled by k, into k x k times the pixels. The upload hash is
// what ties the shadow to the 1x pixels. A shadow is only ever handed out for an upload whose
// bytes are the 1x pixels it was last drawn over. Anything the game did to the buffer in
// between (or a draw the shadow could not follow) changes the hash, so the upload is kept as
// it is.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "cg/cg_objects.h"
#include "maplethief/cg.h"
#include "maplethief/gfx.h"

namespace mt_cg {

namespace {

constexpr u32 k_gl_rgba = MT_GFX_RGBA;
constexpr u32 k_gl_unsigned_byte = MT_GFX_UNSIGNED_BYTE;
// TextCache_DrawText releases its context before it uploads; a few released shadows stay
// findable for the upload.
constexpr std::size_t k_text_shadows = 4;

struct HiRes {
    u32 k = 1;
    mt_cg_text_stats stats{};
    std::deque<std::shared_ptr<TextShadow>> recent; // the most recently drawn last
};

HiRes &hires() {
    static HiRes h;
    return h;
}

// The upload hash of a surface's pixels as a GL_RGBA / GL_UNSIGNED_BYTE upload.
std::uint64_t upload_hash(const Surface &s) {
    return mt_gfx_upload_hash(s.width, s.height, k_gl_rgba, k_gl_unsigned_byte, s.pixels);
}

// Rebuilds `t` as the k-times image of `s`: each pixel copied to k x k pixels.
bool rebuild(TextShadow &t, const Surface &s, u32 k) {
    const std::size_t kw = static_cast<std::size_t>(s.width) * k;
    try {
        t.pixels.resize(kw * s.height * k * 4u);
    } catch (const std::bad_alloc &) {
        return false;
    }
    t.k = k;
    t.width = s.width;
    t.height = s.height;
    for (u32 y = 0; y < s.height; y++) {
        const u8 *src = s.pixels + static_cast<std::size_t>(y) * s.bytes_per_row;
        u8 *row = t.pixels.data() + static_cast<std::size_t>(y) * k * kw * 4u;
        for (u32 x = 0; x < s.width; x++) {
            for (u32 i = 0; i < k; i++) {
                std::copy(src + x * 4u, src + x * 4u + 4u,
                          row + (static_cast<std::size_t>(x) * k + i) * 4u);
            }
        }
        for (u32 i = 1; i < k; i++) {
            std::copy(row, row + kw * 4u, row + i * kw * 4u);
        }
    }
    return true;
}

// Makes `t` the most recent shadow.
void remember(const std::shared_ptr<TextShadow> &t) {
    auto &recent = hires().recent;
    recent.erase(std::remove(recent.begin(), recent.end(), t), recent.end());
    recent.push_back(t);
    while (recent.size() > k_text_shadows) {
        recent.pop_front();
    }
}

} // namespace

void hires_forget() { hires().recent.clear(); }

void draw_context_text(mt_cpu *cpu, u32 ctx, Canvas &canvas, Surface &s, const std::u16string &text,
                       const Font &f, Point at, float *width, float *height) {
    const u32 k = hires().k;
    std::shared_ptr<TextShadow> *slot = k > 1 ? context_text_shadow(cpu, ctx) : nullptr;
    if (slot == nullptr || canvas.state().text_mode == k_text_invisible) {
        draw_text(canvas, s, text, f, at, width, height); // nothing drawn, or nothing to shadow
        return;
    }
    const bool layout_ok = s.format.bytes_per_pixel == 4 && s.bytes_per_row == s.width * 4u &&
                           static_cast<std::uint64_t>(s.width) * k <= MT_GFX_MAX_SIZE &&
                           static_cast<std::uint64_t>(s.height) * k <= MT_GFX_MAX_SIZE;
    if (!layout_ok || canvas.state().clip != nullptr) {
        ++hires().stats.unshadowed;
        log_once(cpu, "hires_text.unshadowed",
                 "hd text: a text draw into a %ux%u context (%u bytes per row%s) is drawn at 1x "
                 "only",
                 static_cast<unsigned>(s.width), static_cast<unsigned>(s.height),
                 static_cast<unsigned>(s.bytes_per_row),
                 canvas.state().clip != nullptr ? ", through a clip" : "");
        draw_text(canvas, s, text, f, at, width, height);
        return;
    }
    // The shadow is the 1x pixels' k-times image only if nothing drew there since its last
    // draw; otherwise it starts again from them.
    std::shared_ptr<TextShadow> &shadow = *slot;
    if (shadow == nullptr) {
        shadow = std::make_shared<TextShadow>();
    }
    if (shadow->k != k || shadow->width != s.width || shadow->height != s.height ||
        shadow->hash != upload_hash(s)) {
        ++hires().stats.rebuilds;
        if (!rebuild(*shadow, s, k)) {
            shadow.reset();
            ++hires().stats.unshadowed;
            draw_text(canvas, s, text, f, at, width, height);
            return;
        }
    }
    draw_text(canvas, s, text, f, at, width, height);
    Canvas big(s.width * k, s.height * k);
    big.state() = canvas.state();
    big.state().ctm = canvas.state().ctm.then(Affine::scale(k, k));
    Surface bs;
    bs.pixels = shadow->pixels.data();
    bs.width = s.width * k;
    bs.height = s.height * k;
    bs.bytes_per_row = bs.width * 4u;
    bs.format = s.format;
    float w2 = 0, h2 = 0;
    draw_text(big, bs, text, f, at, &w2, &h2);
    shadow->hash = upload_hash(s);
    ++hires().stats.shadowed_draws;
    remember(shadow);
}

} // namespace mt_cg

using namespace mt_cg;

extern "C" void mt_cg_set_text_scale(std::uint32_t k) noexcept {
    hires().k = std::clamp<std::uint32_t>(k, 1u, MT_GFX_MAX_SCALE);
    hires().stats = mt_cg_text_stats{};
    hires().recent.clear();
}

extern "C" std::uint32_t mt_cg_text_scale(void) noexcept { return hires().k; }

extern "C" int mt_cg_text_replace(void *, const mt_gfx_upload *upload,
                                  mt_gfx_replacement *out) noexcept {
    const u32 k = hires().k;
    if (k <= 1 || upload == nullptr || out == nullptr || upload->format != k_gl_rgba ||
        upload->type != k_gl_unsigned_byte) {
        return 0;
    }
    const auto &recent = hires().recent;
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        const TextShadow &t = **it;
        if (t.k == k && t.width == upload->width && t.height == upload->height &&
            t.hash == upload->hash) {
            out->pixels = t.pixels.data();
            out->width = t.width * k;
            out->height = t.height * k;
            ++hires().stats.replacements;
            return 1;
        }
    }
    return 0;
}

extern "C" mt_cg_text_stats mt_cg_get_text_stats(void) noexcept { return hires().stats; }
