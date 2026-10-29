# CTest platform.audio_capture (tests/CMakeLists.txt, #61, ADR 0045): runs the game twice with
# the same scripted, fixed-step command line and an --audio-capture each, from a fresh data
# directory each time, and fails unless both runs exit 0 and write the same WAV, byte for byte,
# with the 16-bit stereo 44.1 kHz header and the frames of the virtual time covered.
#
#   cmake -DPROGRAM=<MapleThief> -DOUT=<dir> -P audio_capture_test.cmake
#
# The run: --fixed-step 16.667 --frames 120, a tap on Play at 3 s (as platform.game_boot).

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(captures "")
foreach(run a b)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${run}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(wav "${OUT}/capture_${run}.wav")
    execute_process(
        COMMAND "${PROGRAM}" --fixed-step 16.667 --frames 120 --tap 3,106,165 --audio-capture
                "${wav}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "run ${run} exited with ${result}:\n${error}")
    endif()
    if(NOT error MATCHES "audio: capture [^\n]*: ([0-9]+) frames")
        message(FATAL_ERROR "run ${run}: no capture line in the log:\n${error}")
    endif()
    set(frames_${run} "${CMAKE_MATCH_1}")
    list(APPEND captures "${wav}")
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files ${captures} RESULT_VARIABLE differ)
if(NOT differ EQUAL 0)
    message(FATAL_ERROR "the two captures differ: ${captures}")
endif()
file(SIZE "${OUT}/capture_a.wav" size)
math(EXPR expected "44 + 4 * ${frames_a}")
if(frames_a EQUAL 0 OR NOT size EQUAL expected)
    message(FATAL_ERROR "capture_a.wav: ${size} bytes for ${frames_a} frames (expected ${expected})")
endif()
file(READ "${OUT}/capture_a.wav" header LIMIT 44 HEX)
# RIFF....WAVEfmt , 16, PCM, 2 channels, 44100 Hz, 176400 B/s, align 4, 16 bits, data
if(NOT header MATCHES "^52494646........57415645666d7420100000000100020044ac000010b102000400100064617461")
    message(FATAL_ERROR "capture_a.wav: not a 16-bit stereo 44.1 kHz WAV header: ${header}")
endif()
message(STATUS "two runs, the same capture: ${frames_a} frames (${size} bytes)")
