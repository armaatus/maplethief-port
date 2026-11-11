// The Nintendo Switch's part of MapleThief's start (switch_app.h, #238).
#include "switch_app.h"

#include <SDL3/SDL.h>
#include <netinet/in.h> // struct in_addr, for nxlink.h
#include <switch.h>
#include <unistd.h>

#include <malloc.h>

#include <cstdio>
#include <memory>
#include <mutex>

#include "maplethief/memory.h"

// newlib's heap bounds, which libnx's start-up sets to the heap the loader handed over.
extern "C" char *fake_heap_start;
extern "C" char *fake_heap_end;

namespace mt::switch_nx {

namespace {

using maplethief::frontend::switch_focus_name;

// The applet messages, for the log (#240): what sdl3-nx's hook (SWITCH_AppletHook,
// SWITCH_PumpEvents) turns into SDL's events, seen on the console.
AppletHookCookie g_applet_hook;

void log_applet(AppletHookType hook, void *) {
    switch (hook) {
    case AppletHookType_OnFocusState:
        SDL_Log("applet: focus %s",
                switch_focus_name(static_cast<int>(appletGetFocusState())).c_str());
        break;
    case AppletHookType_OnOperationMode:
        SDL_Log("applet: operation mode %s",
                appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
        break;
    case AppletHookType_OnPerformanceMode:
        SDL_Log("applet: performance mode %d", static_cast<int>(appletGetPerformanceMode()));
        break;
    case AppletHookType_OnExitRequest:
        SDL_Log("applet: exit requested (closed from the HOME menu)");
        break;
    case AppletHookType_OnResume:
        SDL_Log("applet: resumed");
        break;
    default:
        break;
    }
}

} // namespace

NxApp::NxApp() {
    appletHook(&g_applet_hook, log_applet, nullptr);
    hooked_ = true;
    // nxlink fills __nxlink_host from the arguments it sends; the Homebrew Menu does not. Only
    // then are the sockets worth their memory (applet mode is short of it, ADR 0138).
    if (__nxlink_host.s_addr == 0) {
        return;
    }
    sockets_ = R_SUCCEEDED(socketInitializeDefault());
    nxlink_ = sockets_ ? nxlinkStdio() : -1;
    if (nxlink_ >= 0) {
        std::fprintf(stderr, "MapleThief " MT_VERSION ": stdout and stderr over nxlink\n");
        std::fflush(stderr);
    }
}

NxApp::~NxApp() {
    if (hooked_) {
        appletUnhook(&g_applet_hook);
    }
    std::fflush(stdout);
    std::fflush(stderr);
    if (nxlink_ >= 0) {
        close(nxlink_);
    }
    if (sockets_) {
        socketExit();
    }
}

void prepare_sdl() { SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2"); }

maplethief::frontend::SwitchScreen current_screen() {
    return maplethief::frontend::switch_screen(appletGetOperationMode() ==
                                               AppletOperationMode_Console);
}

maplethief::frontend::SwitchRunMode run_mode() {
    return maplethief::frontend::switch_run_mode(static_cast<int>(appletGetAppletType()));
}

std::optional<maplethief::frontend::MemorySample> memory_sample() {
    maplethief::frontend::MemorySample s;
    std::uint64_t committed = 0;
    mt_mem_committed_bytes(&committed);
    s.guest_committed = committed;
    s.heap_size = static_cast<std::uint64_t>(fake_heap_end - fake_heap_start);
    s.heap_used = static_cast<std::uint64_t>(mallinfo().uordblks);
    u64 total = 0, used = 0;
    if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
        return std::nullopt;
    }
    s.process_total = total;
    s.process_used = used;
    return s;
}

namespace {

std::mutex g_boost_mutex;
int g_boosts = 0;
bool g_boost_refused = false; // logged once

void apply_boost(bool on) {
    const Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    if (R_FAILED(rc)) {
        if (!g_boost_refused) {
            g_boost_refused = true;
            SDL_Log("switch: no CPU boost (appletSetCpuBoostMode: 0x%X): loads run at the "
                    "normal clock",
                    static_cast<unsigned>(rc));
        }
        return;
    }
    SDL_Log("switch: CPU boost %s", on ? "on (a long load)" : "off");
}

std::unique_ptr<CpuBoost> g_load_boost; // set_load_boost's

} // namespace

CpuBoost::CpuBoost() {
    const std::lock_guard<std::mutex> lock(g_boost_mutex);
    if (g_boosts++ == 0) {
        apply_boost(true);
    }
}

CpuBoost::~CpuBoost() {
    const std::lock_guard<std::mutex> lock(g_boost_mutex);
    if (--g_boosts == 0) {
        apply_boost(false);
    }
}

void set_load_boost(bool on) {
    if (on && g_load_boost == nullptr) {
        g_load_boost = std::make_unique<CpuBoost>();
    } else if (!on) {
        g_load_boost.reset();
    }
}

} // namespace mt::switch_nx
