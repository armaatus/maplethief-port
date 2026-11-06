// The game's full-screen flashes (#186, ADR 0093 addendum, docs/re/flashes.md); see
// maplethief/hooks.h (mt_hooks_install_flashes).
//
// A flash is a whole-screen fill through FillRect 0x8079 from one of a few call sites: the
// hero's level-up, revive and effect 15 white-then-black pairs in CHero::Paint 0x61A19 (through
// Canvas_FillRectColor 0x23D95), and a scene's fade-in from a colour in CStateGame::Paint 0x87501
// (through Canvas_FillRect 0x23D1D). The two canvas fills' handlers mark the call when it comes
// from such a site; FillRect's wrapper scales the graphics context's alpha for that one fill (to
// 0 at 0 %: drawn, unseen), then writes the game's alpha back and sets GL's current colour to
// the one the game's fill leaves. Only the flash's pixels change.
#include "maplethief/hle/abi.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/hooks.h"

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The return addresses (lr, Thumb bit set) of the flash calls of Canvas_FillRectColor in
// CHero::Paint 0x61A19 (docs/re/flashes.md).
constexpr u32 k_hero_flash_returns[] = {
    0x61CDFu, // a revive's frame 2: white (the colour table 0x117150, row 2)
    0x61D33u, // a revive's frame 3: black (row 3)
    0x6200Fu, // effect 12 (a level-up) or 13, frame 2: white
    0x62059u, // effect 12 or 13, frame 3: black
    0x621DFu, // effect 15, frame 1: white
    0x62213u, // effect 15, frame 2: black
};
// CStateGame::Paint's Canvas_FillRect of a scene's fade-in from colour +0x4B8 (command 29).
constexpr u32 k_fade_in_return = 0x8A6EFu;

// SkyCanvas's graphics context, as FillRect reads it (its sixth argument, at sp + 4).
constexpr u32 k_canvas_global = 0x11A950u; // SkyCanvas *
constexpr u32 k_canvas_context = 0x10u;    // SkyCanvas +0x10
constexpr u32 k_context_color = 0x14u;     // 0xRRGGBB
constexpr u32 k_context_alpha = 0x20u;     // 0-255, or -1: opaque
constexpr u32 k_opaque = 0xFFFFFFFFu;
// glColor4x's import stub: FillRect leaves the fill's colour current (glColor4x(r << 8, g << 8,
// b << 8, alpha << 8), or 0x10000 for opaque).
constexpr u32 k_gl_color4x = 0xF0F10u;

std::atomic<u32> g_percent{100};
std::atomic<u64> g_marked{0}, g_scaled{0}, g_hidden{0};
bool g_mark = false; // the next FillRect is a flash's (guest thread only)

