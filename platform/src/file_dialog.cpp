// The system's file dialogs and "show this folder" (#192); see file_dialog.h.
#include "file_dialog.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace maplethief::frontend {

const char *dialog_outcome_name(FileDialogResult::Outcome outcome) {
    switch (outcome) {
    case FileDialogResult::Outcome::kChosen:
        return "chosen";
    case FileDialogResult::Outcome::kCancelled:
        return "cancelled";
    case FileDialogResult::Outcome::kFailed:
        return "failed";
    }
    return "?";
}

DialogRoute dialog_route(const DialogPlace &place) {
    if (place.ios || place.gamescope || (place.linux_os && place.fullscreen)) {
        return DialogRoute::kFolder;
    }
    return DialogRoute::kDialog;
}

DialogPlace dialog_place(SDL_Window *window) {
    DialogPlace place;
#if defined(SDL_PLATFORM_IOS)
    place.ios = true;
#endif
#if defined(SDL_PLATFORM_LINUX)
    place.linux_os = true;
#endif
    const char *desktop = SDL_getenv("XDG_CURRENT_DESKTOP");
    const char *deck = SDL_getenv("SteamDeck");
    place.gamescope = (desktop != nullptr && SDL_strcasecmp(desktop, "gamescope") == 0) ||
                      (deck != nullptr && std::strcmp(deck, "1") == 0);
    place.fullscreen = window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    return place;
}

std::string file_url(std::string_view path) {
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    const bool drive = p.size() >= 2 && p[1] == ':' &&
                       ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
    std::string out = "file://";
    if (drive || p.empty() || p[0] != '/') {
        out += '/';
    }
    static const char kHex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < p.size(); i++) {
        const auto ch = static_cast<unsigned char>(p[i]);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' ||
                                ch == '~' || ch == '/' || (drive && i == 1);
        if (unreserved) {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += kHex[ch >> 4];
            out += kHex[ch & 15];
        }
    }
    return out;
}

FileDialogs::FileDialogs()
    : backend_(&FileDialogs::sdl_backend), opener_(&FileDialogs::sdl_url_opener),
      shared_(std::make_shared<Shared>()) {}

bool FileDialogs::busy() const {
    const std::lock_guard<std::mutex> lock(shared_->mutex);
    return !shared_->pending.empty();
}

bool FileDialogs::show(SDL_Window *window, const FileDialogRequest &request, Done done,
                       std::string *why) {
    if (busy()) {
        if (why != nullptr) {
            *why = "a file dialog is already open";
        }
        return false;
    }
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->pending.push_back(pending);
    }
    // The answer holds the queue and its request, never `this`: it may come on any thread, and
    // after this object is gone.
    std::weak_ptr<Shared> weak = shared_;
    auto answer = [weak, pending](FileDialogResult result) {
        const std::shared_ptr<Shared> shared = weak.lock();
        if (shared == nullptr) {
            return;
        }
        const std::lock_guard<std::mutex> lock(shared->mutex);
        if (!pending->answered) {
            pending->answered = true;
            pending->result = std::move(result);
        }
    };
    SDL_Log("files: the %s dialog for %s opens at %s",
            request.kind == FileDialogRequest::Kind::kSaveFile ? "save" : "open",
            request.title.c_str(), request.default_location.c_str());
    if (!backend_ || !backend_(window, request, answer)) {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        std::erase(shared_->pending, pending);
        if (why != nullptr) {
            *why = "the file dialog cannot open";
        }
        return false;
    }
    return true;
}

std::size_t FileDialogs::poll() {
    std::vector<std::shared_ptr<Pending>> ready;
    {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        for (auto it = shared_->pending.begin(); it != shared_->pending.end();) {
            if ((*it)->answered) {
                ready.push_back(*it);
                it = shared_->pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto &p : ready) {
        SDL_Log("files: the dialog: %s%s%s", dialog_outcome_name(p->result.outcome),
                p->result.path.empty() && p->result.why.empty() ? "" : ": ",
                p->result.outcome == FileDialogResult::Outcome::kFailed ? p->result.why.c_str()
                                                                        : p->result.path.c_str());
        if (p->done) {
            p->done(p->result);
        }
    }
    return ready.size();
}

bool FileDialogs::open_folder(const std::string &folder, std::string *why) {
    const std::string url = file_url(folder);
    std::string error;
    if (!opener_ || !opener_(url, &error)) {
        SDL_Log("files: cannot open %s: %s", url.c_str(), error.c_str());
        if (why != nullptr) {
            *why = error.empty() ? "it cannot be opened here" : error;
        }
        return false;
    }
    SDL_Log("files: opened %s", url.c_str());
    return true;
}

namespace {

// What SDL's callback needs: the filters stay valid until it is called (SDL's rule), and the
// answer. Deleted by the callback.
struct SdlDialogContext {
    std::vector<std::string> strings;
    std::vector<SDL_DialogFileFilter> filters;
    std::function<void(FileDialogResult)> answer;
};

void SDLCALL sdl_dialog_done(void *userdata, const char *const *filelist, int /*filter*/) {
    std::unique_ptr<SdlDialogContext> ctx(static_cast<SdlDialogContext *>(userdata));
    FileDialogResult r;
    if (filelist == nullptr) {
        r.outcome = FileDialogResult::Outcome::kFailed;
        r.why = SDL_GetError();
    } else if (filelist[0] == nullptr) {
        r.outcome = FileDialogResult::Outcome::kCancelled;
    } else {
        r.outcome = FileDialogResult::Outcome::kChosen;
        r.path = filelist[0];
    }
    ctx->answer(std::move(r));
}

} // namespace

bool FileDialogs::sdl_backend(SDL_Window *window, const FileDialogRequest &request,
                              std::function<void(FileDialogResult)> answer) {
    auto ctx = std::make_unique<SdlDialogContext>();
    ctx->answer = std::move(answer);
    ctx->strings.reserve(request.filters.size() * 2);
    for (const FileDialogFilter &f : request.filters) {
        ctx->strings.push_back(f.name);
        ctx->strings.push_back(f.pattern);
    }
    for (std::size_t i = 0; i < request.filters.size(); i++) {
        ctx->filters.push_back(
            SDL_DialogFileFilter{ctx->strings[2 * i].c_str(), ctx->strings[2 * i + 1].c_str()});
    }
    const SDL_DialogFileFilter *filters = ctx->filters.empty() ? nullptr : ctx->filters.data();
    const int count = static_cast<int>(ctx->filters.size());
    const char *where =
        request.default_location.empty() ? nullptr : request.default_location.c_str();
    SdlDialogContext *raw = ctx.release(); // the callback owns it now; it may run at once
    if (request.kind == FileDialogRequest::Kind::kSaveFile) {
        SDL_ShowSaveFileDialog(sdl_dialog_done, raw, window, filters, count, where);
    } else {
        SDL_ShowOpenFileDialog(sdl_dialog_done, raw, window, filters, count, where, false);
    }
    return true; // SDL reports a dialog that cannot show through the callback (filelist NULL)
}

bool FileDialogs::sdl_url_opener(const std::string &url, std::string *why) {
#if defined(SDL_PLATFORM_IOS)
    (void)url;
    if (why != nullptr) {
        *why = "not on iOS: the saves are in the Files app";
    }
    return false;
#else
    if (!SDL_OpenURL(url.c_str())) {
        if (why != nullptr) {
            *why = SDL_GetError();
        }
        return false;
    }
    return true;
#endif
}

} // namespace maplethief::frontend
