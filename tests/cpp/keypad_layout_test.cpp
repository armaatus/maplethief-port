// The customisable touch key pad (#159, ADR 0086) with synthetic data only: keypad_layout's
// text in settings.ini and on the command line, the clamping on the screen, where the hit mask
// is read for a touch (keypad_mask_point) and how a GL draw moves with its group, both hooks
// (-[TouchKeyPadView colorAtPixel:onImage:], QuickSlots_Paint) on synthetic calls as the
// recompiler writes them, the layout on the views of a synthetic game pad (the original:
// nothing changes), and the editor's touches, pinches, keys, gamepad and drawing. The real
// game's touches at moved keys are gamepad_real_test's "touch-layout" mode
// (input.keypad_layout).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "keypad_editor.h"
#include "keypad_layout.h"
#include "maplethief/cf.h"
#include "maplethief/gles.h"
#include "maplethief/hle/imports.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/uikit.h"
#include "settings.h"
#include "uikit/uikit_internal.h"

namespace {

namespace fe = maplethief::frontend;
using fe::GroupLayout;
using fe::KeypadEditor;
using fe::KeypadGroup;
using fe::KeypadLayout;
using fe::KeypadPoint;
using fe::KeypadRect;
using u32 = std::uint32_t;

KeypadLayout layout_of(const char *text) {
    KeypadLayout l;
    REQUIRE(fe::parse_keypad_layout(text, &l));
    return l;
}

bool inside_screen(const KeypadRect &r) {
    return r.x >= -0.001f && r.y >= -0.001f && r.x + r.w <= 480.001f && r.y + r.h <= 320.001f;
}

} // namespace

TEST_CASE("keypad layout: the text, original by default, in settings.ini and on the command line") {
    KeypadLayout l;
    CHECK(l.original());
    CHECK(fe::keypad_layout_text(l) == "original");
    REQUIRE(fe::parse_keypad_layout(" Original ", &l));
    CHECK(l.original());

    REQUIRE(fe::parse_keypad_layout("dpad=80,12,-20,70", &l));
    CHECK(l[KeypadGroup::kDpad] == GroupLayout{80, 12, -20, 70});
    CHECK(l[KeypadGroup::kActions].original());
    CHECK(fe::keypad_layout_text(l) == "dpad=80,12,-20,70");
    // Any order, any case, blanks, a trailing ';'; written back in the order dpad, actions, slots.
    REQUIRE(fe::parse_keypad_layout(" SLOTS = 50, +0, 4, 0 ; dpad=200,-480,480,100;", &l));
    CHECK(l[KeypadGroup::kSlots] == GroupLayout{50, 0, 4, 0});
    CHECK(l[KeypadGroup::kDpad] == GroupLayout{200, -480, 480, 100});
    CHECK(fe::keypad_layout_text(l) == "dpad=200,-480,480,100;slots=50,0,4,0");
    KeypadLayout back;
    REQUIRE(fe::parse_keypad_layout(fe::keypad_layout_text(l), &back));
    CHECK(back == l);
    // A group set to its original values is left out; all of them is `original`.
    REQUIRE(fe::parse_keypad_layout("actions=100,0,0,100", &l));
    CHECK(l.original());
    CHECK(fe::keypad_layout_text(l) == "original");

    const KeypadLayout kept = layout_of("actions=120,0,-6,100");
    l = kept;
    for (const char *bad :
         {"", ";", "dpad", "dpad=", "dpad=80,0,0", "dpad=80,0,0,70,1", "dpad=49,0,0,100",
          "dpad=201,0,0,100", "dpad=100,481,0,100", "dpad=100,0,-481,100", "dpad=100,0,0,101",
          "dpad=100,0,0,-1", "dpad=-100,0,0,100", "dpad=1e2,0,0,100",
          "dpad=100,0,0,50;dpad=90,0,0,50", "stick=100,0,0,100",
          "dpad=100,0,0,100;;slots=90,0,0,100", "originals"}) {
        CHECK_FALSE(fe::parse_keypad_layout(bad, &l));
        CHECK(l == kept); // unchanged
    }

    // settings.ini and the command line (apply_setting, as main.cpp's --keypad-layout).
    fe::Settings s;
    CHECK(s.keypad_layout.original());
    CHECK(fe::parse_settings("keypad_layout = slots=90,-10,0,80\n", &s).empty());
    CHECK(s.keypad_layout[KeypadGroup::kSlots] == GroupLayout{90, -10, 0, 80});
    CHECK(fe::parse_settings("keypad_layout = sideways\n", &s).size() == 1);
    CHECK(s.keypad_layout[KeypadGroup::kSlots] == GroupLayout{90, -10, 0, 80});
    const std::string fresh = fe::serialise_settings(s);
    CHECK(fresh.find("\nkeypad_layout = slots=90,-10,0,80\n") != std::string::npos);
    fe::Settings again;
    CHECK(fe::parse_settings(fresh, &again).empty());
    CHECK(again == s);
    CHECK(fe::serialise_settings(fe::Settings{}).find("\nkeypad_layout = original\n") !=
          std::string::npos);
    std::string why;
    CHECK(fe::apply_setting(&s, "keypad_layout", "original", &why));
    CHECK(s.keypad_layout.original());
    CHECK_FALSE(fe::apply_setting(&s, "keypad_layout", "dpad=10,0,0,0", &why));
}

