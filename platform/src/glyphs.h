// Button glyphs (#70, ADR 0054): how the port's own overlays show a control. Every glyph is
// drawn in code from plain shapes and the port's 5x7 font (canvas.h): no console maker's art.
//
//   Xbox         the letters A B X Y in discs (South, East, West, North), LB RB, LT RT,
//                View Menu, LS RS
//   PlayStation  a cross, a circle, a square and a triangle as plain outlines in discs, L1 R1,
//                L2 R2, Share (PS4) or Create (PS5) and Options, L3 R3
//   Nintendo     the letters by their place on a Nintendo pad (B South, A East, Y West,
//                X North), L R, ZL ZR, - +, LS RS
//   keys         a key cap with a short name ("Z", "Enter", "LCtrl")
//
// The set follows the gamepad the player uses (GlyphTracker): SDL_GetGamepadType of the
// active pad; a pad that connects shows its set at once; with none, the Xbox set.
//
// A face button's glyph depends on FaceButtons (gamepad.h): by position, the control "a" is
// the bottom button, whose label is B on a Nintendo pad; by label, "a" is the button labelled A.
#ifndef MAPLETHIEF_PLATFORM_GLYPHS_H
#define MAPLETHIEF_PLATFORM_GLYPHS_H

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "actions.h"
#include "canvas.h"
#include "gamepad.h"

namespace maplethief::frontend {

enum class GlyphSet { kXbox, kPlayStation, kNintendo };
// "Xbox", "PlayStation", "Nintendo".
const char *glyph_set_name(GlyphSet set);
// The set for a pad type: PS3/PS4/PS5 PlayStation, the Switch pads and the GameCube adapter
// Nintendo, everything else (Xbox, Steam, standard, unknown) Xbox.
GlyphSet glyph_set_for(SDL_GamepadType type);

// The text a glyph shows for a gamepad control on a pad of `type` ("A", "Cross", "LB", "L2",
// "D-pad up", "View"): what the overlay draws, and what tests compare.
std::string pad_glyph_label(const Control &c, SDL_GamepadType type, FaceButtons face);
// A key's short name on its cap: "Z", "Enter", "Esc", "LCtrl", "Space", "Up".
std::string key_label(int keycode);

// The glyph's size in points (all are kButtonHeight high).
inline constexpr int kButtonHeight = 12;
int glyph_width(const Control &c, SDL_GamepadType type, FaceButtons face);
// Draws the glyph with its top-left at (x, y); returns its width.
int draw_glyph(Canvas &canvas, int x, int y, const Control &c, SDL_GamepadType type,
               FaceButtons face);

// Which glyph set to show: the pad the player uses.
//   a pad connects                       its type, at once (a different pad plugged in)
//   a press on a pad                     its type, if it is the active pad (`active`: Gamepads'
//                                        active_id(), 0 for none yet) or there is none
//   the shown pad disconnects            the active pad's type, else the last connected pad's,
//                                        else none (the Xbox set)
class GlyphTracker {
  public:
    // An SDL event; `active` is Gamepads::active_id() after the event was handled.
    void handle(const SDL_Event &e, SDL_JoystickID active);
    // The pad whose glyphs are shown (0: none) and its type.
    SDL_JoystickID pad() const { return pad_; }
    SDL_GamepadType type() const { return type_; }
    GlyphSet set() const { return glyph_set_for(type_); }
    // How often the set changed (for logs and tests).
    unsigned changes() const { return changes_; }

  private:
    void show(SDL_JoystickID id);

    SDL_JoystickID pad_ = 0;
    SDL_GamepadType type_ = SDL_GAMEPAD_TYPE_UNKNOWN;
    std::vector<SDL_JoystickID> connected_; // in the order they connected
    unsigned changes_ = 0;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GLYPHS_H
