# platform.guest_program (#385, ADR 0213): the guest program through every layer of the port.
#
#   cmake -DPROGRAM=<MapleThiefGuest> -DBUNDLE=<Guest.app> -DSLICE=<MapleThiefGuest image>
#         -DGOLDEN=<png> -DOUT=<dir> -DTOOLS=<repo>/tools -DPYTHON=<python3>
#         -P guest_program_test.cmake
#
# Boots the image (recompiled at configure time, cmake/GuestProgram.cmake) headless on the
# virtual clock for FRAMES window frames, then checks
#   - the log: every "guest:" line the program prints (main, the static constructor, the ObjC
#     hierarchy, the C++ catch, the GL setup and first frame, the timer, the sound) with the
#     values it must compute, and the boot's milestones; no trap, no unimplemented import
#   - the frame: the last captured window frame against the committed synthetic golden
#     (tools/mtport/goldencmp.py, exact: channel 0, ratio 0)
#   - the sound: the captured mix (--audio-capture) is not silent where the tone plays
# With -DPROGRAM= (no image in this build) it prints SKIPPED, which the test's
# SKIP_REGULAR_EXPRESSION reports as skipped.
# MAPLETHIEF_UPDATE_GOLDEN=1 writes the captured frame over the golden instead of comparing.

# A -P script gets no policies from the project: set the project's minimum (#341).
cmake_minimum_required(VERSION 3.28)

if(NOT PROGRAM)
    message("SKIPPED: no guest image in this build (scripts/guest-image on macOS writes "
            "build/guest/; CI's guest-image job uploads it)")
    return()
endif()

set(FRAMES 10)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data")
execute_process(
    COMMAND "${PROGRAM}" --bundle "${BUNDLE}" --slice "${SLICE}" --fixed-step 33.333 --frames
            ${FRAMES} --capture "${OUT}/capture" --audio-capture "${OUT}/audio.wav"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
set(log "${output}${error}")
file(WRITE "${OUT}/log.txt" "${log}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "MapleThiefGuest exited with ${result} (log in ${OUT}/log.txt):\n${log}")
endif()

# The log: each layer's line, with the values the program computes.
set(expected
    # the boot: crt1's start found, the static constructor before main, no nib
    "boot: image: [^\n]*crt1 start, main 0x"
    "boot: constructor: 0x[0-9A-F]+ \\(1 of 1\\)"
    "boot: objc-image: [^\n]* 0 problems"
    "boot: nib: no main nib"
    "guest: main\n"
    # ObjC: a class method, super (init and weight), a category, -class
    "guest: objc: corners 3, doubled 6, weight 13, made 1, class Triangle\n"
    # C++: the static constructor's value, a throw caught as its base class through SjLj
    "guest: c\\+\\+: static constructor 385\n"
    "guest: c\\+\\+: caught 7\n"
    # UIKit + EAGL: the portrait window's renderbuffer; the CFRunLoop timer; the first frame
    "guest: gl: renderbuffer 320x480, framebuffer complete\n"
    "guest: timer: first tick\n"
    "guest: gl: frame 1 presented\n"
    # OpenAL: the bundle's WAV read through CFBundle and queued
    "guest: sound: playing 5512 samples at 22050 Hz, al error 0\n"
    "game: stopped after [0-9.]+ s: ${FRAMES} turns, ${FRAMES} window frames \\(first not black 1"
    "GL draws: [0-9]+ drawn, 0 skipped, 0 misuses")
foreach(pattern IN LISTS expected)
    if(NOT log MATCHES "${pattern}")
        message(FATAL_ERROR "the log has no line matching '${pattern}' (log in "
                            "${OUT}/log.txt):\n${log}")
    endif()
endforeach()
# The unimplemented report is printed only when an import or selector had no implementation.
foreach(bad "boot: trap" "=== guest crash" "unrecognized selector" "unimplemented report")
    string(FIND "${log}" "${bad}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "the log has '${bad}' (log in ${OUT}/log.txt):\n${log}")
    endif()
endforeach()

# The frame: the last window frame, as the screen (960x640 averaged to 480x320), exactly.
set(frame "${OUT}/capture/window_000${FRAMES}.png")
if(NOT EXISTS "${frame}")
    message(FATAL_ERROR "no captured frame ${frame}")
endif()
if("$ENV{MAPLETHIEF_UPDATE_GOLDEN}" STREQUAL "1")
    execute_process(
        COMMAND "${PYTHON}" -E -c "import pathlib, sys; from mtport import goldencmp as g, png; g.save(pathlib.Path(sys.argv[2]), g.normalise(png.load(sys.argv[1])).image)" "${frame}" "${GOLDEN}"
        WORKING_DIRECTORY "${TOOLS}"
        RESULT_VARIABLE result)
    message(STATUS "MAPLETHIEF_UPDATE_GOLDEN=1: wrote ${GOLDEN} (${result})")
endif()
execute_process(
    COMMAND "${PYTHON}" -E -m mtport.goldencmp "${frame}" "${GOLDEN}" --diff "${OUT}/diff.png"
    WORKING_DIRECTORY "${TOOLS}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cmp
    ERROR_VARIABLE cmp)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the frame ${frame} differs from ${GOLDEN} (diff ${OUT}/diff.png):\n${cmp}")
endif()

# The sound: the capture holds the tone (a square wave at a quarter of full scale, mixed): its
# loudest sample in the first 0.2 s is well above silence.
execute_process(
    COMMAND "${PYTHON}" -I -c "import array, sys, wave; w = wave.open(sys.argv[1]); a = array.array('h', w.readframes(w.getframerate() // 5)); sys.exit(0 if a and max(abs(v) for v in a) > 2000 else 1)" "${OUT}/audio.wav"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the captured audio ${OUT}/audio.wav is silent where the tone plays")
endif()
message(STATUS "guest program: every layer's log line, the frame matches ${GOLDEN}, the tone "
               "is in the captured mix")
