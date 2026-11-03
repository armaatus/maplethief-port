# CTests of the desktop conveniences (#145, ADR 0077): fullscreen, vsync, pause_unfocused,
# language and --unfocus T,S.
#
#   cmake -DPROGRAM=<MapleThief> -DMODE=usage -P desktop_test.cmake
#     (platform.desktop_usage, every build) Every invalid value of --fullscreen (and
#     --fullscreen-mode), --vsync,
#     --pause-unfocused, --language and --unfocus is refused with exit status 2 (kExitUsage)
#     and the flag's message, before anything starts.
#   cmake -DPROGRAM=<MapleThief> -DMODE=game -DOUT=<dir> -P desktop_test.cmake
#     (platform.game_desktop, real assets) The game on SDL's dummy drivers, each run from a
#     fresh data directory:
#     - pause_unfocused on: --fixed-step 16.667 --pause-unfocused on --unfocus 6,4 --quit 12.
#       The focus lost at 6 s resigns the delegate (Clet_Pause: "phone suspend") and the guest
#       waits with nothing presented; the focus back at 10 s makes it active again ("phone
#       resume"); the quit at 12 s ends the run. One message each.
#     - pause_unfocused off (the default): the same --unfocus sends nothing but the quit's
#       applicationWillTerminate: and the game is never hidden.
#     - language: --language ko and MAPLETHIEF_LANGUAGE=ko write the same shot at 5 s, and en
#       (the default) another; the variable overrides the setting (--language ko with
#       MAPLETHIEF_LANGUAGE=en plays en).
#     - fullscreen and vsync: --fullscreen desktop --vsync off --frames 30 logs a fullscreen
#       window with vsync off; the default run logs neither.
#     - no scripted run reads or writes window.ini or writes a screenshot.