TEST_CASE("keypad layout: each group scales about its centre and stays on the screen") {
    for (const KeypadGroup g : fe::kKeypadGroups) {
        const KeypadRect r = fe::keypad_group_rect(g);
        // The original: the identity, its own rect.
        CHECK(fe::keypad_group_transform(g, GroupLayout{}).identity());
        const KeypadRect same = fe::keypad_drawn_rect(g, GroupLayout{});
        CHECK(same.x == r.x);
        CHECK(same.w == r.w);
        // Every size and a far offset: on the screen, clamped as drawn.
        for (const std::uint32_t scale : {50u, 75u, 100u, 150u, 200u}) {
            for (const auto &[dx, dy] : {std::pair{0, 0}, std::pair{-480, -480},
                                         std::pair{480, 480}, std::pair{-480, 480}}) {
                const GroupLayout v{scale, dx, dy, 100};
                const KeypadRect d = fe::keypad_drawn_rect(g, v);
                CHECK(d.w == doctest::Approx(r.w * static_cast<float>(scale) / 100));
                if (d.w <= 480 && d.h <= 320) {
                    CHECK(inside_screen(d));
                }
                const GroupLayout c = fe::keypad_clamped(g, v);
                CHECK(c.scale == scale);
                const KeypadRect dc = fe::keypad_drawn_rect(g, c);
                CHECK(std::abs(dc.x - d.x) <= 0.5f); // whole points
                CHECK(std::abs(dc.y - d.y) <= 0.5f);
                CHECK(fe::keypad_clamped(g, c) == c); // clamping twice changes nothing
            }
        }
    }
    // The d-pad at half its size keeps its centre (84, 248): (42, 212, 84, 72); moved 20 right.
    const KeypadRect half = fe::keypad_drawn_rect(KeypadGroup::kDpad, GroupLayout{50, 20, 0, 100});
    CHECK(half.x == doctest::Approx(62));
    CHECK(half.y == doctest::Approx(212));
    CHECK(half.w == doctest::Approx(84));
    CHECK(half.h == doctest::Approx(72));
    // Twice its size, it is held at the screen's left and bottom edges.
    const KeypadRect big = fe::keypad_drawn_rect(KeypadGroup::kDpad, GroupLayout{200, 0, 0, 100});
    CHECK(big.x == doctest::Approx(0));
    CHECK(big.y + big.h == doctest::Approx(320));
    CHECK(fe::keypad_clamped(KeypadGroup::kDpad, GroupLayout{200, 0, 0, 100}).dx == 84);
}

TEST_CASE("keypad layout: the hit mask is read under the drawn key, or at no key") {
    // The transparent point lies outside every group's rect.
    for (const KeypadGroup g : fe::kKeypadGroups) {
        CHECK_FALSE(fe::keypad_group_rect(g).contains(fe::kKeypadNoKeyPoint));
    }
    // The original: every point is read where it is.
    const KeypadLayout original;
    for (const KeypadPoint p : {KeypadPoint{30, 250}, KeypadPoint{370, 290}, KeypadPoint{460, 100},
                                KeypadPoint{240, 160}, KeypadPoint{0, 0}}) {
        const KeypadPoint q = fe::keypad_mask_point(original, p);
        CHECK(q.x == p.x);
        CHECK(q.y == p.y);
    }
    // The d-pad moved 100 right and 40 up: a touch on its drawn left arm reads the original
    // left arm; its old place, where nothing is drawn now, reads no key; the jump key and the
    // rest of the screen read where they are.
    const KeypadLayout moved = layout_of("dpad=100,100,-40,100");
    KeypadPoint q = fe::keypad_mask_point(moved, {130, 208});
    CHECK(q.x == doctest::Approx(30));
    CHECK(q.y == doctest::Approx(248));
    q = fe::keypad_mask_point(moved, {30, 300});
    CHECK(q.x == fe::kKeypadNoKeyPoint.x);
    CHECK(q.y == fe::kKeypadNoKeyPoint.y);
    q = fe::keypad_mask_point(moved, {370, 290});
    CHECK(q.x == 370);
    q = fe::keypad_mask_point(moved, {240, 20});
    CHECK(q.x == 240);
    CHECK(q.y == 20);
    // Where it still covers its old place, the drawn d-pad wins.
    q = fe::keypad_mask_point(moved, {110, 200});
    CHECK(q.x == doctest::Approx(10));
    CHECK(q.y == doctest::Approx(240));
    // Scaled: the drawn rect's corners map to the original rect's.
    const KeypadLayout big = layout_of("actions=150,0,0,100");
    const KeypadRect d = fe::keypad_drawn_rect(KeypadGroup::kActions, big[KeypadGroup::kActions]);
    const KeypadRect r = fe::keypad_group_rect(KeypadGroup::kActions);
    q = fe::keypad_mask_point(big, {d.x + 0.01f, d.y + 0.01f});
    CHECK(q.x == doctest::Approx(r.x).epsilon(0.001));
    CHECK(q.y == doctest::Approx(r.y).epsilon(0.001));
    q = fe::keypad_mask_point(big, {d.x + d.w / 2, d.y + d.h / 2});
    CHECK(q.x == doctest::Approx(r.x + r.w / 2));
    // The slots lie over a bigger action group's rect: the topmost (slots) is read first.
    KeypadGroup at{};
    REQUIRE(fe::keypad_group_at(big, {460, 250}, &at));
    CHECK(at == KeypadGroup::kSlots);
}

