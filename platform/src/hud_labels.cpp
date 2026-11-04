// Button labels on the field's key pad (ADR 0101); see hud_labels.h.
#include "hud_labels.h"

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "maplethief/input.h"

namespace maplethief::frontend {

namespace {

// docs/controls/keypad.md, "Panel 0": each key's art (its view's frame), the action it sends in
// the field. The keys' groups are keypad_layout.h's.
constexpr HudKey kHudKeys[kHudKeyCount] = {
    {MT_ACTION_JUMP, {335, 277, 70, 34}, true, KeypadGroup::kActions, LabelSide::kAbove, false},
    {MT_ACTION_ATTACK, {405, 277, 70, 34}, true, KeypadGroup::kActions, LabelSide::kAbove, true},
    {MT_ACTION_SLOT1, {446, 224, 34, 31}, true, KeypadGroup::kSlots, LabelSide::kLeft, true},
    {MT_ACTION_SLOT2, {446, 178, 34, 31}, true, KeypadGroup::kSlots, LabelSide::kLeft, true},
    {MT_ACTION_SLOT3, {446, 132, 34, 31}, true, KeypadGroup::kSlots, LabelSide::kLeft, true},
    {MT_ACTION_SLOT4, {446, 86, 34, 31}, true, KeypadGroup::kSlots, LabelSide::kLeft, true},
    // Map ('*'), Bag (-16: the popup menu) and Cash (-26): where the game put them, always.
    {MT_ACTION_MAP, {0, 0, 52, 52}, false, KeypadGroup::kDpad, LabelSide::kBelow, false},
    {MT_ACTION_MENU, {383, 0, 50, 52}, false, KeypadGroup::kDpad, LabelSide::kBelow, false},
    {MT_ACTION_CASH_SHOP, {433, 0, 47, 52}, false, KeypadGroup::kDpad, LabelSide::kBelow, false},
};

constexpr float kScreenH = 320;

// The prompts' backing (prompts.cpp), one glyph point around the glyph.
constexpr Rgba kBacking = {0x10, 0x12, 0x16, 0xB0};

bool same_bindings(const std::vector<Binding> &a, const std::vector<Binding> &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const Binding &x, const Binding &y) {
               return x.control == y.control && x.field == y.field && x.menu == y.menu;
           });
}

enum class Side { kLeft, kRight, kAbove, kBelow };

Side side_of(LabelSide s) {
    switch (s) {
    case LabelSide::kLeft:
        return Side::kLeft;
    case LabelSide::kAbove:
        return Side::kAbove;
    case LabelSide::kBelow:
        return Side::kBelow;
    }
    return Side::kLeft;
}

Side opposite(Side s) {
    switch (s) {
    case Side::kLeft:
        return Side::kRight;
    case Side::kRight:
        return Side::kLeft;
    case Side::kAbove:
        return Side::kBelow;
    case Side::kBelow:
        return Side::kAbove;
    }
    return s;
}

KeypadRect beside(const KeypadRect &k, Side s, float w, float h) {
    const KeypadPoint c = k.centre();
    switch (s) {
    case Side::kLeft:
        return {k.x - kHudLabelGap - w, c.y - h / 2, w, h};
    case Side::kRight:
        return {k.x + k.w + kHudLabelGap, c.y - h / 2, w, h};
    case Side::kAbove:
        return {c.x - w / 2, k.y - kHudLabelGap - h, w, h};
    case Side::kBelow:
        return {c.x - w / 2, k.y + k.h + kHudLabelGap, w, h};
    }
    return {};
}

bool fits(const KeypadRect &r, float lo, float hi) {
    return r.x >= lo && r.x + r.w <= hi && r.y >= 0 && r.y + r.h <= kScreenH;
}

} // namespace

const HudKey *hud_keys() { return kHudKeys; }

KeypadRect hud_key_rect(const HudKey &k, const KeypadLayout &layout, const KeypadWide &wide) {
    if (!k.grouped) {
        return k.rect;
    }
    const KeypadTransform t = keypad_view_transform(k.group, layout[k.group], wide);
    const KeypadPoint a = t.apply({k.rect.x, k.rect.y});
    return {a.x, a.y, k.rect.w * t.scale, k.rect.h * t.scale};
}

