// The hero's EXP as a percentage beside the field HUD's bar (#191, ADR 0109); see exp_percent.h.
#include "exp_percent.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "autosave.h"
#include "canvas.h"
#include "hud_labels.h"
#include "port_text.h"

namespace maplethief::frontend {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 kCanvasState = 0x0C; // SkyCanvas: the engine's current IState
constexpr i32 kFieldMode = 0;
constexpr i32 kMaxLevel = 255; // the table's key is a u8

// The prompts' and the HUD labels' backing, and the HUD's own EXP green (HUD_Paint's 0xB6DA00).
constexpr Rgba kBacking = {0x10, 0x12, 0x16, 0xB0};
constexpr Rgba kLabel = {0xB6, 0xDA, 0x00, 0xFF};
constexpr Rgba kFigure = {0xF0, 0xF2, 0xF5, 0xFF};

bool read_u32(const GuestSpan &mem, u32 addr, u32 *out) {
    const u8 *p = mem(addr, 4);
    if (p == nullptr) {
        return false;
    }
    std::memcpy(out, p, 4);
    return true;
}
bool read_i32(const GuestSpan &mem, u32 addr, i32 *out) {
    u32 v = 0;
    if (!read_u32(mem, addr, &v)) {
        return false;
    }
    *out = static_cast<i32>(v);
    return true;
}

// The EXP the level needs (CHero::ExpToLevelUp 0x65919): the first row of hero.dat whose u8
// key is the level (0x3990D), its i32 at +1 (0x385D1 reads table + 6 + row * stride + 1).
bool level_need(const GuestSpan &mem, i32 level, i32 *need) {
    u32 table = 0;
    if (!read_u32(mem, kExpHeroTable, &table) || table == 0) {
        return false;
    }
    const u8 *head = mem(table, 6);
    if (head == nullptr) {
        return false;
    }
    std::uint16_t rows = 0, stride = 0;
    std::memcpy(&rows, head + 2, 2);
    std::memcpy(&stride, head + 4, 2);
    if (rows == 0 || rows > kExpMaxTableRows || stride < kExpMinTableStride ||
        stride > kExpMaxTableStride) {
        return false;
    }
    const u8 *body = mem(table + 6, static_cast<u32>(rows) * stride);
    if (body == nullptr) {
        return false;
    }
    for (u32 r = 0; r < rows; r++) {
        const u8 *row = body + static_cast<std::size_t>(r) * stride;
        if (row[0] == level) {
            std::memcpy(need, row + 1, 4);
            return true;
        }
    }
    return false;
}

} // namespace

ExpView read_exp(const GuestSpan &mem) {
    ExpView v;
    u32 canvas = 0, state = 0, vptr = 0, hero = 0;
    i32 mode = -1, level = 0, exp = 0, need = 0;
    if (!read_u32(mem, kAutosaveCanvas, &canvas) || canvas == 0 ||
        !read_u32(mem, canvas + kCanvasState, &state) || state == 0 ||
        !read_u32(mem, state, &vptr) || vptr != kAutosaveGameVptr ||
        !read_i32(mem, kAutosaveGameMode, &mode) || mode != kFieldMode ||
        !read_u32(mem, state + kMinimapStateHero, &hero) || hero == 0 ||
        !read_i32(mem, hero + kExpHeroLevel, &level) || level < 1 || level > kMaxLevel ||
        !read_i32(mem, hero + kExpHeroExp, &exp) || !level_need(mem, level, &need)) {
        return v;
    }
    v.level = level;
    v.exp = exp;
    v.needed = need;
    v.shown = need > 0;
    return v;
}

std::string exp_percent_text(std::int32_t exp, std::int32_t needed) {
    if (needed <= 0) {
        return {};
    }
    const std::int64_t hundredths =
        std::clamp<std::int64_t>(std::int64_t{std::max(exp, 0)} * 10000 / needed, 0, 9999);
    char figure[32];
    std::snprintf(figure, sizeof figure, "%lld.%02lld%%", static_cast<long long>(hundredths / 100),
                  static_cast<long long>(hundredths % 100));
    // The label (port text, "EXP"), a space, the figure: the drawing splits at the last space.
    return std::string(tr(Txt::hud_exp)) + " " + figure;
}

