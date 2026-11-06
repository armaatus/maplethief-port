# Install rules and CPack: the per-platform kit (ADR 0023, issue #113), the port kit a
# release carries (ADR 0024, issue #114), and with MAPLETHIEF_PLAYER_BUILD the player download
# MapleThief-<version>-<os>-<arch> instead (ADR 0125, issue #182; its layout is below).
#
# The kit holds only our own work plus the third-party libraries it links, never anything
# derived from the IPA: no maplethief_gen (it has no install rule), nothing from work/ or
# assets-out/. Layout below the kit's top-level directory MapleThief-kit-<version>-<os>-<arch>/:
#
#   MapleThief.app (macOS) | bin/MapleThief[.exe]   the platform shell (no game code yet)
#   lib/                    runtime + static SDL3 libraries
#   include/                runtime headers (maplethief/) + SDL3 headers (SDL3/)
#   recomp/ tools/ scripts/ the recompiler, the Python tools and the user-facing scripts, in
#   pyproject.toml uv.lock  the repository's layout, so the scripts run unchanged
#   README.md THIRD_PARTY.md docs/ licenses/
#
# Generators: ZIP on Windows, TGZ on Linux, and on macOS an External script that zips with
# `ditto -c -k --norsrc --keepParent`, which keeps the bundle's symlinks and signature. The
# package lands in the build directory (build/<preset>/). Use the package presets:
# `cmake --workflow --preset <os>-release` configures, builds, tests and packages.

include(FetchContent)

# The macOS game app (MAPLETHIEF_MAC_APP, cmake/MacApp.cmake, ADR 0064) carries the IPA's bundle,
# its icon and the generated code: it is for this machine only. No install rules and no CPack, and
# `cmake --install` fails with the reason.
if(MAPLETHIEF_MAC_APP)
    install(
        CODE "message(FATAL_ERROR \"This is the game app build (MAPLETHIEF_MAC_APP): it holds the IPA's files and the generated code, so it is never installed or packaged (ADR 0024, ADR 0064). The kit comes from the mac-release-universal preset.\")"
    )
    return()
endif()

function(_maplethief_kit_platform out_var)
    if(APPLE)
        list(LENGTH CMAKE_OSX_ARCHITECTURES count)
        if(count GREATER 1)
            set(arch universal)
        elseif(count EQUAL 1)
            set(arch "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        set(${out_var} "macos-${arch}" PARENT_SCOPE)
    elseif(WIN32)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
        if(arch STREQUAL "amd64")
            set(arch x64)
        endif()
        set(${out_var} "windows-${arch}" PARENT_SCOPE)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" platform)
        set(${out_var} "${platform}" PARENT_SCOPE)
    endif()
endfunction()

# --- The player build (MAPLETHIEF_PLAYER_BUILD, cmake/PlayerBuild.cmake, ADR 0125) -----------
#
# MapleThief-<version>-<os>-<arch>/ holds only what a player needs, none of the game's assets:
#
#   MapleThief.app (macOS) | MapleThief.exe (Windows) | bin/MapleThief + MapleThief.sh +
#   icon.png (Linux)        the game, linked with the recompiled code; the port's own icon
#   README.md               platform/player/README.md: first launch, the import, help
#   LICENSE THIRD_PARTY.md licenses/
#
# No kit contents (lib/, include/, the tools): a contributor takes the kit or the source.

FetchContent_GetProperties(SDL3 SOURCE_DIR sdl3_source_dir)
if(MAPLETHIEF_PLAYER_BUILD)
    # The package builds only what it installs: the player workflows build MapleThief alone,
    # not the test executables that link the generated code too.
    set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY ON)
    if(WIN32)
        set(_maplethief_player_bin .)
    else()
        set(_maplethief_player_bin bin)
    endif()
    install(
        TARGETS MapleThief
        BUNDLE DESTINATION . COMPONENT player
        RUNTIME DESTINATION ${_maplethief_player_bin} COMPONENT player)
    if(APPLE)
        # Sealed as a whole, ad hoc, as the kit's bundle (ADR 0024, ADR 0064).
        install(
            CODE "execute_process(COMMAND codesign --force --sign - --timestamp=none \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/MapleThief.app\" COMMAND_ERROR_IS_FATAL ANY)"
            COMPONENT player)
    elseif(NOT WIN32)
        # The launcher (ADR 0065: the gamescope window, --install-desktop) and the icon its
        # .desktop entry names.
        install(PROGRAMS platform/linux/MapleThief.sh DESTINATION . COMPONENT player)
        install(FILES platform/icon/icon.png DESTINATION . COMPONENT player)
    endif()
    install(
        FILES platform/player/README.md
        DESTINATION .
        COMPONENT player)
    install(FILES LICENSE THIRD_PARTY.md DESTINATION . COMPONENT player)
    install(
        FILES "${sdl3_source_dir}/LICENSE.txt"
        DESTINATION licenses
        RENAME SDL3.txt
        COMPONENT player)
    install(
        FILES "${MAPLETHIEF_STB_LICENSE}"
        DESTINATION licenses
        RENAME stb.txt
        COMPONENT player)
    install(
        FILES "${MAPLETHIEF_FONT_LICENSE}"
        DESTINATION licenses
        RENAME NanumGothic-OFL.txt
        COMPONENT player)
    install(
        FILES "${MAPLETHIEF_FALLBACK_FONT_LICENSE}"
        DESTINATION licenses
        RENAME FiraSans-OFL.txt
        COMPONENT player)
    set(_maplethief_package_prefix "MapleThief")
