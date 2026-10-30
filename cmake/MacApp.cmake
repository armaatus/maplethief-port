# The macOS game app (#81, ADR 0064): a self-contained, double-clickable MapleThief.app built
# locally from the user's own IPA. The `mac-app` preset (universal, Release) turns it on.
#
# maplethief_mac_app(<target>)
#   Called by platform/CMakeLists.txt for the MapleThief bundle when MAPLETHIEF_MAC_APP is ON
#   (macOS only). Needs the game build (the target maplethief_gen, from work/gen) and the
#   extracted IPA next to it (work/ipa, from scripts/bootstrap); fails at configure time with
#   the steps that make them otherwise. It then
#   - copies the extracted bundle work/ipa/Payload/MapleThief.app into
#     MapleThief.app/Contents/Resources/Game/ (MAPLETHIEF_MAC_APP_GAME_DIR), where the program
#     finds it through SDL_GetBasePath() (MT_EMBEDDED_BUNDLE, main.cpp) instead of the build's
#     work/ipa path, so the app can be moved anywhere;
#   - writes the icon MapleThief.icns from the IPA's own artwork with `python -m mtport.macicon`
#     (sips + Python, tools/mtport/macicon.py) into the build directory and puts it in
#     Contents/Resources (CFBundleIconFile);
#   - signs the whole bundle ad hoc after every build (`codesign --force --sign -`), without the
#     hardened runtime (no `--options runtime`): no Developer ID, no notarisation (ADR 0024).
#   Everything it writes is derived from the IPA and stays below build/: such a build is never
#   installed or packaged (cmake/Packaging.cmake refuses it).

option(MAPLETHIEF_MAC_APP
       "Build MapleThief.app as the self-contained game app: the IPA's bundle and icon inside, ad-hoc signed (macOS, needs work/gen; ADR 0064)"
       OFF)
set(MAPLETHIEF_MAC_APP_GAME_DIR Game)

function(maplethief_mac_app target)
    if(NOT APPLE)
        message(FATAL_ERROR "MAPLETHIEF_MAC_APP is for macOS builds only")
    endif()
    if(NOT TARGET maplethief_gen)
        message(
            FATAL_ERROR
                "MAPLETHIEF_MAC_APP: the game app needs the generated code, and there is none in "
                "'${MAPLETHIEF_GEN_DIR}'. From the repository: scripts/bootstrap, then "
                "python3 -m recomp all, then configure again (README, 'The macOS app').")
    endif()
    cmake_path(GET MAPLETHIEF_GEN_DIR PARENT_PATH work)
    cmake_path(ABSOLUTE_PATH work BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    set(ipa "${work}/ipa")
    set(bundle "${ipa}/Payload/MapleThief.app")
    if(NOT EXISTS "${bundle}/Info.plist")
        message(FATAL_ERROR "MAPLETHIEF_MAC_APP: no extracted bundle at ${bundle}; run "
                            "scripts/bootstrap")
    endif()

    # The icon, from the IPA's own artwork (never committed: it is written below build/).
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(icns "${CMAKE_CURRENT_BINARY_DIR}/MapleThief.icns")
    set(depends "${bundle}/Info.plist" "${PROJECT_SOURCE_DIR}/tools/mtport/macicon.py"
                "${PROJECT_SOURCE_DIR}/tools/mtport/png.py" "${PROJECT_SOURCE_DIR}/tools/mtport/cgbi.py")
    file(GLOB icons CONFIGURE_DEPENDS "${bundle}/Icon*.png")
    list(APPEND depends ${icons})
    if(EXISTS "${ipa}/iTunesArtwork")
        list(APPEND depends "${ipa}/iTunesArtwork")
    endif()
    # Not -I: -m mtport needs tools/ on sys.path, the working directory.
    add_custom_command(
        OUTPUT "${icns}"
        COMMAND "${Python3_EXECUTABLE}" -E -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
                mtport.macicon --ipa "${ipa}" --out "${icns}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools"
        DEPENDS ${depends}
        COMMENT "Writing the app icon MapleThief.icns from the IPA's artwork"
        VERBATIM)
    target_sources(${target} PRIVATE "${icns}")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE MapleThief.icns)

    # The program looks for the game in <app>/Contents/Resources/<MAPLETHIEF_MAC_APP_GAME_DIR>.
    target_compile_definitions(${target}
                               PRIVATE MT_EMBEDDED_BUNDLE="${MAPLETHIEF_MAC_APP_GAME_DIR}")

    # Copy the game's bundle in, then seal the whole app with an ad-hoc signature. Runs on every
    # build: copy_directory_if_different copies only changed files, and codesign takes about a
    # second. The linker's own signature covers only the executable.
    add_custom_target(
        ${target}_app ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${bundle}"
                "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/${MAPLETHIEF_MAC_APP_GAME_DIR}"
        COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:${target}>"
        COMMENT "MapleThief.app: copying the game's bundle in and signing it ad hoc"
        VERBATIM)
    add_dependencies(${target}_app ${target})
    message(STATUS "MapleThief: the macOS game app (bundle from ${bundle}, ad-hoc signed)")
endfunction()
