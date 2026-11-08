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
#     - --data-location elsewhere: a usage error (exit 2);
#     - --migrate-from (#262) under MAPLETHIEF_DATA_DIR=<OUT>/env-data, from an old-layout folder
#       <OUT>/old: its saves copied byte for byte into saves/, gamecenter.json too, the source
#       unchanged (exit 0); again: refused, the folder has data (exit 3); with --force: the
#       replaced settings.ini in backups/import/<time>/ (exit 0); --force alone: usage (exit 2);
#     - the first-start copy (#262), Linux only (SDL's per-user folder follows XDG_DATA_HOME
#       there; macOS and Windows ask the system, so a test would read the real one): a second
#       kit <OUT>/kit2 started with the old layout in <OUT>/home/MapleThief gets it copied into
#       <kit2>/data with the notice in the log, the old folder unchanged; the next start copies
#       nothing.
#   MODE=game (real assets): the game itself, --fixed-step 1 with a tap on Play at 4.5 s (a new
#     game: slot 0 saved) and --quit 8: exit 0, the log's "data: <kit>/data (default)", and
#     <kit>/data/saves/theif0. <kit>/data/saves/ holds a file first, so the first-start copy
#     (#262) leaves out the per-user folder, which on macOS and Windows is the real one.
#   Both: <OUT>/home is still empty afterwards.
#
#   cmake -DPROGRAM=<MapleThief> [-DBUNDLE=<MapleThief.app>] -DPYTHON=<python3> -DTOOLS=<tools/>
#         -DMODE=cli|game -DOUT=<dir> -P data_folder_test.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home" "${OUT}/kit")
set(kit "${OUT}/kit")

# A kit in folder `dir`; its program's path in <out_exe>.
function(make_kit dir out_exe)
    get_filename_component(exe_name "${PROGRAM}" NAME)
    if(DEFINED BUNDLE AND NOT BUNDLE STREQUAL "")
        file(COPY "${BUNDLE}" DESTINATION "${dir}")
        get_filename_component(app "${BUNDLE}" NAME)
        set(${out_exe} "${dir}/${app}/Contents/MacOS/${exe_name}" PARENT_SCOPE)
    elseif(CMAKE_HOST_WIN32)
        file(COPY "${PROGRAM}" DESTINATION "${dir}")
        set(${out_exe} "${dir}/${exe_name}" PARENT_SCOPE)
    else()
        file(COPY "${PROGRAM}" DESTINATION "${dir}/bin")
        file(WRITE "${dir}/MapleThief.sh"
             "#!/bin/sh\nexec \"$(dirname \"$0\")/bin/MapleThief\" \"$@\"\n")
        set(${out_exe} "${dir}/bin/${exe_name}" PARENT_SCOPE)
    endif()
