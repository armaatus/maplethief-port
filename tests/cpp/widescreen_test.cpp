// Widescreen (#86, ADR 0090, maplethief/widescreen.h) on a synthetic guest: __DATA with the
// game's globals (SkyCanvas 0x11A950, the game mode 0x11AA00, the screen surface 0x11AA88 and
// width 0x11AA8C), a canvas, a CStateGame, its map and the surface on the heap. The view width's
// decision (field, NPC chat, menus, other states, the map's width, the host's word), what apply
// writes (the clip rect only when it is the whole screen), off as a no-op, the hooked camera
// and backdrop draws as the generated code calls them (MT_FN_HOOK), and the menus' screens drawn
// at 480 in the view's centre with the popup menu's dim stretched (#169), and the scenes (#396):
// wide only when listed, their speech box wrapped at the 3:2 width and drawn in the centre;
// widescreen = fill (#425): the host's limit on the view, and the backdrop tiled when its texels
// repeat every 160 points (synthetic images bound through the GL imports), else stretched. The
// real game's runs are play.widescreen_off / play.widescreen_on and
// play.widescreen_scene_<n>_off_synthetic / _on_synthetic (play_real_test.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

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
        mt_gles_reset(); // no texture bound
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
        mt_gles_reset();
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;
};

// The hooked functions as the recompiler writes them (recomp/lift/core.py).
int g_camera_original = 0;
void camera_original(mt_cpu *MT_RESTRICT) { g_camera_original++; }
void hooked_clamp(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_CAMERA_CLAMP, camera_original); }

// The image draw's original records the host transform it was drawn with, and runs the draw
// filter installed (as its glDrawArrays would) on the 480-point image's box in a GL layer
// g_draw_layer points wide, keeping what it answered.
float g_draw_scale = 0.0f, g_draw_offset = 0.0f;
int g_draw_original = 0, g_draw_filtered = -1;
float g_draw_m[6] = {};
float g_draw_layer = 568.0f;
u32 g_draw_image = 0; // r0 of the last call
void draw_original(mt_cpu *MT_RESTRICT cpu) {
    g_draw_original++;
    g_draw_image = cpu->r[0];
    cpu->r[0] = cpu->r[1] = cpu->r[2] = 0xDEADu; // a call clobbers its argument registers
    mt_gles_get_host_x_transform(&g_draw_scale, &g_draw_offset);
    void *user = nullptr;
    const mt_gles_draw_filter filter = mt_gles_get_draw_filter(&user);
    g_draw_filtered = -1;
    if (filter != nullptr) {
        const float box[4] = {-1.0f, -1.0f, 960.0f / g_draw_layer - 1.0f, 1.0f};
        float m[6] = {1, 0, 0, 1, 0, 0};
        g_draw_filtered = filter(user, box, m);
        std::memcpy(g_draw_m, m, sizeof m);
    }
}
void hooked_draw(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_DRAW_IMAGE, draw_original); }

