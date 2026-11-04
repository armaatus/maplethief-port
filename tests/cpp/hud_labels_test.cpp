// The key pad's button labels (ADR 0101, hud_labels.h) with synthetic data only: the keys'
// table against the key pad's groups, the control each label shows (the defaults of both
// devices, none for a touch player, a remap through the overlay at once), the labels' places in
// the original layout, widescreen's edges and narrow, custom layouts (moved, scaled, flipped at
// the screen's edges), the glyph size per window scale, the span over the wide picture, the
// fade with the pad, a golden image of the labels over a synthetic pad, and the layer on a
// software renderer against the same drawing on the CPU.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "canvas.h"
#include "gamepad.h"
#include "glyphs.h"
#include "golden.h"
#include "hud_labels.h"
#include "input.h"
#include "keypad_layout.h"
#include "maplethief/input.h"
#include "prompts.h"
#include "remap.h"
#include "scaling.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
namespace golden = maplethief::golden;
using fe::Control;
using fe::ControlKind;
using fe::HudLabel;
using fe::KeypadGroup;
using fe::KeypadLayout;
using fe::KeypadRect;
using fe::KeypadWide;
using fe::LabelSide;
using fe::PromptDevice;
using fe::PromptStyle;

const PromptStyle kKeys{PromptDevice::kKeyboard, SDL_GAMEPAD_TYPE_UNKNOWN,
                        fe::FaceButtons::kPosition};
const PromptStyle kXbox{PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_XBOXONE,
                        fe::FaceButtons::kPosition};
const PromptStyle kPs5{PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_PS5, fe::FaceButtons::kPosition};
const PromptStyle kSwitch{PromptDevice::kGamepad, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
                          fe::FaceButtons::kPosition};

bool overlaps(const KeypadRect &a, const KeypadRect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool inside(const KeypadRect &r, float lo, float hi) {
    return r.x >= lo - 0.01f && r.x + r.w <= hi + 0.01f && r.y >= -0.01f && r.y + r.h <= 320.01f;
}

// What a label shows: the glyph's text for its control ("" for none).
std::string shown(std::uint32_t action, const PromptStyle &s,
                  const std::vector<fe::Binding> &bindings) {
    Control c;
    if (!fe::hud_label_control(bindings, action, s.device, &c)) {
        return "";
    }
    return c.kind == ControlKind::kKey ? fe::key_label(c.code)
                                       : fe::pad_glyph_label(c, s.type, s.face);
}

// The labels drawn over a transparent canvas spanning [lo, hi] at `k` pixels a point.
std::vector<fe::HudLabelDrawn> place_all(const std::vector<HudLabel> &labels, const PromptStyle &s,
                                         float lo, float hi, int k = 2) {
    const int w = static_cast<int>(hi - lo) * k, h = 320 * k;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
    fe::Canvas canvas(px.data(), static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                      fe::hud_label_unit(k));
    std::vector<fe::HudLabelDrawn> drawn;
    fe::draw_hud_labels(canvas, k, lo, hi, labels, s, fe::default_bindings(), &drawn);
    return drawn;
}

// Every drawn label: beside its key on `side` (within a point or two of its edge, centred on
// it), inside [lo, hi], over no key and no other label.
void check_places(const std::vector<HudLabel> &labels, const std::vector<fe::HudLabelDrawn> &drawn,
                  float lo, float hi) {
    REQUIRE(drawn.size() == labels.size());
    for (std::size_t i = 0; i < drawn.size(); i++) {
        const KeypadRect &b = drawn[i].box;
        const HudLabel &l = labels[drawn[i].label];
        CAPTURE(l.action);
        CAPTURE(b.x);
        CAPTURE(b.y);
        CHECK(inside(b, lo, hi));
        for (const HudLabel &other : labels) {
            CHECK_FALSE(overlaps(b, other.key));
        }
        for (std::size_t j = i + 1; j < drawn.size(); j++) {
            CHECK_FALSE(overlaps(b, drawn[j].box));
        }
    }
}

// The side a drawn label is on.
LabelSide side_of(const HudLabel &l, const KeypadRect &b) {
    if (b.y + b.h <= l.key.y + 0.01f) {
        return LabelSide::kAbove;
    }
    if (b.y >= l.key.y + l.key.h - 0.01f) {
        return LabelSide::kBelow;
    }
    return LabelSide::kLeft; // left or right: see the x
}

float gap(const HudLabel &l, const KeypadRect &b) {
    switch (side_of(l, b)) {
    case LabelSide::kAbove:
        return l.key.y - (b.y + b.h);
    case LabelSide::kBelow:
        return b.y - (l.key.y + l.key.h);
    default:
        return b.x + b.w <= l.key.x + 0.01f ? l.key.x - (b.x + b.w) : b.x - (l.key.x + l.key.w);
    }
}

} // namespace

