// The input display (#404, ADR 0156); see input_display.h.
#include "input_display.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "maplethief/input.h"
#include "port_text.h"

namespace maplethief::frontend {

namespace {

static_assert(MT_ACTION_COUNT <= 32, "the lit cells are a 32-bit mask by action");

// The prompts' and the HUD labels' backing; a cell's face and frame, and lit.
constexpr Rgba kBacking = {0x10, 0x12, 0x16, 0xB0};
constexpr Rgba kCell = {0x30, 0x34, 0x3C, 0xC0};
constexpr Rgba kCellFrame = {0x70, 0x76, 0x80, 0xFF};
constexpr Rgba kCellText = {0xC0, 0xC4, 0xCC, 0xFF};
constexpr Rgba kLit = {0xF5, 0xC5, 0x42, 0xFF};
constexpr Rgba kLitText = {0x10, 0x12, 0x16, 0xFF};

// The 5x7 font's capitals and digits: 7 font pixels high.
constexpr int kTextRows = 7;
// The d-pad's arms, its hub between them.
constexpr int kArm = 9;
constexpr int kDpadX = 3, kDpadY = 3;

// The field between the Map button and the d-pad, in screen points (docs/controls/keypad.md).
constexpr float kFieldTop = 56;
constexpr float kFieldBottom = 172;

std::uint32_t bit(std::uint32_t action) { return 1u << action; }

} // namespace

const std::vector<InputDisplayCell> &input_display_cells() {
    static const std::vector<InputDisplayCell> cells = {
        {MT_ACTION_UP, kDpadX + kArm, kDpadY, kArm, kArm, true},
        {MT_ACTION_DOWN, kDpadX + kArm, kDpadY + 2 * kArm, kArm, kArm, true},
        {MT_ACTION_LEFT, kDpadX, kDpadY + kArm, kArm, kArm, true},
        {MT_ACTION_RIGHT, kDpadX + 2 * kArm, kDpadY + kArm, kArm, kArm, true},
        {MT_ACTION_JUMP, 33, 3, 28, 12, false},
        {MT_ACTION_ATTACK, 33, 18, 28, 12, false},
        {MT_ACTION_SLOT1, 64, 3, 16, 12, false},
        {MT_ACTION_SLOT2, 82, 3, 16, 12, false},
        {MT_ACTION_SLOT3, 64, 18, 16, 12, false},
        {MT_ACTION_SLOT4, 82, 18, 16, 12, false},
        {MT_ACTION_OK, 3, 33, 15, 11, false},
        {MT_ACTION_BACK, 20, 33, 27, 11, false},
        {MT_ACTION_MENU, 49, 33, 27, 11, false},
        {MT_ACTION_MAP, 78, 33, 21, 11, false},
    };
    return cells;
}

std::string input_display_label(std::uint32_t action) {
    switch (action) {
    case MT_ACTION_JUMP:
        return tr(Txt::input_jump);
    case MT_ACTION_ATTACK:
        return tr(Txt::input_attack);
    case MT_ACTION_SLOT1:
        return "1";
    case MT_ACTION_SLOT2:
        return "2";
    case MT_ACTION_SLOT3:
        return "3";
    case MT_ACTION_SLOT4:
        return "4";
    case MT_ACTION_OK:
        return tr(Txt::input_ok);
    case MT_ACTION_BACK:
        return tr(Txt::input_back);
    case MT_ACTION_MENU:
        return tr(Txt::input_menu);
    case MT_ACTION_MAP:
        return tr(Txt::input_map);
    default:
        return {};
    }
}

std::uint32_t input_display_lit(InputHeld held, std::uint32_t pad_keys) {
    if (held == nullptr) {
        held = mt_input_held;
    }
    std::uint32_t lit = 0;
    for (const InputDisplayCell &c : input_display_cells()) {
        if (held(c.action) != 0 || (pad_keys & bit(c.action)) != 0) {
            lit |= bit(c.action);
        }
    }
    return lit;
}

std::uint32_t input_display_lit_now() { return input_display_lit(nullptr, mt_input_pad_keys()); }

void draw_input_display(Canvas &canvas, int x, int y, std::uint32_t lit) {
    canvas.fill(x, y, kInputDisplayW, kInputDisplayH, kBacking);
    canvas.fill(x + kDpadX + kArm, y + kDpadY + kArm, kArm, kArm, kCell);
    for (const InputDisplayCell &c : input_display_cells()) {
        const bool on = (lit & bit(c.action)) != 0;
        canvas.fill(x + c.x, y + c.y, c.w, c.h, on ? kLit : kCell);
        canvas.frame(x + c.x, y + c.y, c.w, c.h, 1, on ? kLit : kCellFrame);
        if (c.dpad) {
            continue;
        }
        const std::string label = Canvas::fit_width(input_display_label(c.action), c.w - 2);
        canvas.text(x + c.x + (c.w - Canvas::text_width(label)) / 2,
                    y + c.y + (c.h - kTextRows) / 2, label, on ? kLitText : kCellText);
    }
}

std::vector<std::uint8_t> render_input_display(std::uint32_t lit, int unit) {
    const int u = std::max(unit, 1);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kInputDisplayW * u) *
                                     static_cast<std::size_t>(kInputDisplayH * u) * 4,
                                 0);
    Canvas canvas(px.data(), static_cast<std::uint32_t>(kInputDisplayW * u),
                  static_cast<std::uint32_t>(kInputDisplayH * u), u);
    draw_input_display(canvas, 0, 0, lit);
    return px;
}