else()
    set(_maplethief_package_prefix "MapleThief-kit")

# --- Install rules ---------------------------------------------------------------------------

install(
    TARGETS MapleThief
    BUNDLE DESTINATION . COMPONENT platform
    RUNTIME DESTINATION bin COMPONENT platform)
if(APPLE)
    # The linker signs only the executable (ad hoc). Sign the installed bundle as a whole, ad
    # hoc, so that its Info.plist is sealed too and `codesign --verify` passes on the unzipped
    # kit. Releases stay ad-hoc signed: no Developer ID, no notarisation (ADR 0024).
    install(
        CODE "execute_process(COMMAND codesign --force --sign - \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/MapleThief.app\" COMMAND_ERROR_IS_FATAL ANY)"
        COMPONENT platform)
endif()

install(TARGETS runtime ARCHIVE DESTINATION lib COMPONENT sdk)
install(DIRECTORY runtime/include/ DESTINATION include COMPONENT sdk)
install(FILES "$<TARGET_FILE:SDL3-static>" DESTINATION lib COMPONENT sdk)
install(DIRECTORY "${sdl3_source_dir}/include/SDL3" DESTINATION include COMPONENT sdk)

# Python parts: sources only (bytecode is written below build/, never next to them).
set(_maplethief_py_excludes PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(
    DIRECTORY recomp
    DESTINATION .
    COMPONENT tools
    USE_SOURCE_PERMISSIONS ${_maplethief_py_excludes})
install(
    DIRECTORY tools/mtport tools/ghidra
    DESTINATION tools
    COMPONENT tools
    USE_SOURCE_PERMISSIONS ${_maplethief_py_excludes})
install(FILES pyproject.toml uv.lock DESTINATION . COMPONENT tools)
# The scripts a user runs against their own IPA. The developer scripts (test, lint,
# local-ci, test-linux, re-analyze) need the whole repository and stay out.
install(
    PROGRAMS scripts/bootstrap
             scripts/bootstrap.ps1
             scripts/verify-original.sh
             scripts/pipeline
             scripts/pipeline.ps1
             scripts/difftest
             scripts/difftest.ps1
    DESTINATION scripts
    COMPONENT tools)

# Docs and licences of what the shipped binaries link: SDL3, stb (the runtime's PNG codec and
# glyph rasterizer, #51) and the fonts embedded in the runtime (NanumGothic, ADR 0036, and the
# fallback Fira Sans, ADR 0108: OFL 1.1, which requires its licence to travel with it). Add
# dr_mp3's when the runtime links it.
install(FILES README.md THIRD_PARTY.md DESTINATION . COMPONENT docs)
# Only the Markdown: should docs/ ever hold a reference crop of game art (CLAUDE.md allows
# small ones), it stays out of the kit (ADR 0024).
install(
    DIRECTORY docs/
    DESTINATION docs
    COMPONENT docs
    FILES_MATCHING
    PATTERN "*.md")
install(
    FILES "${sdl3_source_dir}/LICENSE.txt"
    DESTINATION licenses
    RENAME SDL3.txt
    COMPONENT docs)
install(
    FILES "${MAPLETHIEF_STB_LICENSE}"
    DESTINATION licenses
    RENAME stb.txt
    COMPONENT docs)
install(
    FILES "${MAPLETHIEF_FONT_LICENSE}"
    DESTINATION licenses
    RENAME NanumGothic-OFL.txt
    COMPONENT docs)
install(
    FILES "${MAPLETHIEF_FALLBACK_FONT_LICENSE}"
    DESTINATION licenses
    RENAME FiraSans-OFL.txt
    COMPONENT docs)

endif() # the kit

# --- CPack -----------------------------------------------------------------------------------

_maplethief_kit_platform(_maplethief_kit_platform)
set(CPACK_PACKAGE_NAME MapleThief)
set(CPACK_PACKAGE_VENDOR armaatus)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME
    "${_maplethief_package_prefix}-${MAPLETHIEF_VERSION}-${_maplethief_kit_platform}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
# One package with every component; the External generator would otherwise stage each component
# in its own directory.
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_VERBATIM_VARIABLES ON)
# The source package would be a copy of the checkout: GitHub attaches the source archive.
set(CPACK_SOURCE_GENERATOR "")
if(APPLE)
    set(CPACK_GENERATOR External)
    set(CPACK_EXTERNAL_ENABLE_STAGING ON)
    set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/DittoZip.cmake")
elseif(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
