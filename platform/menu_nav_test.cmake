# CTests input.menu_* (tests/CMakeLists.txt, #69, ADR 0051): the real game driven by a virtual
# gamepad only, flow by flow (tests/cpp/menu_nav_real_test.cpp). Each FLOW runs one or more
# launches of the program in order on one fresh data directory, so that a launch sees the save
# files the one before it left (the character select needs a saved character):
#
#   main        main_menu                       (English)
#   terms       terms                           (MAPLETHIEF_LANGUAGE=ko)
#   characters  seed, create, select, delete    (slot 0 by the pad, then New, Start, Delete)
#   game        game                            (NPC chat, the popup menu, the shops)
#
# Fails at the first launch that fails. Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION)
# when the program reports itself skipped (no generated code, slice or bundle).
#
#   cmake -DPROGRAM=<menu_nav_real_test> -DFLOW=<flow> -DOUT=<dir> -P menu_nav_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/data")
set(language en)
if(FLOW STREQUAL "main")
    set(modes main_menu)
elseif(FLOW STREQUAL "terms")
    set(modes terms)
    set(language ko)
elseif(FLOW STREQUAL "characters")
    set(modes seed create select delete)
elseif(FLOW STREQUAL "game")
    set(modes game)
else()
    message(FATAL_ERROR "unknown FLOW '${FLOW}'")
endif()

set(ENV{MAPLETHIEF_LANGUAGE} "${language}")
set(ENV{SDL_VIDEO_DRIVER} "dummy")
set(ENV{SDL_AUDIO_DRIVER} "dummy")
foreach(mode IN LISTS modes)
    execute_process(
        COMMAND "${PROGRAM}" ${mode} "${OUT}/data" "${OUT}/${mode}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_FILE "${OUT}/${mode}.log")
    message(STATUS "--- ${mode}\n${output}")
    if(result EQUAL 77)
        message(STATUS "skipped: ${output}")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${mode} exited with ${result} (log: ${OUT}/${mode}.log)")
    endif()
endforeach()
