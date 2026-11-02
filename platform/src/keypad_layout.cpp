// The customisable touch key pad (#159, ADR 0086); see keypad_layout.h.
#include "keypad_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "maplethief/gles.h"
#include "maplethief/hooks.h"
#include "maplethief/input.h"
#include "maplethief/uikit.h"

namespace maplethief::frontend {

namespace {

constexpr const char *kGroupNames[kKeypadGroupCount] = {"dpad", "actions", "slots"};
constexpr const char *kGroupTitles[kKeypadGroupCount] = {"D-pad", "Jump and attack", "Quick slots"};

// docs/controls/keypad.md, "Panel 0": the union of each group's views and mask regions.
//   dpad     the d-pad (0, 180, 168, 136); the mask's arms x 0-167, y 176-319
//   actions  jump (335, 277, 70, 34), its icon (356, 277, 28, 31), attack (405, 277, 70, 34);
//            the mask's jump and attack x 335-479, y 263-319
//   slots    short keys (446, 86 ... 224, 34, 31); the mask's x 430-479, y 77-262
constexpr KeypadRect kGroupRects[kKeypadGroupCount] = {
    {0, 176, 168, 144},
    {335, 263, 145, 57},
    {430, 77, 50, 186},
};

constexpr float kScreenW = 480, kScreenH = 320;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        const char c = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

// A whole number: an optional sign (when `sign`), then 1 to 4 digits.
bool parse_int(std::string_view s, bool sign, std::int32_t *out) {
    s = trim(s);
    bool negative = false;
    if (sign && !s.empty() && (s.front() == '-' || s.front() == '+')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty() || s.size() > 4) {
        return false;
    }
    std::int32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *out = negative ? -v : v;
    return true;
}

bool parse_group(std::string_view text, GroupLayout *out) {
    std::int32_t v[4] = {};
    for (int i = 0; i < 4; i++) {
        const std::size_t comma = text.find(',');
        if ((i < 3) != (comma != std::string_view::npos)) {
            return false;
        }
        const std::string_view part = i < 3 ? text.substr(0, comma) : text;
        if (!parse_int(part, i == 1 || i == 2, &v[i])) {
            return false;
        }
        text = i < 3 ? text.substr(comma + 1) : std::string_view();
    }
    if (v[0] < static_cast<std::int32_t>(kKeypadMinScale) ||
        v[0] > static_cast<std::int32_t>(kKeypadMaxScale) || std::abs(v[1]) > kKeypadMaxOffset ||
        std::abs(v[2]) > kKeypadMaxOffset || v[3] > 100) {
        return false;
    }
    out->scale = static_cast<std::uint32_t>(v[0]);
    out->dx = v[1];
    out->dy = v[2];
    out->opacity = static_cast<std::uint32_t>(v[3]);
    return true;
}

// The scaled rect's origin on one axis kept on the screen: inside when it fits, else centred.
float clamp_axis(float origin, float size, float screen) {
    if (size > screen) {
        return (screen - size) / 2;
    }
    return std::clamp(origin, 0.0f, screen - size);
}

// The clamped offset, in screen points (floats).
void clamped_offset(KeypadGroup group, const GroupLayout &g, float *dx, float *dy) {
    const KeypadRect r = kGroupRects[static_cast<std::size_t>(group)];
    const float s = static_cast<float>(g.scale) / 100.0f;
    const KeypadPoint c = r.centre();
    const float w = r.w * s, h = r.h * s;
    const float x = c.x - w / 2 + static_cast<float>(g.dx);
    const float y = c.y - h / 2 + static_cast<float>(g.dy);
    *dx = clamp_axis(x, w, kScreenW) - (c.x - w / 2);
    *dy = clamp_axis(y, h, kScreenH) - (c.y - h / 2);
}

} // namespace

const char *keypad_group_name(KeypadGroup group) {
    return kGroupNames[static_cast<std::size_t>(group)];
}

const char *keypad_group_title(KeypadGroup group) {
    return kGroupTitles[static_cast<std::size_t>(group)];
}

bool KeypadLayout::original() const {
    return std::all_of(groups.begin(), groups.end(),
                       [](const GroupLayout &g) { return g.original(); });
}

bool parse_keypad_layout(std::string_view text, KeypadLayout *out) {
    text = trim(text);
    if (same_name(text, "original")) {
        *out = KeypadLayout{};
        return true;
    }
    KeypadLayout layout;
    bool seen[kKeypadGroupCount] = {};
    bool any = false;
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        const std::string_view item = trim(text.substr(0, semi));
        text = semi == std::string_view::npos ? std::string_view() : text.substr(semi + 1);
        if (item.empty()) {
            if (semi == std::string_view::npos && any) {
                break; // a trailing ';'
            }
            return false;
        }
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const std::string_view name = trim(item.substr(0, eq));
        std::size_t index = kKeypadGroupCount;
        for (std::size_t i = 0; i < kKeypadGroupCount; i++) {
            if (same_name(name, kGroupNames[i])) {
                index = i;
            }
        }
        if (index == kKeypadGroupCount || seen[index] ||
            !parse_group(item.substr(eq + 1), &layout.groups[index])) {
            return false;
        }
        seen[index] = true;
        any = true;
    }
    if (!any) {
        return false;
    }
    *out = layout;
    return true;
}