TEST_CASE("keypad layout: a GL draw moves with the group under its centre") {
    float m[6] = {};
    // The field HUD's first quick slot at (441, 292) (QuickSlots_Paint, mode 1): 20 x 20.
    const auto ndc_box = [](float x, float y, float w, float h, float box[4]) {
        box[0] = x / 240 - 1;
        box[2] = (x + w) / 240 - 1;
        box[1] = 1 - (y + h) / 160;
        box[3] = 1 - y / 160;
    };
    float box[4];
    ndc_box(441, 292, 20, 20, box); // centre (451, 302): in the actions group
    CHECK_FALSE(fe::keypad_draw_transform(KeypadLayout{}, box, m));
    CHECK_FALSE(fe::keypad_draw_transform(layout_of("slots=80,0,0,100"), box, m));
    const KeypadLayout l = layout_of("actions=100,-200,-100,100");
    REQUIRE(fe::keypad_draw_transform(l, box, m));
    // The box moves by (-200, -100) points: in NDC by (-200 / 240, +100 / 160).
    const float x0 = m[0] * box[0] + m[2] * box[1] + m[4];
    const float y1 = m[1] * box[2] + m[3] * box[3] + m[5];
    CHECK(x0 == doctest::Approx(box[0] - 200.0f / 240));
    CHECK(y1 == doctest::Approx(box[3] + 100.0f / 160));
    // Scaled: the screen point p maps to t(p) in NDC as in points.
    const KeypadLayout s = layout_of("slots=150,-30,10,100");
    ndc_box(462, 147, 10, 10, box);
    REQUIRE(fe::keypad_draw_transform(s, box, m));
    const fe::KeypadTransform t =
        fe::keypad_group_transform(KeypadGroup::kSlots, s[KeypadGroup::kSlots]);
    const KeypadPoint want = t.apply({462, 147});
    const float X = m[0] * box[0] + m[4], Y = m[3] * box[3] + m[5];
    CHECK((X + 1) * 240 == doctest::Approx(want.x));
    CHECK((1 - Y) * 160 == doctest::Approx(want.y));
    // A draw outside the groups (the HUD's gauges at the top) never moves.
    ndc_box(10, 10, 100, 12, box);
    CHECK_FALSE(fe::keypad_draw_transform(s, box, m));
}

namespace {

// The hooked functions as the recompiler writes them (recomp/lift/core.py), with stand-in
// bodies: the mask read records where it was read, the painter asks the draw filter in place
// for its slot icon's box.
float g_read_x = 0, g_read_y = 0;
int g_reads = 0;
void color_at_pixel_original(mt_cpu *MT_RESTRICT cpu) {
    g_reads++;
    std::memcpy(&g_read_x, &cpu->r[2], 4);
    std::memcpy(&g_read_y, &cpu->r[3], 4);
    cpu->r[0] = 0x1234u;
}
void color_at_pixel(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, color_at_pixel_original);
}

