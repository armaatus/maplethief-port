# CTest platform.player_contents (#182, ADR 0125), registered by cmake/PlayerBuild.cmake for
# the player build: the executable must not know where this machine keeps the game. It fails if
# PROGRAM contains the build's work/ directory (WORK_DIR, with / or \ separators), which
# MT_DEV_BUNDLE_DIR compiles into a developer build. ("Payload/MapleThief.app" alone is
# allowed: the importer reads that folder from the player's IPA.)
#
#   cmake -DPROGRAM=<MapleThief> -DWORK_DIR=<repo>/work -P player_contents_test.cmake

foreach(var PROGRAM WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "player_contents_test: -D${var}=... is required")
    endif()
endforeach()
if(NOT EXISTS "${PROGRAM}")
    message(FATAL_ERROR "player_contents_test: no ${PROGRAM}")
endif()

string(REPLACE "\\" "/" work_slash "${WORK_DIR}")
string(REPLACE "/" "\\" work_backslash "${work_slash}")
# file(STRINGS) reads the printable runs (8 bytes or longer), so the path is found wherever the
# compiler put it.
file(STRINGS "${PROGRAM}" runs LENGTH_MINIMUM 8)
set(found "")
foreach(run IN LISTS runs)
    foreach(needle IN ITEMS "${work_slash}" "${work_backslash}")
        string(FIND "${run}" "${needle}" at)
        if(NOT at EQUAL -1)
            list(APPEND found "'${run}'")
            break()
        endif()
    endforeach()
endforeach()
if(found)
    list(JOIN found "\n  " text)
    message(FATAL_ERROR "player_contents_test: ${PROGRAM} names this build's ${work_slash}:\n"
                        "  ${text}")
endif()
list(LENGTH runs count)
message(STATUS "player_contents_test: ${PROGRAM}: ${count} strings, none names ${work_slash}")
