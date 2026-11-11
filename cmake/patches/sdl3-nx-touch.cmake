# A fix to nx-mod/sdl3-nx at the pinned commit 07ea26a3 (ADR 0140, #239), run by FetchContent as
# part of SDL3's PATCH_COMMAND in the extracted source folder, after sdl3-nx-window-size.cmake:
#   cmake -P cmake/patches/sdl3-nx-touch.cmake
#
# SDL's Switch backend reads the touch screen in SWITCH_PollTouch, a port of SDL 2's. SDL 3's
# SDL_SendTouch takes the event's type (SDL_EVENT_FINGER_DOWN, _UP) where SDL 2's took a down
# flag, and the backend still passes `false` (0, neither type) for both, so SDL never adds a
# finger: no SDL_EVENT_FINGER_DOWN ever reaches the game, and the motions of a finger that is not
# down are dropped. Its loops also never set `found` when a finger id matched, so every finger
# would be lifted at every poll. The fix sends a finger that left as FINGER_UP, a new one as
# FINGER_DOWN and one that moved as a motion, matched by libnx's finger id.
#
# The file is changed only if it holds the expected text; anything else (another commit, the fix
# already there upstream) fails the configure step, loudly.

set(file "src/video/switch/SDL_switchtouch.c")
file(READ "${file}" text)
string(FIND "${text}" "MapleThief #239" done)
if(NOT done EQUAL -1)
    return() # already fixed (the patch step run again on the same source)
endif()

set(old_body [==[void SWITCH_PollTouch(Uint64 timestamp)
{
    const float rel_w = 1280.0f, rel_h = 720.0f;
    SDL_Window *window = SDL_GetKeyboardFocus();
    bool found;
    SDL_TouchID id = touch_id;
    s32 i, j;

    if (!window) {
        return;
    }

    SDL_memcpy(&touchStateOld, &touchState, sizeof(touchState));

    if (hidGetTouchScreenStates(&touchState, 1)) {
        /* Finger down */
        if (touchStateOld.count < touchState.count) {
            for (i = 0; i < touchState.count; i++) {
                found = false;

                for (j = 0; j < touchStateOld.count; j++) {
                    if (touchStateOld.touches[j].finger_id == touchState.touches[i].finger_id) {
                        found = false;
                        break;
                    }
                }

                if (!found) {
                    SDL_SendTouch(timestamp, id,
                                  (SDL_FingerID)touchState.touches[i].finger_id + 1,
                                  window, false,
                                  (float)touchState.touches[i].x / rel_w,
                                  (float)touchState.touches[i].y / rel_h, 1);
                }
            }
        }

        /* Scan for moves or up */
        for (i = 0; i < touchStateOld.count; i++) {
            found = false;

            for (j = 0; j < touchState.count; j++) {
                if (touchState.touches[j].finger_id == touchStateOld.touches[i].finger_id) {
                    found = false;
                    /* Finger moved */
                    if (touchState.touches[j].x != touchStateOld.touches[i].x || touchState.touches[j].y != touchStateOld.touches[i].y) {
                        SDL_SendTouchMotion(timestamp, id,
                                            (SDL_FingerID)touchState.touches[j].finger_id + 1,
                                            window,
                                            (float)touchState.touches[j].x / rel_w,
                                            (float)touchState.touches[j].y / rel_h, 1);
                    }
                    break;
                }
            }

            if (!found) {
                /* Finger Up */
                SDL_SendTouch(timestamp, id,
                              (SDL_FingerID)touchStateOld.touches[i].finger_id + 1,
                              window,
                              false,
                              (float)touchStateOld.touches[i].x / rel_w,
                              (float)touchStateOld.touches[i].y / rel_h, 1);
            }
        }
    }
}
]==])
set(new_body [==[void SWITCH_PollTouch(Uint64 timestamp)
{
    /* MapleThief #239: SDL 3's SDL_SendTouch takes the event's type where SDL 2 took a down
       flag, and `false` reached it, so no finger ever went down; and a finger still down was
       never matched by its id, so every one was lifted at every poll. Here a finger that left
       is lifted, a new one goes down and one that moved moves. */
    const float rel_w = 1280.0f, rel_h = 720.0f;
    SDL_Window *window = SDL_GetKeyboardFocus();
    SDL_TouchID id = touch_id;
    s32 i, j;

    if (!window) {
        return;
    }

    SDL_memcpy(&touchStateOld, &touchState, sizeof(touchState));

    if (hidGetTouchScreenStates(&touchState, 1)) {
        /* Lifted: down before, not now */
        for (i = 0; i < touchStateOld.count; i++) {
            bool found = false;
            for (j = 0; j < touchState.count; j++) {
                if (touchState.touches[j].finger_id == touchStateOld.touches[i].finger_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                SDL_SendTouch(timestamp, id,
                              (SDL_FingerID)touchStateOld.touches[i].finger_id + 1,
                              window, SDL_EVENT_FINGER_UP,
                              (float)touchStateOld.touches[i].x / rel_w,
                              (float)touchStateOld.touches[i].y / rel_h, 0.0f);
            }
        }

        /* Down now: new, or moved */
        for (i = 0; i < touchState.count; i++) {
            bool found = false;
            bool moved = false;
            for (j = 0; j < touchStateOld.count; j++) {
                if (touchStateOld.touches[j].finger_id == touchState.touches[i].finger_id) {
                    found = true;
                    moved = touchStateOld.touches[j].x != touchState.touches[i].x ||
                            touchStateOld.touches[j].y != touchState.touches[i].y;
                    break;
                }
            }
            if (!found) {
                SDL_SendTouch(timestamp, id,
                              (SDL_FingerID)touchState.touches[i].finger_id + 1,
                              window, SDL_EVENT_FINGER_DOWN,
                              (float)touchState.touches[i].x / rel_w,
                              (float)touchState.touches[i].y / rel_h, 1.0f);
            } else if (moved) {
                SDL_SendTouchMotion(timestamp, id,
                                    (SDL_FingerID)touchState.touches[i].finger_id + 1,
                                    window,
                                    (float)touchState.touches[i].x / rel_w,
                                    (float)touchState.touches[i].y / rel_h, 1.0f);
            }
        }
    }
}
]==])

string(FIND "${text}" "${old_body}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "sdl3-nx-touch.cmake: ${file} does not hold the expected "
                        "SWITCH_PollTouch; the SDL3 source is not commit 07ea26a3 (ADR 0138)")
endif()
string(REPLACE "${old_body}" "${new_body}" text "${text}")
file(WRITE "${file}" "${text}")
message(STATUS "sdl3-nx: SWITCH_PollTouch sends fingers down, moved and up (#239)")
