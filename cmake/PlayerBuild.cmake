# The player build (#182, ADR 0125): the game executable for a release download. It links the
# recompiled code and has none of the game's assets. The player's own IPA is imported on first
# launch into the data directory's game/ folder (platform/src/ipa_import.*).
#
# MAPLETHIEF_PLAYER_BUILD (OFF by default), turned on by the <os>-player presets:
#   - needs the generated code (the target maplethief_gen, from work/gen) and fails at
#     configure time without it, or together with MAPLETHIEF_MAC_APP (which embeds the bundle);
#   - links maplethief_gen and defines MT_PLAYER_BUILD=1 on MapleThief, but not
#     MT_DEV_BUNDLE_DIR (no path into work/ is compiled in) and not MT_EMBEDDED_BUNDLE; no
#     bundle is copied anywhere (platform/CMakeLists.txt);
#   - packages only the player's files (cmake/Packaging.cmake): the executable or MapleThief.app
#     (with the port's own icon, platform/icon/), the player README (platform/player/README.md),
#     LICENSE, THIRD_PARTY.md and licenses/, as MapleThief-<version>-<os>-<arch>;
#   - registers platform.player_contents, which fails if the executable holds the build's work/
#     path (platform/player_contents_test.cmake);
#   - on Windows, links the static MSVC runtime (the win-player preset sets
#     CMAKE_MSVC_RUNTIME_LIBRARY), so a player needs no Visual C++ redistributable.
#
# maplethief_player_build(<target>): the checks and definitions above, for the MapleThief
# executable. Called by platform/CMakeLists.txt when MAPLETHIEF_PLAYER_BUILD is ON.

option(MAPLETHIEF_PLAYER_BUILD
       "Build the player download: the game with the recompiled code, no bundle, no path into work/ (needs work/gen; ADR 0125)"
       OFF)

function(maplethief_player_build target)
    if(NOT TARGET maplethief_gen)
        message(
            FATAL_ERROR
                "MAPLETHIEF_PLAYER_BUILD: the player build needs the generated code, and there is "
                "none in '${MAPLETHIEF_GEN_DIR}'. From the repository: scripts/bootstrap, then "
                "python3 -m recomp all, then configure again (docs/release-checklist.md).")
    endif()
    if(MAPLETHIEF_MAC_APP)
        message(FATAL_ERROR "MAPLETHIEF_PLAYER_BUILD and MAPLETHIEF_MAC_APP exclude each other: "
                            "the game app embeds the IPA's bundle, a player build never does")
    endif()
    target_compile_definitions(${target} PRIVATE MT_PLAYER_BUILD=1)

    cmake_path(GET MAPLETHIEF_GEN_DIR PARENT_PATH work)
    cmake_path(ABSOLUTE_PATH work BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    add_test(
        NAME platform.player_contents
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:${target}>" "-DWORK_DIR=${work}" -P
                "${PROJECT_SOURCE_DIR}/platform/player_contents_test.cmake")
    set_tests_properties(platform.player_contents PROPERTIES TIMEOUT 60)
    message(STATUS "MapleThief: the player build (generated code, no bundle, MT_PLAYER_BUILD)")
endfunction()