bool readable(const mt_cpu *cpu, u32 addr) {
    return cpu->mem != nullptr && static_cast<u64>(addr) + 4u <= cpu->mem_size;
}
u32 read_word(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}
void write_word(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

// Runs the canvas fill with the next FillRect marked as a flash's.
int marked(mt_cpu *cpu, const mt_hook_call *call) {
    g_mark = true;
    call->original(cpu);
    g_mark = false; // a fill clipped away never reached FillRect's draw
    return MT_HOOK_HANDLED;
}

// Canvas_FillRectColor 0x23D95 (x, y, w, h, colour at sp).
int fill_color_hook(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 lr = cpu->r[MT_LR];
    for (const u32 site : k_hero_flash_returns) {
        if (lr == site) {
            return marked(cpu, call);
        }
    }
    return MT_HOOK_PASS;
}

// Canvas_FillRect 0x23D1D (x, y, w, h): the fade-in's fill in the canvas's colour, a flash when
// the colour is not black (the scenes fade in from black at their start; from white after a
// boss or a blow).
int fill_hook(mt_cpu *cpu, const mt_hook_call *call) {
    if (cpu->r[MT_LR] != k_fade_in_return || !readable(cpu, k_canvas_global)) {
        return MT_HOOK_PASS;
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    const u32 context = canvas + k_canvas_context;
    if (canvas == 0u || !readable(cpu, context + k_context_color) ||
        (read_word(cpu, context + k_context_color) & 0xFFFFFFu) == 0u) {
        return MT_HOOK_PASS;
    }
    return marked(cpu, call);
}

// FillRect 0x8079 (surface, x, y, w, h at sp, graphics context at sp + 4): a marked fill.
int rect_wrapper(mt_cpu *cpu, const mt_hook_call *call) {
    if (!g_mark) {
        return MT_HOOK_PASS;
    }
    g_mark = false;
    g_marked.fetch_add(1, std::memory_order_relaxed);
    const u32 sp = cpu->r[MT_SP];
    if (!readable(cpu, sp + 4u)) {
        return MT_HOOK_PASS;
    }
    const u32 context = read_word(cpu, sp + 4u);
    if (!readable(cpu, context + k_context_alpha)) {
        return MT_HOOK_PASS;
    }
    // The fill runs at every percent, 0 included (drawn at alpha 0): FillRect's GL state
    // changes (the blend switch and function) stay the game's, so nothing drawn after it in the
    // frame changes. Only the current colour's alpha would, so it is set back afterwards.
    const u32 percent = g_percent.load(std::memory_order_relaxed);
    const u32 alpha = read_word(cpu, context + k_context_alpha);
    const u32 scaled = mt_flash_scale_alpha(alpha, percent);
    write_word(cpu, context + k_context_alpha, scaled);
    call->original(cpu);
    write_word(cpu, context + k_context_alpha, alpha);
    const u32 rgb = read_word(cpu, context + k_context_color);
    const u32 color[4] = {((rgb >> 16) & 0xFFu) << 8, ((rgb >> 8) & 0xFFu) << 8, (rgb & 0xFFu) << 8,
                          alpha == k_opaque ? 0x10000u : alpha << 8};
    mt_call_guest(cpu, k_gl_color4x, color, 4, MT_HOOK_FN_FILL_RECT);
    (percent == 0u ? g_hidden : g_scaled).fetch_add(1, std::memory_order_relaxed);
    mt_tracef("hooks", "flash fill: colour 0x%06X, alpha %d drawn at %u",
              static_cast<unsigned>(rgb & 0xFFFFFFu), static_cast<int>(alpha),
              static_cast<unsigned>(scaled));
    return MT_HOOK_HANDLED;
}

} // namespace

// The alpha a flash is drawn with at `percent`: the context's alpha (-1, or anything above 255:
// opaque, 255) times percent / 100, rounded half up.
extern "C" std::uint32_t mt_flash_scale_alpha(std::uint32_t alpha, std::uint32_t percent) noexcept {
    const u32 a = alpha > 255u ? 255u : alpha;
    const u32 p = percent > 100u ? 100u : percent;
    return (a * p + 50u) / 100u;
}

extern "C" void mt_hooks_install_flashes(std::uint32_t percent) noexcept {
    const u32 p = percent > 100u ? 100u : percent;
    g_percent.store(p, std::memory_order_relaxed);
    const int on = p < 100u ? 1 : 0;
    g_mark = false;
    mt_hook_register(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR, fill_color_hook, nullptr, on);
    mt_hook_register(MT_HOOK_FN_CANVAS_FILL_RECT, fill_hook, nullptr, on);
    mt_hook_wrap(MT_HOOK_FN_FILL_RECT, rect_wrapper, nullptr, on);
}

extern "C" mt_flash_stats mt_hooks_flash_stats(void) noexcept {
    mt_flash_stats s{};
    s.marked = g_marked.load(std::memory_order_relaxed);
    s.scaled = g_scaled.load(std::memory_order_relaxed);
    s.hidden = g_hidden.load(std::memory_order_relaxed);
    return s;
}
