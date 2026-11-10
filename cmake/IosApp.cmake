# The iOS app (#83, ADR 0136): MapleThief.app for an iPhone or iPad, built with Xcode.
#
# maplethief_ios_target(<target>), called by platform/CMakeLists.txt on every iOS build:
#   - the asset catalog platform/icon/Assets.xcassets (the port's own icon and launch image,
#     written by tools/mtport/appicon.py), compiled by Xcode's actool;
#   - automatic signing with the team MAPLETHIEF_IOS_TEAM and the bundle identifier
#     MAPLETHIEF_IOS_BUNDLE_ID (both also read from the environment on the first configure),
#     which a free Apple account usually has to change;
#   - the device family: iPhone and iPad.
#
# MAPLETHIEF_IOS_APP (OFF by default), turned on by the ios-app preset: the game app. Needs the
# generated code (target maplethief_gen, from work/gen) and the extracted IPA (work/ipa, from
# scripts/bootstrap); fails at configure time with the steps that make them otherwise. It copies
# the extracted bundle work/ipa/Payload/MapleThief.app into MapleThief.app/Game
# (MAPLETHIEF_IOS_APP_GAME_DIR) as a POST_BUILD step, which in Xcode runs before the app is
# signed, so the signature covers it. The program finds it through SDL_GetBasePath()
# (MT_EMBEDDED_BUNDLE, main.cpp). Everything it writes is derived from the IPA and stays below
# build/: such a build is never packaged (ADR 0024).
#
# The ios-ci preset leaves it off: no generated code, no IPA, no signing. It compiles and links
# our own code into the placeholder shell.

option(MAPLETHIEF_IOS_APP
       "Build MapleThief.app for iOS as the game app: generated code and the IPA's bundle inside (needs work/gen, work/ipa; ADR 0136)"
       OFF)
set(MAPLETHIEF_IOS_APP_GAME_DIR Game)
set(MAPLETHIEF_IOS_TEAM
    "$ENV{MAPLETHIEF_IOS_TEAM}"
    CACHE STRING "Apple Developer team id that signs the iOS app (Xcode: Signing & Capabilities)")
if("$ENV{MAPLETHIEF_IOS_BUNDLE_ID}" STREQUAL "")
    set(_maplethief_ios_bundle_default "io.github.armaatus.maplethief")
else()
    set(_maplethief_ios_bundle_default "$ENV{MAPLETHIEF_IOS_BUNDLE_ID}")
endif()
set(MAPLETHIEF_IOS_BUNDLE_ID
    "${_maplethief_ios_bundle_default}"
    CACHE STRING "The iOS app's bundle identifier (a free Apple account needs one of its own)")

function(maplethief_ios_target target)
    set(catalog "${MAPLETHIEF_ICON_DIR}/Assets.xcassets")
    target_sources(${target} PRIVATE "${catalog}")
    set_source_files_properties("${catalog}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(
        ${target}
        PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
                   XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
                   XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MAPLETHIEF_IOS_BUNDLE_ID}"
                   XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
                   XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development")
    if(MAPLETHIEF_IOS_TEAM)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM
                                                   "${MAPLETHIEF_IOS_TEAM}")
    endif()
    if(MAPLETHIEF_IOS_APP)
        _maplethief_ios_game_app(${target})
    else()
        message(STATUS "MapleThief: the iOS shell (no game bundle)")
    endif()
endfunction()

function(_maplethief_ios_game_app target)
    if(NOT TARGET maplethief_gen)
        message(
            FATAL_ERROR
                "MAPLETHIEF_IOS_APP: the game app needs the generated code, and there is none in "
                "'${MAPLETHIEF_GEN_DIR}'. From the repository: scripts/bootstrap, then "
                "python3 -m recomp all, then configure again (docs/play/iphone.md).")
    endif()
    cmake_path(GET MAPLETHIEF_GEN_DIR PARENT_PATH work)
    cmake_path(ABSOLUTE_PATH work BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    set(bundle "${work}/ipa/Payload/MapleThief.app")
    if(NOT EXISTS "${bundle}/Info.plist")
        message(FATAL_ERROR "MAPLETHIEF_IOS_APP: no extracted bundle at ${bundle}; run "
                            "scripts/bootstrap")
    endif()
    target_compile_definitions(${target}
                               PRIVATE MT_EMBEDDED_BUNDLE="${MAPLETHIEF_IOS_APP_GAME_DIR}")
    # POST_BUILD: in Xcode this script phase runs before the app is signed, so the signature
    # seals the game's files too. The destination comes from Xcode's own variables, read by a
    # shell at build time: $<TARGET_BUNDLE_CONTENT_DIR> holds ${EFFECTIVE_PLATFORM_NAME}, which
    # reaches the script phase escaped, so the files would land in a folder literally named after
    # the variable.
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            /bin/sh -c
            "\"$1\" -E copy_directory_if_different \"$2\" \"$TARGET_BUILD_DIR/$CONTENTS_FOLDER_PATH/$3\""
            sh "${CMAKE_COMMAND}" "${bundle}" "${MAPLETHIEF_IOS_APP_GAME_DIR}"
        COMMENT "MapleThief.app: copying the game's bundle in"
        VERBATIM)
    message(STATUS "MapleThief: the iOS game app (bundle from ${bundle})")
endfunction()