TEST_CASE("hud labels: the keys' table: each key in its group, none over another") {
    const fe::HudKey *keys = fe::hud_keys();
    std::vector<std::uint32_t> actions;
    for (std::size_t i = 0; i < fe::kHudKeyCount; i++) {
        const fe::HudKey &k = keys[i];
        actions.push_back(k.action);
        if (k.grouped) {
            const KeypadRect g = fe::keypad_group_rect(k.group);
            CHECK(k.rect.x >= g.x);
            CHECK(k.rect.y >= g.y);
            CHECK(k.rect.x + k.rect.w <= g.x + g.w);
            CHECK(k.rect.y + k.rect.h <= g.y + g.h);
            KeypadGroup at{};
            REQUIRE(fe::keypad_group_of_frame(k.rect, &at));
            CHECK(at == k.group);
        } else {
            // Map, Bag and Cash: in no group (a layout never moves them).
            KeypadGroup at{};
            CHECK_FALSE(fe::keypad_group_of_frame(k.rect, &at));
        }
        for (std::size_t j = i + 1; j < fe::kHudKeyCount; j++) {
            CHECK_FALSE(overlaps(k.rect, keys[j].rect));
        }
    }
    CHECK(actions == std::vector<std::uint32_t>{MT_ACTION_JUMP, MT_ACTION_ATTACK, MT_ACTION_SLOT1,
                                                MT_ACTION_SLOT2, MT_ACTION_SLOT3, MT_ACTION_SLOT4,
                                                MT_ACTION_MAP, MT_ACTION_MENU,
                                                MT_ACTION_CASH_SHOP});
}

