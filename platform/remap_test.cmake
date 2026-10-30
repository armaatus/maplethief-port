# CTests of the bindings (tests/CMakeLists.txt, #70, ADR 0054). Print "skipped:" (the tests'
# SKIP_REGULAR_EXPRESSION) when the program reports itself skipped (no generated code, slice or
# bundle).
#
#   cmake -DPROGRAM=<remap_real_test> -DOUT=<dir> -P remap_test.cmake         (input.remap_play)
#   cmake -DPROGRAM=<MapleThief> -DMODE=flags -DOUT=<dir> -P remap_test.cmake (input.bindings_flags)
#     MapleThief --bindings FILE --save-bindings: a file with a bad line is read (the line
#     reported by file and line, the rest applied, the warning for an action left without a
#     gamepad control), then written back whole; the game runs 1 s of virtual time.
#
# input.remap_play runs the real game twice on one data directory (remap_real_test.cpp):
#   first   a fresh install whose bindings.ini moves attack to V and the left stick's click: in
#           the first map the new controls attack and the old ones do not; then the remap
#           overlay binds quick slot 1 to Q and saves the file
#   again   a restart on the same directory: the saved game, the file's bindings still hold

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

if(MODE STREQUAL "flags")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{SDL_AUDIO_DRIVER} dummy)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data")
    file(WRITE "${OUT}/mine.ini" "# a player's file\n[field]\nattack = key V\njump = key Nonsense\n")
    execute_process(
        COMMAND "${PROGRAM}" --bindings "${OUT}/mine.ini" --save-bindings --fixed-step 1 --seconds
                1 --no-audio
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 4)
        message(STATUS "skipped: this build has no generated code")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "MapleThief --bindings --save-bindings: exit ${result}:\n${error}")
    endif()
    foreach(want "mine.ini:4: unknown key 'Nonsense': skipped"
                 "mine.ini: attack has no gamepad control in the field" "bindings: wrote ")
        string(FIND "${error}" "${want}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "the log lacks '${want}':\n${error}")
        endif()
    endforeach()
    file(READ "${OUT}/mine.ini" text)
    foreach(want "\nnintendo_buttons = position\n" "\n[field]\n" "\nattack = key V\n"
                 "\njump = button a, key X, key Space, key Left Alt, key Right Alt\n" "\n[menu]\n")
        string(FIND "${text}" "${want}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "the written file lacks '${want}':\n${text}")
        endif()
    endforeach()
    if(EXISTS "${OUT}/data/bindings.ini")
        message(FATAL_ERROR "--bindings FILE: the data directory's bindings.ini was written")
    endif()
    message(STATUS "--bindings read the file (a bad line reported, the rest applied), "
                   "--save-bindings wrote it back whole")
    return()
endif()

foreach(step first again)
    execute_process(
        COMMAND "${PROGRAM}" ${step} "${OUT}/data"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 77)
        message(STATUS "skipped: ${output}")
        return()
    endif()
    string(REGEX MATCHALL "(ok|FAIL)[^\n]*" lines "${output}")
    list(JOIN lines "\n" summary)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${step} exited with ${result}:\n${output}\n${error}")
    endif()
    message(STATUS "${step}:\n${summary}")
endforeach()
message(STATUS "remapped bindings work and persist across a restart")