endfunction()
make_kit("${kit}" exe)

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

    # --migrate-from (#262): from an old-layout folder, into MAPLETHIEF_DATA_DIR.
    set(old "${OUT}/old")
    file(COPY "${OUT}/fixtures/new_character/" DESTINATION "${old}/Documents")
    file(WRITE "${old}/gamecenter.json" "{\"from\": \"old\"}")
    file(WRITE "${old}/settings.ini" "master_volume = 70\n")
    file(GLOB_RECURSE old_files RELATIVE "${old}" "${old}/*")
    set(old_hashes "")
    foreach(f IN LISTS old_files)
        file(SHA256 "${old}/${f}" h)
        string(APPEND old_hashes "${f} ${h}\n")
    endforeach()
    set(env_data "${OUT}/env-data")
    set(ENV{MAPLETHIEF_DATA_DIR} "${env_data}")
    run(migrate --migrate-from "${old}")
    if(NOT migrate_result EQUAL 0 OR NOT migrate_out MATCHES "migrated: ")
        message(FATAL_ERROR "--migrate-from: exit ${migrate_result}, expected 0\n${migrate_out}\n"
                            "${migrate_log}")
    endif()
    foreach(name theif0 quest0)
        file(SHA256 "${old}/Documents/${name}" want_hash)
        file(SHA256 "${env_data}/saves/${name}" got_hash)
        if(NOT got_hash STREQUAL want_hash)
            message(FATAL_ERROR "--migrate-from: saves/${name} is not the source's, byte for byte")
        endif()
    endforeach()
    file(READ "${env_data}/saves/gamecenter.json" gc)
    if(NOT gc STREQUAL "{\"from\": \"old\"}")
        message(FATAL_ERROR "--migrate-from: gamecenter.json is not in saves/")
    endif()
    run(migrate_again --migrate-from "${old}")
    if(NOT migrate_again_result EQUAL 3 OR NOT migrate_again_out MATCHES "already has data")
        message(FATAL_ERROR "--migrate-from into a folder with data: exit "
                            "${migrate_again_result}, expected 3\n${migrate_again_out}")
    endif()
    file(WRITE "${env_data}/settings.ini" "mine = 1\n")
    run(migrate_force --migrate-from "${old}" --force)
    file(GLOB kept "${env_data}/backups/import/*/settings.ini")
    if(NOT migrate_force_result EQUAL 0 OR NOT migrate_force_out MATCHES "replaced: settings.ini"
       OR kept STREQUAL "")
        message(FATAL_ERROR "--migrate-from --force: exit ${migrate_force_result}, expected 0 "
                            "and the replaced settings.ini backed up\n${migrate_force_out}")
    endif()
    file(READ "${kept}" kept_text)
    if(NOT kept_text STREQUAL "mine = 1\n")
        message(FATAL_ERROR "--migrate-from --force: the backup is not the replaced settings.ini")
    endif()
    run(force_alone --force)
    if(NOT force_alone_result EQUAL 2)
        message(FATAL_ERROR "--force alone: exit ${force_alone_result}, expected 2")
    endif()
    unset(ENV{MAPLETHIEF_DATA_DIR})
    set(new_hashes "")
    foreach(f IN LISTS old_files)
        file(SHA256 "${old}/${f}" h)
        string(APPEND new_hashes "${f} ${h}\n")
    endforeach()
    file(GLOB_RECURSE old_files_after RELATIVE "${old}" "${old}/*")
    if(NOT new_hashes STREQUAL old_hashes OR NOT old_files_after STREQUAL old_files)
        message(FATAL_ERROR "--migrate-from changed its source ${old}")
    endif()

    # The first-start copy (#262), where the per-user folder follows XDG_DATA_HOME.
    if(CMAKE_HOST_UNIX AND NOT CMAKE_HOST_APPLE)
        set(user "${OUT}/home/MapleThief")
        file(COPY "${old}/" DESTINATION "${user}")
        make_kit("${OUT}/kit2" exe)
        run(first --list-backups)
        string(REPLACE "\n" " " first_flat "${first_log}")
        if(NOT first_result EQUAL 0
           OR NOT first_flat MATCHES "Your saves and settings were copied from ${user} to")
            message(FATAL_ERROR "the first start of ${OUT}/kit2: exit ${first_result}, no "
                                "notice of the copy (log: ${OUT}/first.log)")
        endif()
        foreach(name theif0 quest0)
            file(SHA256 "${old}/Documents/${name}" want_hash)
            file(SHA256 "${OUT}/kit2/data/saves/${name}" got_hash)
            if(NOT got_hash STREQUAL want_hash)
                message(FATAL_ERROR "the first-start copy: saves/${name} is not the old one's")
            endif()
        endforeach()
        if(NOT EXISTS "${OUT}/kit2/data/settings.ini"
           OR NOT EXISTS "${OUT}/kit2/data/saves/gamecenter.json")
            message(FATAL_ERROR "the first-start copy left out settings.ini or gamecenter.json")
        endif()
        if(EXISTS "${user}/saves" OR NOT EXISTS "${user}/Documents/theif0")
            message(FATAL_ERROR "the first-start copy changed the old folder ${user}")
        endif()
        run(second --list-backups)
        if(NOT second_result EQUAL 0 OR second_log MATCHES "were copied from")
            message(FATAL_ERROR "the second start copied again (log: ${OUT}/second.log)")
        endif()
        file(REMOVE_RECURSE "${user}")
        check_home_empty()
        message(STATUS "the first-start copy: ${user} copied into ${OUT}/kit2/data")
    else()
        message(STATUS "the first-start copy's kit check runs on Linux only (the per-user folder "
                       "here is the system's)")
    endif()
    message(STATUS "the data folder is ${kit}/data (default); the import wrote saves/ there "
                   "through the real write path after migrating the old layout; the per-user "
                   "folder ${OUT}/home is empty; --migrate-from copied an old layout")
elseif(MODE STREQUAL "game")
    file(WRITE "${kit}/data/saves/.not-empty" "")
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