TEST_CASE("hud labels: each key's control in the field, per device and glyph set") {
    const std::vector<fe::Binding> b = fe::default_bindings();
    // The keyboard (docs/controls/keyboard.md): the first key of each.
    CHECK(shown(MT_ACTION_JUMP, kKeys, b) == "X");
    CHECK(shown(MT_ACTION_ATTACK, kKeys, b) == "Z");
    CHECK(shown(MT_ACTION_SLOT1, kKeys, b) == "A");
    CHECK(shown(MT_ACTION_SLOT2, kKeys, b) == "S");
    CHECK(shown(MT_ACTION_SLOT3, kKeys, b) == "D");
    CHECK(shown(MT_ACTION_SLOT4, kKeys, b) == "F");
    CHECK(shown(MT_ACTION_MAP, kKeys, b) == "M");
    CHECK(shown(MT_ACTION_MENU, kKeys, b) == "Esc");
    CHECK(shown(MT_ACTION_CASH_SHOP, kKeys, b) == "C");
    // An Xbox pad (docs/controls/keypad.md's mapping).
    CHECK(shown(MT_ACTION_JUMP, kXbox, b) == "A");
    CHECK(shown(MT_ACTION_ATTACK, kXbox, b) == "X");
    CHECK(shown(MT_ACTION_SLOT1, kXbox, b) == "Y");
    CHECK(shown(MT_ACTION_SLOT2, kXbox, b) == "B");
    CHECK(shown(MT_ACTION_SLOT3, kXbox, b) == "LB");
    CHECK(shown(MT_ACTION_SLOT4, kXbox, b) == "RB");
    CHECK(shown(MT_ACTION_MAP, kXbox, b) == "View");
    CHECK(shown(MT_ACTION_MENU, kXbox, b) == "Menu");
    CHECK(shown(MT_ACTION_CASH_SHOP, kXbox, b) == "LT");
    // PlayStation and Nintendo: the same controls, their own glyphs.
    CHECK(shown(MT_ACTION_JUMP, kPs5, b) == "Cross");
    CHECK(shown(MT_ACTION_ATTACK, kPs5, b) == "Square");
    CHECK(shown(MT_ACTION_SLOT3, kPs5, b) == "L1");
    CHECK(shown(MT_ACTION_CASH_SHOP, kPs5, b) == "L2");
    CHECK(shown(MT_ACTION_JUMP, kSwitch, b) == "B");
    CHECK(shown(MT_ACTION_ATTACK, kSwitch, b) == "Y");
    PromptStyle by_label = kSwitch;
    by_label.face = fe::FaceButtons::kLabel;
    CHECK(shown(MT_ACTION_JUMP, by_label, b) == "A");
    // A touch player (the device none): nothing.
    const PromptStyle none{};
    CHECK(shown(MT_ACTION_JUMP, none, b).empty());
    // An action without a control of the device: none either.
    std::vector<fe::Binding> pad_only = fe::default_gamepad_bindings();
    CHECK(shown(MT_ACTION_JUMP, kKeys, pad_only).empty());
    CHECK(shown(MT_ACTION_JUMP, kXbox, pad_only) == "A");
}

TEST_CASE("hud labels: a remap through the overlay shows at once") {
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    fe::Input input;
    fe::Actions actions{&input};
    fe::Gamepads gamepads{&actions, &input};
    fe::Remap remap{&actions, &gamepads};
    actions.set_sink([](void *, std::uint32_t, bool) {}, nullptr);
    actions.set_panel_source([](void *) { return std::int32_t{0}; }, nullptr);
    const fs::path dir = fs::temp_directory_path() / "mt_hud_labels_remap";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    remap.set_path(fe::bindings_path(dir.string(), ""));
    double now = 10;
    const auto send = [&](SDL_Keycode k, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.key = k;
        e.key.down = down;
        if (!remap.handle(e, now)) {
            input.handle(e);
        }
        remap.track(e);
        now += 0.05;
    };
    const auto press = [&](SDL_Keycode k) {
        send(k, true);
        send(k, false);
    };
    CHECK(shown(MT_ACTION_SLOT1, kKeys, actions.bindings()) == "A");
    // F1, down to quick slot 1 in the field, Enter, K, then Save.
    press(SDLK_F1);
    REQUIRE(remap.is_open());
    for (int i = 0; i < 64; i++) {
        const fe::Remap::Row &r = remap.rows()[remap.selected()];
        if (r.kind == fe::Remap::RowKind::kAction && r.slot.action == MT_ACTION_SLOT1 &&
            r.slot.context == fe::Context::kField) {
            break;
        }
        press(SDLK_DOWN);
    }
    press(SDLK_RETURN);
    REQUIRE(remap.capturing());
    press(SDLK_K);
    for (int i = 0; i < 64 && remap.rows()[remap.selected()].kind != fe::Remap::RowKind::kSave;
         i++) {
        press(SDLK_DOWN);
    }
    press(SDLK_RETURN);
    CHECK_FALSE(remap.is_open());
    // The label reads the bindings in effect: K now, A gone (and the number key 1 too: K is
    // the slot's only key).
    CHECK(shown(MT_ACTION_SLOT1, kKeys, actions.bindings()) == "K");
    CHECK(shown(MT_ACTION_SLOT1, kXbox, actions.bindings()) == "Y");
    // A bindings table edited as the overlay does: a gamepad button moved to jump.
    std::vector<fe::Binding> t = actions.bindings();
    fe::bind_control(&t, fe::Context::kField, MT_ACTION_JUMP,
                     Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_NORTH});
    CHECK(shown(MT_ACTION_JUMP, kXbox, t) == "Y");
    CHECK(shown(MT_ACTION_SLOT1, kXbox, t).empty()); // Y left slot 1
    fs::remove_all(dir, ec);
    SDL_Quit();
}

