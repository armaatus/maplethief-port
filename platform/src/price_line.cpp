// The cash shop's price line (#156, ADR 0072); see price_line.h.
#include "price_line.h"

#include <algorithm>
#include <cstdio>

namespace maplethief::frontend {

std::string meso_text(std::int64_t meso) {
    const bool negative = meso < 0;
    std::string digits = std::to_string(negative ? -meso : meso);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); i++) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return negative ? "-" + out : out;
}

// At most this many glyphs a line: a 480-pixel-wide window (the game's screen at 1x) holds 60
// of SDL's 8-pixel debug glyphs, with the box's padding.
constexpr std::size_t kWidth = 56;

std::vector<std::string> price_lines(const mt_cash_view &view) {
    std::vector<std::string> lines;
    if (view.screen == MT_CASH_SCREEN_NONE || view.count == 0) {
        return lines;
    }
    lines.push_back(std::string(view.screen == MT_CASH_SCREEN_SHOP ? "Cash shop" : "Revive") +
                    " prices in meso (you have " + meso_text(view.meso) + ")");
    std::string line;
    bool packs = false;
    const auto add = [&lines, &line](const std::string &item) {
        if (!line.empty() && line.size() + 3 + item.size() > kWidth) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : "   ") + item;
    };
    for (std::uint32_t i = 0; i < view.count && i < MT_CASH_PRODUCT_COUNT; i++) {
        const mt_cash_quote &q = view.quotes[i];
        if (q.price < 0) {
            packs = true;
            continue;
        }
        const char *title = mt_cash_product_title(q.product);
        add(std::string(title != nullptr ? title : "?") + " " + meso_text(q.price));
    }
    if (packs) {
        add("meso packs: not sold");
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

void draw_price_line(SDL_Renderer *renderer, const mt_cash_view &view, const TextFrame &frame) {
    const std::vector<std::string> text = price_lines(view);
    if (renderer == nullptr || text.empty()) {
        return;
    }
    std::size_t longest = 0;
    for (const std::string &line : text) {
        longest = std::max(longest, line.size());
    }
    const float glyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    const float line_h = glyph + 2;
    const float pad = 4;

    float sx = 1;
    float sy = 1;
    SDL_GetRenderScale(renderer, &sx, &sy);
    Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
    SDL_GetRenderDrawColor(renderer, &cr, &cg, &cb, &ca);
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    const float h = line_h * static_cast<float>(text.size()) - 2 + 2 * pad;
    const TextBox placed = place_text_box(frame, glyph * static_cast<float>(longest) + 2 * pad, h,
                                          TextCorner::kBottomLeft, 2);
    const float k = static_cast<float>(placed.scale);
    SDL_SetRenderScale(renderer, k, k);
    const SDL_FRect box = {placed.box.x, placed.box.y, placed.box.w, placed.box.h};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0x0A, 0x0D, 0x12, 0xD8);
    SDL_RenderFillRect(renderer, &box);
    float y = box.y + pad;
    for (std::size_t i = 0; i < text.size(); i++) {
        // The heading in the game's gold, the prices in white.
        if (i == 0) {
            SDL_SetRenderDrawColor(renderer, 0xF2, 0xC4, 0x4C, SDL_ALPHA_OPAQUE);
        } else {
            SDL_SetRenderDrawColor(renderer, 0xF0, 0xF2, 0xF5, SDL_ALPHA_OPAQUE);
        }
        SDL_RenderDebugText(renderer, box.x + pad, y, text[i].c_str());
        y += line_h;
    }
    SDL_SetRenderScale(renderer, sx, sy);
    SDL_SetRenderDrawBlendMode(renderer, blend);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
}

} // namespace maplethief::frontend
