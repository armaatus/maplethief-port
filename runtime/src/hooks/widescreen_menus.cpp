// Widescreen menus (#169, ADR 0090's addendum): the screens over the field drawn at 480 points
// in the centre of the wide view; see maplethief/widescreen.h and docs/re/widescreen.md.
//
// CStateGame::Paint 0x87501 draws the field (the backdrop, the map, its objects, the HUD) in every
// game mode while a map is loaded, then switches on the mode and draws the screen over it:
//   mode 1  0x56B99  the popup menu: a black fill at alpha 100 over (0, 0, 480, 320) through
//                    FillRect 0x8079, its window at (0x4A, 0x1C), the tabs at fixed x
//   mode 2  0x56E91  the NPC shop: windows at fixed x, clipped to +0x88
//   mode 3  0x59BB1, 0x59119, 0x58AE9  the map screen's views (Map_PaintSub0/1/2)
// Each is called only from Paint. Wrapped, each runs with the engine's width words back at 480
// (as widescreen.cpp's apply writes them) and a host clip-space x offset of (E - 480) / W, which
// moves its draws (E - 480) / 2 points right, into the GL frame's centre 480 points: the screen
// image the host shows (gl_screen_columns) and takes its touches on. Afterwards the words are as
// they were. Nothing the game keeps reads them in between: only drawing code reads the width.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "maplethief/gles.h"
#include "maplethief/hooks.h"
#include "maplethief/widescreen.h"
#include "widescreen_internal.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_canvas_global = 0x11A950u;
constexpr u32 k_canvas_clip_x0 = 0x14u;
constexpr u32 k_canvas_clip_x1 = 0x1Cu;
constexpr u32 k_canvas_width = 0x88u;
constexpr u32 k_canvas_half_width = 0x90u;
constexpr u32 k_surface_global = 0x11AA88u;
constexpr u32 k_surface_row_bytes = 0x10u;
constexpr u32 k_screen_width_global = 0x11AA8Cu;
constexpr u32 k_narrow = MT_WIDESCREEN_NARROW;
constexpr u32 k_height = MT_WIDESCREEN_HEIGHT;

constexpr u32 k_screens[] = {MT_HOOK_FN_POPUP_MENU_PAINT, MT_HOOK_FN_SHOP_PAINT,
                             MT_HOOK_FN_MAP_PAINT_0, MT_HOOK_FN_MAP_PAINT_1,
                             MT_HOOK_FN_MAP_PAINT_2};

// Guest thread: the screen whose paint runs (its function pointer), 0 outside one; the view E and
// the host offset during it.
u32 g_screen = 0;
u32 g_view = 0;
float g_offset = 0.0f;
std::atomic<std::uint64_t> g_screens{0}, g_dims{0};

// The image maps every word read here (mt_widescreen_view() is only above 480 once
// mt_widescreen_apply found the game's globals, but a test's guest may differ).
bool readable(const mt_cpu *cpu, u32 addr) {
    return cpu != nullptr && cpu->mem != nullptr && addr != 0u &&
           static_cast<std::uint64_t>(addr) + 4u <= cpu->mem_size;
}

u32 read_word(const mt_cpu *cpu, u32 addr) {
    u32 v = 0;
    if (readable(cpu, addr)) {
        std::memcpy(&v, cpu->mem + addr, 4);
    }
    return v;
}

void write_word(mt_cpu *cpu, u32 addr, u32 v) {
    if (readable(cpu, addr)) {
        std::memcpy(cpu->mem + addr, &v, 4);
    }
}

// The engine's width words: the surface's width and row bytes, 0x11AA8C, +0x88, +0x90.
struct Words {
    u32 surface_width, row_bytes, screen_width, width, half_width;
};

Words read_words(const mt_cpu *cpu, u32 canvas, u32 surface) {
    return Words{read_word(cpu, surface), read_word(cpu, surface + k_surface_row_bytes),
                 read_word(cpu, k_screen_width_global), read_word(cpu, canvas + k_canvas_width),
                 read_word(cpu, canvas + k_canvas_half_width)};
}

void write_words(mt_cpu *cpu, u32 canvas, u32 surface, const Words &w) {
    write_word(cpu, surface, w.surface_width);
    write_word(cpu, surface + k_surface_row_bytes, w.row_bytes);
    write_word(cpu, k_screen_width_global, w.screen_width);
    write_word(cpu, canvas + k_canvas_width, w.width);
    write_word(cpu, canvas + k_canvas_half_width, w.half_width);
}

