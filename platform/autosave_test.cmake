# CTest of autosave in the frontend (#150, autosave.h, docs/re/autosave.md): the turn hook runs
# the game's save pair before applicationWillResignActive: and applicationWillTerminate:, where
# the game's own Save would be allowed. play_real_test's autosave modes check the guards and the
# reload in the game; this checks that MapleThief's own lifecycle path (--minimise, --quit, as a
# minimise and Cmd-Q are) calls it, and that autosave = off never does.
#
#   cmake -DPROGRAM=<MapleThief> -DSAVE=<play.save_quit's data dir> -DOUT=<dir> -P autosave_test.cmake
#
# (platform.game_autosave, real assets, after play.save_quit) The game on SDL's dummy drivers,
# --fixed-step 16.667, each run from its own data directory:
#   - on: a copy of play.save_quit's data/saves/ (a hero in Henesys, 2.0); taps on Play, the first
#     slot and Start; --autosave on_quit --minimise 12,1 --quit 16. Slot 0 is saved before the
#     resign and again before the terminate, each through SaveQuestData's backup hook (#148:
#     backups/0/ gets a folder; the second save has the same bytes, so ADR 0078 adds none);
#     no temporary file is left.
#   - off: the same without --autosave (the default, off): no autosave line, no backups folder.
#   - scene: a fresh data directory, --autosave on_quit --minimise 1,1 (the main menu: not in the
#     game), a tap on Play (a new game: the intro's scene), --minimise 8,1 and --quit 12 (both in
#     the scene): three "not saved" lines naming the guard, and SaveQuestData never ran (no
#     backups folder).
# Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION) when play.save_quit left no save.

# skip_intros is pinned off (#174, ADR 0096): it runs the guest clock x8 through scenes, and this
# test times or counts the guest's ticks; a suite run with MAPLETHIEF_SETTINGS="skip_intros = on"
# must still pass. The flag comes after the environment's settings, so it wins.

if(NOT EXISTS "${SAVE}/data/saves/theif0")
    message(STATUS "skipped: no save in ${SAVE} (run play.save_quit first)")
    return()
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{MAPLETHIEF_LANGUAGE} en)

# Runs the game with ${ARGN} on the data directory <OUT>/data_<name>; fails unless it exits 0.
# The log goes to <OUT>/<name>.log and is returned in <name>_log.
function(run name)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${name}")
    execute_process(
        COMMAND "${PROGRAM}" --skip-intros off ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/${name}.log" "${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${result} (log: ${OUT}/${name}.log):\n${error}")
    endif()
    set(${name}_log "${error}" PARENT_SCOPE)
endfunction()

# Fails unless the lines matching `regexes` come in that order in `text`.
function(expect_order name text)
    set(rest "${text}")
    foreach(re IN LISTS ARGN)
        string(REGEX MATCH "${re}" hit "${rest}")
        if(hit STREQUAL "")
            message(FATAL_ERROR "${name}: '${re}' is missing or out of order (log: "
                                "${OUT}/${name}.log)")
        endif()
        string(FIND "${rest}" "${hit}" at)
        string(LENGTH "${hit}" n)
        math(EXPR at "${at} + ${n}")
        string(SUBSTRING "${rest}" ${at} -1 rest)
    endforeach()
endfunction()

set(load_taps --tap 3,106,165 --tap 5,99,232 --tap 6.5,420,79)

# on
file(COPY "${SAVE}/data/saves" DESTINATION "${OUT}/data_on")
run(on --fixed-step 16.667 --autosave on_quit ${load_taps} --minimise 12,1 --quit 16)
expect_order(
    on "${on_log}"
    "autosave: slot 0 saved at the background \\(SDL_EVENT_WINDOW_MINIMIZED, scripted\\) \\(12\\.000 s\\): map 2\\.0"
    "lifecycle: applicationWillResignActive: at 12\\.000 s"
    "lifecycle: applicationDidBecomeActive: at 13\\.000 s"
    "autosave: slot 0 saved at the quit \\(SDL_EVENT_QUIT, scripted\\) \\(16\\.000 s\\): map 2\\.0"
    "lifecycle: applicationWillTerminate: at 16\\.000 s"
    "stop reason: UIApplicationMain: the host ended the run loop")
file(GLOB backups "${OUT}/data_on/backups/0/*")
file(GLOB leftovers "${OUT}/data_on/saves/*.mt-tmp")
file(SIZE "${OUT}/data_on/saves/theif0" theif_size)
if(backups STREQUAL "" OR NOT leftovers STREQUAL "" OR NOT theif_size EQUAL 1124)
    message(FATAL_ERROR "on: backups '${backups}', leftovers '${leftovers}', theif0 "
                        "${theif_size} bytes: expected a backup of slot 0, no temporary file and "
                        "1124 bytes")
endif()

# off (the default)
file(COPY "${SAVE}/data/saves" DESTINATION "${OUT}/data_off")
run(off --fixed-step 16.667 ${load_taps} --minimise 12,1 --quit 16)
expect_order(off "${off_log}" "lifecycle: applicationWillResignActive: at 12\\.000 s"
             "lifecycle: applicationWillTerminate: at 16\\.000 s")
if(off_log MATCHES "autosave:" OR EXISTS "${OUT}/data_off/backups")
    message(FATAL_ERROR "off: autosave ran with autosave = off (log: ${OUT}/off.log)")
endif()

# scene
run(scene --fixed-step 16.667 --autosave on_quit --minimise 1,1 --tap 3,106,165 --minimise 8,1
    --quit 12)
expect_order(
    scene "${scene_log}"
    "autosave: not saved at the background \\(SDL_EVENT_WINDOW_MINIMIZED, scripted\\) \\(1\\.000 s\\): not in the game"
    "lifecycle: applicationWillResignActive: at 1\\.000 s"
    "autosave: not saved at the background \\(SDL_EVENT_WINDOW_MINIMIZED, scripted\\) \\(8\\.000 s\\): a scene is playing \\(game mode 9\\)"
    "lifecycle: applicationWillResignActive: at 8\\.000 s"
    "autosave: not saved at the quit \\(SDL_EVENT_QUIT, scripted\\) \\(12\\.000 s\\): a scene is playing \\(game mode 9\\)"
    "lifecycle: applicationWillTerminate: at 12\\.000 s")
if(scene_log MATCHES "autosave: slot" OR EXISTS "${OUT}/data_scene/backups")
    message(FATAL_ERROR "scene: something was saved (log: ${OUT}/scene.log)")
endif()

message(STATUS "autosave: saved before the resign and the quit in Henesys (backups/0 written), "
               "nothing with autosave = off, nothing at the main menu or in the intro's scene")
