# CTest of frame interpolation (#87, ADR 0091, platform/src/frame_interp.h): frame_rate = smooth
# changes what the window shows between two ticks and nothing of the game.
#
#   cmake -DPROGRAM=<MapleThief> -DSAVE=<play.save_quit's data dir> -DOUT=<dir> -P frame_rate_test.cmake
#
# (platform.frame_rate, real assets, after play.save_quit) The same scripted run twice on SDL's
# dummy drivers, --fixed-step 16.667 for 60 s of game time, from a copy of play.save_quit's
# save (a hero in Henesys, 2.0): taps on Play, the first slot and Start, then the key pad's left
# arm held for 8 s and its right arm for 8 s (the hero walks and the camera scrolls). Once with
# --frame-rate original, once with --frame-rate smooth:
#   - the exit log's tick count ("game: ticks: N GL frames") is the same, and the audio capture
#     (the game's own sounds and music, ADR 0045) the same bytes: the game ran the same;
#   - smooth interpolated most ticks (its exit line), drew interpolated frames, and its window
#     presented more frames than ticks; original logs no frame_rate = smooth line.
# Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION) when play.save_quit left no save.

if(NOT EXISTS "${SAVE}/data/Documents/theif0")
    message(STATUS "skipped: no save in ${SAVE} (run play.save_quit first)")
    return()
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{MAPLETHIEF_LANGUAGE} en)

foreach(rate IN ITEMS original smooth)
    file(COPY "${SAVE}/data/Documents" DESTINATION "${OUT}/data_${rate}")
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${rate}")
    execute_process(
        COMMAND
            "${PROGRAM}" --fixed-step 16.667 --seconds 60 --frame-rate ${rate} --tap 3,106,165
            --tap 5,99,232 --tap 6.5,420,79 --hold 12,35,257,8 --hold 22,132,257,8 --audio-capture
            "${OUT}/${rate}.wav"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE log)
    file(WRITE "${OUT}/${rate}.log" "${log}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${rate}: exit ${result} (log: ${OUT}/${rate}.log)")
    endif()
    string(REGEX MATCH "game: ticks: ([0-9]+) GL frames" hit "${log}")
    if(hit STREQUAL "")
        message(FATAL_ERROR "${rate}: no tick count in the log (${OUT}/${rate}.log)")
    endif()
    set(ticks_${rate} "${CMAKE_MATCH_1}")
    string(REGEX MATCH "frame split: ([0-9]+) window frames" hit "${log}")
    set(windows_${rate} "${CMAKE_MATCH_1}")
    set(log_${rate} "${log}")
endforeach()

if(NOT ticks_original EQUAL ticks_smooth)
    message(FATAL_ERROR "the tick count differs: ${ticks_original} with original, "
                        "${ticks_smooth} with smooth (logs in ${OUT})")
endif()
file(SHA256 "${OUT}/original.wav" wav_original)
file(SHA256 "${OUT}/smooth.wav" wav_smooth)
if(NOT wav_original STREQUAL wav_smooth)
    message(FATAL_ERROR "the audio captures differ (${OUT}/original.wav, ${OUT}/smooth.wav)")
endif()
if(log_original MATCHES "frame_rate = smooth")
    message(FATAL_ERROR "original logged frame interpolation (${OUT}/original.log)")
endif()
string(
    REGEX MATCH
          "frame_rate = smooth: ([0-9]+) ticks, ([0-9]+) interpolated[^;]*; ([0-9]+) interpolated frames drawn"
          hit "${log_smooth}")
if(hit STREQUAL "")
    message(FATAL_ERROR "smooth: no frame_rate = smooth line (${OUT}/smooth.log)")
endif()
set(seen "${CMAKE_MATCH_1}")
set(interpolated "${CMAKE_MATCH_2}")
set(drawn "${CMAKE_MATCH_3}")
math(EXPR most "${seen} * 3 / 4")
math(EXPR more "${ticks_smooth} * 2")
if(NOT seen EQUAL ticks_smooth OR interpolated LESS most OR drawn LESS interpolated OR
   windows_smooth LESS more)
    message(FATAL_ERROR "smooth: ${seen} ticks seen of ${ticks_smooth}, ${interpolated} "
                        "interpolated, ${drawn} frames drawn, ${windows_smooth} window frames: "
                        "expected every tick seen, at least 3/4 interpolated, at least one frame "
                        "drawn each and two window frames a tick (${OUT}/smooth.log)")
endif()

message(STATUS "frame_rate: ${ticks_original} ticks either way, the same audio capture; smooth "
               "interpolated ${interpolated} of ${seen} ticks, ${drawn} frames drawn, "
               "${windows_smooth} window frames against ${windows_original}")
