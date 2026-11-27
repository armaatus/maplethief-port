// The input display (#404, input_display.h, ADR 0156) with synthetic data only: the action layer
// (Actions over the runtime's mt_input_press / mt_input_release) driven by keys and a gamepad
// lights exactly the cells it delivers, the touch key pad's keys light theirs, the place in a
// border or over the picture, goldens of the panel alone on a synthetic frame, the layer on a
// software renderer, and the setting's row (outside the presets: presets_test).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "golden.h"
#include "input_display.h"
#include "maplethief/input.h"
#include "scaling.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace golden = maplethief::golden;
using u32 = std::uint32_t;

std::int32_t g_panel = 0;

u32 bit(u32 action) { return 1u << action; }

fe::Control key(SDL_Keycode k) { return {fe::ControlKind::kKey, static_cast<int>(k)}; }
fe::Control button(SDL_GamepadButton b) { return {fe::ControlKind::kGamepadButton, b}; }
fe::Control direction(fe::Direction d) {
    return {fe::ControlKind::kDirection, static_cast<int>(d)};
}

// The names of the lit cells, in the cells' order (readable failures).
std::string lit_names(u32 lit) {
    std::string s;
    for (const fe::InputDisplayCell &c : fe::input_display_cells()) {
        if ((lit & bit(c.action)) != 0) {
            s += s.empty() ? "" : " ";
            s += mt_input_action_get_info(c.action)->name;
        }
    }
    return s;
}

u32 nothing_held(u32) { return 0; }

golden::Image frame_with_panel(u32 lit, int unit) {
    // A plain synthetic frame (no game art), the panel a margin in from its top left.
    const int m = fe::kInputDisplayMargin;
    const int w = (fe::kInputDisplayW + 2 * m) * unit, h = (fe::kInputDisplayH + 2 * m) * unit;
    golden::Image img(w, h, {0x40, 0x70, 0x50, 255});
    std::vector<std::uint8_t> px = img.bytes();
    fe::Canvas c(px.data(), static_cast<u32>(w), static_cast<u32>(h), unit);
    fe::draw_input_display(c, m, m, lit);
    golden::Image out(w, h);
    out.bytes() = px;
    return out;
}

} // namespace

TEST_CASE("input display: the action layer lights the cells it delivers") {
    mt_input_configure(nullptr);
    fe::Actions actions(nullptr);
    actions.set_panel_source([](void *) { return g_panel; }, nullptr);
    g_panel = 0; // the field
    CHECK(lit_names(fe::input_display_lit_now()).empty());

    // Keys: the left arrow walks, Z attacks, 2 is quick slot 2.
    CHECK(actions.control(fe::InputSource::kKeyboard, key(SDLK_LEFT), true) == MT_ACTION_LEFT);
    CHECK(actions.control(fe::InputSource::kKeyboard, key(SDLK_Z), true) == MT_ACTION_ATTACK);
    CHECK(actions.control(fe::InputSource::kKeyboard, key(SDLK_2), true) == MT_ACTION_SLOT2);
    CHECK(lit_names(fe::input_display_lit_now()) == "left attack slot2");

    // A pad as well: its south button jumps, its d-pad up climbs. Two devices on one action
    // keep it lit until the last lets go.
    CHECK(actions.control(fe::InputSource::kGamepad, button(SDL_GAMEPAD_BUTTON_SOUTH), true) ==
          MT_ACTION_JUMP);
    CHECK(actions.control(fe::InputSource::kGamepad, direction(fe::Direction::kLeft), true) ==
          MT_ACTION_LEFT);
    CHECK(lit_names(fe::input_display_lit_now()) == "left jump attack slot2");
    actions.control(fe::InputSource::kKeyboard, key(SDLK_LEFT), false);
    CHECK(lit_names(fe::input_display_lit_now()) == "left jump attack slot2");
    actions.release_source(fe::InputSource::kGamepad);
    CHECK(lit_names(fe::input_display_lit_now()) == "attack slot2");
    actions.release_source(fe::InputSource::kKeyboard);
    CHECK(lit_names(fe::input_display_lit_now()).empty());

    // direction_priority = last: only the newest direction is delivered, so only it lights.
    actions.set_direction_priority(fe::DirectionPriority::kLast);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_LEFT), true);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_UP), true);
    CHECK(lit_names(fe::input_display_lit_now()) == "up");
    actions.control(fe::InputSource::kKeyboard, key(SDLK_UP), false);
    CHECK(lit_names(fe::input_display_lit_now()) == "left");
    actions.release_source(fe::InputSource::kKeyboard);
    actions.set_direction_priority(fe::DirectionPriority::kGame);

    // Menus: Enter confirms, Esc goes back; the field's menu key and M the map.
    g_panel = 1;
    actions.control(fe::InputSource::kKeyboard, key(SDLK_RETURN), true);
    CHECK(lit_names(fe::input_display_lit_now()) == "ok");
    actions.control(fe::InputSource::kKeyboard, key(SDLK_RETURN), false);
    actions.control(fe::InputSource::kKeyboard, key(SDLK_ESCAPE), true);
    CHECK(lit_names(fe::input_display_lit_now()) == "back");
    actions.release_source(fe::InputSource::kKeyboard);
    g_panel = 0;
    actions.control(fe::InputSource::kKeyboard, key(SDLK_M), true);
    CHECK(lit_names(fe::input_display_lit_now()) == "map");
    actions.release_source(fe::InputSource::kKeyboard);

    // The port's own actions never reach the guest and have no cell: fast-forward lights nothing.
    actions.control(fe::InputSource::kKeyboard, key(SDLK_TAB), true);
    CHECK(lit_names(fe::input_display_lit_now()).empty());
    actions.release_source(fe::InputSource::kKeyboard);
    mt_input_configure(nullptr);
}

