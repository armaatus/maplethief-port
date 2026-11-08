# CTests platform.data_folder and platform.game_data_folder (#261, ADR 0127): the data folder is
# next to the game by default, and nothing goes to the per-user folder.
#
# The program is copied into a game folder of its own, <OUT>/kit/, laid out as a player kit:
# MapleThief.app (macOS), MapleThief.exe (Windows), or MapleThief.sh with bin/MapleThief (Linux,
# the bin/ rule). HOME, APPDATA, LOCALAPPDATA and XDG_DATA_HOME point at the empty folder
# <OUT>/home, and MAPLETHIEF_DATA_DIR is unset.
#
#   MODE=cli (every build, CI) on an old-layout <kit>/data (Documents/config, gamecenter.json at
#     the top):
#     - --print-data-dir: data <kit>/data, location game, reason default, saves <kit>/data/saves,
#       game <kit>/game, exit 0;
#     - --import-saves of savedata.py's new character: the save pair written through the real
#       write path (mt_saves_replace_file) into <kit>/data/saves/, the old layout migrated first
#       (Documents/ renamed to saves/, gamecenter.json moved into it), the replaced config in
#       backups/import/;
#     - portable.txt next to the game: logged, no effect;
#     - --data-location elsewhere: a usage error (exit 2).
#   MODE=game (real assets): the game itself, --fixed-step 1 with a tap on Play at 4.5 s (a new
#     game: slot 0 saved) and --quit 8: exit 0, the log's "data: <kit>/data (default)", and
#     <kit>/data/saves/theif0.
#   Both: <OUT>/home is still empty afterwards.
#
#   cmake -DPROGRAM=<MapleThief> [-DBUNDLE=<MapleThief.app>] -DPYTHON=<python3> -DTOOLS=<tools/>
#         -DMODE=cli|game -DOUT=<dir> -P data_folder_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/kit")
set(kit "${OUT}/kit")

# The kit.
get_filename_component(exe_name "${PROGRAM}" NAME)
if(DEFINED BUNDLE AND NOT BUNDLE STREQUAL "")
    file(COPY "${BUNDLE}" DESTINATION "${kit}")
    get_filename_component(app "${BUNDLE}" NAME)
    set(exe "${kit}/${app}/Contents/MacOS/${exe_name}")
elseif(CMAKE_HOST_WIN32)
    file(COPY "${PROGRAM}" DESTINATION "${kit}")
    set(exe "${kit}/${exe_name}")
else()
    file(COPY "${PROGRAM}" DESTINATION "${kit}/bin")
    file(WRITE "${kit}/MapleThief.sh" "#!/bin/sh\nexec \"$(dirname \"$0\")/bin/MapleThief\" \"$@\"\n")
    set(exe "${kit}/bin/${exe_name}")
endif()

set(ENV{HOME} "${OUT}/home")
set(ENV{APPDATA} "${OUT}/home")
set(ENV{LOCALAPPDATA} "${OUT}/home")
set(ENV{XDG_DATA_HOME} "${OUT}/home")
unset(ENV{MAPLETHIEF_DATA_DIR})
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)

