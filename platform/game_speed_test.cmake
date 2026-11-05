# CTests of game_speed and fast_forward (#151, ADR 0087).
#
#   cmake -DPROGRAM=<MapleThief> -DMODE=usage -P game_speed_test.cmake
#     (platform.game_speed_usage, every build) Bad values of --game-speed, --fast-forward and
#     --hold-fast-forward are refused with exit status 2 (kExitUsage) and the flag's message.
#   cmake -DPROGRAM=<MapleThief> -DMODE=game -DOUT=<dir> -P game_speed_test.cmake
#     (platform.game_speed, real assets) The game in real time on SDL's dummy drivers (vsync
#     off: the dummy renderer's emulated vsync holds every present to 60 Hz) with
#     --fast-forward x2 and x3 held from 4 s: the clock switches to the fast wall plan at 4 s,
#     and the exit line's tick rate is within 3 % of what the fast-forward makes of the game's
#     50 ms (the exact numbers, and the 1 % of the acceptance, are play.game_speed's on the
#     virtual clock and game_speed_test.cpp's; a real-time run on a busy machine wanders more).
#     The same at x2 with pacing = wall (the launch clock's rate). Then --game-speed 25: the
#     table is patched and the clock line compares with 25 ms (5 %: the game's own gate polls
#     about every millisecond, a slack of 0.5-0.8 ms a tick on the wall clock, 2-3.3 % of 25 ms;
#     the run's exit line measured -3.18 % with the boot's loads in it). Each run's start-up log
#     names the main thread's priority (#206, ADR 0120), and every deviation is reported with
#     the machine's load average, so a failure on a busy machine reads as such.

# skip_intros is pinned off (#174, ADR 0096): it runs the guest clock x8 through scenes, and this
# test times or counts the guest's ticks; a suite run with MAPLETHIEF_SETTINGS="skip_intros = on"
# must still pass. The flag comes after the environment's settings, so it wins.

function(run_game name)
    set(dir "${OUT}/${name}")
    file(MAKE_DIRECTORY "${dir}")
    set(ENV{MAPLETHIEF_DATA_DIR} "${dir}")
    execute_process(
        COMMAND "${PROGRAM}" --preset original --no-audio --vsync off --skip-intros off ${ARGN}
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    file(WRITE "${dir}/log.txt" "${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exited with ${result}:\n${error}")
    endif()
    # The start-up log names the main thread's priority (#206, ADR 0120: raised, or refused).
    if(NOT error MATCHES "game: main thread: [^\n]*")
        message(FATAL_ERROR "${name}: no game: main thread: line:\n${error}")
    endif()
    message(STATUS "${name}: ${CMAKE_MATCH_0}")
    set(log "${error}" PARENT_SCOPE)
endfunction()

# The machine's load averages ("load average 3.43 3.83 5.82") for the messages, so a run that
# fails on a busy machine reads as such (#206); empty where the system does not say (Windows).
function(load_average out)
    set(text "")
    if(EXISTS "/proc/loadavg")
        file(READ "/proc/loadavg" raw)
        string(REGEX MATCH "^[0-9.]+ [0-9.]+ [0-9.]+" text "${raw}")
    elseif(CMAKE_HOST_APPLE)
        execute_process(
            COMMAND sysctl -n vm.loadavg
            OUTPUT_VARIABLE raw
            RESULT_VARIABLE result
            ERROR_QUIET)
        if(result EQUAL 0)
            string(REGEX MATCH "[0-9.]+ [0-9.]+ [0-9.]+" text "${raw}")
        endif()
    endif()
    if(text STREQUAL "")
        set(${out} "" PARENT_SCOPE)
    else()
        cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
        set(${out} ", load average ${text} on ${cores} cores" PARENT_SCOPE)
    endif()
endfunction()

# The deviation in the exit clock line ("(-0.60 %)") within `percent`.
function(check_deviation name percent)
    load_average(load)
    if(NOT log MATCHES "game: clock: [^\n]*\\(([-+][0-9]+\\.[0-9]+) %\\)")
        message(FATAL_ERROR "${name}: no game: clock: line with a deviation:\n${log}")
    endif()
    set(dev "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "^\\+" "" dev "${dev}")
    if(dev LESS -${percent} OR dev GREATER ${percent})
        message(FATAL_ERROR "${name}: the tick rate is ${dev} % from the option's (${percent} % "
                            "allowed${load}):\n${log}")
    endif()
    message(STATUS "${name}: the tick rate is ${dev} % from the option's${load}")
endfunction()

if(MODE STREQUAL "usage")
    foreach(args "--game-speed;20" "--game-speed;fast" "--fast-forward;x4" "--fast-forward;on")
        execute_process(
            COMMAND "${PROGRAM}" ${args}
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "is not")
            message(FATAL_ERROR "${args}: exit ${result}, expected 2 and the setting's "
                                "message:\n${error}")
        endif()
    endforeach()
    foreach(value "1" "a,2" "1,0" "-1,2")
        execute_process(
            COMMAND "${PROGRAM}" --hold-fast-forward ${value}
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "--hold-fast-forward needs T,S")
            message(FATAL_ERROR "--hold-fast-forward ${value}: exit ${result}, expected 2:\n${error}")
        endif()
    endforeach()
    message(STATUS "game speed: 8 invalid command lines refused with exit status 2")
elseif(MODE STREQUAL "game")
    file(REMOVE_RECURSE "${OUT}")
    file(MAKE_DIRECTORY "${OUT}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{SDL_AUDIO_DRIVER} dummy)
    foreach(rate 2 3)
        run_game(ff_x${rate} --seconds 24 --fast-forward x${rate} --hold-fast-forward 4,100)
        if(NOT log MATCHES "game: fast-forward x${rate} at 4\\.0[0-9]+ s"
           OR NOT log MATCHES "pacing: the wall clock x${rate} \\(fast-forward\\)"
           OR NOT log MATCHES "fast-forward [0-9.]+ s, [0-9.]+ s of game time gained")
            message(FATAL_ERROR "ff_x${rate}: the fast-forward did not start at 4 s:\n${log}")
        endif()
        check_deviation(ff_x${rate} 3)
    endforeach()
    # pacing = wall: the launch clock's rate (clock.h).
    run_game(ff_wall --seconds 24 --pacing wall --fast-forward x2 --hold-fast-forward 4,100)
    if(NOT log MATCHES "game: fast-forward x2 at 4\\.0[0-9]+ s"
       OR NOT log MATCHES "game: clock: pacing = wall[^\n]*fast-forward [0-9.]+ s")
        message(FATAL_ERROR "ff_wall: the fast-forward did not start at 4 s:\n${log}")
    endif()
    check_deviation(ff_wall 3)
    run_game(speed_25 --seconds 24 --game-speed 25)
    if(NOT log MATCHES "game: game_speed 25: every Speed option of the game ticks at it"
       OR NOT log MATCHES "against the game's 25 ms tick's 40\\.000")
        message(FATAL_ERROR "speed_25: the table was not patched to 25 ms:\n${log}")
    endif()
    check_deviation(speed_25 5)
else()
    message(FATAL_ERROR "MODE must be usage or game")
endif()
