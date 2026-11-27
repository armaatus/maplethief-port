// The hang watchdog (hang_watchdog.h, #337, ADR 0195).
#include "hang_watchdog.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "crash_report.h"
#include "game.h"
#include "maplethief/diag.h"

namespace maplethief::frontend {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

std::atomic<std::int64_t> g_last{0};    // the last beat (or the end of the last pause)
std::atomic<int> g_paused{0};           // HangPause objects alive
std::atomic<bool> g_gap_broken{false};  // a pause since the last beat: no gap to measure
std::atomic<bool> g_stalled{false};     // this stall was reported
std::atomic<std::int64_t> g_longest{0}; // ns
std::atomic<std::uint32_t> g_hangs{0};
std::atomic<bool> g_running{false};
double g_seconds = 0;

std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_wake;
bool g_stop = false;

char g_report[32768]; // the watchdog thread's only

// A stall: the report, the file, the question.
void on_stall(double gap) {
    char what[96];
    std::snprintf(what, sizeof what, "no run-loop turn for %.1f s", gap);
    std::size_t n = mt_hang_report(what, g_report, sizeof g_report);
    if (n == 0u) {
        n = static_cast<std::size_t>(std::snprintf(
            g_report, sizeof g_report, "=== hang: %s (no guest thread to sample) ===\n", what));
    }
    n = std::min(n, sizeof g_report - 1u);
    g_hangs.fetch_add(1);
    const std::string path = hang_report_write(g_report, n);
    SDL_Log("hang: %s: the guest's thread is stuck; report %s", what,
            path.empty() ? "not written (no crashes folder)" : path.c_str());
    if (path.empty()) {
        return;
    }
    if (ask_hang_choice(path) == HangChoice::kQuit) {
        SDL_Log("hang: the player chose Quit: the process ends (status %d)", kExitHang);
        std::_Exit(kExitHang);
    }
    SDL_Log("hang: keep waiting");
}

void watch() {
    const auto limit_ns = static_cast<std::int64_t>(g_seconds * 1e9);
    const auto tick = std::chrono::milliseconds(
        std::clamp<std::int64_t>(limit_ns / 10'000'000, 10, 250)); // a tenth of the limit
    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        g_wake.wait_for(lock, tick);
        if (g_stop || g_paused.load() > 0 || g_stalled.load()) {
            continue;
        }
        const std::int64_t gap = now_ns() - g_last.load();
        if (gap >= limit_ns) {
            g_stalled.store(true);
            lock.unlock(); // stop() may wait for the question's answer, not for the lock
            on_stall(static_cast<double>(gap) / 1e9);
            lock.lock();
        }
    }
}

// Every event SDL hands the main thread is a sign of life: a macOS window drag or live resize
// runs the system's own loop, outside the run loop's turns.
bool SDLCALL on_event(void *, SDL_Event *) {
    if (SDL_IsMainThread()) {
        hang_watchdog_beat();
    }
    return true;
}

} // namespace

bool hang_watchdog_supported() {
#if defined(SDL_PLATFORM_IOS) || defined(__ANDROID__) || defined(__SWITCH__)
    return false;
#else
    return true;
#endif
}

double hang_watch_seconds(double asked, bool fixed_step, bool forced) {
    if (!hang_watchdog_supported()) {
        return 0;
    }
    if (asked >= 0) {
        return asked;
    }
    return fixed_step && !forced ? 0 : kHangSeconds;
}

void hang_watchdog_start(mt_cpu *cpu, double seconds) {
    hang_watchdog_stop();
    g_longest.store(0);
    g_hangs.store(0);
    g_seconds = hang_watchdog_supported() ? seconds : 0;
    if (g_seconds <= 0) {
        return;
    }
    if (mt_hang_watch_thread(cpu) != 0) {
        SDL_Log("hang: this thread cannot be sampled: a hang report has the registers only");
    }
    g_last.store(now_ns());
    g_gap_broken.store(false);
    g_stalled.store(false);
    g_stop = false;
    SDL_AddEventWatch(on_event, nullptr);
    g_thread = std::thread(watch);
    g_running.store(true);
    SDL_Log("hang: the watchdog reports a run-loop turn that takes %g s or more", g_seconds);
}

void hang_watchdog_stop() {
    if (!g_running.exchange(false)) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    g_thread.join();
    SDL_RemoveEventWatch(on_event, nullptr);
    mt_hang_watch_thread(nullptr);
}

bool hang_watchdog_running() { return g_running.load(); }

void hang_watchdog_beat() {
    if (!g_running.load()) {
        return;
    }
    const std::int64_t now = now_ns();
    if (!g_gap_broken.exchange(false)) {
        const std::int64_t gap = now - g_last.load();
        if (gap > g_longest.load()) {
            g_longest.store(gap);
        }
    }
    g_last.store(now);
    g_stalled.store(false);
}

HangPause::HangPause() {
    g_paused.fetch_add(1);
    g_gap_broken.store(true);
}

HangPause::~HangPause() {
    g_last.store(now_ns());
    g_gap_broken.store(true);
    g_paused.fetch_sub(1);
}

HangStats hang_watchdog_stats() {
    HangStats s;
    s.longest_gap = static_cast<double>(g_longest.load()) / 1e9;
    s.hangs = g_hangs.load();
    s.seconds = g_seconds;
    return s;
}

} // namespace maplethief::frontend
