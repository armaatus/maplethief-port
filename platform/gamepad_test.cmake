# CTest input.gamepad_hold (tests/CMakeLists.txt, #67, ADR 0048): the real game twice, from a
# fresh data directory each time, with the same script up to the first map; then once the key
# pad's left, attack and right keys are held as touches (the #65 path: Input, UIKit touches,
# TouchKeyPadView) and once the same keys as a virtual Xbox 360 pad's d-pad, X and left stick
# (Gamepads, Actions, the runtime's action queue), on the same turns. Fails unless both runs
# pass their own checks (the hero walked and attacked) and write the same per-tick hero (time,
# x, y, state, flags, HP, MP), byte for byte. Prints "skipped:" (the test's
# SKIP_REGULAR_EXPRESSION) when the program reports itself skipped (no generated code, slice or
# bundle).
#
#   cmake -DPROGRAM=<gamepad_real_test> -DOUT=<dir> -P gamepad_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(files "")
foreach(variant touch pad)
    set(file "${OUT}/hero_${variant}.txt")
    execute_process(
        COMMAND "${PROGRAM}" hold ${variant} "${file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 77)
        message(STATUS "skipped: ${output}")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "hold ${variant} exited with ${result}:\n${output}\n${error}")
    endif()
    list(APPEND files "${file}")
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files ${files} RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "the held pad and the held key pad touches move the hero differently: "
                        "${files}")
endif()
file(STRINGS "${OUT}/hero_pad.txt" ticks)
list(LENGTH ticks count)
message(STATUS "held pad = held key pad touches: the same hero on all ${count} ticks")
