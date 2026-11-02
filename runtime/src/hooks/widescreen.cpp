// Widescreen (#86, ADR 0090): the engine's view width, its camera hooks and the backdrop's
// stretch; see maplethief/widescreen.h and docs/re/widescreen.md.
//
// The guest words (docs/re/widescreen.md, docs/formats/mapprj.md):
//   0x11A950            SkyCanvas::Instance: the canvas
//     +0x0C             the engine's current state (a CStateGame's vtable pointer 0x119B78)
//     +0x10             the graphics context: its clip rect x0, y0, x1, y1 at +0x14 ... +0x20
//     +0x88, +0x90      the screen width and half of it (+0x8C, +0x94: the height, unchanged)
//   0x11AA00            the game mode (0 field, 1 popup menu, 2 shop, 3 map, 4 NPC chat,
//                       0xB cash shop, ...)
//   0x11AA88            the screen surface: +0x00 width, +0x10 bytes a row (width x 4)
//   0x11AA8C            Screen_Init's width
//   CStateGame +0x2C    the map (CMMap): +0x5C its width in pixels
//   CStateGame +0x454   the backdrop (CMImage), drawn at (0, 0) by 0x24DC9 each field frame
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/widescreen.h"
#include "widescreen_internal.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_canvas_global = 0x11A950u;
constexpr u32 k_canvas_state = 0x0Cu;
constexpr u32 k_canvas_clip_x0 = 0x14u; // the graphics context (+0x10) +4
constexpr u32 k_canvas_clip_x1 = 0x1Cu; // +0x0C
constexpr u32 k_canvas_width = 0x88u;
constexpr u32 k_canvas_half_width = 0x90u;
constexpr u32 k_game_mode = 0x11AA00u;
constexpr u32 k_surface_global = 0x11AA88u;
constexpr u32 k_surface_row_bytes = 0x10u;
constexpr u32 k_screen_width_global = 0x11AA8Cu;
constexpr u32 k_game_vptr = 0x119B78u; // CStateGame: vtable 0x119B70, slots from +8
constexpr u32 k_state_map = 0x2Cu;
constexpr u32 k_state_backdrop = 0x454u;
constexpr u32 k_map_width = 0x5Cu;

std::atomic<u32> g_width{MT_WIDESCREEN_NARROW}; // configured; 480 = off
std::atomic<u32> g_view{MT_WIDESCREEN_NARROW};  // applied
std::atomic<bool> g_allow{false};               // the host's last word (mt_widescreen_turn)
std::atomic<std::uint64_t> g_turns{0}, g_wide_turns{0}, g_changes{0}, g_camera{0}, g_backdrops{0};

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

// Whether the image maps the game's globals (0x11A950 ... 0x11AA8F): a guest without the game's
// image has nothing there, and its words are never read.
bool globals_mapped(const mt_cpu *cpu) {
    if (cpu == nullptr || cpu->mem == nullptr) {
        return false;
    }
    mt_mem *mem = mt_mem_from_cpu(cpu);
    u32 n = 0;
    const mt_segment *segs = mem != nullptr ? mt_mem_segments(mem, &n) : nullptr;
    for (u32 i = 0; segs != nullptr && i < n; i++) {
        const std::uint64_t end = static_cast<std::uint64_t>(segs[i].vmaddr) + segs[i].vmsize;
        if (k_canvas_global >= segs[i].vmaddr && k_screen_width_global + 4u <= end &&
            end <= cpu->mem_size) {
            return true;
        }
    }
    return false;
}

// The current state when it is a CStateGame, else 0.
u32 game_state(const mt_cpu *cpu) {
    if (!globals_mapped(cpu)) {
        return 0u;
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    const u32 state = canvas != 0u ? read_word(cpu, canvas + k_canvas_state) : 0u;
    return state != 0u && read_word(cpu, state) == k_game_vptr ? state : 0u;
}

void decide_and_apply(mt_cpu *cpu) {
    const u32 e = mt_widescreen_decide(cpu, g_width.load(), g_allow.load() ? 1 : 0);
    mt_widescreen_apply(cpu, e);
}

// 0xC069 (map) and 0xC175 (map, x, y, smooth): the view width for the map, then the original.
int camera_hook(mt_cpu *cpu, const mt_hook_call *) {
    g_camera.fetch_add(1, std::memory_order_relaxed);
    if (!mt_widescreen_detail::menu_active()) { // a screen's paint (#169) keeps its 480
        decide_and_apply(cpu);
    }
    return MT_HOOK_PASS;
}

// 0x24DC9 (image, x, y): the field's backdrop at (0, 0) is stretched to the view's width, in
// clip space: x' = k x + (k - 1) w with k = E / 480 keeps the left edge and moves column 480 to
// column E. Every other image is drawn as the original draws it.
int draw_image_hook(mt_cpu *cpu, const mt_hook_call *call) {
    const u32 e = g_view.load();
    if (e <= MT_WIDESCREEN_NARROW || cpu->r[1] != 0u || cpu->r[2] != 0u ||
        mt_widescreen_detail::menu_active()) {
        return MT_HOOK_PASS;
    }
    const u32 state = game_state(cpu);
    if (state == 0u || cpu->r[0] != state + k_state_backdrop) {
        return MT_HOOK_PASS;
    }
    const float k = static_cast<float>(e) / static_cast<float>(MT_WIDESCREEN_NARROW);
    mt_gles_set_host_x_transform(k, k - 1.0f);
    call->original(cpu);
    mt_gles_set_host_x_transform(1.0f, 0.0f);
    g_backdrops.fetch_add(1, std::memory_order_relaxed);
    return MT_HOOK_HANDLED;
}

} // namespace

