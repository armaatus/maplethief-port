# CTests input.menu_* (tests/CMakeLists.txt, #69, ADR 0051): the real game driven by a virtual
# gamepad only, flow by flow (tests/cpp/menu_nav_real_test.cpp). Each FLOW runs one or more
# launches of the program in order on one fresh data directory, so that a launch sees the save
# files the one before it left (the character select needs a saved character):
#
#   main        main_menu                       (English)
#   terms       terms                           (MAPLETHIEF_LANGUAGE=ko)
#   early       early                           (A at 0.05 s, before the engine's main menu:
#                                                dropped, #129; then Play)
#   characters  seed, create, select, delete    (slot 0 by the pad, then New, Start, Delete)
#   game        game                            (NPC chat, the popup menu, the shops)
#   shops_save  tutorial, hunt                  (a post-tutorial save in slot 0, #127)
#   shops       shops                           (on a copy of SEED: shops_save's data directory)
#   shop_scroll shop_scroll                     (the same: the shop's lists by the d-pad, ADR 0102)
#   popup_grids equip_save, popup_grids         (the same, with 30 equipment items written into
#                                                the copy's theif0 first: the popup menu's item
#                                                and equipment grids, ADR 0102, ADR 0114)
#   pointer     pointer                         (MAPLETHIEF_LANGUAGE=ko: the gamepad pointer
#                                                taps the terms, About, Help and swipes Help's
#                                                pages, #185, ADR 0107)
#   chords      chords_touch, chords_pad,       (fresh data each: the chords as two fingers, by the
#               chords_keys                      pad and by the keys; the per-tick hero files must
#                                                match, #139, ADR 0128)
#
# Fails at the first launch that fails. Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION)
# when the program reports itself skipped (no generated code, slice or bundle), or when SEED
# has no save (its fixture did not run or failed).
#
#   cmake -DPROGRAM=<menu_nav_real_test> -DFLOW=<flow> -DOUT=<dir> [-DSEED=<dir>]
#         -P menu_nav_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/data")
if(DEFINED SEED AND NOT SEED STREQUAL "")
    if(NOT EXISTS "${SEED}/saves/quest0")
        message(STATUS "skipped: no save in ${SEED} (its fixture input.menu_shops_save)")
        return()
    endif()
    file(COPY "${SEED}/" DESTINATION "${OUT}/data")
endif()
set(language en)
if(FLOW STREQUAL "main")
    set(modes main_menu)
elseif(FLOW STREQUAL "terms")
    set(modes terms)
    set(language ko)
elseif(FLOW STREQUAL "early")
    set(modes early)
elseif(FLOW STREQUAL "characters")
    set(modes seed create select delete)
elseif(FLOW STREQUAL "game")
    set(modes game)
elseif(FLOW STREQUAL "shops_save")
    set(modes tutorial hunt)
elseif(FLOW STREQUAL "shops")
    set(modes shops)
elseif(FLOW STREQUAL "shop_scroll")
    set(modes shop_scroll)
elseif(FLOW STREQUAL "popup_grids")
    set(modes equip_save popup_grids)
elseif(FLOW STREQUAL "pointer")
    set(modes pointer)
    set(language ko)
elseif(FLOW STREQUAL "chords")
    set(modes chords_touch chords_pad chords_keys)
    set(fresh_each TRUE)
elseif(FLOW MATCHES "^climb_|^chord_")
    set(modes ${FLOW}) # the rope (#138): by priority; chord_mash, chord_mixed (#139)
else()
    message(FATAL_ERROR "unknown FLOW '${FLOW}'")
endif()

set(ENV{MAPLETHIEF_LANGUAGE} "${language}")
set(ENV{SDL_VIDEO_DRIVER} "dummy")
set(ENV{SDL_AUDIO_DRIVER} "dummy")
foreach(mode IN LISTS modes)
    set(data "${OUT}/data")
    if(fresh_each)
        set(data "${OUT}/data-${mode}")
    endif()
    execute_process(
        COMMAND "${PROGRAM}" ${mode} "${data}" "${OUT}/${mode}"
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
if(FLOW STREQUAL "chords")
    foreach(mode chords_pad chords_keys)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/chords_touch/chords.txt"
                    "${OUT}/${mode}/chords.txt"
            RESULT_VARIABLE differ)
        if(NOT differ EQUAL 0)
            message(FATAL_ERROR "the chords by ${mode} differ from two fingers: "
                                "${OUT}/chords_touch/chords.txt ${OUT}/${mode}/chords.txt")
        endif()
    endforeach()
    file(STRINGS "${OUT}/chords_touch/chords.txt" ticks)
    list(LENGTH ticks count)
    message(STATUS "the chords: two fingers = pad = keys on all ${count} hero ticks")
endif()