int exp_percent_units_w(const std::string &text) { return Canvas::text_width(text) + 2; }

int exp_percent_pixels(const Rect &image) {
    const float sp = image.w / static_cast<float>(kScreenWidth);
    return std::clamp(static_cast<int>(std::lround(sp)), 1, 8);
}

Rect exp_percent_points(const std::string &text, int unit, int pixels) {
    // Screen points a glyph point.
    const float g = static_cast<float>(std::max(unit, 1)) / static_cast<float>(std::max(pixels, 1));
    const float w = static_cast<float>(exp_percent_units_w(text)) * g;
    const float h = static_cast<float>(kExpPercentUnitsH) * g;
    return Rect{kExpPercentRight - w, kExpPercentBottom - h, w, h};
}

Rect exp_percent_rect(const Rect &image, const std::string &text) {
    if (!(image.w > 0 && image.h > 0) || text.empty()) {
        return {};
    }
    const int k = exp_percent_pixels(image);
    const Rect pt = exp_percent_points(text, hud_label_unit(k), k);
    const float sx = image.w / static_cast<float>(kScreenWidth);
    const float sy = image.h / static_cast<float>(kScreenHeight);
    // Whole pixels: the right and bottom edges where the points put them, the size the glyphs'
    // pixels at the layer's scale times the picture's (equal at an integer scale).
    const float right = std::floor(image.x + (pt.x + pt.w) * sx);
    const float bottom = std::floor(image.y + (pt.y + pt.h) * sy);
    const float w = std::max(1.0f, std::round(pt.w * sx));
    const float h = std::max(1.0f, std::round(pt.h * sy));
    return Rect{right - w, bottom - h, w, h};
}

std::vector<std::uint8_t> render_exp_percent(const std::string &text, int unit) {
    const int u = std::max(unit, 1);
    const int w = exp_percent_units_w(text), h = kExpPercentUnitsH;
    std::vector<std::uint8_t> px(
        static_cast<std::size_t>(w * u) * static_cast<std::size_t>(h * u) * 4, 0);
    if (text.empty()) {
        return px;
    }
    Canvas canvas(px.data(), static_cast<std::uint32_t>(w * u), static_cast<std::uint32_t>(h * u),
                  u);
    canvas.fill(0, 0, w, h, kBacking);
    // "EXP" in the HUD's EXP green, the figure in white.
    const std::size_t split = text.rfind(' ');
    const std::string label = split == std::string::npos ? std::string{} : text.substr(0, split);
    const std::string figure = split == std::string::npos ? text : text.substr(split);
    int x = 1;
    if (!label.empty()) {
        x += canvas.text(x, 1, label, kLabel);
    }
    canvas.text(x, 1, figure, kFigure);
    return px;
}

void ExpPercentLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    text_.clear();
    unit_ = tex_w_ = tex_h_ = 0;
}

bool ExpPercentLayer::draw(SDL_Renderer *renderer, const Rect &image, const std::string &text) {
    if (renderer == nullptr || text.empty() || !(image.w > 0 && image.h > 0)) {
        return true;
    }
    const int unit = hud_label_unit(exp_percent_pixels(image));
    const int tw = exp_percent_units_w(text) * unit, th = kExpPercentUnitsH * unit;
    if (texture_ == nullptr || tw != tex_w_ || th != tex_h_) {
        stop();
        texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                     SDL_TEXTUREACCESS_STREAMING, tw, th);
        if (texture_ == nullptr) {
            if (!told_) {
                SDL_Log("exp percent: cannot make the figure's layer: %s", SDL_GetError());
                told_ = true;
            }
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
        tex_w_ = tw;
        tex_h_ = th;
    }
    if (text != text_ || unit != unit_) {
        const std::vector<std::uint8_t> px = render_exp_percent(text, unit);
        if (!SDL_UpdateTexture(texture_, nullptr, px.data(), tw * 4)) {
            return false;
        }
        text_ = text;
        unit_ = unit;
        redraws_++;
    }
    const Rect r = exp_percent_rect(image, text);
    const SDL_FRect dst{r.x, r.y, r.w, r.h};
    draws_++;
    return SDL_RenderTexture(renderer, texture_, nullptr, &dst);
}

} // namespace maplethief::frontend
