// The app lifecycle (#128, ADR 0063): SDL's app and window events become the app delegate's
// lifecycle messages (mt_uikit_lifecycle_event), and decide when the frontend may present.
//
//   the event (SDL 3.4)                         the delegate                  the frontend
//   SDL_EVENT_WINDOW_MINIMIZED (desktop)        applicationWillResignActive:  hidden
//   SDL_EVENT_WINDOW_RESTORED / _MAXIMIZED      applicationDidBecomeActive:   shown
//   SDL_EVENT_WINDOW_FOCUS_LOST (desktop,       applicationWillResignActive:  hidden (on screen)
//     pause_unfocused on; #145)
//   SDL_EVENT_WINDOW_FOCUS_GAINED (the same)    applicationDidBecomeActive:   shown
//   SDL_EVENT_WILL_ENTER_BACKGROUND             applicationWillResignActive:
//   SDL_EVENT_DID_ENTER_BACKGROUND                                            hidden
//   SDL_EVENT_WILL_ENTER_FOREGROUND                                           shown
//   SDL_EVENT_DID_ENTER_FOREGROUND              applicationDidBecomeActive:   (shown)
//   SDL_EVENT_TERMINATING, SDL_EVENT_QUIT,      applicationWillTerminate:     the run ends
//   SDL_EVENT_WINDOW_CLOSE_REQUESTED
//
// The delegate is active while nothing resigned it: the window is not minimised (on the
// desktop; on iOS SDL sends a minimise with every resign, and the app events are the ones that
// count) and no WILL_ENTER_BACKGROUND is waiting for its DID_ENTER_FOREGROUND. Each change of
// that sends one message, so every resign is followed by exactly one become-active, whatever
// mix of events led there (iOS: a minimise, two app events, two more and a restore). The game
// implements all three (0x2BD9 Clet_Pause, 0x2BB5 Clet_Resume, 0x2ACD: deletes its state).
//
// Hidden (minimised on the desktop, between DID_ENTER_BACKGROUND and WILL_ENTER_FOREGROUND on
// iOS): nothing is uploaded or presented (iOS allows no GPU work in the background), and the
// guest waits in the turn hook as iOS suspends an app (game.cpp), so its run loop and timers
// stand still and the game folds the gap into its sub-timers when it comes back, as it did.
//
// The messages are queued here and sent by deliver() from the turn hook, between run-loop
// turns: an event pumped inside -presentRenderbuffer: (game.cpp's present hook) arrives while
// the guest is inside a call. The hidden state changes at once, so the present that follows
// in the same hook is already not shown. applicationWillTerminate: is the last message; after
// it the run ends (the game has deleted its state).
//
// SDL wants the app events handled in an event watch (SDL_AddEventWatch: on iOS they come from
// UIKit's callbacks, and the app may be suspended before the queue is polled): watch() takes
// them there; handle() takes the window events and the quits from the polled queue and ignores
// the app events there (else each would count twice). In the fixed-step mode (set_real(false))
// only scripted events (game.cpp's --minimise, --background, --quit) change the lifecycle, so a
// run is a function of its command line; a quit still ends it.
//
// pause_unfocused (#145, ADR 0077), the desktop only: with the setting on, the window losing
// focus (SDL_EVENT_WINDOW_FOCUS_LOST) is a minimise to the delegate and the guest
// (applicationWillResignActive:, the guest waits, nothing new is presented), and focus gained
// (SDL_EVENT_WINDOW_FOCUS_GAINED) ends it (applicationDidBecomeActive:). The window stays on
// screen with its last frame (paused_visible(): the frontend redraws that frame on a resize).
// The focus is tracked whatever the setting, so switching it on while unfocused pauses at once
// and switching it off resumes. The iOS mapping ignores the focus (the app events say it all).
//
// Thread-safe: the watch can run on any thread that pushes an event.
#ifndef MAPLETHIEF_PLATFORM_LIFECYCLE_H
#define MAPLETHIEF_PLATFORM_LIFECYCLE_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include <SDL3/SDL_platform_defines.h>

#include "maplethief/recomp/cpu.h"

union SDL_Event;

