# CTests of the app lifecycle (#128, ADR 0063): --minimise T,S, --background T,S and --quit T.
#
#   cmake -DPROGRAM=<MapleThief> -DMODE=usage -P lifecycle_test.cmake
#     (platform.lifecycle_usage, every build) Every invalid value is refused with exit status 2
#     (kExitUsage) and the flag's message, before anything starts.
#   cmake -DPROGRAM=<MapleThief> -DMODE=game -DOUT=<dir> -P lifecycle_test.cmake
#     (platform.game_lifecycle, real assets) The game on SDL's dummy drivers, each run from a
#     fresh data directory, with the audio trace:
#     - minimise, twice: --fixed-step 16.667 --minimise 6,4 --tap 11,106,165 --quit 16
#       --audio-capture. The window is minimised at 6 s: Clet_Pause ("phone suspend", the
#       music's AVAudioPlayer stops, applicationWillResignActive: once), nothing is presented and
#       the capture is silent until the restore at 10 s; then Clet_Resume ("phone resume", the
#       music again, applicationDidBecomeActive: once), the tap on Play at 11 s starts a new
#       game and the GL frames go on; the quit at 16 s sends applicationWillTerminate: ("The
#       Game is Terminated!") and the run ends cleanly. Both runs write the same WAV.
#     - background: --fixed-step 16.667 --background 6,4 --quit 12, SDL's iOS sequence for the
#       home gesture: one message each, in order.
#     - real time: --minimise 3,2 --quit 7 with the device stream: the same messages in order.

if(MODE STREQUAL "usage")
    foreach(flag --minimise --background)
        foreach(value "" 6 "6,0" "6,-1" "-1,2" "a,b" "6,4,1" "nan,1" "6,inf" "604801,1")
            execute_process(
                COMMAND "${PROGRAM}" ${flag} "${value}"
                RESULT_VARIABLE result
                ERROR_VARIABLE error)
            if(NOT result EQUAL 2 OR NOT error MATCHES "${flag} needs T,S")
                message(FATAL_ERROR "${flag} '${value}': exit ${result}, expected 2 and the "
                                    "flag's message:\n${error}")
            endif()
        endforeach()
    endforeach()
    foreach(value -1 x inf nan 604801 "")
        execute_process(
            COMMAND "${PROGRAM}" --quit "${value}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2 OR NOT error MATCHES "--quit needs seconds")
            message(FATAL_ERROR "--quit '${value}': exit ${result}, expected 2 and the flag's "
                                "message:\n${error}")
        endif()
    endforeach()
    foreach(flag --minimise --background --quit)
        execute_process(
            COMMAND "${PROGRAM}" ${flag}
            RESULT_VARIABLE result
            ERROR_VARIABLE error)
        if(NOT result EQUAL 2)
            message(FATAL_ERROR "${flag} without a value: exit ${result}, expected 2:\n${error}")
        endif()
    endforeach()
    message(STATUS "--minimise, --background, --quit: 29 invalid command lines refused with "
                   "exit status 2")
    return()
endif()

if(NOT MODE STREQUAL "game")
    message(FATAL_ERROR "MODE must be usage or game")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{MAPLETHIEF_TRACE} audio)

# Runs the game with ${ARGN} from the fresh data directory <OUT>/data_<name>; fails unless it
# exits 0. The log goes to <OUT>/<name>.log, the guest's stdout to <OUT>/<name>.out; both are
# returned in <name>_log and <name>_out.
function(run name)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${name}")
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/${name}.log" "${error}")
    file(WRITE "${OUT}/${name}.out" "${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${result} (log: ${OUT}/${name}.log):\n${error}")
    endif()
    set(${name}_log "${error}" PARENT_SCOPE)
    set(${name}_out "${output}" PARENT_SCOPE)
endfunction()

# Fails unless `text` matches `regex`.
function(expect name text regex what)
    if(NOT text MATCHES "${regex}")
        message(FATAL_ERROR "${name}: ${what} (log: ${OUT}/${name}.log)")
    endif()
endfunction()

