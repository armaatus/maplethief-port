// Widescreen (#86, ADR 0090, maplethief/widescreen.h) on a synthetic guest: __DATA with the
// game's globals (SkyCanvas 0x11A950, the game mode 0x11AA00, the screen surface 0x11AA88 and
// width 0x11AA8C), a canvas, a CStateGame, its map and the surface on the heap. The view width's
// decision (field, NPC chat, menus, other states, the map's width, the host's word), what apply
// writes (the clip rect only when it is the whole screen), off as a no-op, and the hooked camera
// and backdrop draws as the generated code calls them (MT_FN_HOOK). The real game's run is
// play.widescreen_off / play.widescreen_on (play_real_test.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "maplethief/gles.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/widescreen.h"

namespace {

using u32 = std::uint32_t;

constexpr u32 k_canvas_global = 0x11A950u;
constexpr u32 k_game_mode = 0x11AA00u;
constexpr u32 k_surface_global = 0x11AA88u;
constexpr u32 k_screen_width = 0x11AA8Cu;
constexpr u32 k_game_vptr = 0x119B78u;

void put32(mt_cpu &cpu, u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
u32 get32(const mt_cpu &cpu, u32 addr) {
    u32 v = 0;
    std::memcpy(&v, cpu.mem + addr, 4);
    return v;
}

// The guest: its addresses, set up as Screen_Init(480, 320) and the field leave them.
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    u32 canvas = 0, state = 0, map = 0, surface = 0;

    Guest() {
        mt_widescreen_reset();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        const std::uint8_t none[4] = {};
        mt_segment seg{};
        std::strcpy(seg.name, "__DATA");
        seg.vmaddr = 0x11A000;
        seg.vmsize = 0x1000;
        seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
        REQUIRE(mt_mem_map_segment(mem, &seg, none, sizeof none) == 0);
        canvas = mt_heap_calloc(mem, 1, 0x100);
        state = mt_heap_calloc(mem, 1, 0x460);
        map = mt_heap_calloc(mem, 1, 0x80);
        surface = mt_heap_calloc(mem, 1, 0x20);
        REQUIRE((canvas != 0 && state != 0 && map != 0 && surface != 0));
        put32(cpu, k_canvas_global, canvas);
        put32(cpu, canvas + 0x0C, state);
        put32(cpu, canvas + 0x14, 0);   // clip x0
        put32(cpu, canvas + 0x1C, 480); // clip x1
        put32(cpu, canvas + 0x88, 480);
        put32(cpu, canvas + 0x8C, 320);
        put32(cpu, canvas + 0x90, 240);
        put32(cpu, canvas + 0x94, 160);
        put32(cpu, k_surface_global, surface);
        put32(cpu, surface, 480);
        put32(cpu, surface + 0x10, 480 * 4);
        put32(cpu, k_screen_width, 480);
        put32(cpu, state, k_game_vptr);
        put32(cpu, state + 0x2C, map);
        put32(cpu, map + 0x5C, 60 * 16); // 60 tiles
        put32(cpu, k_game_mode, 0);
    }
    ~Guest() {
        mt_widescreen_reset();
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;
};

// The hooked functions as the recompiler writes them (recomp/lift/core.py).
int g_camera_original = 0;
void camera_original(mt_cpu *MT_RESTRICT) { g_camera_original++; }
void hooked_clamp(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_CAMERA_CLAMP, camera_original); }

// The image draw's original records the host transform it was drawn with.
float g_draw_scale = 0.0f, g_draw_offset = 0.0f;
int g_draw_original = 0;
void draw_original(mt_cpu *MT_RESTRICT) {
    g_draw_original++;
    mt_gles_get_host_x_transform(&g_draw_scale, &g_draw_offset);
}
void hooked_draw(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_DRAW_IMAGE, draw_original); }

void draw(mt_cpu &cpu, u32 image, u32 x, u32 y) {
    cpu.r[0] = image;
    cpu.r[1] = x;
    cpu.r[2] = y;
    cpu.r[MT_LR] = 0x1235u;
    hooked_draw(&cpu);
}

} // namespace

TEST_CASE("widescreen: the field is as wide as its map, up to the configured width") {
    Guest g;
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 568u); // 60 tiles: 960 points
    put32(g.cpu, g.map + 0x5C, 35 * 16);                 // 35 tiles: 560 points
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 560u);
    put32(g.cpu, g.map + 0x5C, 30 * 16); // 30 tiles: the original 480
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    put32(g.cpu, g.map + 0x5C, 20 * 16); // narrower than the screen
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
}

TEST_CASE("widescreen: 480 off the field, while the host says no, or when off") {
    Guest g;
    CHECK(mt_widescreen_decide(&g.cpu, 568, 0) == 480u); // the key pad shown
    CHECK(mt_widescreen_decide(&g.cpu, 480, 1) == 480u); // off
    CHECK(mt_widescreen_decide(&g.cpu, 0, 1) == 480u);
    put32(g.cpu, k_game_mode, 4); // NPC chat: centred on +0x90, wide
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 568u);
    for (const u32 mode : {1u, 2u, 3u, 6u, 10u, 11u}) { // popup menu, shop, map, loading, cash
        put32(g.cpu, k_game_mode, mode);
        CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    }
    put32(g.cpu, k_game_mode, 0);
    put32(g.cpu, g.state + 0x2C, 0); // no map loaded
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    put32(g.cpu, g.state + 0x2C, g.map);
    put32(g.cpu, g.state, 0x119C98u); // a CStateTitle
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    put32(g.cpu, g.state, k_game_vptr);
    put32(g.cpu, k_canvas_global, 0); // no canvas yet
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
}