// A screen's paint (#169) records what the engine and the GL layer look like while it runs, and
// draws the popup menu's dim through the hooked FillRect as 0x56B99 does.
struct Seen {
    u32 width = 0, half = 0, screen = 0, surface = 0, row = 0, clip = 0;
    float scale = 0.0f, offset = 0.0f;
    int calls = 0;
};
Seen g_seen;
u32 g_canvas = 0, g_surface = 0, g_stack = 0;
float g_fill_scale = 0.0f, g_fill_offset = 0.0f, g_dim_scale = 0.0f, g_dim_offset = 0.0f;
int g_fill_original = 0, g_camera_inside = 0;
void fill_original(mt_cpu *MT_RESTRICT cpu) {
    g_fill_original++;
    mt_gles_get_host_x_transform(&g_fill_scale, &g_fill_offset);
    if (cpu->r[3] == 480u) {
        g_dim_scale = g_fill_scale;
        g_dim_offset = g_fill_offset;
    }
}
void hooked_fill(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_FILL_RECT, fill_original); }
void fill(mt_cpu &cpu, u32 x, u32 y, u32 w, u32 h) {
    cpu.r[0] = 0x1000;
    cpu.r[1] = x;
    cpu.r[2] = y;
    cpu.r[3] = w;
    cpu.r[MT_SP] = g_stack;
    put32(cpu, g_stack, h);
    put32(cpu, g_stack + 4, g_canvas + 0x10);
    hooked_fill(&cpu);
}
void screen_original(mt_cpu *MT_RESTRICT cpu) {
    g_seen.calls++;
    g_seen.width = get32(*cpu, g_canvas + 0x88);
    g_seen.half = get32(*cpu, g_canvas + 0x90);
    g_seen.screen = get32(*cpu, k_screen_width);
    g_seen.surface = get32(*cpu, g_surface);
    g_seen.row = get32(*cpu, g_surface + 0x10);
    g_seen.clip = get32(*cpu, g_canvas + 0x1C);
    mt_gles_get_host_x_transform(&g_seen.scale, &g_seen.offset);
    fill(*cpu, 0, 0, 480, 320); // the dim
    fill(*cpu, 74, 28, 332, 264);
    // Map_PaintSub0 moves +0x90 by 10 and back; a camera call inside leaves the width alone.
    put32(*cpu, g_canvas + 0x90, get32(*cpu, g_canvas + 0x90) + 10);
    hooked_clamp(cpu);
    g_camera_inside = static_cast<int>(get32(*cpu, g_canvas + 0x88));
    put32(*cpu, g_canvas + 0x90, get32(*cpu, g_canvas + 0x90) - 10);
}
void hooked_popup(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_POPUP_MENU_PAINT, screen_original);
}
void hooked_map(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_MAP_PAINT_0, screen_original); }
void hooked_chat_box(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_CHAT_BOX_DRAW, screen_original);
}

// Text_Wrap's original records the width it was given.
u32 g_wrap_width = 0;
void wrap_original(mt_cpu *MT_RESTRICT cpu) { g_wrap_width = cpu->r[2]; }
void hooked_wrap(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_TEXT_WRAP, wrap_original); }
u32 wrap(mt_cpu &cpu, u32 width, u32 lr) {
    cpu.r[0] = 0x2000; // the owner, the text, &count: passed through
    cpu.r[1] = 0x3000;
    cpu.r[2] = width;
    cpu.r[3] = 0x4000;
    cpu.r[MT_LR] = lr;
    hooked_wrap(&cpu);
    return g_wrap_width;
}

} // namespace