int g_paints = 0, g_filtered = 0;
float g_moved[6] = {};
void quick_slots_original(mt_cpu *MT_RESTRICT) {
    g_paints++;
    void *user = nullptr;
    const mt_gles_draw_filter f = mt_gles_get_draw_filter(&user);
    if (f != nullptr) {
        const float box[4] = {441.0f / 240 - 1, 1 - 312.0f / 160, 461.0f / 240 - 1,
                              1 - 292.0f / 160};
        g_filtered += f(user, box, g_moved);
    }
}
void quick_slots(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_QUICK_SLOTS_PAINT, quick_slots_original);
}

u32 fb(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

void read_at(mt_cpu &cpu, float x, float y) {
    cpu.r[2] = fb(x);
    cpu.r[3] = fb(y);
    color_at_pixel(&cpu);
}

} // namespace

TEST_CASE("keypad layout: the hooks read the mask at the mapped point and move the HUD's icons") {
    mt_cpu cpu{};
    cpu.r[MT_LR] = 0x2345u;
    g_reads = g_paints = g_filtered = 0;

    // The original: both hooks registered but off, the reads where they are.
    fe::install_keypad_layout(KeypadLayout{});
    CHECK(mt_hook_enabled(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_QUICK_SLOTS_PAINT) == 0);
    read_at(cpu, 30, 300);
    CHECK(g_reads == 1);
    CHECK(g_read_x == 30.0f);
    CHECK(fe::keypad_layout_stats().hits == 0);
    cpu.r[1] = 1;
    quick_slots(&cpu);
    CHECK(g_paints == 1);
    CHECK(g_filtered == 0);

    // The d-pad moved and the actions moved: on.
    fe::install_keypad_layout(layout_of("dpad=100,100,-40,100;actions=100,-200,-100,60"));
    CHECK(fe::installed_keypad_layout()[KeypadGroup::kActions].opacity == 60);
    CHECK(mt_hook_enabled(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_QUICK_SLOTS_PAINT) == 1);
    read_at(cpu, 130, 208); // the drawn left arm
    CHECK(g_reads == 2);
    CHECK(g_read_x == doctest::Approx(30));
    CHECK(g_read_y == doctest::Approx(248));
    CHECK(cpu.r[0] == 0x1234u); // the original's result
    read_at(cpu, 30, 300);      // the d-pad's old place
    CHECK(g_read_x == fe::kKeypadNoKeyPoint.x);
    CHECK(g_read_y == fe::kKeypadNoKeyPoint.y);
    read_at(cpu, 200, 100); // nothing there: unchanged
    CHECK(g_read_x == 200.0f);
    CHECK(g_read_y == 100.0f);
    fe::KeypadLayoutStats st = fe::keypad_layout_stats();
    CHECK(st.hits == 3);
    CHECK(st.remapped == 2);

    // The field HUD's painter (mode 1) runs with the filter, which moves the attack key's slot;
    // the popup menu's (mode 0) without; after either, no filter is left installed.
    cpu.r[1] = 1;
    quick_slots(&cpu);
    CHECK(g_paints == 2);
    CHECK(g_filtered == 1);
    CHECK(g_moved[4] == doctest::Approx(-200.0f / 240));
    CHECK(g_moved[5] == doctest::Approx(100.0f / 160));
    CHECK(mt_gles_get_draw_filter(nullptr) == nullptr);
    cpu.r[1] = 0;
    quick_slots(&cpu);
    CHECK(g_paints == 3);
    CHECK(g_filtered == 1);
    st = fe::keypad_layout_stats();
    CHECK(st.paints == 1);
    CHECK(st.draws_moved == 1);

    // Back to the original: off again; removed.
    fe::install_keypad_layout(KeypadLayout{});
    CHECK(mt_hook_enabled(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL) == 0);
    fe::uninstall_keypad_layout();
    CHECK(mt_hook_get_stats(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL).handled == 3);
    read_at(cpu, 130, 208);
    CHECK(g_read_x == 130.0f);
    CHECK(fe::keypad_layout_stats().hits == 0);
}