TEST_CASE("input display: the touch key pad's keys light their cells") {
    mt_input_configure(nullptr);
    // Without a game there is no pad: no keys.
    CHECK(mt_input_pad_keys() == 0);
    // A finger on the d-pad's up arm and on quick slot 4 (TouchKeyPadView's BOOLs), nothing held
    // by a device.
    CHECK(lit_names(fe::input_display_lit(nothing_held,
                                          bit(MT_ACTION_UP) | bit(MT_ACTION_SLOT4))) == "up slot4");
    // Bits of no cell (a port action) light nothing.
    CHECK(fe::input_display_lit(nothing_held, bit(MT_ACTION_FAST_FORWARD)) == 0);
    // Both sources together.
    CHECK(lit_names(fe::input_display_lit([](u32 a) { return a == MT_ACTION_JUMP ? 1u : 0u; },
                                          bit(MT_ACTION_RIGHT))) == "right jump");
}

TEST_CASE("input display: the cells and their labels") {
    const auto &cells = fe::input_display_cells();
    REQUIRE(cells.size() == 14);
    for (const fe::InputDisplayCell &c : cells) {
        CAPTURE(c.action);
        CHECK(c.x >= 0);
        CHECK(c.y >= 0);
        CHECK(c.x + c.w <= fe::kInputDisplayW);
        CHECK(c.y + c.h <= fe::kInputDisplayH);
        CHECK(c.dpad == fe::input_display_label(c.action).empty());
        if (!c.dpad) {
            CHECK(fe::Canvas::text_width(fe::input_display_label(c.action)) <= c.w - 2);
        }
        CHECK(!fe::port_action(c.action));
    }
    // No two cells overlap.
    for (std::size_t i = 0; i < cells.size(); i++) {
        for (std::size_t j = i + 1; j < cells.size(); j++) {
            const auto &a = cells[i];
            const auto &b = cells[j];
            CAPTURE(i);
            CAPTURE(j);
            CHECK((a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y));
        }
    }
}

TEST_CASE("input display: in the border when it has room, else over the picture's left edge") {
    // A 1080p window, the picture 3x (1440 x 960) in the middle: a 240-pixel bar each side. The
    // frame's scale 3 does not fit the bar, 2 does: bottom left of the bar.
    const fe::Rect safe{0, 0, 1920, 1080};
    fe::InputDisplayPlace p = fe::input_display_place({240, 60, 1440, 960}, safe, 3);
    CHECK(p.scale == 2);
    CHECK(p.rect.x == 8);
    CHECK(p.rect.y == 1080 - 8 - fe::kInputDisplayH * 2);
    CHECK(p.rect.x + p.rect.w <= 240);
    // A tall window: the bar under the picture.
    p = fe::input_display_place({0, 0, 960, 640}, {0, 0, 960, 900}, 2);
    CHECK(p.scale == 2);
    CHECK(p.rect.y >= 640);
    CHECK(p.rect.y + p.rect.h <= 900);
    // The picture fills the window: over it, between the Map button and the d-pad.
    p = fe::input_display_place({0, 0, 1440, 960}, {0, 0, 1440, 960}, 3);
    CHECK(p.scale == 3);
    CHECK(p.rect.x == 12);
    CHECK(p.rect.y == 56 * 3);
    CHECK(p.rect.y + p.rect.h <= 172 * 3);
    // Small: 480 x 320 at scale 1 still fits between them.
    p = fe::input_display_place({0, 0, 480, 320}, {0, 0, 480, 320}, 1);
    CHECK(p.scale == 1);
    CHECK(p.rect.y == 56);
    CHECK(p.rect.y + p.rect.h <= 172);
    // A notch: the safe area's left inset pushes it right.
    p = fe::input_display_place({0, 0, 1440, 960}, {90, 0, 1350, 960}, 3);
    CHECK(p.rect.x == 90 + 12);
    // No picture yet: nothing.
    p = fe::input_display_place({}, safe, 2);
    CHECK(p.rect.w == 0);
}