extern "C" {
void mt_hle_glGenTextures(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_glBindTexture(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_glTexImage2D(mt_cpu *cpu, std::uint32_t id) noexcept;
}

namespace {

// A backdrop's texture as the game uploads it: a 512 x 512 RGBA8 texture with the 480 x 320
// image in its top-left corner, `scale` texels a point (a pack's replacement: scale 2 gives
// 1024 x 1024). The image repeats every 160 points; with `band`, rows 96 ... 224 don't (as
// backdrops 11 and 12).
std::vector<std::uint8_t> backdrop(u32 scale, bool band) {
    const u32 w = 512 * scale, h = 512 * scale;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    for (u32 y = 0; y < 320 * scale; y++) {
        for (u32 x = 0; x < 480 * scale; x++) {
            std::uint8_t *p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            const u32 col = band && y >= 96 * scale && y < 224 * scale ? x : x % (160 * scale);
            p[0] = static_cast<std::uint8_t>(col * 7);
            p[1] = static_cast<std::uint8_t>(y * 3);
            p[2] = static_cast<std::uint8_t>(col / 3 + y);
            p[3] = 255;
        }
    }
    return rgba;
}

// Binds a texture with `rgba` (w x h) through the GL imports, as the backdrop's draw does.
void bind_backdrop(Guest &g, u32 w, u32 h, const std::vector<std::uint8_t> &rgba) {
    const u32 names = mt_heap_calloc(g.mem, 1, 4);
    const u32 pixels = mt_heap_calloc(g.mem, 1, static_cast<u32>(rgba.size()));
    const u32 stack = mt_heap_calloc(g.mem, 1, 32);
    REQUIRE((names != 0 && pixels != 0 && stack != 0));
    std::memcpy(g.cpu.mem + pixels, rgba.data(), rgba.size());
    g.cpu.r[0] = 1;
    g.cpu.r[1] = names;
    mt_hle_glGenTextures(&g.cpu, 0);
    const u32 name = get32(g.cpu, names);
    g.cpu.r[0] = 0x0DE1; // GL_TEXTURE_2D
    g.cpu.r[1] = name;
    mt_hle_glBindTexture(&g.cpu, 0);
    // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels)
    const u32 sp = g.cpu.r[MT_SP];
    g.cpu.r[MT_SP] = stack;
    const u32 rest[5] = {h, 0, 0x1908, 0x1401, pixels};
    std::memcpy(g.cpu.mem + stack, rest, sizeof rest);
    g.cpu.r[0] = 0x0DE1;
    g.cpu.r[1] = 0;
    g.cpu.r[2] = 0x1908;
    g.cpu.r[3] = w;
    mt_hle_glTexImage2D(&g.cpu, 0);
    g.cpu.r[MT_SP] = sp;
    REQUIRE(mt_gles_bound_texture_gfx() != 0u);
}

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
    // NPC chat (centred on +0x90), the popup menu, the shop, the map screen (drawn in the view's
    // centre, #169) and the cash shop (UIKit): wide behind them.
    for (const u32 mode : {4u, 1u, 2u, 3u, 11u}) {
        put32(g.cpu, k_game_mode, mode);
        CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 568u);
    }
    // Loading, the waiting box, scene 0 (unlisted, #396) and a scene without its list: 3:2.
    for (const u32 mode : {5u, 6u, 7u, 8u, 9u, 10u, 12u}) {
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

TEST_CASE("widescreen: game modes 9 and 10 are wide only for the listed scenes (#396)") {
    Guest g;
    put32(g.cpu, g.state + 0x38, 0x1000u); // the scene's command list
    u32 listed = 0;
    for (u32 scene = 0; scene <= 20; scene++) {
        put32(g.cpu, g.state + 0x1E4, scene);
        const bool wide = mt_widescreen_scene_wide(scene) != 0;
        listed += wide ? 1u : 0u;
        for (const u32 mode : {9u, 10u}) {
            put32(g.cpu, k_game_mode, mode);
            CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == (wide ? 568u : 480u));
            CHECK(mt_widescreen_decide(&g.cpu, 480, 1) == 480u); // off
            CHECK(mt_widescreen_decide(&g.cpu, 568, 0) == 480u); // the host says no
        }
    }
    CHECK(listed == 7u);
    for (const u32 scene : {2u, 11u, 13u, 15u, 16u, 19u, 20u}) {
        CHECK(mt_widescreen_scene_wide(scene) == 1);
    }
    CHECK(mt_widescreen_scene_wide(21u) == 0);
    CHECK(mt_widescreen_scene_wide(0xFFFFFFFFu) == 0);
    // A listed scene is as wide as its map: 35 tiles, 560; 30 tiles, the original 480.
    put32(g.cpu, g.state + 0x1E4, 16u);
    put32(g.cpu, k_game_mode, 9u);
    put32(g.cpu, g.map + 0x5C, 35 * 16);
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 560u);
    put32(g.cpu, g.map + 0x5C, 30 * 16);
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    put32(g.cpu, g.map + 0x5C, 60 * 16);
    // Game mode 9 with no command list (the runner has nothing to run): 480. Queued (10): the
    // list comes with StartScene, so none is needed yet.
    put32(g.cpu, g.state + 0x38, 0u);
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 480u);
    put32(g.cpu, k_game_mode, 10u);
    CHECK(mt_widescreen_decide(&g.cpu, 568, 1) == 568u);
}

TEST_CASE("widescreen: off, a listed scene changes nothing (#396)") {
    Guest g;
    mt_widescreen_configure(480);
    put32(g.cpu, g.state + 0x38, 0x1000u);
    put32(g.cpu, g.state + 0x1E4, 16u);
    for (const u32 mode : {0u, 10u, 9u}) {
        put32(g.cpu, k_game_mode, mode);
        CHECK(mt_widescreen_turn(&g.cpu, 1) == 480u);
    }
    CHECK(get32(g.cpu, g.canvas + 0x88) == 480u);
    CHECK(mt_widescreen_get_stats().changes == 0u);
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_WRAP) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CHAT_BOX_DRAW) == 0);
}

