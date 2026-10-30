# CTests input.keyboard_hold and input.keyboard_menus (tests/CMakeLists.txt, #68, ADR 0048's
# addendum). Prints "skipped:" (the tests' SKIP_REGULAR_EXPRESSION) when the program reports
# itself skipped (no generated code, slice or bundle).
#
#   WHAT=hold   the real game twice, from a fresh data directory each time, with the same keys up
#               to the first map; then once the key pad's left, attack and right keys are held as
#               touches (the #65 path: Input, UIKit touches, TouchKeyPadView) and once as the
#               Left, Z and Right keys with their repeats (Keyboard, Actions, the runtime's action
#               queue), on the same turns. Fails unless both runs pass their own checks (the hero
#               walked and attacked) and write the same per-tick hero (time, x, y, state, flags,
#               HP, MP), byte for byte.
#   WHAT=menus  the menu steps fresh, new, start and delete in turn on one data directory: a
#               keyboard-only run through the main menu and the character screens (each step
#               checks itself; keyboard_real_test.cpp). Then the step early on a data directory
#               of its own: Enter at 0.05 s, before the engine's main menu, must not end the game
#               (#129), and Enter at 3.5 s is Play.
#
#   cmake -DPROGRAM=<keyboard_real_test> -DWHAT=hold|menus -DOUT=<dir> -P keyboard_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run_step)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 77)
        message(STATUS "skipped: ${output}")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCHALL "(ok|FAIL|     )[^\n]*" lines "${output}")
    list(JOIN lines "\n" summary)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} exited with ${result}:\n${output}\n${error}")
    endif()
    message(STATUS "${ARGN}:\n${summary}")
    set(skipped FALSE PARENT_SCOPE)
endfunction()

if(WHAT STREQUAL "hold")
    set(files "")
    foreach(variant touch keys)
        set(file "${OUT}/hero_${variant}.txt")
        run_step(hold ${variant} "${file}")
        if(skipped)
            return()
        endif()
        list(APPEND files "${file}")
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files ${files} RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
        message(FATAL_ERROR "the held keys and the held key pad touches move the hero "
                            "differently: ${files}")
    endif()
    file(STRINGS "${OUT}/hero_keys.txt" ticks)
    list(LENGTH ticks count)
    message(STATUS "held keys = held key pad touches: the same hero on all ${count} ticks")
elseif(WHAT STREQUAL "menus")
    foreach(step fresh new start delete)
        run_step(menus ${step} "${OUT}/data")
        if(skipped)
            return()
        endif()
    endforeach()
    run_step(menus early "${OUT}/data-early")
    message(STATUS "the main menu and the character screens by keys: every step passed")
else()
    message(FATAL_ERROR "WHAT must be hold or menus, not '${WHAT}'")
endif()
