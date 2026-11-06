// The cash shop's price line (#156, ADR 0072); see price_line.h.
#include "price_line.h"

#include "port_text.h"

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
    lines.push_back(tr(view.screen == MT_CASH_SCREEN_SHOP ? Txt::price_shop : Txt::price_revive,
                       {meso_text(view.meso)}));
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
        add(tr(Txt::price_packs_not_sold));
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

namespace {
constexpr float kGlyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
constexpr float kLineH = kGlyph + 2;
constexpr float kPad = 4;
constexpr float kMargin = 2;
} // namespace

void price_line_size(const std::vector<std::string> &lines, float *w, float *h) {
    std::size_t longest = 0;
    for (const std::string &line : lines) {
        longest = std::max(longest, line.size());
    }
    *w = kGlyph * static_cast<float>(longest) + 2 * kPad;
    *h = kLineH * static_cast<float>(lines.size()) - 2 + 2 * kPad;
}

TextBox place_price_line(const TextFrame &frame, const Rect &picture, float w, float h,
                         bool *below) {
    if (below != nullptr) {
        *below = false;
    }
    const Rect &a = frame.area;
    if (!(picture.w > 0 && picture.h > 0)) {
        return place_text_box(frame, w, h, TextCorner::kBottomLeft, kMargin);
    }
    // The strip below the picture, inside the area.
    const float strip_top = std::max(a.y, picture.y + picture.h);
    const float strip_bottom = a.y + a.h;
    if (strip_bottom > strip_top) {
        const TextFrame strip{frame.scale, Rect{a.x, strip_top, a.w, strip_bottom - strip_top}};
        const TextBox t = place_text_box(strip, w, h, TextCorner::kBottomLeft, kMargin);
        const float k = static_cast<float>(t.scale);
        if (t.scale == std::max(frame.scale, 1) && t.box.y * k >= strip_top &&
            (t.box.y + t.box.h) * k <= strip_bottom && (t.box.x + t.box.w) * k <= a.x + a.w) {
            if (below != nullptr) {
                *below = true;
            }
            return t;
        }
    }
    // Inside the picture: the area it covers.
    const float x0 = std::max(a.x, picture.x), y0 = std::max(a.y, picture.y);
    const float x1 = std::min(a.x + a.w, picture.x + picture.w);
    const float y1 = std::min(a.y + a.h, picture.y + picture.h);
    const TextFrame inside{frame.scale,
                           Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)}};
    return place_text_box(inside, w, h, TextCorner::kBottomLeft, kMargin);
}

void draw_price_line(SDL_Renderer *renderer, const mt_cash_view &view, const TextFrame &frame,
                     const Rect &picture) {
    const std::vector<std::string> text = price_lines(view);
    if (renderer == nullptr || text.empty()) {
        return;
    }
    const float line_h = kLineH;
    const float pad = kPad;

    float sx = 1;
    float sy = 1;
    SDL_GetRenderScale(renderer, &sx, &sy);
    Uint8 cr = 0, cg = 0, cb = 0, ca = 0;
    SDL_GetRenderDrawColor(renderer, &cr, &cg, &cb, &ca);
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    float w = 0, h = 0;
    price_line_size(text, &w, &h);
    const TextBox placed = place_price_line(frame, picture, w, h);
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
