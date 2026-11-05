# CTest platform.audio_pack (tests/CMakeLists.txt, #188, ADR 0106): audio packs in the real
# game. The run of platform.audio_capture (--fixed-step 16.667 --frames 180, a tap on Play at
# 3 s) plays the main menu's track 31 from the start and the intro's 30 after the tap
# (docs/re/audio.md). Four runs, each with an --audio-capture, from a fresh data directory:
#
#   none     no audio_pack: the capture as without #188
#   unplayed a pack whose only file replaces 0.mp3, which this run never plays (0 is the
#            Henesys ending's): the same bytes as none
#   corrupt  a pack whose 31.mp3 is no MP3: rejected and logged, the game's 31.mp3 plays, the
#            same bytes as none
#   menu     a pack whose 31.wav is a generated tone (2 s, stereo, 44.1 kHz; PYTHON writes it):
#            the capture differs from none, and from the track's first frame on, for 1 s (before
#            the tap), it is the tone at the game's music volume 0.7, within 1 LSB
#
# The tones are written at run time into the test output directory: no sound is committed.
#
#   cmake -DPROGRAM=<MapleThief> -DOUT=<dir> -DPYTHON=<python3> -P audio_pack_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/pack_unplayed" "${OUT}/pack_corrupt" "${OUT}/pack_menu")

set(write_tone [=[
import struct, sys, wave
frames = 2 * 44100
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(44100)
    data = bytearray()
    for i in range(frames):
        left = (i * 37) % 20000 - 10000 + 3  # a sawtooth that is not 0 at frame 0
        right = 9000 if (i // 50) % 2 == 0 else -9000  # a square wave
        data += struct.pack("<hh", left, right)
    w.writeframes(bytes(data))
]=])
foreach(file "${OUT}/pack_unplayed/0.wav" "${OUT}/pack_menu/31.wav")
    execute_process(COMMAND "${PYTHON}" -I -c "${write_tone}" "${file}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "writing the tone ${file} failed: ${result}")
    endif()
endforeach()
file(WRITE "${OUT}/pack_corrupt/31.mp3" "this is no MP3 file, only some text in its place")

foreach(run none unplayed corrupt menu)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${run}")
    set(ENV{SDL_VIDEO_DRIVER} dummy)
    set(ENV{MAPLETHIEF_TRACE} audio)
    set(pack "")
    if(NOT run STREQUAL "none")
        set(pack --audio-pack "${OUT}/pack_${run}")
    endif()
    execute_process(
        COMMAND "${PROGRAM}" --fixed-step 16.667 --frames 180 --tap 3,106,165 ${pack}
                --audio-capture "${OUT}/capture_${run}.wav"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/log_${run}.txt" "${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "run ${run} exited with ${result}:\n${error}")
    endif()
    if(NOT error MATCHES "AVAudioPlayer initWithContentsOfURL:[^\n]*/31\\.mp3")
        message(FATAL_ERROR "run ${run}: the main menu's track 31 was not opened:\n${error}")
    endif()
    set(log_${run} "${error}")
endforeach()

if(log_none MATCHES "audiopack")
    message(FATAL_ERROR "run none: an audiopack line without a pack")
endif()
if(NOT log_unplayed MATCHES "audiopack: pack: [0-9]+ effects, 0 replaced; [0-9]+ tracks, 0 replaced; 0 rejected")
    message(FATAL_ERROR "run unplayed: not the exit line of a pack that replaced nothing")
endif()
if(NOT log_corrupt MATCHES "audiopack: music [^\n]*31\\.mp3 holds no MP3 audio[^\n]*rejected, the game's 31\\.mp3 kept"
   OR NOT log_corrupt MATCHES "0 replaced; 1 rejected")
    message(FATAL_ERROR "run corrupt: 31.mp3 was not rejected and logged")
endif()
if(NOT log_menu MATCHES "31\\.mp3 error:\\] -> [^\n]*from the audio pack: [^\n]*31\\.wav"
   OR NOT log_menu MATCHES "[0-9]+ tracks, 1 replaced; 0 rejected")
    message(FATAL_ERROR "run menu: 31.mp3 was not replaced by the pack's 31.wav")
endif()

foreach(run unplayed corrupt)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/capture_none.wav"
                            "${OUT}/capture_${run}.wav" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
        message(FATAL_ERROR "capture_${run}.wav differs from capture_none.wav")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/capture_none.wav"
                        "${OUT}/capture_menu.wav" RESULT_VARIABLE differ)
if(differ EQUAL 0)
    message(FATAL_ERROR "capture_menu.wav is capture_none.wav: the pack's track did not play")
endif()

# The menu run's first second of music is the tone at volume 0.7 (the capture's rounding,
# lround(x * 32767), within 1 LSB of the float math written out here).
set(check_tone [=[
import array, sys, wave
def samples(path):
    with wave.open(path, "rb") as w:
        s = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        s.byteswap()
    return s
cap, tone = samples(sys.argv[1]), samples(sys.argv[2])
first = next(i for i in range(0, len(cap), 2) if cap[i] != 0 or cap[i + 1] != 0)
worst = 0
for k in range(0, 2 * 44100, 2):
    for ch in (0, 1):
        want = round(tone[k + ch] / 32768 * 0.7 * 32767)
        worst = max(worst, abs(cap[first + k + ch] - want))
print(f"{first // 2};{worst};", end="")
]=])
execute_process(
    COMMAND "${PYTHON}" -I -c "${check_tone}" "${OUT}/capture_menu.wav" "${OUT}/pack_menu/31.wav"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE checked)
if(NOT result EQUAL 0 OR NOT checked MATCHES "^([0-9]+);([0-9]+);$")
    message(FATAL_ERROR "checking the tone in capture_menu.wav failed: ${result} ${checked}")
endif()
if(CMAKE_MATCH_2 GREATER 1)
    message(FATAL_ERROR "capture_menu.wav from frame ${CMAKE_MATCH_1}: the tone x 0.7 off by "
                        "${CMAKE_MATCH_2} LSB")
endif()
message(STATUS "no pack, a pack for an unplayed track and a rejected file: the same capture; "
               "the pack's 31.wav plays from frame ${CMAKE_MATCH_1}, the tone x 0.7 within "
               "${CMAKE_MATCH_2} LSB for 1 s")