InputDisplayPlace input_display_place(const Rect &picture, const Rect &safe, int scale) {
    if (!(picture.w > 0 && picture.h > 0)) {
        return {};
    }
    const int top = std::clamp(scale, 1, 8);
    const float safe_right = safe.x + safe.w, safe_bottom = safe.y + safe.h;
    const float left_bar = picture.x - safe.x;
    const float below_bar = safe_bottom - (picture.y + picture.h);
    // The border, at the frame's scale down to half of it: not so small that it can't be read.
    for (int s = top; s >= 1 && 2 * s >= top; s--) {
        const float w = static_cast<float>(kInputDisplayW * s);
        const float h = static_cast<float>(kInputDisplayH * s);
        const float m = static_cast<float>(kInputDisplayMargin * s);
        const bool left = left_bar >= w + 2 * m && safe.h >= h + 2 * m;
        const bool below = below_bar >= h + 2 * m && safe.w >= w + 2 * m;
        if (left || below) {
            return {{std::floor(safe.x + m), std::floor(safe_bottom - m - h), w, h}, s};
        }
    }
    // Over the picture: its left edge, between the Map button and the d-pad.
    const float pt = picture.h / static_cast<float>(kScreenHeight);
    const float room = (kFieldBottom - kFieldTop) * pt;
    int s = top;
    while (s > 1 && static_cast<float>((kInputDisplayH + kInputDisplayMargin) * s) > room) {
        s--;
    }
    const float m = static_cast<float>(kInputDisplayMargin * s);
    const float x = std::max(picture.x, safe.x) + m;
    const float w = std::min(static_cast<float>(kInputDisplayW * s), safe_right - m - x);
    return {{std::floor(x), std::floor(picture.y + kFieldTop * pt), std::max(w, 0.0f),
             static_cast<float>(kInputDisplayH * s)},
            s};
}

void InputDisplayLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    lit_ = 0;
    unit_ = 0;
}

bool InputDisplayLayer::draw(SDL_Renderer *renderer, const InputDisplayPlace &at,
                             std::uint32_t lit) {
    if (renderer == nullptr || !(at.rect.w > 0 && at.rect.h > 0)) {
        return true;
    }
    const int unit = std::max(at.scale, 1);
    if (texture_ == nullptr || unit != unit_) {
        stop();
        texture_ =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              kInputDisplayW * unit, kInputDisplayH * unit);
        if (texture_ == nullptr) {
            if (!told_) {
                SDL_Log("input display: cannot make the panel's layer: %s", SDL_GetError());
                told_ = true;
            }
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    }
    if (lit != lit_ || unit != unit_) {
        const std::vector<std::uint8_t> px = render_input_display(lit, unit);
        if (!SDL_UpdateTexture(texture_, nullptr, px.data(), kInputDisplayW * unit * 4)) {
            return false;
        }
        lit_ = lit;
        unit_ = unit;
        redraws_++;
    }
    // A panel cut by a narrow safe area shows its left part.
    const SDL_FRect src{0, 0, at.rect.w, at.rect.h};
    const SDL_FRect dst{at.rect.x, at.rect.y, at.rect.w, at.rect.h};
    draws_++;
    return SDL_RenderTexture(renderer, texture_, &src, &dst);
}

} // namespace maplethief::frontend