if(MODE STREQUAL "usage")
    set(cases
        "--fullscreen|maybe|fullscreen: 'maybe' is not on or off (desktop or borderless: on, in that mode)"
        "--fullscreen-mode|on|fullscreen_mode: 'on' is not desktop or borderless"
        "--vsync|sometimes|vsync: 'sometimes' is not on or off"
        "--pause-unfocused|2|pause_unfocused: '2' is not on or off"
        "--language|fr|language: 'fr' is not en or ko"
        "--language||language: '' is not en or ko"
        "--unfocus|6|--unfocus needs T,S"
        "--unfocus|6,0|--unfocus needs T,S"
        "--unfocus|-1,2|--unfocus needs T,S"
        "--unfocus|a,b|--unfocus needs T,S")
    set(n 0)
    foreach(c IN LISTS cases)
        string(REPLACE "|" ";" parts "${c}")
        list(GET parts 0 flag)
        list(GET parts 1 value)
        list(GET parts 2 message)
        execute_process(
            COMMAND "${PROGRAM}" ${flag} "${value}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        string(FIND "${error}" "${message}" at)
        if(NOT result EQUAL 2 OR at EQUAL -1)
            message(FATAL_ERROR "${flag} '${value}': exit ${result}, expected 2 and "
                                "'${message}':\n${error}")
        endif()
        math(EXPR n "${n} + 1")
    endforeach()
    message(STATUS "--fullscreen, --vsync, --pause-unfocused, --language, --unfocus: ${n} "
                   "invalid command lines refused with exit status 2")
    return()
endif()

if(NOT MODE STREQUAL "game")
    message(FATAL_ERROR "MODE must be usage or game")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
unset(ENV{MAPLETHIEF_LANGUAGE})

# Runs the game with ${ARGN} from the fresh data directory <OUT>/data_<name>; fails unless it
# exits 0. The log goes to <OUT>/<name>.log, the guest's stdout to <OUT>/<name>.out.
function(run name)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${name}")
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/${name}.log" "${error}")
    file(WRITE "${OUT}/${name}.out" "${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${result} (log: ${OUT}/${name}.log):\n${error}")
    endif()
    if(EXISTS "${OUT}/data_${name}/window.ini" OR EXISTS "${OUT}/data_${name}/screenshots")
        message(FATAL_ERROR "${name}: a scripted run wrote window.ini or a screenshot")
    endif()
    set(${name}_log "${error}" PARENT_SCOPE)
    set(${name}_out "${output}" PARENT_SCOPE)
endfunction()

function(expect name text regex what)
    if(NOT text MATCHES "${regex}")
        message(FATAL_ERROR "${name}: ${what} (log: ${OUT}/${name}.log)")
    endif()
endfunction()

function(expect_not name text regex what)
    if(text MATCHES "${regex}")
        message(FATAL_ERROR "${name}: ${what} (log: ${OUT}/${name}.log)")
    endif()
endfunction()

# 1. pause_unfocused on: the focus lost pauses as a minimise does.
run(pause_on --fixed-step 16.667 --pause-unfocused on --unfocus 6,4 --quit 12)
set(order
    "lifecycle: pause_unfocused on"
    "scripted SDL_EVENT_WINDOW_FOCUS_LOST at 6\\.000 s"
    "lifecycle: applicationWillResignActive: at 6\\.000 s \\(SDL_EVENT_WINDOW_FOCUS_LOST, scripted\\)"
    "lifecycle: hidden at 6\\.000 s: nothing is presented, the guest waits \\(window frame ([0-9]+), GL frame ([0-9]+)\\)"
    "scripted SDL_EVENT_WINDOW_FOCUS_GAINED at 10\\.000 s"
    "lifecycle: shown again at 10\\.000 s, 4\\.000 s after it was hidden \\(window frame ([0-9]+), GL frame ([0-9]+)\\)"
    "lifecycle: applicationDidBecomeActive: at 10\\.000 s \\(SDL_EVENT_WINDOW_FOCUS_GAINED, scripted\\)"
    "lifecycle: applicationWillTerminate: at 12\\.000 s"
    "game: lifecycle: 3 messages sent")
string(JOIN ".*" regex ${order})
if(NOT pause_on_log MATCHES "${regex}")
    message(FATAL_ERROR "pause_on: the pause's steps are not all there, in order (log: "
                        "${OUT}/pause_on.log)")
endif()
if(NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_3 OR NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_4)
    message(FATAL_ERROR "pause_on: presented while paused: window frame ${CMAKE_MATCH_1} -> "
                        "${CMAKE_MATCH_3}, GL frame ${CMAKE_MATCH_2} -> ${CMAKE_MATCH_4}")
endif()
expect(pause_on "${pause_on_out}" "phone suspend.*phone resume.*The Game is Terminated!"
       "the guest's own pause and resume are not there")
message(STATUS "pause_unfocused on: resigned at 6 s on the focus lost, nothing presented "
               "(window frame ${CMAKE_MATCH_1}), active again at 10 s")

# 2. pause_unfocused off (the default): the focus is not the lifecycle's.
run(pause_off --fixed-step 16.667 --unfocus 6,4 --quit 12)
expect(pause_off "${pause_off_log}" "scripted SDL_EVENT_WINDOW_FOCUS_LOST at 6\\.000 s"
       "the focus lost was not scripted")
expect_not(pause_off "${pause_off_log}" "applicationWillResignActive|lifecycle: hidden at"
           "the focus lost paused the game with pause_unfocused off")
expect(pause_off "${pause_off_log}" "game: lifecycle: 1 messages sent"
       "not just the quit's message")
message(STATUS "pause_unfocused off: the focus lost changes nothing")

# 3. language: the setting, and the variable over it.
run(lang_setting --fixed-step 16.667 --language ko --capture "${OUT}/lang_setting" --shot 5,menu)
expect(lang_setting "${lang_setting_log}" "game: language ko \\(the language setting\\)"
       "the language setting was not used")
set(ENV{MAPLETHIEF_LANGUAGE} ko)
run(lang_env --fixed-step 16.667 --capture "${OUT}/lang_env" --shot 5,menu)
expect(lang_env "${lang_env_log}" "game: language ko \\(MAPLETHIEF_LANGUAGE\\)"
       "MAPLETHIEF_LANGUAGE was not used")
set(ENV{MAPLETHIEF_LANGUAGE} en)
run(lang_override --fixed-step 16.667 --language ko --frames 3)
expect(lang_override "${lang_override_log}" "game: language en \\(MAPLETHIEF_LANGUAGE\\)"
       "MAPLETHIEF_LANGUAGE did not override the setting")
unset(ENV{MAPLETHIEF_LANGUAGE})
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/lang_setting/menu.png"
                        "${OUT}/lang_env/menu.png" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "language = ko and MAPLETHIEF_LANGUAGE=ko wrote different shots")
endif()
# And the setting does something: the default (en) shows another screen.
run(lang_en --fixed-step 16.667 --capture "${OUT}/lang_en" --shot 5,menu)
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/lang_en/menu.png"
                        "${OUT}/lang_setting/menu.png" RESULT_VARIABLE differ)
if(differ EQUAL 0)
    message(FATAL_ERROR "language = ko wrote the same shot as en")
endif()
message(STATUS "language: --language ko and MAPLETHIEF_LANGUAGE=ko give the same screen; the "
               "variable overrides the setting")

# 4. fullscreen and vsync at start; the default has neither.
run(window_on --fullscreen desktop --vsync off --frames 30)
expect(window_on "${window_on_log}" "fullscreen, vsync off, scale mode"
       "not a fullscreen window with vsync off")
expect(window_on "${window_on_log}" "perf: display [0-9.]+ Hz, vsync off"
       "the run's telemetry does not have vsync off")
run(window_default --frames 30)
expect_not(window_default "${window_default_log}" "fullscreen, vsync"
           "the default window is fullscreen")
expect_not(window_default "${window_default_log}" "desktop: "
           "the default run logged a desktop change")
message(STATUS "fullscreen desktop and vsync off at start; the default run is windowed with "
               "nothing new in its log")