namespace {

const mt_objc_host_class k_eagl = {"EAGLView", "UIView", 96, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_pad = {"TouchKeyPadView", "UIView", 232, nullptr, 0, nullptr, 0};

double g_now = 1000;
double test_clock(void *) { return g_now; }

// A landscape window with an EAGLView holding a TouchKeyPadView whose panel 0 has panel 0's
// views as -[TouchKeyPadView initKeyPadType0:] builds them (docs/controls/keypad.md): the
// d-pad's container (with its pressed image inside), the jump icon, the jump and attack keys,
// a short key, and the Map button, which belongs to no group.
struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0, pad = 0, panel = 0, dpad = 0, dpad_pressed = 0, jump_icon = 0, jump = 0,
        attack = 0, short1 = 0, map = 0;

    Guest() {
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        REQUIRE(mt_objc_add_host_class(rt, &k_eagl, 0, 0) == 0);
        REQUIRE(mt_objc_add_host_class(rt, &k_pad, 0, 0) == 0);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
        mt_ui::State &st = mt_ui::state(&cpu);
        st.config.clock = test_clock;
        st.app = send(send(klass("UIApplication"), "alloc"), "init");
        const u32 window = view("UIWindow", 0, 0, 320, 480);
        st.windows.push_back(window);
        const u32 eagl = view("EAGLView", 0, 0, 320, 480);
        send(eagl, "setTransform:", {fb(0), fb(-1), fb(1), fb(0), fb(0), fb(0)});
        send(eagl, "setBounds:", {fb(0), fb(0), fb(480), fb(320)});
        send(window, "addSubview:", {eagl});
        pad = add(eagl, "TouchKeyPadView", 0, 0, 480, 320);
        std::memcpy(cpu.mem + eagl + 84, &pad, 4);
        panel = add(pad, "UIView", 0, 0, 480, 320);
        add(pad, "UIView", 0, 0, 480, 320); // panel 1
        dpad = add(panel, "UIImageView", 0, 180, 168, 136);
        dpad_pressed = add(dpad, "UIImageView", 0, 0, 168, 136);
        jump_icon = add(panel, "UIImageView", 356, 277, 28, 31);
        jump = add(panel, "UIImageView", 335, 277, 70, 34);
        attack = add(panel, "UIImageView", 405, 277, 70, 34);
        short1 = add(panel, "UIImageView", 446, 224, 34, 31);
        map = add(panel, "UIImageView", 400, 2, 36, 30);
        mt_input_configure(nullptr);
        mt_uikit_turn(&cpu, g_now);
    }
    ~Guest() {
        fe::uninstall_keypad_layout();
        mt_input_configure(nullptr);
        mt_objc_pool_pop(rt, pool);
        mt_ui::state(&cpu).config.clock = nullptr;
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    u32 klass(const char *name) const { return mt_objc_class_named(rt, name); }
    u32 send(u32 self, const char *sel, std::initializer_list<u32> args = {}) {
        const std::vector<u32> v(args);
        return mt_objc_msg(&cpu, self, mt_objc_sel(rt, sel), v.data(), static_cast<u32>(v.size()));
    }
    u32 view(const char *cls, float x, float y, float w, float h) {
        return send(send(klass(cls), "alloc"), "initWithFrame:", {fb(x), fb(y), fb(w), fb(h)});
    }
    u32 add(u32 parent, const char *cls, float x, float y, float w, float h) {
        const u32 v = view(cls, x, y, w, h);
        send(parent, "addSubview:", {v});
        mt_objc_release(&cpu, v);
        return v;
    }
    bool moved(u32 v, float m[6]) const { return mt_uikit_host_transform(v, m) != 0; }
};

} // namespace

TEST_CASE("keypad layout: the groups' views get their transform and opacity, the rest nothing") {
    Guest g;
    REQUIRE(mt_input_pad_panel_view(0) == g.panel);
    float fr[4];
    REQUIRE(mt_uikit_screen_frame(g.dpad, fr) == 1);
    CHECK(fr[0] == doctest::Approx(0));
    CHECK(fr[1] == doctest::Approx(180));
    CHECK(fr[2] == doctest::Approx(168));

    // The original: no view changes.
    fe::install_keypad_layout(KeypadLayout{});
    REQUIRE(fe::apply_keypad_layout());
    float m[6];
    for (const u32 v : {g.dpad, g.jump_icon, g.jump, g.attack, g.short1, g.map}) {
        CHECK_FALSE(g.moved(v, m));
        CHECK(mt_uikit_host_opacity(v) == 1.0f);
    }

    const KeypadLayout l = layout_of("dpad=50,20,0,40;actions=100,-200,-100,100;slots=100,0,0,0");
    fe::install_keypad_layout(l);
    REQUIRE(fe::apply_keypad_layout());
    const fe::KeypadTransform t =
        fe::keypad_group_transform(KeypadGroup::kDpad, l[KeypadGroup::kDpad]);
    REQUIRE(g.moved(g.dpad, m));
    CHECK(m[0] == t.scale);
    CHECK(m[3] == t.scale);
    CHECK(m[4] == t.tx);
    CHECK(m[5] == t.ty);
    CHECK(mt_uikit_host_opacity(g.dpad) == doctest::Approx(0.4f));
    CHECK_FALSE(g.moved(g.dpad_pressed, m)); // it moves with its container
    for (const u32 v : {g.jump_icon, g.jump, g.attack}) {
        REQUIRE(g.moved(v, m));
        CHECK(m[0] == 1.0f);
        CHECK(m[4] == -200.0f);
        CHECK(m[5] == -100.0f);
        CHECK(mt_uikit_host_opacity(v) == 1.0f);
    }
    CHECK_FALSE(g.moved(g.short1, m)); // only faded
    CHECK(mt_uikit_host_opacity(g.short1) == 0.0f);
    CHECK_FALSE(g.moved(g.map, m));
    CHECK(mt_uikit_host_opacity(g.map) == 1.0f);
    // The guest's frames are what the game set.
    REQUIRE(mt_uikit_screen_frame(g.jump, fr) == 1);
    CHECK(fr[0] == doctest::Approx(335));

    // The reveal for the editor, and back; the original again undoes everything.
    REQUIRE(fe::apply_keypad_layout(true));
    CHECK(mt_ui::view_of(&g.cpu, g.panel)->host_reveal);
    fe::install_keypad_layout(KeypadLayout{});
    REQUIRE(fe::apply_keypad_layout(false));
    CHECK_FALSE(mt_ui::view_of(&g.cpu, g.panel)->host_reveal);
    for (const u32 v : {g.dpad, g.jump_icon, g.jump, g.attack, g.short1, g.map}) {
        CHECK_FALSE(g.moved(v, m));
        CHECK(mt_uikit_host_opacity(v) == 1.0f);
    }
}

TEST_CASE("keypad layout: no pad, nothing applied") {
    CHECK(mt_input_pad_panel_view(0) == 0);
    CHECK_FALSE(fe::apply_keypad_layout());
}

namespace {

SDL_Event key(SDL_Keycode k, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    return e;
}

SDL_Event pad_button(SDL_GamepadButton b) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = static_cast<Uint8>(b);
    e.gbutton.down = true;
    return e;
}

KeypadPoint centre_of(KeypadEditor::Button b) { return KeypadEditor::button_rect(b).centre(); }

void tap(KeypadEditor &ed, KeypadPoint p) {
    ed.pointer(0, KeypadEditor::Phase::kDown, p);
    ed.pointer(0, KeypadEditor::Phase::kUp, p);
}

} // namespace