# Runs the kit's program with ${ARGN}; <name>_result, <name>_out (stdout) and <name>_log
# (stderr), with '\' as '/', written to <OUT>/<name>.log.
function(run name)
    execute_process(
        COMMAND "${exe}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    string(REPLACE "\\" "/" output "${output}")
    string(REPLACE "\\" "/" error "${error}")
    file(WRITE "${OUT}/${name}.log" "${output}\n--- stderr\n${error}")
    set(${name}_result "${result}" PARENT_SCOPE)
    set(${name}_out "${output}" PARENT_SCOPE)
    set(${name}_log "${error}" PARENT_SCOPE)
endfunction()

# The per-user folder is never touched.
function(check_home_empty)
    file(GLOB_RECURSE left LIST_DIRECTORIES true "${OUT}/home/*")
    if(NOT left STREQUAL "")
        message(FATAL_ERROR "the per-user folder ${OUT}/home is not empty: ${left}")
    endif()
endfunction()

if(MODE STREQUAL "cli")
    # An old layout: the game's files in Documents/, gamecenter.json at the top.
    file(MAKE_DIRECTORY "${kit}/data/Documents")
    file(WRITE "${kit}/data/Documents/config" "old config")
    file(WRITE "${kit}/data/gamecenter.json" "{\"old\": true}")

    run(print --print-data-dir)
    string(CONCAT want "data: ${kit}/data\nlocation: game\nreason: default\n"
                  "saves: ${kit}/data/saves\ngame: ${kit}/game\n")
    if(NOT print_result EQUAL 0 OR NOT print_out STREQUAL want)
        message(FATAL_ERROR "--print-data-dir: exit ${print_result}, printed\n${print_out}\n"
                            "expected exit 0 and\n${want}\n${print_log}")
    endif()

    # A save through the real write path.
    execute_process(
        COMMAND
            "${PYTHON}" -B -c
            "import sys, pathlib; from mtport.savecheck import write_fixtures; write_fixtures(pathlib.Path(sys.argv[1]))"
            "${OUT}/fixtures"
        WORKING_DIRECTORY "${TOOLS}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "savedata.py's fixtures could not be written")
    endif()
    run(import --import-saves "${OUT}/fixtures/new_character")
    if(NOT import_result EQUAL 0)
        message(FATAL_ERROR "--import-saves: exit ${import_result}, expected 0:\n${import_out}\n"
                            "${import_log}")
    endif()
    foreach(name theif0 quest0)
        if(NOT EXISTS "${kit}/data/saves/${name}")
            message(FATAL_ERROR "no ${kit}/data/saves/${name} after the import\n${import_log}")
        endif()
        file(SHA256 "${OUT}/fixtures/new_character/${name}" want_hash)
        file(SHA256 "${kit}/data/saves/${name}" got_hash)
        if(NOT got_hash STREQUAL want_hash)
            message(FATAL_ERROR "saves/${name} is not the fixture's, byte for byte")
        endif()
    endforeach()
    if(EXISTS "${kit}/data/Documents")
        message(FATAL_ERROR "the old Documents/ was not migrated to saves/")
    endif()
    file(READ "${kit}/data/saves/gamecenter.json" gc)
    if(NOT gc STREQUAL "{\"old\": true}" OR EXISTS "${kit}/data/gamecenter.json")
        message(FATAL_ERROR "gamecenter.json was not moved into saves/")
    endif()
    file(GLOB replaced "${kit}/data/backups/import/*/config")
    if(replaced STREQUAL "")
        message(FATAL_ERROR "the replaced config is not in backups/import/")
    endif()
    if(NOT import_log MATCHES "data: ${kit}/data \\(default\\)")
        message(FATAL_ERROR "the log has no 'data: ${kit}/data (default)':\n${import_log}")
    endif()
    file(GLOB probes "${kit}/data/.mt-probe-*")
    if(NOT probes STREQUAL "")
        message(FATAL_ERROR "the probe left ${probes}")
    endif()

    # portable.txt: accepted, logged, no effect.
    file(WRITE "${kit}/portable.txt" "")
    run(portable --print-data-dir)
    if(NOT portable_result EQUAL 0 OR NOT portable_out STREQUAL want
       OR NOT portable_log MATCHES "portable.txt is no longer needed")
        message(FATAL_ERROR "portable.txt: exit ${portable_result}\n${portable_out}\n"
                            "${portable_log}")
    endif()
    file(REMOVE "${kit}/portable.txt")

    run(bad --data-location elsewhere --print-data-dir)
    if(NOT bad_result EQUAL 2 OR NOT bad_log MATCHES "--data-location needs game or user")
        message(FATAL_ERROR "--data-location elsewhere: exit ${bad_result}, expected 2\n"
                            "${bad_log}")
    endif()
    check_home_empty()
    message(STATUS "the data folder is ${kit}/data (default); the import wrote saves/ there "
                   "through the real write path after migrating the old layout; the per-user "
                   "folder ${OUT}/home is empty")
elseif(MODE STREQUAL "game")
    run(game --fixed-step 1 --tap 4.5,106,165 --quit 8)
    if(NOT game_result EQUAL 0)
        message(FATAL_ERROR "the game: exit ${game_result}, expected 0 (log: ${OUT}/game.log)")
    endif()
    if(NOT game_log MATCHES "data: ${kit}/data \\(default\\)")
        message(FATAL_ERROR "the log has no 'data: ${kit}/data (default)' (log: ${OUT}/game.log)")
    endif()
    if(NOT EXISTS "${kit}/data/saves/theif0")
        message(FATAL_ERROR "the new game did not save slot 0 in ${kit}/data/saves/theif0 "
                            "(log: ${OUT}/game.log)")
    endif()
    check_home_empty()
    message(STATUS "the game saved slot 0 in ${kit}/data/saves; the per-user folder is empty")
else()
    message(FATAL_ERROR "MODE must be cli or game")
endif()
