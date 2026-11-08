# CTest platform.player_kit (#263, ADR 0125, ADR 0127; every build, CI on every OS): the player
# kit's README explains the data folder, and a kit staged as cmake/Packaging.cmake lays it out
# keeps its data in <kit>/data.
#
#   - README (platform/player/README.md): the section "Your data folder" with each subfolder
#     (saves/, backups/, states/, screenshots/, exports/, import/, logs/, crashes/, and the
#     frontend's .ini files), backing up by copying data/, starting over by deleting
#     data/saves/, keeping the folder together, updating to a new kit (--migrate-from), the user
#     folder option (user-data.txt, --switch-data-location); the section "macOS: first start"
#     with both translocation fixes (xattr -dr com.apple.quarantine, a move in the Finder); no
#     portable.txt any more.
#   - Packaging.cmake ships no data/ (the game creates it, ADR 0127).
#   - The staged kit, MapleThief-<os>/ as the player package lays it out: MapleThief.exe
#     (Windows), MapleThief.app (macOS), or MapleThief.sh (the real launcher), bin/MapleThief and
#     icon.png (Linux); README.md, LICENSE, THIRD_PARTY.md. It has no data/ before the first
#     start. Started (through MapleThief.sh on Linux) with --print-data-dir, the per-user folders
#     at an empty folder: data <kit>/data, location game, reason default, game <kit>/game; data/
#     is there afterwards (the probe made it) without the probe's file, and the per-user folder
#     stays empty.
#
#   cmake -DPROGRAM=<MapleThief> [-DBUNDLE=<MapleThief.app>] -DSOURCE=<repo> -DOUT=<dir>
#         -P player_kit_test.cmake

foreach(var PROGRAM SOURCE OUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "player_kit_test: -D${var}=... is required")
    endif()
endforeach()

# --- The README ------------------------------------------------------------------------------
file(READ "${SOURCE}/platform/player/README.md" readme)
set(missing "")
foreach(
    needle
    "## Your data folder"
    "`saves/`"
    "`gamecenter.json`"
    "`settings.ini`"
    "`bindings.ini`"
    "`window.ini`"
    "`backups/`"
    "`states/`"
    "`screenshots/`"
    "`exports/`"
    "`import/`"
    "`logs/`"
    "`crashes/`"
    "`game/`"
    "**Back up** by copying `data/`"
    "deleting `data/saves/`"
    "**Keep the folder together.**"
    "**Updating to a new version**"
    "--migrate-from"
    "user-data.txt"
    "--switch-data-location user"
    "--switch-data-location game"
    "*Data location*"
    "--print-data-dir"
    "## macOS: first start"
    "xattr -dr com.apple.quarantine"
    "in the Finder, drag `MapleThief.app` out of this folder"
    "App Translocation"
    "Program Files")
    string(FIND "${readme}" "${needle}" at)
    if(at EQUAL -1)
        list(APPEND missing "'${needle}'")
    endif()
endforeach()
string(FIND "${readme}" "portable.txt" portable_at)
if(NOT portable_at EQUAL -1)
    list(APPEND missing "(no portable.txt)")
endif()
if(missing)
    list(JOIN missing "\n  " text)
    message(FATAL_ERROR "player_kit_test: platform/player/README.md lacks:\n  ${text}")
endif()

# --- Packaging ships no data/ ----------------------------------------------------------------
file(READ "${SOURCE}/cmake/Packaging.cmake" packaging)
if(packaging MATCHES "DESTINATION[ \t\r\n]+\"?data[/\" \t\r\n)]")
    message(FATAL_ERROR "player_kit_test: cmake/Packaging.cmake installs a data/ folder; the "
                        "game creates it (ADR 0127)")
endif()

# --- The staged kit --------------------------------------------------------------------------
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/home")
get_filename_component(exe_name "${PROGRAM}" NAME)
if(DEFINED BUNDLE AND NOT BUNDLE STREQUAL "")
    set(kit "${OUT}/MapleThief-macos")
    file(COPY "${BUNDLE}" DESTINATION "${kit}")
    get_filename_component(app "${BUNDLE}" NAME)
    set(run "${kit}/${app}/Contents/MacOS/${exe_name}")
    set(layout "${app}")
elseif(CMAKE_HOST_WIN32)
    set(kit "${OUT}/MapleThief-windows")
    file(COPY "${PROGRAM}" DESTINATION "${kit}")
    set(run "${kit}/${exe_name}")
    set(layout "${exe_name}")
else()
    set(kit "${OUT}/MapleThief-linux")
    file(COPY "${PROGRAM}" DESTINATION "${kit}/bin")
    file(COPY "${SOURCE}/platform/linux/MapleThief.sh" DESTINATION "${kit}"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
    file(COPY "${SOURCE}/platform/icon/icon.png" DESTINATION "${kit}")
    set(run "${kit}/MapleThief.sh")
    set(layout "MapleThief.sh;bin/${exe_name};icon.png")
endif()
file(COPY "${SOURCE}/platform/player/README.md" "${SOURCE}/LICENSE" "${SOURCE}/THIRD_PARTY.md"
     DESTINATION "${kit}")
list(APPEND layout README.md LICENSE THIRD_PARTY.md)
foreach(entry IN LISTS layout)
    if(NOT EXISTS "${kit}/${entry}")
        message(FATAL_ERROR "player_kit_test: the staged kit has no ${entry}")
    endif()
endforeach()
if(EXISTS "${kit}/data")
    message(FATAL_ERROR "player_kit_test: the kit has a data/ before its first start")
endif()

set(ENV{HOME} "${OUT}/home")
set(ENV{APPDATA} "${OUT}/home")
set(ENV{LOCALAPPDATA} "${OUT}/home")
set(ENV{XDG_DATA_HOME} "${OUT}/home")
unset(ENV{MAPLETHIEF_DATA_DIR})
unset(ENV{MAPLETHIEF_BUNDLE_DIR})
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
execute_process(
    COMMAND "${run}" --print-data-dir
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
string(REPLACE "\\" "/" output "${output}")
string(CONCAT want "data: ${kit}/data\nlocation: game\nreason: default\n"
              "saves: ${kit}/data/saves\ngame: ${kit}/game\n")
if(NOT result EQUAL 0 OR NOT output STREQUAL want)
    message(FATAL_ERROR "player_kit_test: ${run} --print-data-dir: exit ${result}, printed\n"
                        "${output}\nexpected exit 0 and\n${want}\n${error}")
endif()
if(NOT IS_DIRECTORY "${kit}/data")
    message(FATAL_ERROR "player_kit_test: no ${kit}/data after the first start")
endif()
file(GLOB probes "${kit}/data/.mt-probe-*")
file(GLOB_RECURSE home LIST_DIRECTORIES true "${OUT}/home/*")
if(probes OR home)
    message(FATAL_ERROR "player_kit_test: left behind: ${probes} ${home}")
endif()
list(JOIN layout ", " layout_text)
message(STATUS "player_kit_test: the README explains the data folder and macOS's first start; "
               "the kit (${layout_text}) ships no data/ and keeps its data in ${kit}/data")
