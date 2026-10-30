# CTest input.keypad_hide (tests/CMakeLists.txt, #71, ADR 0052): the key pad's hiding in the
# real game, from a fresh data directory each run.
#
#   - gamepad_real_test keypad: the three modes on one input stream (always hidden from the
#     first turn, never never hidden, auto hiding on the pad's presses and showing on touches
#     and an SDL mouse click, waiting for a held button), and the first map composited hidden
#     vs shown in the same turn: different only inside panel 0's rects.
#   - gamepad_real_test hold, four times: the key pad's left, attack and right keys held as
#     touches with the pad shown and with it hidden (hide_keypad = always: the touches land on
#     the hidden pad), and as a virtual Xbox 360 pad with the pad shown and hidden (auto, after
#     the pad's first A). The per-tick hero (time, x, y, state, flags, HP, MP) must be the same
#     byte for byte in all four, and every GL frame the game presents from the first map on the
#     same, by hash, hidden as shown.
#
# Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION) when the program reports itself skipped
# (no generated code, slice or bundle).
#
#   cmake -DPROGRAM=<gamepad_real_test> -DOUT=<dir> -P keypad_hide_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run_program)
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
    message(STATUS "${ARGN}:\n${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} exited with ${result}:\n${output}\n${error}")
    endif()
    set(skipped FALSE PARENT_SCOPE)
endfunction()

run_program(keypad "${OUT}/keypad")
if(skipped)
    return()
endif()

foreach(variant touch touch-always pad pad-auto)
    run_program(hold ${variant} "${OUT}/hero_${variant}.txt")
endforeach()

foreach(variant touch-always pad pad-auto)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/hero_touch.txt"
                            "${OUT}/hero_${variant}.txt" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
        message(FATAL_ERROR "the hero moves differently in hold ${variant} than in hold touch: "
                            "${OUT}/hero_touch.txt, ${OUT}/hero_${variant}.txt")
    endif()
endforeach()
foreach(pair "touch;touch-always" "pad;pad-auto")
    list(GET pair 0 shown)
    list(GET pair 1 hidden)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/hero_${shown}.txt.gl"
                            "${OUT}/hero_${hidden}.txt.gl" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
        message(FATAL_ERROR "the game's GL frames differ with the key pad hidden (${hidden}) "
                            "and shown (${shown}): ${OUT}/hero_${shown}.txt.gl")
    endif()
endforeach()
file(STRINGS "${OUT}/hero_touch.txt" ticks)
list(LENGTH ticks count)
file(STRINGS "${OUT}/hero_pad.txt.gl" frames)
list(LENGTH frames frame_count)
message(STATUS "the key pad hidden = shown: the same hero on all ${count} ticks in the four "
               "runs, the same ${frame_count} GL frames hidden as shown")
