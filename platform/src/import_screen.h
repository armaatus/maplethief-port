// The first-launch import screen (#182, ADR 0100): the port's own screen, shown before the game
// when no bundle is found (game.h's find_bundle) in an interactive run of a build with the
// generated code. It asks for the player's own MapleThief-1.5.ipa and imports it with
// ipa_import.h into <data dir>/game/; the game then starts in the same run.
//
//   choose      a "Choose file" button: Enter, Space, the gamepad's South (A on an Xbox pad) or
//               Start, or a click or tap on it, opens the system's open dialog (file_dialog.h,
//               *.ipa). Where no dialog can show (dialog_route: iOS, a Steam Deck's Game Mode,
//               a fullscreen Linux window) it imports <data dir>/MapleThief-1.5.ipa instead,
//               which the screen names.
//   drop        a file dropped onto the window (SDL_EVENT_DROP_FILE)
//   importing   the import runs on a thread of its own; the screen shows its progress
//   refused     the reason (IpaResult::why) and that another file can be chosen or dropped
//   quit        closing the window: the run ends; nothing is left half-imported
//
// Drawn with Canvas (canvas.h) on a dark 480 x 320-point screen, as the preset picker is, and
// shown through a Presenter in the window's scale mode. No game asset is needed: the game's
// own fonts and art come from the bundle, which is not there yet.
//
// ImportScreen holds the state and draws it; run_import_screen is its loop on a window. The
// importer is a parameter, so tests drive both with synthetic files.
#ifndef MAPLETHIEF_PLATFORM_IMPORT_SCREEN_H
#define MAPLETHIEF_PLATFORM_IMPORT_SCREEN_H

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "glyphs.h"
#include "ipa_import.h"
#include "scaling.h"
#include "settings.h"

namespace maplethief::frontend {

class ImportScreen {
  public:
    // Imports the file at `path` (UTF-8) into the data directory, reporting progress.
    using Importer = std::function<IpaResult(const std::string &path, const IpaProgress &)>;

    enum class State { kWaiting, kImporting, kRefused, kImported };
    // What the player asked for.
    enum class Action { kNone, kChoose };

    // The button's place, in screen points.
    static constexpr int kButtonX = 24, kButtonY = 112, kButtonW = 168, kButtonH = 30;

    // `data_dir` is shown (and holds the fallback file); `folder_route`: no dialog can show,
    // so Choose imports <data dir>/MapleThief-1.5.ipa.
    ImportScreen(std::string data_dir, Importer importer, bool folder_route = false);
    ~ImportScreen(); // waits for an import that is running
    ImportScreen(const ImportScreen &) = delete;
    ImportScreen &operator=(const ImportScreen &) = delete;

    // Starts importing `path` on a thread of its own; false (nothing done) while one runs.
    bool start(const std::string &path);
    // Takes an import that ended; true if the state changed.
    bool poll();
    // Waits for a running import to end, then poll()s (tests).
    void wait();
    // Asks a running import to stop (it ends kInterrupted, leaving nothing behind).
    void cancel();

    State state() const { return state_; }
    const IpaResult &result() const { return result_; }
    const std::string &file() const { return file_; }
    bool folder_route() const { return folder_route_; }
    // <data dir>/MapleThief-1.5.ipa: the file Choose imports on the folder route.
    std::string folder_file() const;
    // The import's progress, 0 to 1.
    double progress() const;

    // A key or gamepad button (Choose), or a dropped file (starts it). True if it was taken.
    bool handle(const SDL_Event &e, Action *action);
    // A pointer at screen point `p`, pressed or released: Choose on a release over the button
    // it was pressed on.
    Action pointer(const ScreenPoint &p, bool down, bool up);
    // Every event: the glyph set follows the pad in use.
    void track(const SDL_Event &e);
    bool on_button(float x, float y) const;

    bool dirty() const;
    // Draws the screen over `rgba` (width x height, top row first), `scale` pixels per point.
    void draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale);

  private:
    struct Job {
        std::atomic<std::uint64_t> done{0}, total{0};
        std::atomic<bool> finished{false};
        std::atomic<bool> cancel{false};
        IpaResult result;
    };

    std::string data_dir_;
    Importer importer_;
    bool folder_route_ = false;
    State state_ = State::kWaiting;
    IpaResult result_;
    std::string file_;
    std::shared_ptr<Job> job_;
    std::thread worker_;
    bool pressed_ = false;
    bool dirty_ = true;
    int shown_percent_ = -1;
    GlyphTracker glyphs_;
};

// Words of `text` in lines of at most `width` characters (a longer word is cut).
std::vector<std::string> wrap_text(const std::string &text, std::size_t width);
// `path` in at most `width` characters: its end, after "...", if it is longer.
std::string shorten_path(const std::string &path, std::size_t width);

// Shows the import screen in the window until an import succeeds (returns the imported game
// folder) or the window closes (std::nullopt, *quit true). std::nullopt with *quit false if it
// cannot be shown (logged). `importer` empty: import_ipa into `data_dir` with ipa_expected().
std::optional<std::string> run_import_screen(SDL_Window *window, SDL_Renderer *renderer,
                                             const Settings &settings, const std::string &data_dir,
                                             bool *quit, ImportScreen::Importer importer = {});

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_IMPORT_SCREEN_H
