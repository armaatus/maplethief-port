# CTests of MapleThief --seconds S (#124: the time limit of the audio soak).
#
#   cmake -DPROGRAM=<MapleThief> -DMODE=usage -P seconds_test.cmake
#     (platform.seconds_usage, every build) Every invalid value is refused with exit status 2
#     (kExitUsage) and the flag's message, before anything starts.
#   cmake -DPROGRAM=<MapleThief> -DMODE=game -DOUT=<dir> -P seconds_test.cmake
#     (platform.game_seconds, real assets) The game on SDL's dummy drivers ends by itself: once
#     with --fixed-step 16.667 --seconds 2 at 2 s of virtual time (no device in that mode),
#     once in real time with --seconds 3 and the device stream (dummy driver), whose exit log
#     has the audio output's numbers and how long the stream ran.

if(MODE STREQUAL "usage")
    foreach(value 0 -1 abc 1x nan inf 604801)
        execute_process(
            COMMAND "${PROGRAM}" --seconds ${value}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "--seconds needs seconds above 0")
            message(FATAL_ERROR "--seconds ${value}: exit ${result}, expected 2 and the flag's "
                                "message:\n${error}")
        endif()
    endforeach()
    execute_process(
        COMMAND "${PROGRAM}" --seconds
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    if(NOT result EQUAL 2)
        message(FATAL_ERROR "--seconds without a value: exit ${result}, expected 2:\n${error}")
    endif()
    message(STATUS "--seconds: 8 invalid command lines refused with exit status 2")
elseif(MODE STREQUAL "game")
    file(REMOVE_RECURSE "${OUT}")
    file(MAKE_DIRECTORY "${OUT}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{SDL_AUDIO_DRIVER} dummy)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_fixed")
    execute_process(
        COMMAND "${PROGRAM}" --fixed-step 16.667 --seconds 2
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--fixed-step 16.667 --seconds 2 exited with ${result}:\n${error}")
    endif()
    if(NOT error MATCHES "--seconds 2: the run ends 2\\.0[0-9]+ s after the first run-loop turn"
       OR NOT error MATCHES "virtual time at the end: 2\\.0[0-9]+ s"
       OR NOT error MATCHES "stop reason: [^\n]*the host ended the run")
        message(FATAL_ERROR "--fixed-step 16.667 --seconds 2 did not end at 2 s:\n${error}")
    endif()
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_real")
    execute_process(
        COMMAND "${PROGRAM}" --seconds 3
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "--seconds 3 exited with ${result}:\n${error}")
    endif()
    if(NOT error MATCHES "--seconds 3: the run ends 3\\.[0-9]+ s after the first run-loop turn"
       OR NOT error MATCHES "game: audio: [0-9]+ frames rendered in [0-9]+ calls \\(longest [0-9]+ us\\), [0-9]+ late"
       OR NOT error MATCHES "game: audio: the device stream ran [0-9.]+ s and pulled [0-9.]+ s of audio")
        message(FATAL_ERROR "--seconds 3 did not end by itself with the audio numbers:\n${error}")
    endif()
    message(STATUS "--seconds: the game ended by itself at 2 s of virtual time and after 3 s "
                   "of real time, with the device stream's numbers")
else()
    message(FATAL_ERROR "MODE must be usage or game")
endif()