// The canvas's clip rect, when it is the whole screen `from` points wide, becomes `to` wide (as
// mt_widescreen_apply does; 0x233D5 clamps the clip to +0x88).
void widen_full_clip(mt_cpu *cpu, u32 canvas, u32 from, u32 to) {
    if (read_word(cpu, canvas + k_canvas_clip_x0) == 0u &&
        read_word(cpu, canvas + k_canvas_clip_x1) == from) {
        write_word(cpu, canvas + k_canvas_clip_x1, to);
    }
}

// A screen's paint (data manager, game): at 480 points, moved into the wide view's centre.
int screen_hook(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 e = mt_widescreen_view();
    const u32 rb = mt_widescreen_width();
    if (e <= k_narrow || rb < e || g_screen != 0u) {
        return MT_HOOK_PASS; // the engine at 480 (or already inside a screen): as the original
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    const u32 surface = read_word(cpu, k_surface_global);
    if (canvas == 0u || surface == 0u || read_word(cpu, canvas + k_canvas_width) != e) {
        return MT_HOOK_PASS;
    }
    const Words wide = read_words(cpu, canvas, surface);
    write_words(cpu, canvas, surface,
                Words{k_narrow, k_narrow * 4u, k_narrow, k_narrow, k_narrow / 2u});
    widen_full_clip(cpu, canvas, e, k_narrow);
    float scale = 1.0f, offset = 0.0f;
    mt_gles_get_host_x_transform(&scale, &offset);
    // Clip space spans the renderbuffer's rb points over 2: (E - 480) / 2 points is (E - 480) / rb.
    g_offset = static_cast<float>(e - k_narrow) / static_cast<float>(rb);
    g_view = e;
    g_screen = call->fn;
    mt_gles_set_host_x_transform(1.0f, g_offset);
    call->original(cpu);
    mt_gles_set_host_x_transform(scale, offset);
    g_screen = 0u;
    write_words(cpu, canvas, surface, wide);
    widen_full_clip(cpu, canvas, k_narrow, e);
    g_screens.fetch_add(1, std::memory_order_relaxed);
    return MT_HOOK_HANDLED;
}

// FillRect 0x8079 (surface, x, y, w, h at sp, context at sp + 4): inside the popup menu, its dim
// over the whole 480 x 320 screen is stretched to the view's E columns from column 0 (the
// backdrop's transform: x' = k x + (k - 1) w, k = E / 480), so the wings are dimmed too.
int fill_hook(mt_cpu *cpu, const mt_hook_call *call) {
    if (g_screen != MT_HOOK_FN_POPUP_MENU_PAINT || cpu->r[1] != 0u || cpu->r[2] != 0u ||
        cpu->r[3] != k_narrow || read_word(cpu, cpu->r[MT_SP]) != k_height) {
        return MT_HOOK_PASS;
    }
    const float k = static_cast<float>(g_view) / static_cast<float>(k_narrow);
    mt_gles_set_host_x_transform(k, k - 1.0f);
    call->original(cpu);
    mt_gles_set_host_x_transform(1.0f, g_offset);
    g_dims.fetch_add(1, std::memory_order_relaxed);
    return MT_HOOK_HANDLED;
}

} // namespace

namespace mt_widescreen_detail {

void menus_configure(bool on) noexcept {
    for (const u32 fn : k_screens) {
        mt_hook_register(fn, screen_hook, nullptr, on ? 1 : 0);
    }
    mt_hook_register(MT_HOOK_FN_FILL_RECT, fill_hook, nullptr, on ? 1 : 0);
}

void menus_reset() noexcept {
    for (const u32 fn : k_screens) {
        mt_hook_unregister(fn);
    }
    mt_hook_unregister(MT_HOOK_FN_FILL_RECT);
    g_screen = 0u;
    g_view = 0u;
    g_offset = 0.0f;
    g_screens.store(0);
    g_dims.store(0);
}

bool menu_active() noexcept { return g_screen != 0u; }

} // namespace mt_widescreen_detail

extern "C" mt_widescreen_menu_stats mt_widescreen_get_menu_stats(void) noexcept {
    mt_widescreen_menu_stats s{};
    s.screens = g_screens.load();
    s.dims = g_dims.load();
    return s;
}
