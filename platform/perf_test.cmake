# CTest of the frame telemetry and the frame-time overlay (#137, ADR 0071), real assets:
#
#   cmake -DPROGRAM=<MapleThief> -DOUT=<dir> -P perf_test.cmake
#     (platform.perf_telemetry) On SDL's dummy drivers:
#     - a real-time run (--seconds 6, the banner closed and Play tapped, --perf-overlay on,
#       --perf-csv) ends cleanly; its exit log has the tick intervals (p50, p99, max, mean, GL
#       frames a second, overruns over 55 ms), the split of a window frame and the pacing line,
#       next to the frame-time lines platform.gpu_compare reads; the CSV has its header and one
#       row per window frame;
#     - the same scripted fixed-step run (--shot, --capture, --audio-capture) twice, the second
#       with --perf-overlay on and --perf-csv: every PNG (the shot and the window read-backs)
#       and the WAV are the same bytes, and the overlay says it is off in such a run.
# Logs, CSVs, captures and data directories go to OUT (build/, never committed).

# skip_intros is pinned off (#174, ADR 0096): it runs the guest clock x8 through scenes, and this
# test times or counts the guest's ticks; a suite run with MAPLETHIEF_SETTINGS="skip_intros = on"
# must still pass. The flag comes after the environment's settings, so it wins.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(taps --tap 3,462,227 --tap 4.5,106,165)

# --- a real-time run ----------------------------------------------------------------------
set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_real")
execute_process(
    COMMAND "${PROGRAM}" --skip-intros off --seconds 6 ${taps} --perf-overlay on --perf-csv
            "${OUT}/real.csv"
    RESULT_VARIABLE result
    ERROR_VARIABLE log)
file(WRITE "${OUT}/real.log" "${log}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the real-time run exited with ${result} (${OUT}/real.log)")
endif()
set(num "[0-9]+\\.[0-9][0-9]")
foreach(
    pattern
    "perf: display [0-9.]+ Hz, vsync (on|off); the frame-time overlay is on \\(F3\\)"
    "game: ticks: [1-9][0-9]* GL frames, ${num} a second; tick interval p50 ${num} ms, p99 ${num} ms, max ${num} ms, mean ${num} ms; [0-9]+ over 55 ms"
    "game: frame split: [1-9][0-9]* window frames; per frame guest ${num} ms, mt_gfx ${num} ms, taking the GL frame ${num} ms, compositor ${num} ms, upload and draw ${num} ms, SDL_RenderPresent ${num} ms, waiting ${num} ms, other ${num} ms"
    "game: pacing: "
    "game: per window frame: compositing ${num} ms"
    "game: frame time: ${num} ms per window frame in the frontend")
    if(NOT log MATCHES "${pattern}")
        message(FATAL_ERROR "the real-time run's log lacks /${pattern}/ (${OUT}/real.log)")
    endif()
endforeach()
string(REGEX MATCH "game: frame split: ([0-9]+) window frames" _ "${log}")
set(frames "${CMAKE_MATCH_1}")
string(REGEX MATCH "game: ticks: ([0-9]+) GL frames, ([0-9.]+) a second; tick interval p50 ([0-9.]+) ms, p99 ([0-9.]+) ms, max ([0-9.]+) ms" _ "${log}")
set(summary "${CMAKE_MATCH_1} GL frames, ${CMAKE_MATCH_2}/s, p50 ${CMAKE_MATCH_3} ms, p99 ${CMAKE_MATCH_4} ms, max ${CMAKE_MATCH_5} ms")
if(NOT log MATCHES "perf: ${frames} rows written to ")
    message(FATAL_ERROR "the CSV does not have a row per window frame (${frames}) (${OUT}/real.log)")
endif()
file(STRINGS "${OUT}/real.csv" rows)
list(LENGTH rows count)
list(GET rows 0 header)
math(EXPR want "${frames} + 1")
if(NOT count EQUAL want OR NOT header STREQUAL
   "frame,gl_frame,tick,present_s,interval_ms,tick_ms,vblanks,guest_ms,gfx_ms,take_ms,compose_ms,draw_ms,present_ms,wait_ms,other_ms")
    message(FATAL_ERROR "${OUT}/real.csv: ${count} lines (want ${want}), header '${header}'")
endif()

# --- fixed-step runs: the same bytes with the telemetry's options -------------------------
set(fixed --fixed-step 1 ${taps} --shot 8,loading)
foreach(run plain perf)
    set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data_${run}")
    set(extra "")
    if(run STREQUAL "perf")
        set(extra --perf-overlay on --perf-csv "${OUT}/fixed.csv")
    endif()
    execute_process(
        COMMAND "${PROGRAM}" --skip-intros off ${fixed} --capture "${OUT}/${run}" --audio-capture
                "${OUT}/${run}.wav" ${extra}
        RESULT_VARIABLE result
        ERROR_VARIABLE log)
    file(WRITE "${OUT}/${run}.log" "${log}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the fixed-step run (${run}) exited with ${result} (${OUT}/${run}.log)")
    endif()
endforeach()
file(READ "${OUT}/perf.log" log)
if(NOT log MATCHES "the frame-time overlay is off \\(F3: never in a run that writes images\\)")
    message(FATAL_ERROR "the fixed-step run showed the overlay (${OUT}/perf.log)")
endif()
file(GLOB pngs RELATIVE "${OUT}/plain" "${OUT}/plain/*.png")
list(LENGTH pngs png_count)
if(png_count LESS 2 OR NOT EXISTS "${OUT}/plain/loading.png")
    message(FATAL_ERROR "the fixed-step run wrote ${png_count} PNGs (${OUT}/plain)")
endif()
foreach(file ${pngs} ../plain.wav)
    set(other "${file}")
    if(file STREQUAL "../plain.wav")
        set(other "../perf.wav")
    endif()
    file(SHA256 "${OUT}/plain/${file}" a)
    if(NOT EXISTS "${OUT}/perf/${other}")
        message(FATAL_ERROR "${OUT}/perf/${other} is missing")
    endif()
    file(SHA256 "${OUT}/perf/${other}" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "${file} differs with --perf-overlay on and --perf-csv")
    endif()
endforeach()
message(STATUS "perf: the real-time run logged ${summary} and ${frames} CSV rows; the fixed-step "
               "run's ${png_count} PNGs and WAV are the same bytes with the overlay and the CSV on")