TEST_CASE("keypad editor: drag moves a group, a second finger pinches it, Save keeps it") {
    KeypadEditor ed;
    CHECK_FALSE(ed.is_open());
    ed.open(KeypadLayout{});
    CHECK(ed.is_open());
    CHECK(ed.dirty());
    const std::uint64_t v0 = ed.version();

    // A press outside every group selects nothing and moves nothing.
    ed.pointer(0, KeypadEditor::Phase::kDown, {240, 160});
    ed.pointer(0, KeypadEditor::Phase::kMove, {260, 170});
    ed.pointer(0, KeypadEditor::Phase::kUp, {260, 170});
    CHECK(ed.layout().original());
    CHECK(ed.version() == v0);

    // Drag the jump key 50 left and 30 up.
    ed.pointer(0, KeypadEditor::Phase::kDown, {370, 290});
    CHECK(ed.selected() == KeypadGroup::kActions);
    ed.pointer(0, KeypadEditor::Phase::kMove, {345, 275});
    ed.pointer(0, KeypadEditor::Phase::kMove, {320, 260});
    CHECK(ed.layout()[KeypadGroup::kActions] == GroupLayout{100, -50, -30, 100});
    CHECK(ed.version() > v0);
    // A second finger pinches: twice the distance, twice the size (the midpoint stays).
    ed.pointer(7, KeypadEditor::Phase::kDown, {360, 260});
    ed.pointer(7, KeypadEditor::Phase::kMove, {400, 260});
    ed.pointer(0, KeypadEditor::Phase::kMove, {280, 260});
    CHECK(ed.layout()[KeypadGroup::kActions].scale == 200);
    // The first finger lifts: the other drags on from where it is.
    ed.pointer(0, KeypadEditor::Phase::kUp, {280, 260});
    const GroupLayout before = ed.layout()[KeypadGroup::kActions];
    ed.pointer(7, KeypadEditor::Phase::kMove, {390, 250});
    const GroupLayout after = ed.layout()[KeypadGroup::kActions];
    CHECK(after.scale == before.scale);
    CHECK(after.dx ==
          fe::keypad_clamped(KeypadGroup::kActions,
                             GroupLayout{before.scale, before.dx - 10, before.dy - 10, 100})
              .dx);
    ed.pointer(7, KeypadEditor::Phase::kUp, {390, 250});
    // Everything is clamped as drawn: the group is on the screen.
    CHECK(inside_screen(fe::keypad_drawn_rect(KeypadGroup::kActions, after)));
    // Dragged far off the screen: held at its edge.
    ed.pointer(0, KeypadEditor::Phase::kDown, {84, 248});
    CHECK(ed.selected() == KeypadGroup::kDpad);
    ed.pointer(0, KeypadEditor::Phase::kMove, {-400, 900});
    ed.pointer(0, KeypadEditor::Phase::kUp, {-400, 900});
    CHECK(ed.layout()[KeypadGroup::kDpad] == GroupLayout{});

    tap(ed, centre_of(KeypadEditor::Button::kSave));
    CHECK_FALSE(ed.is_open());
    CHECK(ed.saved());
    CHECK(ed.layout()[KeypadGroup::kActions] == after);
}

