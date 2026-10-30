// The frame-time overlay (#137, ADR 0071); see perf_overlay.h.
#include "perf_overlay.h"

#include <algorithm>
#include <cstdio>

namespace maplethief::frontend {

bool PerfOverlay::handle(const SDL_Event &e) {
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) {
        return false;
    }
    if (e.key.key != kHotkey || (e.key.mod & ~(SDL_KMOD_SHIFT | SDL_KMOD_NUM)) != 0) {
        return false;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        presses_++;
        if (allowed_) {
            toggle();
            SDL_Log("perf: the frame-time overlay is %s (F3)", on_ ? "on" : "off");
        } else {
            SDL_Log("perf: no frame-time overlay in a run that writes images (--fixed-step, "
                    "--shot, --capture)");
        }
    }
    return true; // its up and repeats too, so the keyboard's bindings never see F3 half
}

std::vector<std::string> PerfOverlay::lines(const RecentStats &r, const FrameRecord *last,
                                            const std::string &note) {
    std::vector<std::string> out;
    char b[128];
    std::snprintf(b, sizeof b, "%5.1f fps %5.1f ticks/s  tick %5.1f ms (max %.1f)", r.fps, r.tps,
                  r.tick_ms, r.max_tick_ms);
    out.emplace_back(b);
    std::snprintf(b, sizeof b, "guest %5.2f  gfx %5.2f  take %5.2f ms", r.guest_ms, r.gfx_ms,
                  r.take_ms);
    out.emplace_back(b);
    std::snprintf(b, sizeof b, "comp  %5.2f  draw %5.2f  frontend %5.2f (max %.1f)", r.compose_ms,
                  r.draw_ms, r.frontend_ms, r.max_frontend_ms);
    out.emplace_back(b);
    if (last != nullptr && last->vblanks >= 0) {
        std::snprintf(b, sizeof b, "present %5.2f  wait %5.2f ms  vblanks %d", r.present_ms,
                      r.wait_ms, static_cast<int>(last->vblanks));
    } else {
        std::snprintf(b, sizeof b, "present %5.2f  wait %5.2f ms", r.present_ms, r.wait_ms);
    }
    out.emplace_back(b);
    if (!note.empty()) {
        out.push_back(note);
    }
    return out;
}

namespace {

// Lines of SDL's debug font in a dark translucent box at (x, y) (output pixels / `scale`, or
// from the bottom when `from_bottom`), each font pixel `scale` x `scale` output pixels. Leaves
// the renderer's draw colour, blend mode and scale as they were.
void draw_box(SDL_Renderer *renderer, const std::vector<std::string> &text, int scale,
              bool from_bottom) {
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

    const float k = static_cast<float>(std::max(1, scale));
    SDL_SetRenderScale(renderer, k, k);
    // A dark translucent box behind light text: readable over any art.
    SDL_FRect box = {2, 2, glyph * static_cast<float>(longest) + 2 * pad,
                     line_h * static_cast<float>(text.size()) - 2 + 2 * pad};
    if (from_bottom) {
        int ow = 0;
        int oh = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh);
        box.y = std::max(2.0f, static_cast<float>(oh) / k - box.h - 2);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0x0A, 0x0D, 0x12, 0xC8);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 0xE8, 0xEC, 0xF2, SDL_ALPHA_OPAQUE);
    float y = box.y + pad;
    for (const std::string &line : text) {
        SDL_RenderDebugText(renderer, box.x + pad, y, line.c_str());
        y += line_h;
    }
    SDL_SetRenderScale(renderer, sx, sy);
    SDL_SetRenderDrawBlendMode(renderer, blend);
    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
}

} // namespace

void PerfOverlay::draw(SDL_Renderer *renderer, const FrameStats &stats, int scale) const {
    if (!visible() || renderer == nullptr) {
        return;
    }
    draw_box(renderer, lines(stats.recent(), stats.last(), note_), scale, false);
}

void PerfOverlay::draw_notice(SDL_Renderer *renderer, const std::string &text, int scale) {
    if (renderer == nullptr || text.empty()) {
        return;
    }
    draw_box(renderer, {text}, scale, true);
}

} // namespace maplethief::frontend