# Fails unless each of the delegate's three messages is in the log exactly once.
function(expect_once name log)
    foreach(sel applicationWillResignActive applicationDidBecomeActive applicationWillTerminate)
        string(REGEX MATCHALL "lifecycle: ${sel}:" hits "${log}")
        list(LENGTH hits n)
        if(NOT n EQUAL 1)
            message(FATAL_ERROR "${name}: ${sel}: sent ${n} times, expected once (log: "
                                "${OUT}/${name}.log)")
        endif()
    endforeach()
    expect(${name} "${log}" "game: lifecycle: 3 messages sent" "not 3 messages in the summary")
    expect(${name} "${log}" "stop reason: UIApplicationMain: the host ended the run loop"
           "the run did not end at a turn hook")
endfunction()

# The bytes of a 44.1 kHz 16-bit stereo WAV from second `from` to second `to` (hex).
function(wav_hex file from to out)
    math(EXPR offset "44 + 4 * 441 * ${from}")
    math(EXPR length "4 * 441 * (${to} - ${from})")
    file(READ "${file}" hex OFFSET ${offset} LIMIT ${length} HEX)
    set(${out} "${hex}" PARENT_SCOPE)
endfunction()

# 1. Minimise and restore, twice.
foreach(r a b)
    run(minimise_${r} --fixed-step 16.667 --minimise 6,4 --tap 11,106,165 --quit 16
        --audio-capture "${OUT}/minimise_${r}.wav")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/minimise_a.wav"
                        "${OUT}/minimise_b.wav" RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "the two minimise runs wrote different captures")
endif()
set(log "${minimise_a_log}")
expect_once(minimise_a "${log}")
# In order: the music plays; the minimise; Clet_Pause stops it; the resign; hidden with no
# present until the restore (the same window frame); Clet_Resume plays it again; active; the
# tap; the quit's terminate.
set(order
    "AVAudioPlayer play. 0x"
    "scripted SDL_EVENT_WINDOW_MINIMIZED at 6\\.000 s"
    "AVAudioPlayer stop. 0x[0-9A-F]+ at"
    "lifecycle: applicationWillResignActive: at 6\\.000 s \\(SDL_EVENT_WINDOW_MINIMIZED, scripted\\)"
    "lifecycle: hidden at 6\\.000 s: nothing is presented, the guest waits \\(window frame ([0-9]+), GL frame ([0-9]+)\\)"
    "scripted SDL_EVENT_WINDOW_RESTORED at 10\\.000 s"
    "lifecycle: shown again at 10\\.000 s, 4\\.000 s after it was hidden \\(window frame ([0-9]+), GL frame ([0-9]+)\\)"
    "AVAudioPlayer play. 0x"
    "lifecycle: applicationDidBecomeActive: at 10\\.000 s \\(SDL_EVENT_WINDOW_RESTORED, scripted\\)"
    "game: touch down at \\(106, 165\\)"
    "scripted SDL_EVENT_QUIT at 16\\.000 s"
    "lifecycle: applicationWillTerminate: at 16\\.000 s \\(SDL_EVENT_QUIT, scripted\\)"
    "game: stopped after [0-9.]+ s: [0-9]+ turns, [0-9]+ window frames [^\n]*, ([0-9]+) GL frames")
string(JOIN ".*" regex ${order})
if(NOT log MATCHES "${regex}")
    message(FATAL_ERROR "minimise_a: the lifecycle's steps are not all there, in order (log: "
                        "${OUT}/minimise_a.log)")
endif()
set(hidden_frame ${CMAKE_MATCH_1})
set(hidden_gl ${CMAKE_MATCH_2})
set(shown_frame ${CMAKE_MATCH_3})
set(shown_gl ${CMAKE_MATCH_4})
set(end_gl ${CMAKE_MATCH_5})
if(NOT hidden_frame EQUAL shown_frame OR NOT hidden_gl EQUAL shown_gl)
    message(FATAL_ERROR "minimise_a: presented while minimised: window frame ${hidden_frame} -> "
                        "${shown_frame}, GL frame ${hidden_gl} -> ${shown_gl}")
endif()
math(EXPR after "${end_gl} - ${shown_gl}")
if(after LESS 60)
    message(FATAL_ERROR "minimise_a: only ${after} GL frames in the 6 s after the restore")