TEST_CASE("widescreen scenes: the speech box is wrapped at 3:2 and drawn in the centre (#396)") {
    Guest g;
    g_canvas = g.canvas;
    g_surface = g.surface;
    g_stack = mt_heap_calloc(g.mem, 1, 0x40);
    REQUIRE(g_stack != 0);
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_WRAP) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CHAT_BOX_DRAW) == 1);
    put32(g.cpu, g.state + 0x38, 0x1000u);
    put32(g.cpu, g.state + 0x1E4, 16u);
    put32(g.cpu, k_game_mode, 9u);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u);
    // The runner's command 50 (the call at 0x85158) passes +0x88 - 24: the 3:2 456 instead.
    constexpr u32 runner = 0x8515Du;
    CHECK(wrap(g.cpu, 568 - 24, runner) == 456u);
    CHECK(mt_widescreen_get_menu_stats().box_wraps == 1u);
    // Every other caller (an NPC's dialogue page, the menus) wraps as the original.
    CHECK(wrap(g.cpu, 568 - 24, 0x34B29u) == 544u);
    CHECK(mt_widescreen_get_menu_stats().box_wraps == 1u);
    // CChatBox::drawChatBox: at 480 points, 44 points right; the wide words afterwards.
    g_seen = Seen{};
    hooked_chat_box(&g.cpu);
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.width == 480u);
    CHECK(g_seen.half == 240u);
    CHECK(g_seen.offset == doctest::Approx(88.0f / 568.0f));
    CHECK(get32(g.cpu, g.canvas + 0x88) == 568u);
    // An unlisted scene is drawn at 480: the box and its wrap as the original's.
    put32(g.cpu, g.state + 0x1E4, 14u);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 480u);
    CHECK(wrap(g.cpu, 480 - 24, runner) == 456u);
    CHECK(mt_widescreen_get_menu_stats().box_wraps == 1u);
    g_seen = Seen{};
    hooked_chat_box(&g.cpu);
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.width == 480u);
    CHECK(g_seen.offset == 0.0f);
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
    g_draw_layer = 568.0f;
    const u32 backdrop = g.state + 0x454;
    // At 480 points: drawn as the original draws it, unfiltered.
    g_draw_original = 0;
    draw(g.cpu, backdrop, 0, 0);
    CHECK(g_draw_original == 1);
    CHECK(g_draw_scale == 1.0f);
    CHECK(g_draw_offset == 0.0f);
    CHECK(g_draw_filtered == -1);
    // Wide, with no texture to measure (nothing repeats): its draw is stretched by the filter,
    // x' = k x + (k - 1) in device coordinates, keeping the left edge and moving 480 to 568.
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u);
    draw(g.cpu, backdrop, 0, 0);
    CHECK(g_draw_original == 2);
    const float k = 568.0f / 480.0f;
    CHECK(g_draw_scale == 1.0f);
    CHECK(g_draw_filtered == 1);
    CHECK(g_draw_m[0] == doctest::Approx(k));
    CHECK(g_draw_m[3] == 1.0f);
    CHECK(g_draw_m[4] == doctest::Approx(k - 1.0f));
    CHECK(g_draw_m[5] == 0.0f);
    // The filter and the transform are gone after it, and for any other image or place.
    CHECK(mt_gles_get_draw_filter(nullptr) == nullptr);
    float s = 0.0f, o = 1.0f;
    mt_gles_get_host_x_transform(&s, &o);
    CHECK(s == 1.0f);
    CHECK(o == 0.0f);
    draw(g.cpu, backdrop, 10, 0);
    CHECK(g_draw_filtered == -1);
    draw(g.cpu, g.state + 0x100, 0, 0);
    CHECK(g_draw_filtered == -1);
    CHECK(g_draw_original == 4);
    CHECK(mt_widescreen_get_stats().backdrops == 1u);
    CHECK(mt_widescreen_get_stats().tiles == 0u);
}