TEST_CASE("keypad editor: the bar's buttons, Cancel restores what it opened on") {
    const KeypadLayout start = layout_of("slots=90,-10,0,80");
    KeypadEditor ed;
    ed.open(start);
    CHECK(ed.selected() == KeypadGroup::kDpad);
    tap(ed, centre_of(KeypadEditor::Button::kSizeDown));
    tap(ed, centre_of(KeypadEditor::Button::kSizeDown));
    tap(ed, centre_of(KeypadEditor::Button::kFadeDown));
    CHECK(ed.layout()[KeypadGroup::kDpad] == GroupLayout{90, 0, 0, 90});
    CHECK(ed.status() == "D-pad: size 90%, opacity 90%, moved 0,0");
    tap(ed, centre_of(KeypadEditor::Button::kFadeUp));
    tap(ed, centre_of(KeypadEditor::Button::kFadeUp)); // at most 100
    tap(ed, centre_of(KeypadEditor::Button::kSizeUp));
    CHECK(ed.layout()[KeypadGroup::kDpad] == GroupLayout{95, 0, 0, 100});
    tap(ed, centre_of(KeypadEditor::Button::kReset));
    CHECK(ed.layout()[KeypadGroup::kDpad].original());
    CHECK(ed.layout()[KeypadGroup::kSlots] == start[KeypadGroup::kSlots]);
    tap(ed, centre_of(KeypadEditor::Button::kResetAll));
    CHECK(ed.layout().original());
    // Every button lies in the bar, on the screen, none over another.
    for (int i = 0; i < KeypadEditor::kButtonCount; i++) {
        const KeypadRect r = KeypadEditor::button_rect(static_cast<KeypadEditor::Button>(i));
        CHECK(inside_screen(r));
        CHECK(r.y == KeypadEditor::kBarY);
        if (i > 0) {
            const KeypadRect l =
                KeypadEditor::button_rect(static_cast<KeypadEditor::Button>(i - 1));
            CHECK(l.x + l.w <= r.x);
        }
        CHECK(std::string(KeypadEditor::button_label(static_cast<KeypadEditor::Button>(i))).size() >
              0);
    }
    tap(ed, centre_of(KeypadEditor::Button::kCancel));
    CHECK_FALSE(ed.is_open());
    CHECK_FALSE(ed.saved());
    CHECK(ed.layout() == start);
    // Closed, it takes nothing.
    ed.pointer(0, KeypadEditor::Phase::kDown, {84, 248});
    CHECK_FALSE(ed.handle(key(SDLK_LEFT)));
    CHECK(ed.layout() == start);
}