TEST_CASE("hud labels: beside each key in the original layout, for both devices") {
    const std::vector<HudLabel> labels = fe::hud_labels(KeypadLayout{}, KeypadWide{}, 1);
    REQUIRE(labels.size() == fe::kHudKeyCount);
    for (std::size_t i = 0; i < labels.size(); i++) {
        const KeypadRect &r = fe::hud_keys()[i].rect;
        CHECK(labels[i].key.x == r.x);
        CHECK(labels[i].key.y == r.y);
        CHECK(labels[i].alpha == 1);
    }
    for (const PromptStyle &s : {kKeys, kXbox, kPs5}) {
        for (const int k : {1, 2, 3, 4, 6}) {
            CAPTURE(k);
            const std::vector<fe::HudLabelDrawn> drawn = place_all(labels, s, 0, 480, k);
            check_places(labels, drawn, 0, 480);
            for (const fe::HudLabelDrawn &d : drawn) {
                const HudLabel &l = labels[d.label];
                CAPTURE(l.action);
                // On the key's own side (nothing in the original layout flips) and close.
                if (l.side == LabelSide::kLeft) {
                    CHECK(d.box.x + d.box.w <= l.key.x + 0.01f);
                } else {
                    CHECK(side_of(l, d.box) == l.side);
                }
                CHECK(gap(l, d.box) >= 0.0f);
                CHECK(gap(l, d.box) <= 2.0f);
            }
        }
    }
}

TEST_CASE("hud labels: widescreen's edges and narrow, and the span over the wide picture") {
    // 568 points: off 44. Edges: the d-pad 44 left, the others 44 right; Map, Bag, Cash stay.
    const KeypadWide edges = fe::keypad_wide(568, 568, true);
    const KeypadWide narrow = fe::keypad_wide(568, 568, false);
    const std::vector<HudLabel> e = fe::hud_labels(KeypadLayout{}, edges, 1);
    const std::vector<HudLabel> n = fe::hud_labels(KeypadLayout{}, narrow, 1);
    for (std::size_t i = 0; i < fe::kHudKeyCount; i++) {
        const fe::HudKey &k = fe::hud_keys()[i];
        CHECK(e[i].key.x == k.rect.x + (k.grouped ? 44.0f : 0.0f));
        CHECK(n[i].key.x == k.rect.x);
    }
    // The span of a 568-point picture at 2x in a 1136 x 640 window: [-44, 524].
    const fe::Placement p = fe::place(fe::ScaleMode::kInteger, 1136, 640, 1136, 640, {}, 1, 1, 568);
    fe::Placement img = p;
    const fe::Rect wide = p.rect;
    img.rect = fe::Rect{wide.x + 88, wide.y, 960, 640};
    float lo = 0, hi = 0;
    fe::hud_label_span(img, wide, &lo, &hi);
    CHECK(lo == -44);
    CHECK(hi == 524);
    // Without widescreen: the screen image's [0, 480].
    fe::hud_label_span(img, img.rect, &lo, &hi);
    CHECK(lo == 0);
    CHECK(hi == 480);
    for (const PromptStyle &s : {kKeys, kXbox}) {
        const std::vector<fe::HudLabelDrawn> de = place_all(e, s, -44, 524);
        check_places(e, de, -44, 524);
        // The quick slots' labels in the right wing's reach: left of keys at x 490.
        for (const fe::HudLabelDrawn &d : de) {
            if (e[d.label].action == MT_ACTION_SLOT1) {
                CHECK(d.box.x + d.box.w <= 490.01f);
                CHECK(d.box.x + d.box.w >= 488.0f);
            }
        }
        check_places(n, place_all(n, s, -44, 524), -44, 524);
    }
}

