// The app lifecycle (#128, ADR 0063); see lifecycle.h.
#include "lifecycle.h"

#include <SDL3/SDL.h>

#include <cstdio>

#include "maplethief/uikit.h"

namespace maplethief::frontend {

const char *app_message_selector(AppMessage m) {
    switch (m) {
    case AppMessage::kWillResignActive:
        return "applicationWillResignActive:";
    case AppMessage::kDidBecomeActive:
        return "applicationDidBecomeActive:";
    case AppMessage::kWillTerminate:
        return "applicationWillTerminate:";
    }
    return "?";
}

const char *lifecycle_event_name(std::uint32_t type) {
    switch (type) {
    case SDL_EVENT_WINDOW_MINIMIZED:
        return "SDL_EVENT_WINDOW_MINIMIZED";
    case SDL_EVENT_WINDOW_RESTORED:
        return "SDL_EVENT_WINDOW_RESTORED";
    case SDL_EVENT_WINDOW_MAXIMIZED:
        return "SDL_EVENT_WINDOW_MAXIMIZED";
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        return "SDL_EVENT_WILL_ENTER_BACKGROUND";
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        return "SDL_EVENT_DID_ENTER_BACKGROUND";
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        return "SDL_EVENT_WILL_ENTER_FOREGROUND";
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        return "SDL_EVENT_DID_ENTER_FOREGROUND";
    case SDL_EVENT_TERMINATING:
        return "SDL_EVENT_TERMINATING";
    case SDL_EVENT_QUIT:
        return "SDL_EVENT_QUIT";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return "SDL_EVENT_WINDOW_CLOSE_REQUESTED";
    default: {
        static char other[32];
        std::snprintf(other, sizeof other, "SDL event 0x%X", static_cast<unsigned>(type));
        return other;
    }
    }
}

namespace {

bool is_app_event(std::uint32_t type) {
    return type == SDL_EVENT_WILL_ENTER_BACKGROUND || type == SDL_EVENT_DID_ENTER_BACKGROUND ||
           type == SDL_EVENT_WILL_ENTER_FOREGROUND || type == SDL_EVENT_DID_ENTER_FOREGROUND ||
           type == SDL_EVENT_TERMINATING;
}

bool is_window_event(std::uint32_t type) {
    return type == SDL_EVENT_WINDOW_MINIMIZED || type == SDL_EVENT_WINDOW_RESTORED ||
           type == SDL_EVENT_WINDOW_MAXIMIZED;
}

bool is_quit(std::uint32_t type) {
    return type == SDL_EVENT_QUIT || type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
           type == SDL_EVENT_TERMINATING;
}

} // namespace

Lifecycle::Lifecycle(bool minimise_resigns) : minimise_resigns_(minimise_resigns) {}

Lifecycle::~Lifecycle() { unwatch(); }

bool Lifecycle::watch() {
    if (watching_) {
        return true;
    }
    if (!SDL_AddEventWatch(on_watch, this)) {
        SDL_Log("lifecycle: no event watch (%s): the app events are not seen", SDL_GetError());
        return false;
    }
    watching_ = true;
    return true;
}

void Lifecycle::unwatch() {
    if (watching_) {
        SDL_RemoveEventWatch(on_watch, this);
        watching_ = false;
    }
}

bool Lifecycle::on_watch(void *user, SDL_Event *e) {
    if (is_app_event(e->type)) {
        Lifecycle &l = *static_cast<Lifecycle *>(user);
        const std::lock_guard<std::mutex> lock(l.mutex_);
        l.apply(e->type, false);
    }
    return true; // a watch's answer is ignored; the event stays in the queue
}

void Lifecycle::set_real(bool accept) {
    const std::lock_guard<std::mutex> lock(mutex_);
    real_ = accept;
}

void Lifecycle::handle(const SDL_Event &e, bool scripted) {
    const std::uint32_t type = e.type;
    if (!scripted && is_app_event(type)) {
        return; // the watch has applied it
    }
    if (!is_window_event(type) && !is_app_event(type) && !is_quit(type)) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    apply(type, scripted);
}

void Lifecycle::apply(std::uint32_t type, bool scripted) {
    if (is_quit(type)) {
        if (!quit_) {
            quit_ = true;
            queue_.push_back({AppMessage::kWillTerminate, type, scripted});
        }
        return;
    }
    if (!scripted && !real_) {
        ignored_++;
        SDL_Log("lifecycle: %s ignored (the fixed-step mode takes only scripted lifecycle events)",
                lifecycle_event_name(type));
        return;
    }
    switch (type) {
    case SDL_EVENT_WINDOW_MINIMIZED:
        if (!minimise_resigns_) {
            return;
        }
        minimised_ = true;
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        if (!minimise_resigns_) {
            return;
        }
        minimised_ = false;
        break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        resigned_ = true;
        break;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        background_ = true;
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        background_ = false;
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        background_ = false;
        resigned_ = false;
        break;
    default:
        return;
    }
    update(type, scripted);
}

void Lifecycle::update(std::uint32_t cause, bool scripted) {
    const bool active = !minimised_ && !resigned_ && !background_;
    if (active != active_ && !quit_) {
        active_ = active;
        queue_.push_back({active ? AppMessage::kDidBecomeActive : AppMessage::kWillResignActive,
                          cause, scripted});
    }
}

bool Lifecycle::active() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool Lifecycle::hidden() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return minimised_ || background_;
}

bool Lifecycle::terminating() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return quit_;
}

bool Lifecycle::pending() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return !queue_.empty();
}

int Lifecycle::deliver(mt_cpu *cpu, double t) {
    int sent = 0;
    while (true) {
        AppMessageRecord r{};
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty() || terminated_) {
                return sent;
            }
            r = queue_.front();
            queue_.pop_front();
            if (r.message == AppMessage::kWillTerminate) {
                terminated_ = true;
            }
        }
        // Unlocked: the guest's method may present, and the present hook pumps events.
        const int implemented =
            mt_uikit_lifecycle_event(cpu, static_cast<std::uint32_t>(r.message));
        SDL_Log("lifecycle: %s at %.3f s (%s%s)%s", app_message_selector(r.message), t,
                lifecycle_event_name(r.cause), r.scripted ? ", scripted" : "",
                implemented != 0 ? "" : ": the delegate does not implement it");
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            delivered_.push_back(r);
        }
        sent++;
    }
}

std::vector<AppMessageRecord> Lifecycle::delivered() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return delivered_;
}

std::uint64_t Lifecycle::ignored() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ignored_;
}

} // namespace maplethief::frontend
