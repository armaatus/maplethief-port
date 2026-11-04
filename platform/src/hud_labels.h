// Button labels on the field's key pad (ADR 0101): with a gamepad or the keyboard in use, each
// key of the always-visible game pad (jump, attack, the four quick slots, Map, Bag and Cash)
// shows the glyph of the control that presses it in the field, so a player sees which button
// drinks the potion on slot 1 or casts the skill on slot 3.
//
//   which keys    kHudKeys (hud_labels.cpp): the keys' rects of docs/controls/keypad.md's
//                 "Panel 0" table and the field action each sends (MT_ACTION_JUMP, _ATTACK,
//                 _SLOT1-4, _MAP, _MENU, _CASH_SHOP). The d-pad gets none (its arrows are the
//                 directions on every device).
//   which control the device used last (prompts.h's DeviceTracker) and, for a gamepad, its glyph
//                 set and face-button mode (glyphs.h): of the bindings that give the action in
//                 the field (Actions::bindings(), so a remap shows at once), the device's first.
//                 Touch only (the device none): no labels.
//   where         beside the key as it is drawn now: its group's custom layout (keypad_layout.h)
//                 and widescreen move (edges or narrow, installed_keypad_wide()) applied to its
//                 rect, then the label on the key's side toward the screen's middle (the quick
//                 slots' left, above jump and attack, below Map, Bag and Cash), on the other side
//                 where that leaves the picture, and kept inside it. Labels never cover the
//                 game's own art on a key (the quick slots' items and counts, the attack key's
//                 weapon).
//   how big       drawn on its own layer in the window at the window's pixels, about three
//                 quarters of a prompt's size (12 points high at one pixel a point: never
//                 smaller than the 5x7 font needs), on the prompts' dark backing.
//   fading        the keys whose only picture is a UIKit view (jump, Map, Bag, Cash) fade with
//                 the pad (hide_keypad, keypad_hide.h); the attack key and the quick slots keep
//                 their labels, as the game keeps drawing their icons in GL.
//
// Drawn by the port into the window only, like the button prompts (prompts.h): never in --shot,
// --capture or the device goldens. Only on the field's panel 0 and not over the port's overlays.
// The setting button_labels (on by default, in every preset) switches them.
//
// The pure part (the table, the labels' places, the drawing with Canvas) has no runtime; the
// layer (HudLabelLayer) needs an SDL renderer.
#ifndef MAPLETHIEF_PLATFORM_HUD_LABELS_H
#define MAPLETHIEF_PLATFORM_HUD_LABELS_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "keypad_layout.h"
#include "prompts.h"
#include "scaling.h"