TEST_CASE("hud labels: custom layouts move, scale and flip them") {
    KeypadLayout layout;
    // The quick slots 150 % and moved left and down; jump and attack half size, to the top.
    REQUIRE(fe::parse_keypad_layout("slots=150,-120,20,60;actions=50,-100,-300,100", &layout));
    const std::vector<HudLabel> labels = fe::hud_labels(layout, KeypadWide{}, 1);
    for (std::size_t i = 0; i < fe::kHudKeyCount; i++) {
        const fe::HudKey &k = fe::hud_keys()[i];
        if (!k.grouped) {
            continue;
        }
        const fe::KeypadTransform t =
            fe::keypad_view_transform(k.group, layout[k.group], KeypadWide{});
        const fe::KeypadPoint a = t.apply({k.rect.x, k.rect.y});
        CHECK(labels[i].key.x == doctest::Approx(a.x));
        CHECK(labels[i].key.y == doctest::Approx(a.y));
        CHECK(labels[i].key.w == doctest::Approx(k.rect.w * t.scale));
        // A group faded to 60 % keeps its labels opaque (the slots' icons are GL, unfaded).
        CHECK(labels[i].alpha == 1);
    }
    const std::vector<fe::HudLabelDrawn> drawn = place_all(labels, kKeys, 0, 480);
    check_places(labels, drawn, 0, 480);
    for (const fe::HudLabelDrawn &d : drawn) {
        const HudLabel &l = labels[d.label];
        if (l.action == MT_ACTION_JUMP || l.action == MT_ACTION_ATTACK) {
            // At the top of the screen (the group's rect clamped to it) there is no room above:
            // below the key.
            CHECK(l.key.y < d.box.h);
            CHECK(side_of(l, d.box) == LabelSide::kBelow);
        }
        if (l.action == MT_ACTION_SLOT2) {
            CHECK(d.box.x + d.box.w <= l.key.x + 0.01f);
        }
    }
    // The quick slots against the left edge (the keys 16 points in): a label that does not fit
    // left of its key goes right of it (LB, RB); a disc (Y, B) still fits on the left.
    REQUIRE(fe::parse_keypad_layout("slots=100,-480,0,100", &layout));
    const std::vector<HudLabel> left = fe::hud_labels(layout, KeypadWide{}, 1);
    const std::vector<fe::HudLabelDrawn> dl = place_all(left, kXbox, 0, 480);
    check_places(left, dl, 0, 480);
    int flipped = 0;
    for (const fe::HudLabelDrawn &d : dl) {
        const HudLabel &l = left[d.label];
        if (l.action >= MT_ACTION_SLOT1 && l.action <= MT_ACTION_SLOT4) {
            CAPTURE(l.action);
            CHECK(l.key.x == 16);
            CHECK(gap(l, d.box) <= 2.0f);
            if (d.box.w + fe::kHudLabelGap > l.key.x) {
                CHECK(d.box.x >= l.key.x + l.key.w - 0.01f);
                flipped++;
            } else {
                CHECK(d.box.x + d.box.w <= l.key.x + 0.01f);
            }
        }
    }
    CHECK(flipped == 2);
}

TEST_CASE("hud labels: the glyph size per window scale, never under the font's pixels") {
    CHECK(fe::hud_label_unit(0) == 1);
    CHECK(fe::hud_label_unit(1) == 1);
    CHECK(fe::hud_label_unit(2) == 2);
    CHECK(fe::hud_label_unit(3) == 2);
    CHECK(fe::hud_label_unit(4) == 3);
    CHECK(fe::hud_label_unit(6) == 5);
    CHECK(fe::hud_label_unit(8) == 6);
    for (int k = 1; k <= 8; k++) {
        // At least one pixel a font pixel (12 pixels high), at most a prompt's size.
        CHECK(fe::hud_label_unit(k) >= 1);
        CHECK(fe::hud_label_unit(k) <= k);
    }
}

