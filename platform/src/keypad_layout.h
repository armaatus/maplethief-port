// The customisable touch key pad (#159, ADR 0086): the game pad's groups (the d-pad, jump and
// attack, the quick slots) resized, moved and faded, with their hit areas following what is
// drawn.
//
//   keypad_layout = original
//   keypad_layout = dpad=80,12,-20,70;actions=120,0,-6,100
//
// Each group is GROUP=SCALE,DX,DY,OPACITY: the size in percent (kMinScale-kMaxScale) about the
// group's centre, then an offset in screen points (480 x 320), then the opacity in percent
// (0-100). A group left out keeps its original place; `original` (the default) is all three.
// The text is written back in the order dpad, actions, slots, without the original groups.
// Map, Bag and Cash stay as the game placed them.
//
// What a group is (docs/controls/keypad.md, "Panel 0"; ADR 0086):
//   dpad     the d-pad's container view (its pressed and NPC-hint images are inside it) and
//            its four mask regions
//   actions  the jump key, its icon and the attack key (each with its pressed image), their
//            mask regions, and the attack key's icon, which the game draws in GL
//   slots    the four short keys (and pressed images), their mask regions, and the items and
//            skills on them with their counts, drawn in GL
// keypad_group_rect is the union of all of it, in screen points; a group's transform maps that
// rect to where it is drawn (keypad_drawn_rect), kept inside the screen.
//
// Nothing of the guest changes (the host side, keypad_layout.cpp's second half):
//   drawing  the group's views get a host transform and a host opacity in the compositor
//            (mt_uikit_set_host_transform / _opacity); the GL icons are moved by a GLES draw
//            filter installed around the quick-slot painter QuickSlots_Paint 0x4CAED (a hook,
//            field mode only)
//   hits     the key pad reads its mask at each touch with -[TouchKeyPadView
//            colorAtPixel:onImage:] 0xB4DB5; a hook reads it at keypad_mask_point instead: the
//            point in the original layout under the drawn key, or for a part of a group's old
//            place that nothing is drawn over any more a transparent pixel (no key; the
//            game's NPC-touch check still runs on the real point)
// With the original layout both hooks are off and no view has a transform: byte for byte the
// game as it was.
//
// The pure part (types, parse, print, geometry) has no SDL and no runtime.
#ifndef MAPLETHIEF_PLATFORM_KEYPAD_LAYOUT_H
#define MAPLETHIEF_PLATFORM_KEYPAD_LAYOUT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "maplethief/recomp/recomp.h"

namespace maplethief::frontend {

enum class KeypadGroup { kDpad, kActions, kSlots };
inline constexpr std::size_t kKeypadGroupCount = 3;
inline constexpr KeypadGroup kKeypadGroups[kKeypadGroupCount] = {
    KeypadGroup::kDpad, KeypadGroup::kActions, KeypadGroup::kSlots};
// "dpad", "actions", "slots" (the file's names).
const char *keypad_group_name(KeypadGroup group);
// "D-pad", "Jump and attack", "Quick slots" (the editor's).
const char *keypad_group_title(KeypadGroup group);

struct KeypadPoint {
    float x = 0, y = 0;
};
struct KeypadRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(KeypadPoint p) const {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
    KeypadPoint centre() const { return {x + w / 2, y + h / 2}; }
};

inline constexpr std::uint32_t kKeypadMinScale = 50, kKeypadMaxScale = 200; // percent
inline constexpr std::int32_t kKeypadMaxOffset = 480;                       // screen points

struct GroupLayout {
    std::uint32_t scale = 100;   // percent of the original size, about the group's centre
    std::int32_t dx = 0, dy = 0; // screen points
    std::uint32_t opacity = 100; // percent

    bool original() const { return scale == 100 && dx == 0 && dy == 0 && opacity == 100; }
    bool operator==(const GroupLayout &) const = default;
};

struct KeypadLayout {
    std::array<GroupLayout, kKeypadGroupCount> groups{};