namespace maplethief::frontend {

// The side of its key a label goes on.
enum class LabelSide { kLeft, kAbove, kBelow };

// A key of the game pad that gets a label: its field action, its rect in the original layout
// (screen points), its group if a custom layout moves it (Map, Bag and Cash stay), the side,
// and whether the game draws its icon in GL (it shows with the pad hidden).
struct HudKey {
    std::uint32_t action;
    KeypadRect rect;
    bool grouped;
    KeypadGroup group;
    LabelSide side;
    bool gl_icon;
};
inline constexpr std::size_t kHudKeyCount = 9;
// Jump, attack, quick slots 1-4, Map, Bag, Cash.
const HudKey *hud_keys();

// A label to draw: the action, its key's rect where it is drawn now (screen-image points: x < 0
// and x >= 480 are the widescreen wings), the side, and its opacity (0-1).
struct HudLabel {
    std::uint32_t action = MT_ACTION_NONE;
    KeypadRect key;
    LabelSide side = LabelSide::kLeft;
    float alpha = 1;
    bool operator==(const HudLabel &o) const {
        return action == o.action && key.x == o.key.x && key.y == o.key.y && key.w == o.key.w &&
               key.h == o.key.h && side == o.side && alpha == o.alpha;
    }
};

// Where key `k` is drawn with `layout` and widescreen's move `wide` (keypad_view_transform for a
// grouped key, its own rect for Map, Bag and Cash).
KeypadRect hud_key_rect(const HudKey &k, const KeypadLayout &layout, const KeypadWide &wide);
// The labels of every key; `pad_opacity` (the pad's host opacity, 0-1: KeypadHider::opacity) is
// the alpha of the keys without a GL icon, the others' is 1 (the game draws their icons in GL
// whatever the pad's or the group's opacity).
std::vector<HudLabel> hud_labels(const KeypadLayout &layout, const KeypadWide &wide,
                                 float pad_opacity);

// The control whose glyph a label shows: of the bindings that give `action` in the field, the
// first of `device`'s (keys for the keyboard; the d-pad, buttons and triggers for a gamepad).
// False when none is bound (or the device is none).
bool hud_label_control(const std::vector<Binding> &bindings, std::uint32_t action,
                       PromptDevice device, Control *out);

// The label's box, in screen points, for a glyph w x h points: kHudLabelGap points beside the
// key on its side (centred on it), on the opposite side where that leaves [lo, hi] x [0, 320],
// then moved inside it.
inline constexpr float kHudLabelGap = 1;
KeypadRect hud_label_box(const HudLabel &label, float w, float h, float lo, float hi);

// The glyphs' size: `pixels` pixels a screen point in the window (at least 1) -> pixels a glyph
// point (the canvas's scale): three quarters of it, rounded, at least 1.
int hud_label_unit(int pixels);

// A label as drawn: its index in the labels, its backing in screen points (`box`) and in the
// canvas's pixels (x, y, w, h).
struct HudLabelDrawn {
    std::size_t label = 0;
    KeypadRect box;
    int x = 0, y = 0, w = 0, h = 0;
};
// Draws the labels with `canvas`, whose scale is the glyph unit (hud_label_unit(pixels)) and
// whose x = 0 is screen point `lo` (the canvas spans [lo, hi] at `pixels` pixels a point; its
// pixels transparent or an image). Each label whose action has a control for the style's device
// and whose alpha is not 0 is drawn opaque on the prompts' backing (HudLabelLayer fades it).
// Returns how many it drew; `drawn` (if given) gets each one.
int draw_hud_labels(Canvas &canvas, int pixels, float lo, float hi,
                    const std::vector<HudLabel> &labels, const PromptStyle &style,
                    const std::vector<Binding> &bindings,
                    std::vector<HudLabelDrawn> *drawn = nullptr);
// Fades each drawn label below alpha 1 in a transparent premultiplied RGBA layer (`width`
// pixels a row): its pixels times its alpha.
void fade_hud_labels(std::uint8_t *rgba, int width, const std::vector<HudLabel> &labels,
                     const std::vector<HudLabelDrawn> &drawn);

// The screen points the labels can use in a window: the placed screen image `placement` (480 x
// 320 points) and the wide picture `wide` (Presenter::wide_rect(), the placement's rect without
// widescreen), both in drawable pixels: [lo, hi] = [0, 480] widened to the wide picture's
// edges, rounded out to whole points.
void hud_label_span(const Placement &placement, const Rect &wide, float *lo, float *hi);

// The labels' layer in the window: a transparent RGBA texture over the wide picture at the
// window's pixels a point, drawn with Canvas when the labels, style, bindings or size change, and
// rendered over the picture.
class HudLabelLayer {
  public:
    HudLabelLayer() = default;
    HudLabelLayer(const HudLabelLayer &) = delete;
    HudLabelLayer &operator=(const HudLabelLayer &) = delete;
    ~HudLabelLayer() { stop(); }

    // Draws `labels` over `placement`'s image and the wide picture `wide` (nothing when empty or
    // none has a control). Call after the read-backs and captures, before SDL_RenderPresent.
    // False (logged once) if the texture cannot be made.
    bool draw(SDL_Renderer *renderer, const Placement &placement, const Rect &wide,
              const std::vector<HudLabel> &labels, const PromptStyle &style,
              const std::vector<Binding> &bindings);
    // Frees the texture (before the renderer goes).
    void stop();
    // How often the layer was drawn anew, and how many labels the last drawing had.
    unsigned redraws() const { return redraws_; }
    int drawn() const { return count_; }

  private:
    SDL_Texture *texture_ = nullptr;
    int pixels_per_point_ = 0, tex_w_ = 0, tex_h_ = 0;
    float lo_ = 0, hi_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<HudLabel> drawn_;
    PromptStyle drawn_style_;
    std::vector<Binding> drawn_bindings_;
    bool told_ = false;
    unsigned redraws_ = 0;
    int count_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_HUD_LABELS_H