std::string keypad_layout_text(const KeypadLayout &layout) {
    std::string out;
    for (std::size_t i = 0; i < kKeypadGroupCount; i++) {
        const GroupLayout &g = layout.groups[i];
        if (g.original()) {
            continue;
        }
        if (!out.empty()) {
            out += ';';
        }
        out += std::string(kGroupNames[i]) + '=' + std::to_string(g.scale) + ',' +
               std::to_string(g.dx) + ',' + std::to_string(g.dy) + ',' + std::to_string(g.opacity);
    }
    return out.empty() ? std::string("original") : out;
}

KeypadRect keypad_group_rect(KeypadGroup group) {
    return kGroupRects[static_cast<std::size_t>(group)];
}

GroupLayout keypad_clamped(KeypadGroup group, const GroupLayout &g) {
    float dx = 0, dy = 0;
    clamped_offset(group, g, &dx, &dy);
    GroupLayout out = g;
    out.dx = static_cast<std::int32_t>(std::lround(dx));
    out.dy = static_cast<std::int32_t>(std::lround(dy));
    return out;
}

KeypadTransform keypad_group_transform(KeypadGroup group, const GroupLayout &g) {
    const KeypadRect r = kGroupRects[static_cast<std::size_t>(group)];
    const float s = static_cast<float>(g.scale) / 100.0f;
    float dx = 0, dy = 0;
    clamped_offset(group, g, &dx, &dy);
    const KeypadPoint c = r.centre();
    return KeypadTransform{s, c.x - s * c.x + dx, c.y - s * c.y + dy};
}

KeypadRect keypad_drawn_rect(KeypadGroup group, const GroupLayout &g) {
    const KeypadRect r = kGroupRects[static_cast<std::size_t>(group)];
    const KeypadTransform t = keypad_group_transform(group, g);
    const KeypadPoint a = t.apply({r.x, r.y});
    return KeypadRect{a.x, a.y, r.w * t.scale, r.h * t.scale};
}

bool keypad_group_at(const KeypadLayout &layout, KeypadPoint p, KeypadGroup *out) {
    for (const KeypadGroup g : kKeypadGroupsTopDown) {
        if (keypad_drawn_rect(g, layout[g]).contains(p)) {
            *out = g;
            return true;
        }
    }
    return false;
}

KeypadPoint keypad_mask_point(const KeypadLayout &layout, KeypadPoint p) {
    KeypadGroup g{};
    if (keypad_group_at(layout, p, &g)) {
        const KeypadTransform t = keypad_group_transform(g, layout[g]);
        if (t.identity()) {
            return p;
        }
        // Inside the original rect (the drawn rect is its image), clamped against rounding.
        const KeypadRect r = kGroupRects[static_cast<std::size_t>(g)];
        const KeypadPoint q = t.invert(p);
        return {std::clamp(q.x, r.x, std::nextafter(r.x + r.w, r.x)),
                std::clamp(q.y, r.y, std::nextafter(r.y + r.h, r.y))};
    }
    for (const KeypadRect &r : kGroupRects) {
        if (r.contains(p)) {
            return kKeypadNoKeyPoint;
        }
    }
    return p;
}

