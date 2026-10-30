# CTests of MapleThief's input (#65, ADR 0047).
#
#   cmake -DPROGRAM=<MapleThief> -DMODE=usage -P input_test.cmake
#     (platform.input_usage, every build) --right-touch takes off or X,Y in the screen (whole
#     points, 0-479, 0-319); everything else is refused with exit status 2 (kExitUsage) and the
#     setting's message, before anything starts. --swipe takes T,X1,Y1,X2,Y2 (#75).
#     --safe-area takes T,L,B,R (#73, ADR 0055); with valid insets the gfx prototype's probe
#     pixels, read back through the presenter's placement inside them, still match.
#   cmake -DPROGRAM=<MapleThief> -DMODE=game -DOUT=<dir> -P input_test.cmake
#     (platform.mouse_clicks, real assets) The game on SDL's dummy drivers, --fixed-step 1 from
#     a fresh data directory, clicked through with --tap (left mouse clicks through the frontend's
#     Input, the path real clicks take) as tests/golden/device_screens.json's "fresh" run does:
#     the promo banner's close button, Play, the intro's five speech boxes, MacArthur's four OKs,
#     then the key pad's cash button. With MAPLETHIEF_TRACE=uikit the log must show
#     -[MainMenuView clickPlay:] and the key pad's -clickCashButton: and -releaseCashButton:.

if(MODE STREQUAL "usage")
    foreach(value 480,10 10,320 -1,5 1.5,2 abc 5 "1,2,3" on)
        execute_process(
            COMMAND "${PROGRAM}" --right-touch "${value}" --smoke-frames 1
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "right_touch: '")
            message(FATAL_ERROR "--right-touch ${value}: exit ${result}, expected 2 and the "
                                "setting's message:\n${error}")
        endif()
    endforeach()
    foreach(value off 430,270 "0, 319")
        execute_process(
            COMMAND "${PROGRAM}" --right-touch "${value}" --smoke-frames 1
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "--right-touch ${value}: exit ${result}, expected 0:\n${error}")
        endif()
    endforeach()
    # --swipe (#75) takes T,X1,Y1,X2,Y2 with T >= 0; anything else is refused before anything
    # starts.
    foreach(value "1,2,3" "1,2,3,4" "1,2,3,4,5,6" "-1,0,0,10,10" "a,0,0,10,10")
        execute_process(
            COMMAND "${PROGRAM}" --swipe "${value}" --smoke-frames 1
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "--swipe needs T,X1,Y1,X2,Y2")
            message(FATAL_ERROR "--swipe '${value}': exit ${result}, expected 2 and the usage:\n"
                                "${error}")
        endif()
    endforeach()
    # --safe-area (#73) takes T,L,B,R window points; anything else is refused before anything
    # starts. Valid insets move the prototype's image, and its probes still match (exit 0).
    foreach(value "0,0,0" "0,0,0,0,0" "-1,0,0,0" "a,0,0,0" "5000,0,0,0" "")
        execute_process(
            COMMAND "${PROGRAM}" --safe-area "${value}" --smoke-frames 1
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "--safe-area needs T,L,B,R")
            message(FATAL_ERROR "--safe-area '${value}': exit ${result}, expected 2 and the "
                                "usage:\n${error}")
        endif()
    endforeach()
    foreach(value "0,0,0,0" "0,59,21,59" "0,100,0,0" "40.5,0,0,100")
        execute_process(
            COMMAND "${PROGRAM}" --gfx-prototype --safe-area "${value}" --smoke-frames 3
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "--gfx-prototype --safe-area ${value}: exit ${result}, expected "
                                "0:\n${error}")
        endif()
    endforeach()
    message(STATUS "--right-touch: 8 invalid values refused with exit status 2, 3 valid ones run; "
                   "--swipe: 5 invalid values refused; --safe-area: 6 invalid values refused, "
                   "the prototype matches with 4 valid ones")
elseif(MODE STREQUAL "game")
    file(REMOVE_RECURSE "${OUT}")
    file(MAKE_DIRECTORY "${OUT}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{SDL_AUDIO_DRIVER} dummy)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data")
    set(ENV{MAPLETHIEF_TRACE} uikit)
    execute_process(
        COMMAND
            "${PROGRAM}" --fixed-step 1 --seconds 58 --tap 3,462,227 --tap 4.5,106,165 --tap
            30,240,290 --tap 34,240,290 --tap 38,240,290 --tap 42,240,290 --tap 46,240,290 --tap
            51,272,222 --tap 52,272,222 --tap 53,272,222 --tap 54,272,222 --tap 57,455,24
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/log.txt" "${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the run exited with ${result} (log: ${OUT}/log.txt)")
    endif()
    foreach(want "action -\\[MainMenuView clickPlay:\\]" "action -\\[TouchKeyPadView clickCashButton:\\]"
                 "action -\\[TouchKeyPadView releaseCashButton:\\]")
        if(NOT error MATCHES "${want}")
            message(FATAL_ERROR "the log has no '${want}' (log: ${OUT}/log.txt)")
        endif()
    endforeach()
    message(STATUS "clicks reached -[MainMenuView clickPlay:] and the key pad's cash button")
else()
    message(FATAL_ERROR "MODE must be usage or game")
endif()
