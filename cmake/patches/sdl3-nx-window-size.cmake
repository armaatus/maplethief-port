# A fix to nx-mod/sdl3-nx at the pinned commit 07ea26a3 (ADR 0138's addendum, #238), run by
# FetchContent as SDL3's PATCH_COMMAND in the extracted source folder:
#   cmake -P cmake/patches/sdl3-nx-window-size.cmake
#
# SDL's Switch backend resizes its window to 1280 x 720 or 1920 x 1080 when the console is
# undocked or docked (SWITCH_PumpEvents calls SDL_SetWindowSize). But SWITCH_SetWindowSize
# rebuilt the EGL surface at window->w x window->h, the size before the change (SDL 3 passes the
# new one in window->pending), and never told SDL the window had changed size: the picture kept
# the old size and the renderer never heard of the new one. The fix reads window->pending,
# rebuilds the surface at that size and sends SDL_EVENT_WINDOW_RESIZED, as SDL 3 expects of a
# backend, which updates window->w and h and the renderer's output size (and gives the
# presenter SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED).
#
# The file is changed only if it holds the expected text; anything else (another commit, the fix
# already there upstream) fails the configure step, loudly.

set(file "src/video/switch/SDL_switchvideo.c")
file(READ "${file}" text)
string(FIND "${text}" "MapleThief #238" done)
if(NOT done EQUAL -1)
    return() # already fixed (the patch step run again on the same source)
endif()

set(old_include [==[#include "../SDL_sysvideo.h"
]==])
set(new_include [==[#include "../SDL_sysvideo.h"
#include "../../events/SDL_windowevents_c.h" /* MapleThief #238: SDL_SendWindowEvent */
]==])

set(old_body [==[static void SWITCH_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    u32 w = 0, h = 0;
    SDL_WindowData *data = (SDL_WindowData *)window->internal;
    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    NWindow *nWindow = nwindowGetDefault();

    if (window->w != w || window->h != h) {
        if (data != NULL && data->egl_surface != EGL_NO_SURFACE) {
            SDL_EGL_MakeCurrent(_this, NULL, NULL);
            SDL_EGL_DestroySurface(_this, data->egl_surface);
            nwindowSetDimensions(nWindow, window->w, window->h);
            data->egl_surface = SDL_EGL_CreateSurface(_this, window, nWindow);
            SDL_EGL_MakeCurrent(_this, data->egl_surface, ctx);
        }
    }
}
]==])
set(new_body [==[static void SWITCH_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    /* MapleThief #238: the new size is window->pending (SDL 3); the surface is rebuilt at it,
       and SDL hears of it (window->w and h, the renderer's output size). */
    const int w = window->pending.w, h = window->pending.h;
    SDL_WindowData *data = (SDL_WindowData *)window->internal;
    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    NWindow *nWindow = nwindowGetDefault();

    if (w <= 0 || h <= 0 || (window->w == w && window->h == h)) {
        return;
    }
    if (data != NULL && data->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_MakeCurrent(_this, NULL, NULL);
        SDL_EGL_DestroySurface(_this, data->egl_surface);
        nwindowSetDimensions(nWindow, (u32)w, (u32)h);
        data->egl_surface = SDL_EGL_CreateSurface(_this, window, nWindow);
        SDL_EGL_MakeCurrent(_this, data->egl_surface, ctx);
    } else {
        nwindowSetDimensions(nWindow, (u32)w, (u32)h);
    }
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, w, h);
}
]==])

foreach(pair IN ITEMS include body)
    string(FIND "${text}" "${old_${pair}}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "sdl3-nx-window-size.cmake: ${file} does not hold the expected "
                            "${pair}; the SDL3 source is not commit 07ea26a3 (ADR 0138)")
    endif()
    string(REPLACE "${old_${pair}}" "${new_${pair}}" text "${text}")
endforeach()
file(WRITE "${file}" "${text}")
message(STATUS "sdl3-nx: SWITCH_SetWindowSize follows a dock or undock (#238)")