    GroupLayout &operator[](KeypadGroup g) { return groups[static_cast<std::size_t>(g)]; }
    const GroupLayout &operator[](KeypadGroup g) const {
        return groups[static_cast<std::size_t>(g)];
    }
    bool original() const;
    bool operator==(const KeypadLayout &) const = default;
};

// `original`, or GROUP=SCALE,DX,DY,OPACITY items separated by ';' (blanks around the parts
// allowed, each group at most once, names in any case). False (`out` unchanged) if not.
bool parse_keypad_layout(std::string_view text, KeypadLayout *out);
// The file's text: `original`, or the groups that are not original, in the order above.
std::string keypad_layout_text(const KeypadLayout &layout);

// A group's original rect in screen points: the union of its views and its mask regions.
KeypadRect keypad_group_rect(KeypadGroup group);

// p' = scale * p + (tx, ty), in screen points.
struct KeypadTransform {
    float scale = 1, tx = 0, ty = 0;

    KeypadPoint apply(KeypadPoint p) const { return {scale * p.x + tx, scale * p.y + ty}; }
    KeypadPoint invert(KeypadPoint p) const { return {(p.x - tx) / scale, (p.y - ty) / scale}; }
    bool identity() const { return scale == 1 && tx == 0 && ty == 0; }
};

// The group's offset as drawn: dx, dy moved just enough that the scaled rect stays on the
// screen (all of it when it fits).
GroupLayout keypad_clamped(KeypadGroup group, const GroupLayout &g);
// The group's transform (scale about the rect's centre, then the clamped offset).
KeypadTransform keypad_group_transform(KeypadGroup group, const GroupLayout &g);
// Where the group is drawn: its rect through its transform.
KeypadRect keypad_drawn_rect(KeypadGroup group, const GroupLayout &g);

// The groups from the top down, as the game's views stack (slots, actions, dpad): the order in
// which a touch finds them.
inline constexpr KeypadGroup kKeypadGroupsTopDown[kKeypadGroupCount] = {
    KeypadGroup::kSlots, KeypadGroup::kActions, KeypadGroup::kDpad};
// The group drawn at screen point p (the topmost), or false.
bool keypad_group_at(const KeypadLayout &layout, KeypadPoint p, KeypadGroup *out);

// A transparent point of KeyPadArea.png, outside every group's rect (tests/test_keypad_real.py
// checks it against the shipped mask): the hit test reads it for "no key".
inline constexpr KeypadPoint kKeypadNoKeyPoint = {240, 160};
// Where the key pad's hit mask is read for a touch at screen point p: in the topmost drawn
// group, the point under it in the original layout; else, inside a group's original rect,
// kKeypadNoKeyPoint (nothing is drawn there any more); else p itself.
KeypadPoint keypad_mask_point(const KeypadLayout &layout, KeypadPoint p);

// The group a view of the game pad belongs to, by the centre of its screen frame; false for the
// others (Map, Bag, Cash).
bool keypad_group_of_frame(const KeypadRect &frame, KeypadGroup *out);

// --- widescreen (#168, ADR 0090's addendum) ----------------------------------------------------
//
// With the field drawn E > 480 points wide (maplethief/widescreen.h) the engine's screen is E
// points wide and the 3:2 screen image is its middle, from x = off = (E - 480) / 2. The GL HUD
// (the bar, the quick slots, the attack key's icon) is drawn at fixed 480-point places; the
// runtime moves it into the middle (maplethief/widescreen.h), so each icon is on its key's original
// place in the screen image. Anchored (widescreen_hud = edges, the default) the d-pad moves left
// by off and jump / attack and the quick slots right by off, partly into the wings beside the
// screen image (drawn there by keypad_wings_composite), and the draw filter around the quick
// slots' painter moves their icons with them. With widescreen_hud = narrow the pad and the
// HUD stay at their 3:2 places.
// The game pad's touches are in the engine's coordinates (the screen image's + off:
// mt_uikit_set_host_touch_area), so that the NPC-touch check and the GL widgets see the points
// they draw at; the hit mask hook takes the off back.
struct KeypadWide {
    float off = 0;        // (E - 480) / 2, 0 with the view 480 points wide
    bool anchored = true; // the groups at the view's edges
    float gl_width = 480; // the GL layer's width in points (its projection's): 480, or 568
    bool operator==(const KeypadWide &) const = default;
};
// The widescreen offset in effect for a view `view` points wide on a GL layer `gl_width` points
// wide (480 when off), anchored or not.
KeypadWide keypad_wide(std::uint32_t view, std::uint32_t gl_width, bool anchored);
// A group's move for widescreen: -off for the d-pad, +off for the others, 0 unless anchored.
float keypad_wide_shift(KeypadGroup group, const KeypadWide &wide);
// Where the group's views are drawn: the layout's transform, then the widescreen move.
KeypadTransform keypad_view_transform(KeypadGroup group, const GroupLayout &g,
                                      const KeypadWide &wide);
KeypadRect keypad_view_rect(KeypadGroup group, const GroupLayout &g, const KeypadWide &wide);
// keypad_mask_point with widescreen: `p` in screen-image points (the guest's point less off).
// Beside the screen image and on no group it is kKeypadNoKeyPoint (no key: the NPC check runs).
KeypadPoint keypad_mask_point(const KeypadLayout &layout, KeypadPoint p, const KeypadWide &wide);
// keypad_draw_transform for a GL layer gl_width points wide whose engine view starts off points
// left of the screen image, the icons drawn at their keys' original places in the screen image:
// they move with the layout's transform and the group's widescreen move.
bool keypad_draw_transform(const KeypadLayout &layout, const float box[4], float m[6],
                           const KeypadWide &wide);

// --- the host side (the runtime: hooks, views, GLES) ------------------------------------------

// Installs `layout` for the running game: the hooks on colorAtPixel:onImage: and QuickSlots_Paint
// registered (once) and enabled only when the layout is not the original. Takes effect at the
// next touch, the next GL frame and the next composite (apply_keypad_layout).
void install_keypad_layout(const KeypadLayout &layout);
// The layout installed (the original before any).
const KeypadLayout &installed_keypad_layout();
// Removes the hooks' handlers (tests).
void uninstall_keypad_layout();

// Puts the installed layout on the game pad's views (mt_input_pad_panel_view(0)'s subviews):
// each group's host transform and opacity; the others untouched. `reveal`: panel 0 is drawn
// even where the game has it off (the editor). Returns false while the game has no pad. Call
// it before every composite.
bool apply_keypad_layout(bool reveal = false);

// Widescreen (#168): once a turn, after mt_widescreen_turn, with the view's width now. Installs
// `wide` for the hooks and apply_keypad_layout, enables the hit mask hook while off > 0, and
// gives the game pad its touch area (the engine's x: + off; the wings, off points each side)
// while it is the panel in use. Returns the margins beside the screen image that touches may
// begin in now (0 when off).
float keypad_widescreen_turn(const KeypadWide &wide);
const KeypadWide &installed_keypad_wide();

// The game pad's views that a widescreen move put beside the screen image, composited into the
// wings (#168): a strip `wing` points wide on each side at `scale` texels a point, side by side
// in `out` (2 * wing * scale x 320 * scale, premultiplied RGBA8: left, then right). False (and
// `out` untouched) when nothing is drawn there: no move, no pad, or the editor is open.
bool keypad_wings_composite(mt_cpu *cpu, double now, std::uint32_t wing, std::uint32_t scale,
                            std::vector<std::uint8_t> *out);

struct KeypadLayoutStats {
    std::uint64_t hits = 0;        // mask reads through the hook
    std::uint64_t remapped = 0;    // of those, read at another point
    std::uint64_t paints = 0;      // field quick-slot paints with the layout
    std::uint64_t draws_moved = 0; // GL draws of those moved with a group
};
KeypadLayoutStats keypad_layout_stats();

// The GL draw filter's arithmetic (exposed for the tests): the NDC transform that moves a draw
// whose NDC box is `box` with the group whose original rect holds its centre; false if none.
bool keypad_draw_transform(const KeypadLayout &layout, const float box[4], float m[6]);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_KEYPAD_LAYOUT_H