namespace maplethief::frontend {

// Whether a minimised window resigns the delegate (and hides): on every desktop; not on iOS,
// where SDL minimises the window at every resign and the app events say what happened.
#if defined(SDL_PLATFORM_IOS)
inline constexpr bool kMinimiseResigns = false;
#else
inline constexpr bool kMinimiseResigns = true;
#endif

// The delegate's messages (maplethief/uikit.h's mt_uikit_lifecycle values).
enum class AppMessage : std::uint32_t {
    kWillResignActive = 0, // MT_UIKIT_WILL_RESIGN_ACTIVE
    kDidBecomeActive = 1,  // MT_UIKIT_DID_BECOME_ACTIVE
    kWillTerminate = 2,    // MT_UIKIT_WILL_TERMINATE
};
// "applicationWillResignActive:" etc.
const char *app_message_selector(AppMessage m);
// "SDL_EVENT_WINDOW_MINIMIZED" etc. for the events above, "SDL event 0x..." otherwise (a
// static buffer for those: logging only).
const char *lifecycle_event_name(std::uint32_t type);

// A message queued or sent, with the event that caused it.
struct AppMessageRecord {
    AppMessage message;
    std::uint32_t cause; // the SDL event type
    bool scripted;
};

class Lifecycle {
  public:
    explicit Lifecycle(bool minimise_resigns = kMinimiseResigns);
    ~Lifecycle();
    Lifecycle(const Lifecycle &) = delete;
    Lifecycle &operator=(const Lifecycle &) = delete;

    // Installs the SDL event watch that takes the app events (SDL must be initialised); false
    // (logged) if SDL refused. unwatch() removes it (the destructor does too).
    bool watch();
    void unwatch();

    // Whether real (not scripted) minimises, restores and app events count (false: the
    // fixed-step mode; they are logged and ignored). Quits always count.
    void set_real(bool accept);
    // pause_unfocused (#145): whether the window losing focus resigns the delegate and hides
    // the game (desktop mapping only). Live: a change while unfocused queues the message.
    void set_pause_unfocused(bool on);
    bool pause_unfocused() const;

    // An event from SDL's queue (or, `scripted`, from the script): the window's minimise,
    // restore and maximise and the quits. A polled app event is ignored (the watch took it);
    // a scripted one is applied here.
    // A quit (SDL_EVENT_QUIT, the window's close button, SDL_EVENT_TERMINATING) queues
    // applicationWillTerminate:, once.
    void handle(const SDL_Event &e, bool scripted = false);

    bool active() const;
    // Nothing may be uploaded or presented, and the guest waits.
    bool hidden() const;
    bool presenting() const { return !hidden(); }
    // Hidden only because the window lost focus under pause_unfocused: it is still on screen.
    bool paused_visible() const;
    // A quit is queued or applicationWillTerminate: was sent: the run ends at the next turn.
    bool terminating() const;
    // Messages queued, not sent yet.
    bool pending() const;
    // The first queued applicationWillResignActive: or applicationWillTerminate:, if any: the
    // game is about to be left (#150's autosave runs before deliver() sends it).
    std::optional<AppMessageRecord> leaving() const;

    // Sends the queued messages to the delegate in order (mt_uikit_lifecycle_event), each
    // logged with its cause and `t` (seconds since the first turn). Call it from the turn hook
    // only: the guest must be between calls. Nothing is sent after applicationWillTerminate:.
    // Returns how many were sent.
    int deliver(mt_cpu *cpu, double t);
    // Every message sent so far, in order (tests and the run's summary).
    std::vector<AppMessageRecord> delivered() const;
    // How many polled real events were ignored (set_real(false)).
    std::uint64_t ignored() const;

  private:
    static bool on_watch(void *user, SDL_Event *e);
    void apply(std::uint32_t type, bool scripted); // with mutex_ held
    void update(std::uint32_t cause, bool scripted);

    const bool minimise_resigns_;
    mutable std::mutex mutex_;
    bool watching_ = false;
    bool real_ = true;
    bool minimised_ = false;  // the window (desktop only)
    bool resigned_ = false;   // WILL_ENTER_BACKGROUND until DID_ENTER_FOREGROUND
    bool background_ = false; // DID_ENTER_BACKGROUND until WILL/DID_ENTER_FOREGROUND
    bool unfocused_ = false;  // FOCUS_LOST until FOCUS_GAINED (desktop only)
    bool pause_unfocused_ = false;
    bool active_ = true;      // what the delegate was last told (the launch: active)
    bool quit_ = false;       // applicationWillTerminate: queued
    bool terminated_ = false; // ... and sent
    std::uint64_t ignored_ = 0;
    std::deque<AppMessageRecord> queue_;
    std::vector<AppMessageRecord> delivered_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_LIFECYCLE_H