bool keypad_group_of_frame(const KeypadRect &frame, KeypadGroup *out) {
    const KeypadPoint c = frame.centre();
    for (std::size_t i = 0; i < kKeypadGroupCount; i++) {
        if (kGroupRects[i].contains(c)) {
            *out = kKeypadGroups[i];
            return true;
        }
    }
    return false;
}

bool keypad_draw_transform(const KeypadLayout &layout, const float box[4], float m[6]) {
    return keypad_draw_transform(layout, box, m, KeypadWide{});
}

KeypadWide keypad_wide(std::uint32_t view, std::uint32_t gl_width, bool anchored) {
    KeypadWide w;
    w.gl_width = gl_width > kScreenW ? static_cast<float>(gl_width) : kScreenW;
    w.off = view > kScreenW ? (static_cast<float>(view) - kScreenW) / 2 : 0.0f;
    w.anchored = anchored;
    return w;
}

float keypad_wide_shift(KeypadGroup group, const KeypadWide &wide) {
    if (!wide.anchored || wide.off <= 0) {
        return 0;
    }
    return group == KeypadGroup::kDpad ? -wide.off : wide.off;
}

KeypadTransform keypad_view_transform(KeypadGroup group, const GroupLayout &g,
                                      const KeypadWide &wide) {
    KeypadTransform t = keypad_group_transform(group, g);
    t.tx += keypad_wide_shift(group, wide);
    return t;
}

KeypadRect keypad_view_rect(KeypadGroup group, const GroupLayout &g, const KeypadWide &wide) {
    KeypadRect r = keypad_drawn_rect(group, g);
    r.x += keypad_wide_shift(group, wide);
    return r;
}

KeypadPoint keypad_mask_point(const KeypadLayout &layout, KeypadPoint p, const KeypadWide &wide) {
    for (const KeypadGroup g : kKeypadGroupsTopDown) {
        if (!keypad_view_rect(g, layout[g], wide).contains(p)) {
            continue;
        }
        const KeypadTransform t = keypad_view_transform(g, layout[g], wide);
        if (t.identity()) {
            return p;
        }
        // Inside the original rect (the drawn rect is its image), clamped against rounding.
        const KeypadRect r = kGroupRects[static_cast<std::size_t>(g)];
        const KeypadPoint q = t.invert(p);
        return {std::clamp(q.x, r.x, std::nextafter(r.x + r.w, r.x)),
                std::clamp(q.y, r.y, std::nextafter(r.y + r.h, r.y))};
    }
    for (const KeypadRect &r : kGroupRects) {
        if (r.contains(p)) {
            return kKeypadNoKeyPoint;
        }
    }
    // Beside the screen image (widescreen's wings): no key there, but a touch (an NPC's).
    if (p.x < 0 || p.x >= kScreenW) {
        return kKeypadNoKeyPoint;
    }
    return p;
}

bool keypad_draw_transform(const KeypadLayout &layout, const float box[4], float m[6],
                           const KeypadWide &wide) {
    // NDC (x right, y up, -1 to 1) <-> the GL layer's points (gl_width x 320, y down), whose x
    // less off is the screen image's.
    const float w = wide.gl_width;
    const KeypadPoint c = {((box[0] + box[2]) / 2 + 1) * (w / 2) - wide.off,
                           (1 - (box[1] + box[3]) / 2) * (kScreenH / 2)};
    // The group whose original rect holds the centre: the HUD is drawn at its 3:2 places in the
    // middle of the view (the runtime's HUD_Paint hook), the icons at their keys' original places.
    for (const KeypadGroup group : kKeypadGroups) {
        if (!kGroupRects[static_cast<std::size_t>(group)].contains(c)) {
            continue;
        }
        const float sh = keypad_wide_shift(group, wide);
        const KeypadTransform t = keypad_group_transform(group, layout[group]);
        if (t.identity() && sh == 0) {
            return false;
        }
        // In screen-image points p' = s p + t + sh (the key's view); in GL points (x = p + off)
        // x' = s x + cx with cx = tx + sh + off - s off; y' = s y + ty. In NDC that is
        // X' = s X + (s - 1 + 2 cx / w) and Y' = s Y + (1 - s - ty / 160).
        const float cx = t.tx + sh + wide.off - t.scale * wide.off;
        m[0] = t.scale;
        m[1] = 0;
        m[2] = 0;
        m[3] = t.scale;
        m[4] = t.scale - 1 + 2 * cx / w;
        m[5] = 1 - t.scale - t.ty / (kScreenH / 2);
        return true;
    }
    return false;
}

