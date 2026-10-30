# CTest input.keypad_layout (tests/CMakeLists.txt, #159, ADR 0086): the customisable key pad in
# the real game, from a fresh data directory each run.
#
#   - gamepad_real_test hold touch: the key pad's left, attack and right keys held as touches
#     with keypad_layout = original installed as the frontend installs it (its hooks off).
#   - gamepad_real_test hold touch-layout: the same with every group moved (two resized, two
#     faded), the touches where the keys are drawn now. The per-tick hero (time, x, y, state,
#     flags, HP, MP) must be the same byte for byte as hold touch's: the hit areas follow the
#     drawn keys. It also checks that the composited screen differs from the original layout's
#     only inside the groups' old and new rects.
#   - gamepad_real_test hold touch-old: the same layout, the touches where the keys were: no
#     key there now, the hero neither walks nor attacks (checked by the program).
#   - gamepad_real_test gldiff: the GL frame both touch runs show at 57.2 s differs only inside
#     the action and slot groups' old and new rects (the icons the game draws in GL, moved).
#
# Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION) when the program reports itself skipped
# (no generated code, slice or bundle).
#
#   cmake -DPROGRAM=<gamepad_real_test> -DOUT=<dir> -P keypad_layout_test.cmake

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

run_program(hold touch "${OUT}/hero_touch.txt")
if(skipped)
    return()
endif()
foreach(variant touch-layout touch-old)
    run_program(hold ${variant} "${OUT}/hero_${variant}.txt")
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/hero_touch.txt"
                        "${OUT}/hero_touch-layout.txt" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "the hero moves differently with the keys touched where the custom "
                        "layout draws them than with the original layout: "
                        "${OUT}/hero_touch.txt, ${OUT}/hero_touch-layout.txt")
endif()
run_program(gldiff "${OUT}/hero_touch.txt.gl57" "${OUT}/hero_touch-layout.txt.gl57")
file(STRINGS "${OUT}/hero_touch.txt" ticks)
list(LENGTH ticks count)
message(STATUS "the custom layout: the same hero on all ${count} ticks with the keys touched "
               "where they are drawn; nothing where they were")