TEST_CASE("widescreen fill: a backdrop that repeats every 160 points is tiled at x = 480 (#425)") {
    Guest g;
    mt_widescreen_configure(MT_WIDESCREEN_FILL);
    g_draw_layer = 768.0f;
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 768u); // 60 tiles: 960 points
    bind_backdrop(g, 512, 512, backdrop(1, false));
    g_draw_original = 0;
    draw(g.cpu, g.state + 0x454, 0, 0);
    // Drawn as it is (the filter left it alone), then again 480 points right: 960 / 768 of clip
    // space.
    CHECK(g_draw_original == 2);
    CHECK(g_draw_image == g.state + 0x454); // the copy draws the same image
    CHECK(g_draw_scale == 1.0f);
    CHECK(g_draw_offset == doctest::Approx(960.0f / 768.0f));
    const mt_widescreen_stats st = mt_widescreen_get_stats();
    CHECK(st.tiles == 1u);
    CHECK(st.backdrops == 0u);
    float s = 0.0f, o = 1.0f;
    mt_gles_get_host_x_transform(&s, &o);
    CHECK(s == 1.0f);
    CHECK(o == 0.0f);
    CHECK(mt_gles_get_draw_filter(nullptr) == nullptr);
    // At 568 (auto's width) the same backdrop is tiled too: not stretched.
    mt_widescreen_set_limit(568);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u);
    draw(g.cpu, g.state + 0x454, 0, 0);
    CHECK(g_draw_original == 4);
    CHECK(mt_widescreen_get_stats().tiles == 2u);
    // Backdrops 11 and 12's kind (a middle band that doesn't repeat): stretched.
    bind_backdrop(g, 512, 512, backdrop(1, true));
    draw(g.cpu, g.state + 0x454, 0, 0);
    CHECK(g_draw_original == 5);
    CHECK(g_draw_filtered == 1);
    CHECK(g_draw_m[0] == doctest::Approx(568.0f / 480.0f));
    CHECK(mt_widescreen_get_stats().backdrops == 1u);
}

TEST_CASE("widescreen fill: the repeat check on synthetic images (#425)") {
    const std::vector<std::uint8_t> tiled = backdrop(1, false), band = backdrop(1, true);
    CHECK(mt_widescreen_backdrop_repeats(tiled.data(), 512, 512, 1) == 1);
    CHECK(mt_widescreen_backdrop_repeats(band.data(), 512, 512, 1) == 0);
    // A pack's replacement at twice the size: the period is 320 texels.
    const std::vector<std::uint8_t> tiled2 = backdrop(2, false), band2 = backdrop(2, true);
    CHECK(mt_widescreen_backdrop_repeats(tiled2.data(), 1024, 1024, 2) == 1);
    CHECK(mt_widescreen_backdrop_repeats(band2.data(), 1024, 1024, 2) == 0);
    // One texel off in the last period: stretched.
    std::vector<std::uint8_t> off = tiled;
    off[(static_cast<std::size_t>(300) * 512 + 470) * 4 + 1] ^= 1;
    CHECK(mt_widescreen_backdrop_repeats(off.data(), 512, 512, 1) == 0);
    // Beyond the image (x >= 480 or y >= 320) the texture doesn't count.
    std::vector<std::uint8_t> pad = tiled;
    pad[(static_cast<std::size_t>(400) * 512 + 100) * 4] ^= 0xFF;
    pad[(static_cast<std::size_t>(10) * 512 + 500) * 4] ^= 0xFF;
    CHECK(mt_widescreen_backdrop_repeats(pad.data(), 512, 512, 1) == 1);
    // Too small to hold two periods, or nothing: not tiled.
    CHECK(mt_widescreen_backdrop_repeats(tiled.data(), 300, 512, 1) == 0);
    CHECK(mt_widescreen_backdrop_repeats(nullptr, 512, 512, 1) == 0);
    CHECK(mt_widescreen_backdrop_repeats(tiled.data(), 512, 512, 0) == 0);
}

TEST_CASE("widescreen fill: the view is capped by the host's limit, the window at 320 high") {
    Guest g;
    mt_widescreen_configure(MT_WIDESCREEN_FILL);
    CHECK(mt_widescreen_width() == 768u);
    CHECK(mt_widescreen_reach() == 768u);
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 768u); // 60 tiles: no limit, the layer's 768
    mt_widescreen_set_limit(568);                 // a 16:9 window
    CHECK(mt_widescreen_reach() == 568u);
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 568u);
    mt_widescreen_set_limit(692); // 19.5:9
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 692u);
    put32(g.cpu, g.map + 0x5C, 40 * 16); // a 40-tile map (640 points) is narrower: its width
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 640u);
    put32(g.cpu, g.map + 0x5C, 60 * 16);
    mt_widescreen_set_limit(2000); // wider than 21:9: the layer's 768
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 768u);
    mt_widescreen_set_limit(400); // a portrait window: 480
    CHECK(mt_widescreen_reach() == 480u);
    CHECK(mt_widescreen_turn(&g.cpu, 1) == 480u);
    // auto keeps 568 whatever the limit says above it; configure and reset forget it.
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    CHECK(mt_widescreen_reach() == 568u);
    mt_widescreen_set_limit(768);
    CHECK(mt_widescreen_reach() == 568u);
    mt_widescreen_reset();
    CHECK(mt_widescreen_reach() == 480u);
}

