# SDL's GPU renderer on deko3d (ADR 0143, #242): a change to nx-mod/sdl3-nx at the pinned commit
# 07ea26a3 (ADR 0138), run by FetchContent as part of SDL3's PATCH_COMMAND in the extracted
# source folder:
#   cmake -P cmake/patches/sdl3-nx-gpu-renderer.cmake
#
# 1. The renderer's shaders. SDL's GPU renderer (SDL_CreateGPURenderer) carries its shaders as
#    SPIR-V, DXIL and MSL, and as "private" shaders only when SDL_GPU_PRIVATE is defined, which
#    the Switch build doesn't (that names a console SDK's own GPU driver). So on deko3d, whose
#    shaders are DKSH (SDL_GPU_SHADERFORMAT_PRIVATE), the renderer could not start. Our
#    sdl3-nx-render-shaders.h holds SDL's own render shaders compiled to DKSH (scripts/
#    build-shaders, from this archive's HLSL); it goes in as SDL_render_private_shaders.h, the
#    header SDL_shaders_gpu.c includes for private shaders, and SDL_shaders_gpu.c uses them
#    when SDL_GPU_DEKO3D is on.
# 2. The swapchain follows the window. The deko3d backend makes its swapchain at the window's
#    size when the window is claimed and never again; at a dock or undock the window changes
#    size (sdl3-nx-window-size.cmake) and the swapchain kept the old one. Acquiring a swapchain
#    texture now remakes the swapchain at the window's size when they differ, with the present
#    mode the caller last set.
#
# The files are changed only if they hold the expected text; anything else (another commit, the
# change already there upstream) fails the configure step, loudly. A second run on the same
# source changes nothing (the header is copied again).

set(header_from "${CMAKE_CURRENT_LIST_DIR}/sdl3-nx-render-shaders.h")
set(header_to "src/render/gpu/SDL_render_private_shaders.h")
if(NOT EXISTS "${header_from}")
    message(FATAL_ERROR "sdl3-nx-gpu-renderer.cmake: no ${header_from}")
endif()
file(COPY_FILE "${header_from}" "${header_to}" ONLY_IF_DIFFERENT)

# Replaces each old_<name> with new_<name> in <file>, or does nothing if <marker> is there.
function(_sdl3_nx_patch file marker)
    file(READ "${file}" text)
    string(FIND "${text}" "${marker}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    foreach(name IN LISTS ARGN)
        string(FIND "${text}" "${old_${name}}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "sdl3-nx-gpu-renderer.cmake: ${file} does not hold the expected "
                                "${name}; the SDL3 source is not commit 07ea26a3 (ADR 0138)")
        endif()
        string(REPLACE "${old_${name}}" "${new_${name}}" text "${text}")
    endforeach()
    file(WRITE "${file}" "${text}")
endfunction()

set(old_private [==[#if defined(SDL_GPU_PRIVATE)
#define IF_PRIVATE(...)     __VA_ARGS__
]==])
set(new_private [==[#if defined(SDL_GPU_PRIVATE) || defined(SDL_GPU_DEKO3D) /* MapleThief #242: DKSH on deko3d */
#define IF_PRIVATE(...)     __VA_ARGS__
]==])
_sdl3_nx_patch("src/render/gpu/SDL_shaders_gpu.c" "MapleThief #242" private)

set(old_interval [==[static bool DEKO3D_SupportsSwapchainComposition(]==])
set(new_interval [==[/* MapleThief #242: the swap interval the caller set, kept for a swapchain made again at a
   new window size (a dock or undock). */
static int DKO_swapInterval = 1;

static bool DEKO3D_SupportsSwapchainComposition(]==])
set(old_setmode [==[    if (renderer->window) {
        dkSwapchainSetSwapInterval(renderer->window->swapchain, mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? 0 : 1);
    }
]==])
set(new_setmode [==[    DKO_swapInterval = mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? 0 : 1;
    if (renderer->window) {
        dkSwapchainSetSwapInterval(renderer->window->swapchain, DKO_swapInterval);
    }
]==])
set(old_acquire [==[    if (!data) {
        return SDL_SetError("deko3d: window not claimed");
    }
    SDL_LockMutex(renderer->submitLock);
    int slot = dkQueueAcquireImage(renderer->queue, data->swapchain); // the queue waits for it
]==])
set(new_acquire [==[    if (!data) {
        return SDL_SetError("deko3d: window not claimed");
    }
    {
        /* MapleThief #242: the window changed size (a dock or undock): the swapchain again, at
           the new size, so that SDL's renderer sees it and remakes its backbuffer. */
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(data->window, &w, &h);
        if (w > 0 && h > 0 && ((Uint32)w != data->width || (Uint32)h != data->height)) {
            SDL_Window *claimed = data->window;
            DEKO3D_ReleaseWindow((SDL_GPURenderer *)renderer, claimed);
            if (!DEKO3D_ClaimWindow((SDL_GPURenderer *)renderer, claimed)) {
                return false;
            }
            data = renderer->window;
            dkSwapchainSetSwapInterval(data->swapchain, DKO_swapInterval);
        }
    }
    SDL_LockMutex(renderer->submitLock);
    int slot = dkQueueAcquireImage(renderer->queue, data->swapchain); // the queue waits for it
]==])
_sdl3_nx_patch("src/gpu/deko3d/SDL_gpu_deko3d.c" "MapleThief #242" interval setmode acquire)

message(STATUS "sdl3-nx: SDL's GPU renderer has DKSH shaders, and the deko3d swapchain follows "
               "the window's size (#242)")