extern "C" void mt_widescreen_configure(std::uint32_t width) noexcept {
    const bool on = width > MT_WIDESCREEN_NARROW;
    g_width.store(on ? width : MT_WIDESCREEN_NARROW);
    g_view.store(MT_WIDESCREEN_NARROW);
    mt_hook_register(MT_HOOK_FN_CAMERA_CLAMP, camera_hook, nullptr, on ? 1 : 0);
    mt_hook_register(MT_HOOK_FN_CAMERA_FOLLOW, camera_hook, nullptr, on ? 1 : 0);
    mt_hook_register(MT_HOOK_FN_DRAW_IMAGE, draw_image_hook, nullptr, on ? 1 : 0);
    mt_widescreen_detail::menus_configure(on);
    mt_tracef("widescreen", "configured: %u", static_cast<unsigned>(g_width.load()));
}

extern "C" std::uint32_t mt_widescreen_width(void) noexcept { return g_width.load(); }

extern "C" std::uint32_t mt_widescreen_decide(const mt_cpu *cpu, std::uint32_t width,
                                              int allow) noexcept {
    if (width <= MT_WIDESCREEN_NARROW || allow == 0) {
        return MT_WIDESCREEN_NARROW;
    }
    const u32 state = game_state(cpu);
    if (state == 0u) {
        return MT_WIDESCREEN_NARROW;
    }
    // The field and the NPC chat (its box is centred on +0x90); the popup menu, the shop and the
    // map screen, drawn at 480 in the view's centre (#169, widescreen_menus.cpp), and the cash
    // shop (UIKit) over the wide field. Loading, exit and scenes stay 480.
    const u32 mode = read_word(cpu, k_game_mode);
    if (mode > 4u && mode != 0xBu) {
        return MT_WIDESCREEN_NARROW;
    }
    const u32 map = read_word(cpu, state + k_state_map);
    if (map == 0u) {
        return MT_WIDESCREEN_NARROW;
    }
    const auto map_width = static_cast<std::int32_t>(read_word(cpu, map + k_map_width));
    if (map_width <= static_cast<std::int32_t>(MT_WIDESCREEN_NARROW)) {
        return MT_WIDESCREEN_NARROW;
    }
    return std::min(static_cast<u32>(map_width), width);
}

extern "C" void mt_widescreen_apply(mt_cpu *cpu, std::uint32_t e) noexcept {
    if (!globals_mapped(cpu)) {
        return;
    }
    const u32 canvas = read_word(cpu, k_canvas_global);
    const u32 surface = read_word(cpu, k_surface_global);
    if (canvas == 0u || surface == 0u) {
        return; // before Screen_Init and the canvas: nothing to set yet
    }
    const u32 old = read_word(cpu, canvas + k_canvas_width);
    if (old == e && g_view.load() == e) {
        return;
    }
    write_word(cpu, surface, e);
    write_word(cpu, surface + k_surface_row_bytes, e * 4u);
    write_word(cpu, k_screen_width_global, e);
    write_word(cpu, canvas + k_canvas_width, e);
    write_word(cpu, canvas + k_canvas_half_width, e / 2u);
    // The clip rect, when it is the whole screen of the old width (0x233D5 clamps it to +0x88).
    if (read_word(cpu, canvas + k_canvas_clip_x0) == 0u &&
        read_word(cpu, canvas + k_canvas_clip_x1) == old) {
        write_word(cpu, canvas + k_canvas_clip_x1, e);
    }
    if (g_view.exchange(e) != e) {
        g_changes.fetch_add(1, std::memory_order_relaxed);
        mt_tracef("widescreen", "view width %u (was %u)", static_cast<unsigned>(e),
                  static_cast<unsigned>(old));
    }
}

extern "C" std::uint32_t mt_widescreen_turn(mt_cpu *cpu, int allow) noexcept {
    if (g_width.load() <= MT_WIDESCREEN_NARROW) {
        return MT_WIDESCREEN_NARROW;
    }
    g_allow.store(allow != 0);
    decide_and_apply(cpu);
    const u32 e = g_view.load();
    g_turns.fetch_add(1, std::memory_order_relaxed);
    if (e > MT_WIDESCREEN_NARROW) {
        g_wide_turns.fetch_add(1, std::memory_order_relaxed);
    }
    return e;
}

extern "C" std::uint32_t mt_widescreen_view(void) noexcept { return g_view.load(); }

extern "C" mt_widescreen_stats mt_widescreen_get_stats(void) noexcept {
    mt_widescreen_stats s{};
    s.turns = g_turns.load();
    s.wide_turns = g_wide_turns.load();
    s.changes = g_changes.load();
    s.camera_calls = g_camera.load();
    s.backdrops = g_backdrops.load();
    return s;
}

extern "C" void mt_widescreen_reset(void) noexcept {
    g_width.store(MT_WIDESCREEN_NARROW);
    g_view.store(MT_WIDESCREEN_NARROW);
    g_allow.store(false);
    g_turns.store(0);
    g_wide_turns.store(0);
    g_changes.store(0);
    g_camera.store(0);
    g_backdrops.store(0);
    mt_hook_unregister(MT_HOOK_FN_CAMERA_CLAMP);
    mt_hook_unregister(MT_HOOK_FN_CAMERA_FOLLOW);
    mt_hook_unregister(MT_HOOK_FN_DRAW_IMAGE);
    mt_widescreen_detail::menus_reset();
    mt_gles_set_host_x_transform(1.0f, 0.0f);
}
