// The input display (#404, ADR 0156): the inputs the game is being given, for runners' videos and
// for practice. A small panel of the pad's keys (the four directions, jump, attack, the four
// quick slots) and the menu keys (OK, back, the popup menu, the map), each lit while it is held.
// Presentation only: nothing here writes guest memory or changes what the game does.
//
//   input_display = off | on   settings.ini (Video, off by default, in no preset),
//                              --input-display on|off; read live
//
// What lights a cell (input_display_lit) is what the action layer delivers, whatever the device:
//   - the runtime's held actions (mt_input_held): every press that reached it this turn from the
//     keyboard, a gamepad or the port's own latches (turbo, auto-run, skip_intros' taps), through
//     Actions' sink (ADR 0048), before the paced delivery
//   - the touch key pad's own keys (mt_input_pad_keys: TouchKeyPadView's ten key BOOLs, read at
//     the turn and after its touches): a finger on the pad, which never passes Actions
// The Map, Bag and Cash buttons on the touch screen are UIKit buttons that call the guest
// directly, so a touch on them does not light Map or Back; their keys and pad buttons do.
//
// Where (input_display_place, in drawable pixels, inside the safe area): the bottom left of the
// border left of the picture when that bar has room for it, else of the border under the
// picture, else over the picture's left edge between the Map button (y < 52) and the d-pad
// (y >= 176): none of the HUD is there. The size is the port's window text's (scaling.h's
// text_frame: one font pixel `scale` drawable pixels), less when it would not fit.
//
// How it draws: the Canvas (the port's 5x7 font, shapes; no game art) on its own layer at the
// window's pixels after the read-backs, like the minimap and the EXP figure: never in a shot, a
// golden of the game, a capture or the regression replay's hashes.
#ifndef MAPLETHIEF_PLATFORM_INPUT_DISPLAY_H
#define MAPLETHIEF_PLATFORM_INPUT_DISPLAY_H

#include <cstdint>
#include <string>
#include <vector>

#include "canvas.h"
#include "scaling.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

// A cell of the panel: its action and where it is in font pixels from the panel's top left.
struct InputDisplayCell {
    std::uint32_t action;
    int x, y, w, h;
    bool dpad; // an arm of the d-pad (no label)
};
// Every cell, the d-pad first, then jump, attack, quick slots 1 to 4, OK, back, menu, map.
const std::vector<InputDisplayCell> &input_display_cells();
// A cell's label (port text; empty for the d-pad).
std::string input_display_label(std::uint32_t action);

// The panel's size in font pixels.
inline constexpr int kInputDisplayW = 102;
inline constexpr int kInputDisplayH = 47;
// Font pixels between the panel and the edges of where it goes.
inline constexpr int kInputDisplayMargin = 4;

// The lit cells as bits 1 << action: a cell is lit when `held` says its action is held, or its
// bit is in `pad_keys` (mt_input_pad_keys). `held` nullptr: the runtime's (mt_input_held).
using InputHeld = std::uint32_t (*)(std::uint32_t action);
std::uint32_t input_display_lit(InputHeld held, std::uint32_t pad_keys);
// The same from the runtime now (mt_input_held, mt_input_pad_keys).
std::uint32_t input_display_lit_now();

// Draws the panel with its top left at (x, y) points of `canvas`, the cells in `lit` lit.
void draw_input_display(Canvas &canvas, int x, int y, std::uint32_t lit);
// The panel alone: premultiplied RGBA8 over transparent, kInputDisplayW x kInputDisplayH font
// pixels at `unit` drawable pixels each, top row first.
std::vector<std::uint8_t> render_input_display(std::uint32_t lit, int unit);

// Where the panel goes and its scale (see above). `picture`: the picture's rectangle (the wide
// picture's with widescreen), `safe`: the safe area, both in drawable pixels; `scale`: the
// frame's text scale. An empty rect when there is no picture.
struct InputDisplayPlace {
    Rect rect;
    int scale = 1;
};
InputDisplayPlace input_display_place(const Rect &picture, const Rect &safe, int scale);

// The panel on the window's renderer.
class InputDisplayLayer {
  public:
    InputDisplayLayer() = default;
    InputDisplayLayer(const InputDisplayLayer &) = delete;
    InputDisplayLayer &operator=(const InputDisplayLayer &) = delete;
    ~InputDisplayLayer() { stop(); }

    // Draws the panel with `lit` at `at`. Call after the read-backs and captures, before
    // SDL_RenderPresent. False (logged once) if the texture cannot be made or drawn.
    bool draw(SDL_Renderer *renderer, const InputDisplayPlace &at, std::uint32_t lit);
    // Frees the texture (before the renderer goes).
    void stop();
    // How often the panel was drawn anew (other cells lit, another scale), and drawn at all.
    unsigned redraws() const { return redraws_; }
    unsigned draws() const { return draws_; }

  private:
    SDL_Texture *texture_ = nullptr;
    std::uint32_t lit_ = 0;
    int unit_ = 0;
    bool told_ = false;
    unsigned redraws_ = 0, draws_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_INPUT_DISPLAY_H
