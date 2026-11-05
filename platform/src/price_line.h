// The cash shop's price line (#156, ADR 0072): with cash_shop = meso, the current prices of what
// the open screen sells, before the player taps a tile or a revive button. The game itself shows
// no price (its tiles and the death popup's buttons are images, and the App Store sheet that
// showed the price is gone), so the port draws them:
//
//   the cash shop (game mode 0xB)   the four items for sale and "meso packs: not sold"
//   the death popup (0x68)          the POW! Package and the Comeback Pack
//
// with the hero's meso, from mt_cash_shop_view() (the runtime's quotes of the last run-loop
// turn, the same function the store charges with). Drawn with SDL's renderer
// (SDL_RenderDebugText) at the picture's text scale (scaling.h's text_frame), after the screen
// image and every read-back, like the frame-time overlay: never in a --shot, a golden or a
// --capture. Where (ADR 0114): in the bar below the picture when it fits there at that scale,
// else inside the picture's bottom-left corner; never across the picture's edge.
#ifndef MAPLETHIEF_PLATFORM_PRICE_LINE_H
#define MAPLETHIEF_PLATFORM_PRICE_LINE_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/cashshop.h"
#include "scaling.h"

namespace maplethief::frontend {

// "1,234,567".
std::string meso_text(std::int64_t meso);

// The lines the price line shows for `view` (none when nothing is open or the shop is off).
std::vector<std::string> price_lines(const mt_cash_view &view);

// The box's size in font pixels at scale 1 for `lines` (padding included).
void price_line_size(const std::vector<std::string> &lines, float *w, float *h);

// Where a w x h box (font pixels at scale 1) goes for the picture `picture` (drawable pixels; the
// wide picture with widescreen) in the frame: at the bottom left of the strip of the frame's
// area below the picture when it fits there whole at the frame's scale (`below` true), else at
// the bottom left of the part of the area the picture covers, its scale less until it fits
// there. An empty picture: the area's bottom left, as place_text_box.
TextBox place_price_line(const TextFrame &frame, const Rect &picture, float w, float h,
                         bool *below = nullptr);

// Draws them where place_price_line puts them, at the frame's scale (less if they would not
// fit). Leaves the renderer's draw colour, blend mode and scale as they were.
void draw_price_line(SDL_Renderer *renderer, const mt_cash_view &view, const TextFrame &frame,
                     const Rect &picture);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRICE_LINE_H