// --- the host side ------------------------------------------------------------------------------

namespace {

KeypadLayout g_layout;
KeypadWide g_wide; // widescreen's move (#168): none until keypad_widescreen_turn says
// The x shift the game pad's touch area gives its touches now (#168): g_wide.off while the game
// pad is the panel in use, else 0. The screens over the wide field (#169: the popup menu, the
// shop, the map screen, the cash shop) keep E wide but take their touches on other panels in the
// screen image's points, as does a popup's alert panel over the field.
float g_touch_shift = 0;
KeypadLayoutStats g_stats;
bool g_registered = false;
bool g_revealed = false; // apply_keypad_layout's last reveal (the editor: no widescreen move)

// The hit mask hook is needed for a custom layout, and with widescreen's offset (the game pad's
// touches come in the engine's x); the quick-slot painter's for a custom layout, and with the
// groups anchored to a wide view's edges (their icons go with them).
void enable_hooks() {
    mt_hook_set_enabled(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL,
                        !g_layout.original() || g_wide.off > 0 ? 1 : 0);
    mt_hook_set_enabled(MT_HOOK_FN_QUICK_SLOTS_PAINT,
                        !g_layout.original() || (g_wide.anchored && g_wide.off > 0) ? 1 : 0);
}

std::uint32_t float_bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}

float bits_float(std::uint32_t u) {
    float f = 0;
    std::memcpy(&f, &u, 4);
    return f;
}

// -[TouchKeyPadView colorAtPixel:(CGPoint)p onImage:(UIImage *)image] (0xB4DB5): self r0, _cmd
// r1, p.x r2, p.y r3 (soft-float), image on the stack. The mask is read at keypad_mask_point.
//
// With widescreen's offset (#168) the point is in the engine's x (the game pad's touch area):
// the screen image's is x - off, read through the anchored groups.
int on_color_at_pixel(mt_cpu *cpu, const mt_hook_call *call) {
    g_stats.hits++;
    const KeypadPoint p = {bits_float(cpu->r[2]), bits_float(cpu->r[3])};
    const KeypadPoint q = g_touch_shift > 0
                              ? keypad_mask_point(g_layout, {p.x - g_touch_shift, p.y}, g_wide)
                              : keypad_mask_point(g_layout, p);
    if (q.x != p.x || q.y != p.y) {
        g_stats.remapped++;
        cpu->r[2] = float_bits(q.x);
        cpu->r[3] = float_bits(q.y);
    }
    call->original(cpu);
    return MT_HOOK_HANDLED;
}

int on_draw(void *, const float box[4], float m[6]) {
    if (!keypad_draw_transform(g_layout, box, m, g_wide)) {
        return 0;
    }
    g_stats.draws_moved++;
    return 1;
}

// QuickSlots_Paint(g, mode, x, y, state) (0x4CAED): mode 1 is the field HUD (the popup menu's
// quick slots, mode 0, stay where its touch widgets are). Its draws are moved with their group.
int on_quick_slots(mt_cpu *cpu, const mt_hook_call *call) {
    if (cpu->r[1] == 0) {
        return MT_HOOK_PASS;
    }
    g_stats.paints++;
    mt_gles_set_draw_filter(on_draw, nullptr);
    call->original(cpu);
    mt_gles_set_draw_filter(nullptr, nullptr);
    return MT_HOOK_HANDLED;
}

} // namespace

void install_keypad_layout(const KeypadLayout &layout) {
    g_layout = layout;
    // Registering again replaces the handler (and survives a test's mt_hook_reset).
    mt_hook_register(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL, on_color_at_pixel, nullptr, 0);
    mt_hook_register(MT_HOOK_FN_QUICK_SLOTS_PAINT, on_quick_slots, nullptr, 0);
    g_registered = true;
    enable_hooks();
}