TEST_CASE("input display: the panel alone on a synthetic frame, against goldens") {
    struct Case {
        const char *name;
        u32 lit;
        int unit;
    };
    const Case cases[] = {
        {"idle", 0, 2},
        {"held",
         bit(MT_ACTION_LEFT) | bit(MT_ACTION_JUMP) | bit(MT_ACTION_SLOT3) | bit(MT_ACTION_OK), 2},
    };
    for (const Case &k : cases) {
        CAPTURE(k.name);
        const std::string name = std::string("input_display_") + k.name;
        const auto r =
            golden::check_golden(frame_with_panel(k.lit, k.unit),
                                 std::filesystem::path(MT_GOLDEN_DATA_DIR) / (name + ".png"), name,
                                 {.channel = 0, .max_differing_ratio = 0});
        CHECK_MESSAGE(r.passed, r.message);
    }
    // The layer's pixels are the panel over transparent: premultiplied, the backing translucent.
    const std::vector<std::uint8_t> px = fe::render_input_display(0, 1);
    REQUIRE(px.size() == std::size_t{fe::kInputDisplayW} * fe::kInputDisplayH * 4);
    CHECK(px[3] == 0xB0);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        CHECK_FALSE((px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3]));
    }
    // A lit cell changes its pixels, and only inside its cell.
    const std::vector<std::uint8_t> lit = fe::render_input_display(bit(MT_ACTION_ATTACK), 1);
    const fe::InputDisplayCell *attack = nullptr;
    for (const fe::InputDisplayCell &c : fe::input_display_cells()) {
        attack = c.action == MT_ACTION_ATTACK ? &c : attack;
    }
    REQUIRE(attack != nullptr);
    int changed = 0;
    for (int y = 0; y < fe::kInputDisplayH; y++) {
        for (int x = 0; x < fe::kInputDisplayW; x++) {
            const std::size_t i = (std::size_t(y) * fe::kInputDisplayW + x) * 4;
            if (px[i] != lit[i] || px[i + 1] != lit[i + 1] || px[i + 2] != lit[i + 2]) {
                changed++;
                CHECK((x >= attack->x && x < attack->x + attack->w && y >= attack->y &&
                       y < attack->y + attack->h));
            }
        }
    }
    CHECK(changed > 0);
}

TEST_CASE("input display: the layer on a software renderer") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE(SDL_CreateWindowAndRenderer("input_display_test", 960, 640, 0, &window, &renderer));
    {
        fe::InputDisplayLayer layer;
        // No place: nothing drawn.
        CHECK(layer.draw(renderer, {}, 0));
        CHECK(layer.draws() == 0);
        const fe::InputDisplayPlace at =
            fe::input_display_place({0, 0, 960, 640}, {0, 0, 960, 640}, 2);
        CHECK(layer.draw(renderer, at, 0));
        CHECK(layer.draw(renderer, at, 0));
        CHECK(layer.draws() == 2);
        CHECK(layer.redraws() == 1); // the same cells: the texture kept
        CHECK(layer.draw(renderer, at, bit(MT_ACTION_JUMP)));
        CHECK(layer.redraws() == 2);
        layer.stop();
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

TEST_CASE("input display: the setting") {
    const fe::SettingInfo *row = nullptr;
    for (const fe::SettingInfo &r : fe::settings_registry()) {
        row = std::string(r.key) == "input_display" ? &r : row;
    }
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kVideo);
    CHECK_FALSE(row->presets.in_presets());
    CHECK(row->applies == fe::Applies::kAtOnce);
    fe::Settings s;
    CHECK_FALSE(s.input_display);
    std::string why;
    REQUIRE(fe::apply_setting(&s, "input_display", "on", &why));
    CHECK(s.input_display);
}