TEST_CASE("widescreen: a guest without the game's image is never read") {
    mt_widescreen_reset();
    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    CHECK(mt_widescreen_decide(&cpu, 568, 1) == 480u);
    mt_widescreen_apply(&cpu, 568); // nothing to write: no fault
    CHECK(mt_widescreen_view() == 480u);
    CHECK(mt_widescreen_decide(nullptr, 568, 1) == 480u);
    mt_widescreen_reset();
    mt_mem_destroy(mem);
}

TEST_CASE("widescreen: apply writes the surface, the width global and the canvas") {
    Guest g;
    mt_widescreen_apply(&g.cpu, 568);
    CHECK(get32(g.cpu, g.surface) == 568u);
    CHECK(get32(g.cpu, g.surface + 0x10) == 568u * 4u);
    CHECK(get32(g.cpu, k_screen_width) == 568u);
    CHECK(get32(g.cpu, g.canvas + 0x88) == 568u);
    CHECK(get32(g.cpu, g.canvas + 0x90) == 284u);
    CHECK(get32(g.cpu, g.canvas + 0x8C) == 320u); // the height never changes
    CHECK(get32(g.cpu, g.canvas + 0x94) == 160u);
    CHECK(get32(g.cpu, g.canvas + 0x1C) == 568u); // the full-screen clip follows
    CHECK(mt_widescreen_view() == 568u);
    // A clip the engine narrowed (a window's) is left as it is.
    put32(g.cpu, g.canvas + 0x14, 40);
    put32(g.cpu, g.canvas + 0x1C, 300);
    mt_widescreen_apply(&g.cpu, 480);
    CHECK(get32(g.cpu, g.canvas + 0x1C) == 300u);
    CHECK(get32(g.cpu, g.canvas + 0x88) == 480u);
    CHECK(get32(g.cpu, g.surface + 0x10) == 480u * 4u);
    CHECK(mt_widescreen_get_stats().changes == 2u);
}

TEST_CASE("widescreen: off, the turn changes nothing and no hook is on") {
    Guest g;
    mt_widescreen_configure(0);
    CHECK(mt_widescreen_width() == 480u);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CAMERA_CLAMP) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_DRAW_IMAGE) == 0);
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 480u);
    CHECK(get32(g.cpu, g.canvas + 0x88) == 480u);
    CHECK(mt_widescreen_get_stats().turns == 0u);
}

TEST_CASE("widescreen: on, the turn and the camera hook set the view width") {
    Guest g;
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    CHECK(mt_widescreen_width() == 568u);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CAMERA_CLAMP) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CAMERA_FOLLOW) == 1);
    CHECK(mt_widescreen_turn(&g.cpu, 0) == 480u); // the key pad shown
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 568u);
    CHECK(get32(g.cpu, g.canvas + 0x88) == 568u);
    // A map change between turns: the camera's clamp sees the new map's width first.
    put32(g.cpu, g.map + 0x5C, 30 * 16);
    g_camera_original = 0;
    hooked_clamp(&g.cpu);
    CHECK(g_camera_original == 1); // the original clamp still runs
    CHECK(get32(g.cpu, g.canvas + 0x88) == 480u);
    const mt_widescreen_stats st = mt_widescreen_get_stats();
    CHECK(st.turns == 2u);
    CHECK(st.wide_turns == 1u);
    CHECK(st.camera_calls == 1u);
}

TEST_CASE("widescreen: only the field's backdrop at (0, 0) is stretched to the view") {
    Guest g;
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    const u32 backdrop = g.state + 0x454;
    // At 480 points: drawn as the original draws it.
    g_draw_original = 0;
    draw(g.cpu, backdrop, 0, 0);
    CHECK(g_draw_original == 1);
    CHECK(g_draw_scale == 1.0f);
    CHECK(g_draw_offset == 0.0f);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u);
    draw(g.cpu, backdrop, 0, 0);
    CHECK(g_draw_original == 2);
    const float k = 568.0f / 480.0f;
    CHECK(g_draw_scale == doctest::Approx(k));
    CHECK(g_draw_offset == doctest::Approx(k - 1.0f));
    // Back to identity after it, and for any other image or place.
    float s = 0.0f, o = 1.0f;
    mt_gles_get_host_x_transform(&s, &o);
    CHECK(s == 1.0f);
    CHECK(o == 0.0f);
    draw(g.cpu, backdrop, 10, 0);
    CHECK(g_draw_scale == 1.0f);
    draw(g.cpu, g.state + 0x100, 0, 0);
    CHECK(g_draw_scale == 1.0f);
    CHECK(g_draw_original == 4);
    CHECK(mt_widescreen_get_stats().backdrops == 1u);
}