std::vector<HudLabel> hud_labels(const KeypadLayout &layout, const KeypadWide &wide,
                                 float pad_opacity) {
    std::vector<HudLabel> out;
    out.reserve(kHudKeyCount);
    const float faded = std::clamp(pad_opacity, 0.0f, 1.0f);
    for (const HudKey &k : kHudKeys) {
        out.push_back(HudLabel{k.action, hud_key_rect(k, layout, wide), k.side,
                               k.gl_icon ? 1.0f : faded});
    }
    return out;
}

bool hud_label_control(const std::vector<Binding> &bindings, std::uint32_t action,
                       PromptDevice device, Control *out) {
    if (device == PromptDevice::kNone || action == MT_ACTION_NONE) {
        return false;
    }
    const bool keys = device == PromptDevice::kKeyboard;
    for (const Binding &b : bindings) {
        if (b.field == action && (b.control.kind == ControlKind::kKey) == keys) {
            *out = b.control;
            return true;
        }
    }
    return false;
}

KeypadRect hud_label_box(const HudLabel &label, float w, float h, float lo, float hi) {
    const Side s = side_of(label.side);
    KeypadRect r = beside(label.key, s, w, h);
    if (!fits(r, lo, hi)) {
        const KeypadRect o = beside(label.key, opposite(s), w, h);
        if (fits(o, lo, hi)) {
            r = o;
        }
    }
    r.x = std::clamp(r.x, lo, std::max(lo, hi - w));
    r.y = std::clamp(r.y, 0.0f, std::max(0.0f, kScreenH - h));
    return r;
}

int hud_label_unit(int pixels) {
    return std::max(1, static_cast<int>(std::lround(0.75 * std::max(pixels, 1))));
}

int draw_hud_labels(Canvas &canvas, int pixels, float lo, float hi,
                    const std::vector<HudLabel> &labels, const PromptStyle &style,
                    const std::vector<Binding> &bindings, std::vector<HudLabelDrawn> *drawn) {
    const int k = std::max(pixels, 1);
    const int u = canvas.scale();
    // Screen points a glyph point.
    const float g = static_cast<float>(u) / static_cast<float>(k);
    int n = 0;
    for (std::size_t i = 0; i < labels.size(); i++) {
        const HudLabel &l = labels[i];
        Control c;
        if (!(l.alpha > 0) || !hud_label_control(bindings, l.action, style.device, &c)) {
            continue;
        }
        // The glyph and its backing, one glyph point around it.
        const int wc = glyph_width(c, style.type, style.face) + 2;
        const int hc = kButtonHeight + 2;
        const KeypadRect box = hud_label_box(l, static_cast<float>(wc) * g,
                                             static_cast<float>(hc) * g, lo, hi);
        const int x = std::clamp(static_cast<int>(std::lround((box.x - lo) / g)), 0,
                                 std::max(0, canvas.width() - wc));
        const int y = std::clamp(static_cast<int>(std::lround(box.y / g)), 0,
                                 std::max(0, canvas.height() - hc));
        canvas.fill(x, y, wc, hc, kBacking);
        draw_glyph(canvas, x + 1, y + 1, c, style.type, style.face);
        if (drawn != nullptr) {
            drawn->push_back(HudLabelDrawn{
                i,
                KeypadRect{lo + static_cast<float>(x) * g, static_cast<float>(y) * g,
                           static_cast<float>(wc) * g, static_cast<float>(hc) * g},
                x * u, y * u, wc * u, hc * u});
        }
        n++;
    }
    return n;
}