TEST_CASE("hud labels: the keys without a GL icon fade with the pad") {
    const std::vector<HudLabel> half = fe::hud_labels(KeypadLayout{}, KeypadWide{}, 0.5f);
    for (const HudLabel &l : half) {
        const bool gl = l.action == MT_ACTION_ATTACK ||
                        (l.action >= MT_ACTION_SLOT1 && l.action <= MT_ACTION_SLOT4);
        CHECK(l.alpha == (gl ? 1.0f : 0.5f));
    }
    // Hidden: jump, Map, Bag and Cash drawn not at all; the others as before.
    const std::vector<HudLabel> hidden = fe::hud_labels(KeypadLayout{}, KeypadWide{}, 0);
    CHECK(place_all(hidden, kXbox, 0, 480).size() == 5);
    // A faded label: its pixels times its alpha.
    std::vector<std::uint8_t> px(480u * 2 * 320 * 2 * 4, 0), full;
    fe::Canvas canvas(px.data(), 960, 640, 2);
    std::vector<fe::HudLabelDrawn> drawn;
    REQUIRE(fe::draw_hud_labels(canvas, 2, 0, 480, half, kXbox, fe::default_bindings(), &drawn) ==
            9);
    full = px;
    fe::fade_hud_labels(px.data(), 960, half, drawn);
    for (const fe::HudLabelDrawn &d : drawn) {
        const std::size_t i = (static_cast<std::size_t>(d.y + 1) * 960 + d.x + 1) * 4 + 3;
        if (half[d.label].alpha < 1) {
            CHECK(px[i] == (full[i] * 128 + 127) / 255);
        } else {
            CHECK(px[i] == full[i]);
        }
    }
}

TEST_CASE("hud labels: a synthetic pad with its labels (golden), edges and narrow") {
    // The key pad's keys as plain frames on a plain field (no game art), 568 points wide at 2
    // pixels a point: widescreen's edges with the keyboard's labels over the top half, narrow
    // with an Xbox pad's over the bottom half's own copy.
    constexpr int k = 2, wpts = 568, w = wpts * k, h = 320 * k;
    const auto picture = [&](const KeypadWide &wide, const PromptStyle &s) {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
                p[0] = static_cast<std::uint8_t>(60 + (x * 60) / w);
                p[1] = static_cast<std::uint8_t>(110 + (y * 50) / h);
                p[2] = 70;
                p[3] = 255;
            }
        }
        fe::Canvas keys(px.data(), w, h, k);
        const std::vector<HudLabel> labels = fe::hud_labels(KeypadLayout{}, wide, 1);
        for (const HudLabel &l : labels) {
            keys.frame(static_cast<int>(l.key.x) + 44, static_cast<int>(l.key.y),
                       static_cast<int>(l.key.w), static_cast<int>(l.key.h), 1,
                       fe::Rgba{230, 230, 230, 255});
        }
        fe::Canvas canvas(px.data(), w, h, fe::hud_label_unit(k));
        CHECK(fe::draw_hud_labels(canvas, k, -44, 524, labels, s, fe::default_bindings()) == 9);
        return px;
    };
    const std::vector<std::uint8_t> top = picture(fe::keypad_wide(568, 568, true), kKeys);
    const std::vector<std::uint8_t> bottom = picture(fe::keypad_wide(568, 568, false), kXbox);
    golden::Image img(w, 2 * h);
    std::memcpy(img.bytes().data(), top.data(), top.size());
    std::memcpy(img.bytes().data() + top.size(), bottom.data(), bottom.size());
    const auto r = golden::check_golden(
        img, std::filesystem::path(MT_GOLDEN_DATA_DIR) / "hud_labels_synthetic.png",
        "hud_labels_synthetic");
    CHECK_MESSAGE(r.passed, r.message);
}