const KeypadLayout &installed_keypad_layout() { return g_layout; }

void uninstall_keypad_layout() {
    if (g_registered) {
        mt_hook_unregister(MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL);
        mt_hook_unregister(MT_HOOK_FN_QUICK_SLOTS_PAINT);
        g_registered = false;
    }
    g_layout = KeypadLayout{};
    g_wide = KeypadWide{};
    g_touch_shift = 0;
    g_stats = KeypadLayoutStats{};
}

bool apply_keypad_layout(bool reveal) {
    const std::uint32_t panel = mt_input_pad_panel_view(0);
    if (panel == 0) {
        return false;
    }
    mt_uikit_set_host_reveal(panel, reveal ? 1 : 0);
    g_revealed = reveal;
    // The editor edits the 3:2 layout: no widescreen move while it is open.
    const KeypadWide wide = reveal ? KeypadWide{} : g_wide;
    std::uint32_t subs[64];
    const std::uint32_t n = std::min<std::uint32_t>(mt_uikit_subviews(panel, subs, 64), 64);
    for (std::uint32_t i = 0; i < n; i++) {
        float fr[4];
        KeypadGroup g{};
        if (mt_uikit_screen_frame(subs[i], fr) == 0 ||
            !keypad_group_of_frame(KeypadRect{fr[0], fr[1], fr[2], fr[3]}, &g)) {
            continue;
        }
        const KeypadTransform t = keypad_view_transform(g, g_layout[g], wide);
        const float m[6] = {t.scale, 0, 0, t.scale, t.tx, t.ty};
        mt_uikit_set_host_transform(subs[i], t.identity() ? nullptr : m);
        mt_uikit_set_host_opacity(subs[i], static_cast<float>(g_layout[g].opacity) / 100.0f);
    }
    return true;
}

float keypad_widescreen_turn(const KeypadWide &wide) {
    // The game pad's touches in the engine's x, and the wings beside the screen image, while it
    // is the panel in use (a popup's, the NPC chat's or a screen's panel keeps the screen's
    // points). On another panel the game pad is hidden: its groups and their GL icons stay at
    // their 3:2 places with the rest of the HUD (no anchoring, no wings), so a screen over the
    // wide field (#169) shows no quick-slot icons at the edges without their keys.
    const std::uint32_t panel = mt_input_pad_panel_view(0);
    const bool on = wide.off > 0 && panel != 0 && mt_input_panel() == 0;
    g_wide = wide;
    if (!on) {
        g_wide.anchored = false;
    }
    g_touch_shift = on ? wide.off : 0.0f;
    if (g_registered) {
        enable_hooks();
    } else if (wide.off > 0) {
        install_keypad_layout(g_layout); // the hooks, for the touches' offset
    }
    mt_uikit_set_host_touch_area(on ? panel : 0u, on ? wide.off : 0.0f, on ? wide.off : 0.0f,
                                 on ? wide.off : 0.0f);
    return on ? wide.off : 0.0f;
}

const KeypadWide &installed_keypad_wide() { return g_wide; }

bool keypad_wings_composite(mt_cpu *cpu, double now, std::uint32_t wing, std::uint32_t scale,
                            std::vector<std::uint8_t> *out) {
    const std::uint32_t panel = mt_input_pad_panel_view(0);
    if (g_revealed || wing == 0 || scale == 0 || g_wide.off <= 0 || !g_wide.anchored ||
        panel == 0) {
        return false;
    }
    const std::uint32_t w = wing * scale, h = 320 * scale;
    out->assign(static_cast<std::size_t>(2 * w) * h * 4, 0);
    const std::uint32_t stride = 2 * w * 4;
    return mt_uikit_composite_view(cpu, now, panel, -static_cast<float>(wing), out->data(), w, h,
                                   stride) == 0 &&
           mt_uikit_composite_view(cpu, now, panel, kScreenW, out->data() + w * 4, w, h, stride) ==
               0;
}

KeypadLayoutStats keypad_layout_stats() { return g_stats; }

} // namespace maplethief::frontend
