// The first-launch import screen (#182, ADR 0100): the port's own screen, shown before the game
// when no bundle is found (game.h's find_bundle) in an interactive run of a build with the
// generated code. It asks for the player's own MapleThief-1.5.ipa and imports it with
// ipa_import.h into the bundle folder (<game folder>/game/ or the data folder's game/, ADR
// 0127; main.cpp passes the importer); the game then starts in the same run.
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
// The SD-card route (#243; kCapIpaOnSdCard: the Nintendo Switch, no dialog and no drop): the IPA
// is a file the player copied beside the game, in <base> (sdmc:/switch/maplethief/). The screen
// looks there at once (ipa_import.h's find_ipas: MapleThief-1.5.ipa, then any other *.ipa) and
// imports the files in that order until one is the game, each refusal with its reason:
//   not found   no IPA in the folder: what to copy where; A (the pad's South by label) looks
//               again, + returns to the Homebrew Menu
//   refused     every file found was refused: each with its reason; A looks again, + quits
//   delete?     imported: a question, No (Keep) focused, whether to delete the IPA that was
//               imported to free space on the SD card (Right to Yes, Left to No, or a tap;
//               A answers, B is No)
// The Switch's importer holds the CPU boost while it runs (main.cpp, ADR 0141).
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
#include <utility>
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

    // How the file is chosen: the system's dialog or a drop; the folder route (no dialog can
    // show: <data dir>/MapleThief-1.5.ipa); the SD-card route (kCapIpaOnSdCard, see above).
    enum class Route { kDialog, kFolder, kSdCard };
    // kNotFound: the SD-card route found no IPA. kAskDelete: the SD-card route imported one and
    // asks whether to delete it.
    enum class State { kWaiting, kImporting, kRefused, kNotFound, kAskDelete, kImported };
    // What the player asked for: Choose (the dialog; on the SD-card route, look again), Quit
    // (the SD-card route's +: back to the Homebrew Menu).
    enum class Action { kNone, kChoose, kQuit };

    // The button's place, in screen points.
    static constexpr int kButtonX = 24, kButtonY = 112, kButtonW = 168, kButtonH = 30;
    // The delete question's buttons, in screen points: No (Keep) on the left, Yes (Delete) on
    // the right, as in the settings menu's questions.
    static constexpr int kAnswerY = 236, kAnswerW = 120, kAnswerH = 26, kNoX = 24,
                         kYesX = kNoX + kAnswerW + 16;

    // `data_dir` is shown (and holds the fallback file); `folder_route`: no dialog can show,
    // so Choose imports <data dir>/MapleThief-1.5.ipa.
    ImportScreen(std::string data_dir, Importer importer, bool folder_route = false);
    // The same with a route; for kSdCard, `dir` is the folder searched and shown (<base>).
    ImportScreen(std::string dir, Importer importer, Route route);
    ~ImportScreen(); // waits for an import that is running
    ImportScreen(const ImportScreen &) = delete;
    ImportScreen &operator=(const ImportScreen &) = delete;

    // Starts importing `path` on a thread of its own; false (nothing done) while one runs.
    bool start(const std::string &path);
    // Starts importing `paths` in order, on a thread of its own, until one imports (the rest are
    // not tried); refused() then lists the ones before it. False while one runs or if empty.
    bool start(const std::vector<std::string> &paths);
    // The SD-card route: looks for the IPAs in the folder (find_ipas) and starts importing them,
    // or shows kNotFound. False while an import runs or once imported.
    bool look();
    // Takes an import that ended; true if the state changed.
    bool poll();
    // Waits for a running import to end, then poll()s (tests).
    void wait();
    // Asks a running import to stop (it ends kInterrupted, leaving nothing behind).
    void cancel();

    State state() const { return state_; }
    const IpaResult &result() const { return result_; }
    // The file being imported, the one imported, or the last one refused.
    const std::string &file() const { return file_; }
    // Each file refused by the last start, with its reason, in the order tried.
    const std::vector<std::pair<std::string, IpaResult>> &refused() const { return refused_; }
    Route route() const { return route_; }
    bool folder_route() const { return route_ == Route::kFolder; }
    // The delete question: whether Yes has the focus (No first), and whether the IPA was deleted.
    bool delete_focused() const { return delete_focus_; }
    bool deleted() const { return deleted_; }
    // Answers the delete question (kAskDelete): Yes deletes file() (logged; a failure is logged
    // and the game starts anyway); either way the state becomes kImported. False otherwise.
    bool answer_delete(bool yes);
    // <data dir>/MapleThief-1.5.ipa: the file Choose imports on the folder route.
    std::string folder_file() const;
    // The import's progress, 0 to 1.
    double progress() const;

    // A key or gamepad button (Choose, Quit, the delete question's answers), or a dropped file
    // (starts it). True if it was taken.
    bool handle(const SDL_Event &e, Action *action);
    // A pointer at screen point `p`, pressed or released: Choose on a release over the button
    // it was pressed on; in the delete question, an answer on a release over it.
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
        std::atomic<std::size_t> index{0}; // the file being imported
        std::atomic<bool> finished{false};
        std::atomic<bool> cancel{false};
        IpaResult result;
        std::vector<std::pair<std::string, IpaResult>> refused;
    };
    // Which button the pointer pressed.
    enum class Pressed { kNone, kChoose, kYes, kNo };

    void ended_imported();

    std::string data_dir_;
    Importer importer_;
    Route route_ = Route::kDialog;
    State state_ = State::kWaiting;
    IpaResult result_;
    std::string file_;
    std::vector<std::string> files_; // the files of the running import, in order
    std::vector<std::pair<std::string, IpaResult>> refused_;
    bool delete_focus_ = false;
    bool deleted_ = false;
    std::shared_ptr<Job> job_;
    std::thread worker_;
    Pressed pressed_ = Pressed::kNone;
    bool dirty_ = true;
    int shown_percent_ = -1;
    std::size_t shown_index_ = 0;
    GlyphTracker glyphs_;
};

// Words of `text` in lines of at most `width` characters (a longer word is cut).
std::vector<std::string> wrap_text(const std::string &text, std::size_t width);
// `path` in at most `width` characters: its end, after "...", if it is longer.
std::string shorten_path(const std::string &path, std::size_t width);

// Shows the import screen in the window until an import succeeds (returns the imported game
// folder) or the window closes (std::nullopt, *quit true). std::nullopt with *quit false if it
// cannot be shown (logged). `importer` empty: import_ipa into `data_dir` with ipa_expected().
// The route: the SD-card route where the platform has kCapIpaOnSdCard (`data_dir` is then the
// folder searched, <base>), else the folder route where no dialog can show, else the dialog.
std::optional<std::string> run_import_screen(SDL_Window *window, SDL_Renderer *renderer,
                                             const Settings &settings, const std::string &data_dir,
                                             bool *quit, ImportScreen::Importer importer = {});

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_IMPORT_SCREEN_H