TEST_CASE("keypad editor: keys, the wheel and the gamepad") {
    KeypadEditor ed;
    ed.open(KeypadLayout{});
    CHECK(ed.handle(key(SDLK_TAB)));
    CHECK(ed.selected() == KeypadGroup::kActions);
    CHECK(ed.handle(key(SDLK_TAB, SDL_KMOD_LSHIFT)));
    CHECK(ed.selected() == KeypadGroup::kDpad);
    ed.handle(key(SDLK_TAB, SDL_KMOD_LSHIFT));
    CHECK(ed.selected() == KeypadGroup::kSlots);
    ed.handle(key(SDLK_LEFT));
    ed.handle(key(SDLK_UP, SDL_KMOD_RSHIFT));
    ed.handle(key(SDLK_MINUS));
    ed.handle(key(SDLK_LEFTBRACKET));
    CHECK(ed.layout()[KeypadGroup::kSlots] == GroupLayout{95, -1, -10, 90});
    ed.handle(key(SDLK_EQUALS));
    ed.handle(key(SDLK_RIGHTBRACKET));
    ed.handle(key(SDLK_RIGHT));
    ed.handle(key(SDLK_DOWN, SDL_KMOD_LSHIFT));
    CHECK(ed.layout()[KeypadGroup::kSlots].original());
    ed.handle(key(SDLK_DOWN)); // the slots touch nothing below: allowed
    CHECK(ed.layout()[KeypadGroup::kSlots].dy == 1);
    ed.handle(key(SDLK_R));
    CHECK(ed.layout()[KeypadGroup::kSlots].original());
    // Keys that are not the editor's are still taken (the game is paused).
    CHECK(ed.handle(key(SDLK_X)));
    SDL_Event up{};
    up.type = SDL_EVENT_KEY_UP;
    CHECK(ed.handle(up));
    SDL_Event other{};
    other.type = SDL_EVENT_WINDOW_RESIZED;
    CHECK_FALSE(ed.handle(other));

    // The wheel sizes the group under the pointer's last point.
    ed.pointer(0, KeypadEditor::Phase::kDown, {84, 248});
    ed.pointer(0, KeypadEditor::Phase::kUp, {84, 248});
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1;
    CHECK(ed.handle(wheel));
    CHECK(ed.layout()[KeypadGroup::kDpad].scale == 105);
    wheel.wheel.y = -2;
    ed.handle(wheel);
    CHECK(ed.layout()[KeypadGroup::kDpad].scale == 100);
    // Backspace resets all.
    ed.handle(key(SDLK_MINUS));
    ed.handle(key(SDLK_BACKSPACE));
    CHECK(ed.layout().original());

    // The gamepad: LB / RB select, the d-pad moves 2 points, X / Y size, Back resets, A saves.
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    CHECK(ed.selected() == KeypadGroup::kActions);
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    CHECK(ed.selected() == KeypadGroup::kSlots);
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_UP));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_WEST));
    CHECK(ed.layout()[KeypadGroup::kSlots] == GroupLayout{95, -2, -2, 100});
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_NORTH));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_NORTH));
    CHECK(ed.layout()[KeypadGroup::kSlots].scale == 105);
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_BACK));
    CHECK(ed.layout()[KeypadGroup::kSlots].original());
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK_FALSE(ed.is_open());
    CHECK(ed.saved());
    CHECK(ed.layout()[KeypadGroup::kSlots].dx == 0); // held at the screen's right edge
    CHECK(ed.layout()[KeypadGroup::kSlots].dy == 2);

    // B cancels; Esc cancels; Enter saves.
    ed.open(KeypadLayout{});
    ed.handle(key(SDLK_MINUS));
    ed.handle(pad_button(SDL_GAMEPAD_BUTTON_EAST));
    CHECK_FALSE(ed.saved());
    CHECK(ed.layout().original());
    ed.open(KeypadLayout{});
    ed.handle(key(SDLK_MINUS));
    ed.handle(key(SDLK_ESCAPE));
    CHECK(ed.layout().original());
    ed.open(KeypadLayout{});
    ed.handle(key(SDLK_MINUS));
    ed.handle(key(SDLK_RETURN));
    CHECK(ed.saved());
    CHECK(ed.layout()[KeypadGroup::kDpad].scale == 95);
}

TEST_CASE(
    "keypad editor: it draws the groups' outlines, the selected one's in yellow, and its bar") {
    KeypadEditor ed;
    ed.open(layout_of("actions=100,-200,-100,100"));
    for (const int scale : {1, 2}) {
        const std::uint32_t w = 480u * scale, h = 320u * scale;
        std::vector<std::uint8_t> px(std::size_t{w} * h * 4, 0);
        for (std::size_t i = 3; i < px.size(); i += 4) {
            px[i] = 255; // opaque black
        }
        ed.draw(px.data(), w, h, scale);
        CHECK_FALSE(ed.dirty());
        const auto at = [&](int x, int y) {
            const std::size_t o = (std::size_t(y) * scale * w + std::size_t(x) * scale) * 4;
            return std::vector<int>{px[o], px[o + 1], px[o + 2], px[o + 3]};
        };
        // The selected d-pad's outline at its rect's corner, in yellow.
        CHECK(at(0, 319) == std::vector<int>{255, 214, 64, 255});
        CHECK(at(167, 200) == std::vector<int>{255, 214, 64, 255});
        // The moved actions' outline where it is drawn now, not where it was.
        const KeypadRect d =
            fe::keypad_drawn_rect(KeypadGroup::kActions, ed.layout()[KeypadGroup::kActions]);
        CHECK(at(static_cast<int>(d.x), static_cast<int>(d.y) + 20)[0] > 100);
        CHECK(at(335, 300) == std::vector<int>{0, 0, 0, 255});
        // The middle of the screen is untouched; the Save button is green.
        CHECK(at(240, 160) == std::vector<int>{0, 0, 0, 255});
        const KeypadRect save = KeypadEditor::button_rect(KeypadEditor::Button::kSave);
        CHECK(at(static_cast<int>(save.x) + 1, static_cast<int>(save.y) + 1) ==
              std::vector<int>{40, 110, 60, 255});
    }
    ed.handle(key(SDLK_TAB));
    CHECK(ed.dirty());
}
