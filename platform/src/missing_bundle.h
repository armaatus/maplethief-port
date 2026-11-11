// The missing-bundle screen (#238): where the import screen (import_screen.h) cannot run yet,
// i.e. on the Nintendo Switch until its IPA import (#243), a game build that finds no bundle
// says so on the screen instead of closing without a word. It names the folder the bundle is
// expected in (data_location.h's placed bundle: sdmc:/switch/maplethief/game/MapleThief.app),
// says that the player's own extracted MapleThief.app goes there, and that importing the IPA on
// the console comes later. Any confirm or back control (the gamepad's South, East, Start or
// Back, Enter, Escape, Space, a tap) or closing the app ends it; the run then ends with the
// game's "no bundle" status.
//
// Drawn with Canvas (canvas.h) on the 480 x 320-point screen in the import screen's colours and
// shown through a Presenter in the window's scale mode, so it needs no file of the game.
// MissingBundleScreen holds the text and draws it; run_missing_bundle_screen is its loop.
#ifndef MAPLETHIEF_PLATFORM_MISSING_BUNDLE_H
#define MAPLETHIEF_PLATFORM_MISSING_BUNDLE_H

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "glyphs.h"
#include "settings.h"

namespace maplethief::frontend {

class MissingBundleScreen {
  public:
    // `expected`: the bundle folder the game looked for (shown, shortened from its start).
    explicit MissingBundleScreen(std::string expected);

    const std::string &expected() const { return expected_; }
    // Whether the event ends the screen: a confirm or back control pressed, or the app closing.
    static bool ends(const SDL_Event &e);
    // Every event: the hint's glyphs follow the pad in use.
    void track(const SDL_Event &e);
    bool dirty() const { return dirty_; }
    // Draws the screen over `rgba` (width x height, top row first), `scale` pixels per point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

  private:
    std::string expected_;
    bool dirty_ = true;
    GlyphTracker glyphs_;
};

// Shows the screen until the player ends it or the app closes; logs what it says. False if it
// cannot be shown (logged).
bool run_missing_bundle_screen(SDL_Renderer *renderer, const Settings &settings,
                               const std::string &expected);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_MISSING_BUNDLE_H
