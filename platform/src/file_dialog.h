// The system's file dialogs and "show this folder" (#192, ADR 0078's addendum): a small wrapper
// over SDL 3's SDL_ShowOpenFileDialog / SDL_ShowSaveFileDialog and SDL_OpenURL that the settings
// menu's Saves tab uses, and #182's IPA import can (the same open dialog with its own filter).
//
//   FileDialogs::show     asks for one file to open or a place to save. SDL may call back on
//                         another thread, or before show() returns; the answer waits in a queue
//                         and poll() hands it to its callback on the caller's (the frontend's)
//                         thread. One dialog at a time: busy() while one is up.
//   dialog_route          whether a dialog can show here, or the caller should use its folder
//                         instead (iOS, a Steam Deck's Game Mode, a fullscreen Linux window).
//   file_url, open_folder a folder as a file:// URL (percent-encoded, Windows drive paths too),
//                         given to SDL_OpenURL: Finder, Explorer or the file manager opens it.
//
// The seam: set_backend() and set_url_opener() replace SDL in the tests, which then answer the
// requests themselves (chosen, cancelled, failed), on any thread.
#ifndef MAPLETHIEF_PLATFORM_FILE_DIALOG_H
#define MAPLETHIEF_PLATFORM_FILE_DIALOG_H

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

struct FileDialogFilter {
    std::string name;    // "MapleThief saves (zip)"
    std::string pattern; // SDL's: extensions without dots, ';'-separated ("zip"), or "*"
};

struct FileDialogRequest {
    enum class Kind { kOpenFile, kSaveFile };
    Kind kind = Kind::kOpenFile;
    std::string title; // a log line's name for it ("export saves"); SDL 3.4 shows its own title
    // Where it starts: a folder, or (a save) a folder and the suggested name.
    std::string default_location;
    std::vector<FileDialogFilter> filters;
};

struct FileDialogResult {
    enum class Outcome { kChosen, kCancelled, kFailed };
    Outcome outcome = Outcome::kCancelled;
    std::string path; // kChosen: the file (UTF-8)
    std::string why;  // kFailed: SDL's error
};

const char *dialog_outcome_name(FileDialogResult::Outcome outcome);

// Where a file is chosen: the system's dialog, or the caller's folder (exports/, import/).
enum class DialogRoute { kDialog, kFolder };
struct DialogPlace {
    bool ios = false;        // the Files app's route (ADR 0074): no dialog
    bool gamescope = false;  // a Steam Deck's Game Mode: a dialog would hide behind the game
    bool linux_os = false;   // X11 / Wayland: a portal's dialog is not the window's child
    bool fullscreen = false; // the window fills the display
    bool no_dialogs = false; // the platform has none (platform_caps.h: the Nintendo Switch, #241)
};
// kFolder on iOS, under gamescope, for a fullscreen window on Linux and where the platform has
// no dialogs; kDialog otherwise (a macOS sheet and a Windows dialog are owned by the window, so
// they show over it in fullscreen).
DialogRoute dialog_route(const DialogPlace &place);
// The place of `window` on this platform (SDL_PLATFORM_*, the window's flags, the environment's
// XDG_CURRENT_DESKTOP = gamescope or SteamDeck = 1).
DialogPlace dialog_place(SDL_Window *window);

// "file:///Users/me/Library/Application%20Support/MapleThief" from an absolute path (UTF-8):
// backslashes become slashes, a drive path gets "file:///C:/...", and every byte outside
// RFC 3986's unreserved set, '/' and a drive's ':' is %XX-encoded.
std::string file_url(std::string_view path);

class FileDialogs {
  public:
    using Done = std::function<void(const FileDialogResult &)>;
    // Shows the dialog for `request`; `answer` is called exactly once (on any thread) with the
    // outcome. False if it could not even be asked (then `answer` is never called).
    using Backend = std::function<bool(SDL_Window *window, const FileDialogRequest &request,
                                       std::function<void(FileDialogResult)> answer)>;
    // Opens a URL; false (with SDL's error in `why`) if it cannot.
    using UrlOpener = std::function<bool(const std::string &url, std::string *why)>;

    FileDialogs();
    FileDialogs(const FileDialogs &) = delete;
    FileDialogs &operator=(const FileDialogs &) = delete;

    void set_backend(Backend backend) { backend_ = std::move(backend); }
    void set_url_opener(UrlOpener opener) { opener_ = std::move(opener); }

    // Asks for a file; `done` gets the result at a later poll(). False (and `done` is never
    // called) while another dialog is up or if the backend could not show it: SDL's error is
    // then in `why`.
    bool show(SDL_Window *window, const FileDialogRequest &request, Done done, std::string *why);
    // Hands every answer that came since to its `done`, on this thread. The number handed over.
    std::size_t poll();
    bool busy() const;

    // Shows `folder` (an absolute path) in the file manager: SDL_OpenURL(file_url(folder)).
    bool open_folder(const std::string &folder, std::string *why);

    // The backends SDL gives (the defaults).
    static bool sdl_backend(SDL_Window *window, const FileDialogRequest &request,
                            std::function<void(FileDialogResult)> answer);
    static bool sdl_url_opener(const std::string &url, std::string *why);

  private:
    // One request: what poll() hands over, filled in by the answer.
    struct Pending {
        Done done;
        bool answered = false;
        FileDialogResult result;
    };
    struct Shared {
        std::mutex mutex;
        std::vector<std::shared_ptr<Pending>> pending;
    };

    Backend backend_;
    UrlOpener opener_;
    std::shared_ptr<Shared> shared_; // kept alive by an answer that comes after this is gone
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_FILE_DIALOG_H