void fade_hud_labels(std::uint8_t *rgba, int width, const std::vector<HudLabel> &labels,
                     const std::vector<HudLabelDrawn> &drawn) {
    for (const HudLabelDrawn &d : drawn) {
        const float alpha = d.label < labels.size() ? labels[d.label].alpha : 1.0f;
        if (alpha >= 1) {
            continue;
        }
        const int a = std::clamp(static_cast<int>(std::lround(alpha * 255)), 0, 255);
        for (int y = d.y; y < d.y + d.h; y++) {
            std::uint8_t *row = rgba + (static_cast<std::size_t>(y) * width + d.x) * 4;
            for (int i = 0; i < d.w * 4; i++) {
                row[i] = static_cast<std::uint8_t>((row[i] * a + 127) / 255);
            }
        }
    }
}

void hud_label_span(const Placement &placement, const Rect &wide, float *lo, float *hi) {
    const Rect &img = placement.rect;
    *lo = 0;
    *hi = static_cast<float>(kScreenWidth);
    if (!(img.w > 0) || !(wide.w > 0)) {
        return;
    }
    const float sp = img.w / static_cast<float>(kScreenWidth); // pixels a point
    // Rounded in, so that a label never reaches past the picture into the border.
    *lo = std::min(0.0f, std::ceil((wide.x - img.x) / sp - 1e-3f));
    *hi = std::max(static_cast<float>(kScreenWidth),
                   std::floor((wide.x + wide.w - img.x) / sp + 1e-3f));
}

void HudLabelLayer::stop() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    pixels_per_point_ = 0;
    tex_w_ = tex_h_ = 0;
    drawn_.clear();
    count_ = 0;
}

bool HudLabelLayer::draw(SDL_Renderer *renderer, const Placement &placement, const Rect &wide,
                         const std::vector<HudLabel> &labels, const PromptStyle &style,
                         const std::vector<Binding> &bindings) {
    const Rect &img = placement.rect;
    if (labels.empty() || style.device == PromptDevice::kNone || renderer == nullptr ||
        !(img.w > 0 && img.h > 0)) {
        return true;
    }
    // The layer's pixels a point: the placement's, rounded, so the glyphs are drawn at about the
    // size they are shown (crisp at an integer scale), as the prompts' layer.
    const float sp = img.w / static_cast<float>(kScreenWidth);
    const int k = std::clamp(static_cast<int>(std::lround(sp)), 1, 8);
    float lo = 0, hi = 0;
    hud_label_span(placement, wide, &lo, &hi);
    const int tw = static_cast<int>(std::lround(hi - lo)) * k;
    const int th = kScreenHeight * k;
    bool fresh = false;
    if (texture_ == nullptr || k != pixels_per_point_ || tw != tex_w_ || th != tex_h_) {
        stop();
        texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                     SDL_TEXTUREACCESS_STREAMING, tw, th);
        if (texture_ == nullptr) {
            if (!told_) {
                SDL_Log("hud labels: cannot make the labels' layer: %s", SDL_GetError());
                told_ = true;
            }
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
        pixels_per_point_ = k;
        tex_w_ = tw;
        tex_h_ = th;
        pixels_.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
        fresh = true;
    }
    if (fresh || lo != lo_ || hi != hi_ || !(labels == drawn_) || !(style == drawn_style_) ||
        !same_bindings(bindings, drawn_bindings_)) {
        std::fill(pixels_.begin(), pixels_.end(), 0);
        Canvas canvas(pixels_.data(), static_cast<std::uint32_t>(tw),
                      static_cast<std::uint32_t>(th), hud_label_unit(k));
        std::vector<HudLabelDrawn> placed;
        count_ = draw_hud_labels(canvas, k, lo, hi, labels, style, bindings, &placed);
        fade_hud_labels(pixels_.data(), tw, labels, placed);
        if (!SDL_UpdateTexture(texture_, nullptr, pixels_.data(), tw * 4)) {
            return false;
        }
        drawn_ = labels;
        drawn_style_ = style;
        drawn_bindings_ = bindings;
        lo_ = lo;
        hi_ = hi;
        redraws_++;
    }
    if (count_ == 0) {
        return true;
    }
    const SDL_FRect dst{img.x + lo * sp, img.y, (hi - lo) * sp, img.h};
    return SDL_RenderTexture(renderer, texture_, nullptr, &dst);
}

} // namespace maplethief::frontend