TEST_CASE("widescreen menus: a screen is drawn at 480 points in the wide view's centre") {
    Guest g;
    g_canvas = g.canvas;
    g_surface = g.surface;
    g_stack = mt_heap_calloc(g.mem, 1, 0x40);
    REQUIRE(g_stack != 0);
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    for (const u32 fn : {MT_HOOK_FN_POPUP_MENU_PAINT, MT_HOOK_FN_SHOP_PAINT, MT_HOOK_FN_MAP_PAINT_0,
                         MT_HOOK_FN_MAP_PAINT_1, MT_HOOK_FN_MAP_PAINT_2, MT_HOOK_FN_FILL_RECT}) {
        CHECK(mt_hook_enabled(fn) == 1);
    }
    put32(g.cpu, k_game_mode, 1);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u); // the popup menu over the wide field
    g_seen = Seen{};
    g_fill_original = 0;
    g.cpu.r[0] = 0x2000; // the data manager, the game: passed through as they are
    g.cpu.r[1] = g.state;
    hooked_popup(&g.cpu);
    // The paint saw the original's 480 points (and the full-screen clip of 480) ...
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.width == 480u);
    CHECK(g_seen.half == 240u);
    CHECK(g_seen.screen == 480u);
    CHECK(g_seen.surface == 480u);
    CHECK(g_seen.row == 480u * 4u);
    CHECK(g_seen.clip == 480u);
    // ... drawn 44 points right: (568 - 480) / 2 points of clip space's 568 / 2 a unit.
    CHECK(g_seen.scale == 1.0f);
    CHECK(g_seen.offset == doctest::Approx(88.0f / 568.0f));
    // Its dim over (0, 0, 480, 320) is stretched over the 568 columns; its window is not.
    CHECK(g_fill_original == 2);
    const float k = 568.0f / 480.0f; // the backdrop's stretch: columns 0 ... 568
    CHECK(g_dim_scale == doctest::Approx(k));
    CHECK(g_dim_offset == doctest::Approx(k - 1.0f));
    CHECK(g_fill_scale == 1.0f); // the last fill: the window, centred
    CHECK(g_fill_offset == doctest::Approx(88.0f / 568.0f));
    CHECK(mt_widescreen_get_menu_stats().dims == 1u);
    // The camera's hook inside it left the width at 480.
    CHECK(g_camera_inside == 480);
    // Afterwards: the wide view's words, the clip and the identity transform again.
    CHECK(get32(g.cpu, g.canvas + 0x88) == 568u);
    CHECK(get32(g.cpu, g.canvas + 0x90) == 284u);
    CHECK(get32(g.cpu, k_screen_width) == 568u);
    CHECK(get32(g.cpu, g.surface) == 568u);
    CHECK(get32(g.cpu, g.surface + 0x10) == 568u * 4u);
    CHECK(get32(g.cpu, g.canvas + 0x1C) == 568u);
    float s = 0.0f, o = 1.0f;
    mt_gles_get_host_x_transform(&s, &o);
    CHECK(s == 1.0f);
    CHECK(o == 0.0f);
    CHECK(mt_widescreen_get_menu_stats().screens == 1u);
    // Outside the popup menu, the same fill is not the dim: drawn as it is.
    fill(g.cpu, 0, 0, 480, 320);
    CHECK(g_fill_scale == 1.0f);
    CHECK(g_fill_offset == 0.0f);
    // A map screen's view on a 35-tile map (560 points): 40 points right.
    put32(g.cpu, k_game_mode, 3);
    put32(g.cpu, g.map + 0x5C, 35 * 16);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 560u);
    hooked_map(&g.cpu);
    CHECK(g_seen.calls == 2);
    CHECK(g_seen.width == 480u);
    CHECK(g_seen.offset == doctest::Approx(80.0f / 568.0f));
    CHECK(mt_widescreen_get_menu_stats().dims == 1u); // the map has no dim to stretch
    CHECK(get32(g.cpu, g.canvas + 0x88) == 560u);
}