TEST_CASE("hud labels: the layer on a renderer equals the drawing over the image") {
    // A 1136 x 640 window: the 568-point picture at 2x, the screen image in its middle.
    SDL_Surface *target = SDL_CreateSurface(1136, 640, SDL_PIXELFORMAT_ABGR8888);
    REQUIRE(target != nullptr);
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(target);
    REQUIRE_MESSAGE(renderer != nullptr, SDL_GetError());
    fe::Placement img;
    img.rect = fe::Rect{88, 0, 960, 640};
    const fe::Rect wide{0, 0, 1136, 640};
    const std::vector<fe::Binding> bindings = fe::default_bindings();
    const std::vector<HudLabel> labels =
        fe::hud_labels(KeypadLayout{}, fe::keypad_wide(568, 568, true), 1);

    const auto frame = [&](const std::vector<HudLabel> &l, const PromptStyle &s,
                           const std::vector<fe::Binding> &b, fe::HudLabelLayer &layer) {
        SDL_SetRenderDrawColor(renderer, 60, 110, 150, 255);
        SDL_RenderClear(renderer);
        CHECK(layer.draw(renderer, img, wide, l, s, b));
        SDL_FlushRenderer(renderer);
        const auto *src = static_cast<const std::uint8_t *>(target->pixels);
        std::vector<std::uint8_t> px(1136u * 640u * 4u);
        for (int y = 0; y < 640; y++) {
            std::memcpy(&px[static_cast<std::size_t>(y) * 1136 * 4],
                        src + static_cast<std::size_t>(y) * static_cast<std::size_t>(target->pitch),
                        1136 * 4);
        }
        return px;
    };

    fe::HudLabelLayer layer;
    const std::vector<std::uint8_t> drawn = frame(labels, kXbox, bindings, layer);
    CHECK(layer.redraws() == 1);
    CHECK(layer.drawn() == 9);
    // The same drawing on the CPU over the same colour, the canvas at the wide picture's left.
    std::vector<std::uint8_t> cpu(1136u * 640u * 4u);
    for (std::size_t i = 0; i < cpu.size(); i += 4) {
        cpu[i] = 60;
        cpu[i + 1] = 110;
        cpu[i + 2] = 150;
        cpu[i + 3] = 255;
    }
    fe::Canvas canvas(cpu.data(), 1136, 640, fe::hud_label_unit(2));
    CHECK(fe::draw_hud_labels(canvas, 2, -44, 524, labels, kXbox, bindings) == 9);
    int worst = 0, changed = 0;
    for (std::size_t i = 0; i < cpu.size(); i++) {
        worst = std::max(worst, std::abs(static_cast<int>(drawn[i]) - static_cast<int>(cpu[i])));
        changed += (i % 4 != 3 && cpu[i] != (i % 4 == 0 ? 60 : i % 4 == 1 ? 110 : 150)) ? 1 : 0;
    }
    CHECK(changed > 0);
    CHECK(worst <= 2);

    // Drawn anew only when something changed: the same frame, then the keyboard, then a remap.
    frame(labels, kXbox, bindings, layer);
    CHECK(layer.redraws() == 1);
    frame(labels, kKeys, bindings, layer);
    CHECK(layer.redraws() == 2);
    std::vector<fe::Binding> remapped = bindings;
    fe::bind_control(&remapped, fe::Context::kField, MT_ACTION_ATTACK,
                     Control{ControlKind::kKey, static_cast<int>(SDLK_K)});
    frame(labels, kKeys, remapped, layer);
    CHECK(layer.redraws() == 3);
    // A layout change moves them: drawn anew.
    KeypadLayout moved;
    REQUIRE(fe::parse_keypad_layout("slots=100,-100,0,100", &moved));
    frame(fe::hud_labels(moved, fe::keypad_wide(568, 568, true), 1), kKeys, remapped, layer);
    CHECK(layer.redraws() == 4);
    // A touch player: nothing over the picture.
    const std::vector<std::uint8_t> bare = frame(labels, PromptStyle{}, bindings, layer);
    for (std::size_t i = 0; i < bare.size(); i += 4) {
        if (bare[i] != 60 || bare[i + 1] != 110 || bare[i + 2] != 150) {
            FAIL("a pixel changed with no device");
            break;
        }
    }
    layer.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(target);
}
