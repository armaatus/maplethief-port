# CTest platform.audio_capture (tests/CMakeLists.txt, #61, #62, ADR 0045): runs the game twice
# with the same scripted, fixed-step command line and an --audio-capture each, from a fresh
# data directory each time, and fails unless both runs exit 0 and write the same WAV, byte for
# byte, with the 16-bit stereo 44.1 kHz header and the frames of the virtual time covered, and
# unless the tap's effect sounds: the run plays source 44 (effect 43, CStateMainMenu::
# OnKeyPressed's select sound, docs/re/audio.md) and the capture is not silence (#62 decodes
# the WAVs). Then mono (#195): a third run with --audio-mono on writes as many frames, its left
# and right samples equal in every frame, while the stereo capture has frames where they differ
# (the joint-stereo music): checked by PYTHON, which reads only the WAVs.
#
#   cmake -DPROGRAM=<MapleThief> -DOUT=<dir> -DPYTHON=<python3> -P audio_capture_test.cmake
#
# The run: --fixed-step 16.667 --frames 180 (about 4.7 s of virtual time with the boot), a tap
# on Play at 3 s (as platform.game_boot); effect 43 (0.89 s) sounds from about 3.16 s.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(captures "")
foreach(run a b mono)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${run}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{MAPLETHIEF_TRACE} audio)
    set(wav "${OUT}/capture_${run}.wav")
    # audio_mono pinned off but in the mono run: a suite run with MAPLETHIEF_SETTINGS="audio_mono
    # = on" must still pass, and the flag wins over it.
    set(mono --audio-mono off)
    if(run STREQUAL "mono")
        set(mono --audio-mono on)
    endif()
    execute_process(
        COMMAND "${PROGRAM}" --fixed-step 16.667 --frames 180 --tap 3,106,165 ${mono}
                --audio-capture "${wav}"
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
    if(NOT error MATCHES "audio: alSourcePlay\\(44\\)")
        message(FATAL_ERROR "run ${run}: the tap played no effect 43 (alSourcePlay(44)):\n${error}")
    endif()
    if(NOT run STREQUAL "mono")
        list(APPEND captures "${wav}")
    endif()
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
# The effect sounds: a sample that is not 0 from 3 s on (the tap's time)
math(EXPR from "44 + 4 * 44100 * 3")
file(READ "${OUT}/capture_a.wav" samples OFFSET ${from} HEX)
if(NOT samples MATCHES "[1-9a-f]")
    message(FATAL_ERROR "capture_a.wav: silence after the tap (effect 43 does not sound)")
endif()

# Mono: as many frames; every frame's left and right equal there, not in the stereo capture.
if(NOT frames_mono EQUAL frames_a)
    message(FATAL_ERROR "capture_mono.wav: ${frames_mono} frames, not ${frames_a}")
endif()
set(count_unequal [=[
import array, sys
for path in sys.argv[1:]:
    with open(path, "rb") as f:
        s = array.array("h", f.read()[44:])
    if sys.byteorder == "big":
        s.byteswap()
    print(sum(1 for i in range(0, len(s) - 1, 2) if s[i] != s[i + 1]), end=";")
]=])
execute_process(
    COMMAND "${PYTHON}" -I -c "${count_unequal}" "${OUT}/capture_a.wav" "${OUT}/capture_mono.wav"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE unequal)
if(NOT result EQUAL 0 OR NOT unequal MATCHES "^([0-9]+);([0-9]+);$")
    message(FATAL_ERROR "counting the frames whose channels differ failed: ${result} ${unequal}")
endif()
if(CMAKE_MATCH_1 EQUAL 0 OR NOT CMAKE_MATCH_2 EQUAL 0)
    message(FATAL_ERROR "frames whose channels differ: ${CMAKE_MATCH_1} in stereo (some "
                        "expected), ${CMAKE_MATCH_2} with --audio-mono on (none expected)")
endif()
message(STATUS "two runs, the same capture: ${frames_a} frames (${size} bytes), effect 43 "
               "sounds; ${CMAKE_MATCH_1} frames with unequal channels, none with --audio-mono on")