TEST_CASE("widescreen menus: at 480 points or off, a screen is drawn as the original draws it") {
    Guest g;
    g_canvas = g.canvas;
    g_surface = g.surface;
    g_stack = mt_heap_calloc(g.mem, 1, 0x40);
    REQUIRE(g_stack != 0);
    // On, but a 30-tile map: the view is 480, nothing moves.
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    put32(g.cpu, k_game_mode, 1);
    put32(g.cpu, g.map + 0x5C, 30 * 16);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 480u);
    g_seen = Seen{};
    hooked_popup(&g.cpu);
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.width == 480u);
    CHECK(g_seen.offset == 0.0f);
    CHECK(g_fill_scale == 1.0f);
    CHECK(mt_widescreen_get_menu_stats().screens == 0u);
    // Off: no hook of the menus is on.
    mt_widescreen_configure(0);
    for (const u32 fn : {MT_HOOK_FN_POPUP_MENU_PAINT, MT_HOOK_FN_SHOP_PAINT, MT_HOOK_FN_MAP_PAINT_0,
                         MT_HOOK_FN_MAP_PAINT_1, MT_HOOK_FN_MAP_PAINT_2, MT_HOOK_FN_FILL_RECT}) {
        CHECK(mt_hook_enabled(fn) == 0);
    }
    hooked_popup(&g.cpu);
    CHECK(g_seen.calls == 2);
    CHECK(g_seen.offset == 0.0f);
}

namespace {

// HUD_Paint's original records the canvas's width and the host transform it ran with (#168).
u32 g_hud_width = 0, g_hud_half = 0;
float g_hud_scale = 0.0f, g_hud_offset = 0.0f;
int g_hud_original = 0;
const mt_cpu *g_hud_cpu = nullptr;
u32 g_hud_canvas = 0;
void hud_original(mt_cpu *MT_RESTRICT) {
    g_hud_original++;
    g_hud_width = get32(*g_hud_cpu, g_hud_canvas + 0x88);
    g_hud_half = get32(*g_hud_cpu, g_hud_canvas + 0x90);
    mt_gles_get_host_x_transform(&g_hud_scale, &g_hud_offset);
}
void hooked_hud(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_HUD_PAINT, hud_original); }

} // namespace

TEST_CASE("widescreen: the field HUD is laid out at 480 and drawn in the middle (#168)") {
    Guest g;
    g_hud_cpu = &g.cpu;
    g_hud_canvas = g.canvas;
    g.cpu.r[MT_LR] = 0x1235u;
    g_hud_original = 0;
    // Off: the hook is registered but off.
    mt_widescreen_configure(0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_HUD_PAINT) == 0);
    hooked_hud(&g.cpu);
    CHECK(g_hud_original == 1);
    CHECK(g_hud_offset == 0.0f);

    // On, the view 568 wide: laid out on 480 / 240, moved right by 44 points of the 568-point
    // layer (88 / 568 of clip space), then the width is back.
    mt_widescreen_configure(MT_WIDESCREEN_WIDE);
    CHECK(mt_hook_enabled(MT_HOOK_FN_HUD_PAINT) == 1);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 568u);
    hooked_hud(&g.cpu);
    CHECK(g_hud_original == 2);
    CHECK(g_hud_width == 480u);
    CHECK(g_hud_half == 240u);
    CHECK(g_hud_scale == 1.0f);
    CHECK(g_hud_offset == doctest::Approx(88.0f / 568.0f));
    CHECK(get32(g.cpu, g.canvas + 0x88) == 568u);
    CHECK(get32(g.cpu, g.canvas + 0x90) == 284u);
    float s = 0.0f, o = 1.0f;
    mt_gles_get_host_x_transform(&s, &o);
    CHECK(s == 1.0f);
    CHECK(o == 0.0f);
    CHECK(mt_widescreen_get_stats().huds == 1u);

    // A 3:2 view (a 30-tile map): nothing to keep, the original as it is.
    put32(g.cpu, g.map + 0x5C, 30 * 16);
    REQUIRE(mt_widescreen_turn(&g.cpu, 1) == 480u);
    hooked_hud(&g.cpu);
    CHECK(g_hud_width == 480u);
    CHECK(g_hud_offset == 0.0f);
    CHECK(mt_widescreen_get_stats().huds == 1u);

    // Configured off again: the hook is off.
    mt_widescreen_configure(0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_HUD_PAINT) == 0);
}