endif()
expect(minimise_a "${minimise_a_out}"
       "phone resume.*phone suspend.*OpenAL sound free.*phone resume.*The Game is Terminated!"
       "the guest's own lines (phone suspend, phone resume, The Game is Terminated!) not in order")
# The music before, silence while minimised (from 6.1 s: the stop's own mix buffer), the music
# again after the restore.
wav_hex("${OUT}/minimise_a.wav" 300 590 before)
wav_hex("${OUT}/minimise_a.wav" 610 1000 minimised)
wav_hex("${OUT}/minimise_a.wav" 1050 1090 restored)
if(NOT before MATCHES "[1-9a-f]")
    message(FATAL_ERROR "minimise_a.wav: no music from 3 s to 5.9 s")
endif()
if(minimised MATCHES "[1-9a-f]")
    message(FATAL_ERROR "minimise_a.wav: not silent from 6.1 s to 10 s (minimised)")
endif()
if(NOT restored MATCHES "[1-9a-f]")
    message(FATAL_ERROR "minimise_a.wav: no music from 10.5 s to 10.9 s (restored)")
endif()
message(STATUS "minimise: Clet_Pause at 6 s (the music stops, silence while minimised, no "
               "present: window frame ${hidden_frame}), Clet_Resume at 10 s, ${after} GL frames "
               "after it, applicationWillTerminate: at 16 s; both runs wrote the same capture")

# 2. SDL's iOS sequence for the home gesture.
run(background --fixed-step 16.667 --background 6,4 --quit 12)
expect_once(background "${background_log}")
set(order
    "scripted SDL_EVENT_WINDOW_MINIMIZED at 6\\.000 s"
    "scripted SDL_EVENT_WILL_ENTER_BACKGROUND at 6\\.000 s"
    "scripted SDL_EVENT_DID_ENTER_BACKGROUND at 6\\.000 s"
    "lifecycle: applicationWillResignActive: at 6\\.000 s"
    "lifecycle: hidden at 6\\.000 s"
    "scripted SDL_EVENT_WILL_ENTER_FOREGROUND at 10\\.000 s"
    "scripted SDL_EVENT_DID_ENTER_FOREGROUND at 10\\.000 s"
    "scripted SDL_EVENT_WINDOW_RESTORED at 10\\.000 s"
    "lifecycle: shown again at 10\\.000 s"
    "lifecycle: applicationDidBecomeActive: at 10\\.000 s"
    "lifecycle: applicationWillTerminate: at 12\\.000 s")
string(JOIN ".*" regex ${order})
expect(background "${background_log}" "${regex}" "the iOS sequence's steps are not all there")
message(STATUS "background: SDL's iOS home-gesture sequence, one message each, in order")

# 3. Real time, with the device stream.
run(realtime --minimise 3,2 --quit 7)
expect_once(realtime "${realtime_log}")
# (The times are the first turns at or after 3, 5 and 7 s: a Debug build's turns on the menu
# take a few hundred ms.)
set(order "scripted SDL_EVENT_WINDOW_MINIMIZED at [3-9]\\.[0-9]+ s"
          "lifecycle: applicationWillResignActive: at [3-9]\\.[0-9]+ s"
          "lifecycle: hidden at [^\n]*window frame ([0-9]+), GL frame ([0-9]+)"
          "scripted SDL_EVENT_WINDOW_RESTORED at"
          "lifecycle: shown again at [^\n]*window frame ([0-9]+), GL frame ([0-9]+)"
          "lifecycle: applicationDidBecomeActive: at"
          "lifecycle: applicationWillTerminate: at")
string(JOIN ".*" regex ${order})
if(NOT realtime_log MATCHES "${regex}")
    message(FATAL_ERROR "realtime: the steps are not all there, in order (log: "
                        "${OUT}/realtime.log)")
endif()
if(NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_3 OR NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_4)
    message(FATAL_ERROR "realtime: presented while minimised: window frame ${CMAKE_MATCH_1} -> "
                        "${CMAKE_MATCH_3}, GL frame ${CMAKE_MATCH_2} -> ${CMAKE_MATCH_4}")
endif()
message(STATUS "real time: minimised at 3 s, restored at 5 s, quit at 7 s, in order; nothing "
               "presented while minimised")
