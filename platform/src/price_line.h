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
// (SDL_RenderDebugText) into the window's bottom-left corner, after the screen image and every
// read-back, like the frame-time overlay: never in a --shot, a golden or a --capture.
#ifndef MAPLETHIEF_PLATFORM_PRICE_LINE_H
#define MAPLETHIEF_PLATFORM_PRICE_LINE_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/cashshop.h"

namespace maplethief::frontend {

// "1,234,567".
std::string meso_text(std::int64_t meso);

// The lines the price line shows for `view` (none when nothing is open or the shop is off).
std::vector<std::string> price_lines(const mt_cash_view &view);

// Draws them at the window's bottom left, each font pixel `scale` x `scale` output pixels.
// Leaves the renderer's draw colour, blend mode and scale as they were.
void draw_price_line(SDL_Renderer *renderer, const mt_cash_view &view, int scale);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PRICE_LINE_H
