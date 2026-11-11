# Pinned third-party dependencies (ADR 0003, docs/adr/0003-dependencies.md).
#
# Every dependency here has a row in THIRD_PARTY.md with the same version, commit and
# SHA-256. Change both files in the same commit, and bump one dependency per commit.
#
# Usage, from the top-level CMakeLists.txt:
#
#     include(cmake/Dependencies.cmake)
#     maplethief_use_sdl3()      # -> SDL3::SDL3 (static library)
#     maplethief_use_doctest()   # -> doctest::doctest, plus doctest_discover_tests()
#     maplethief_use_dr_mp3()    # -> maplethief::dr_mp3 (header only)
#     maplethief_use_dr_flac()   # -> maplethief::dr_flac (header only)
#     maplethief_use_stb()       # -> maplethief::stb (stb_image.h, stb_image_write.h,
#                                #    stb_truetype.h)
#     maplethief_use_font(<var>) # <var> = generated C++ source embedding the bundled font
#     maplethief_use_fallback_font(<var>) # <var> = the same for the fallback font (Fira Sans)
#
# Including this file downloads nothing. Each maplethief_use_*() call downloads its files
# on first use, verifies their SHA-256 and keeps them in MAPLETHIEF_DOWNLOAD_DIR. A later
# configure, also in a fresh build directory that shares that directory, re-checks the
# hash of the cached file and does not touch the network.
#
# Everything is written below the build tree: downloads go to MAPLETHIEF_DOWNLOAD_DIR
# (default <build dir>/_downloads; the presets point it at build/_downloads so that all
# presets and the CI cache share one copy), extracted sources and dependency builds go to
# FetchContent's default <build dir>/_deps.
#
# To build against a local checkout instead (e.g. to bisect SDL), set the standard
# FETCHCONTENT_SOURCE_DIR_SDL3 / FETCHCONTENT_SOURCE_DIR_DOCTEST cache variables.

include_guard(GLOBAL)
include(FetchContent)

set(MAPLETHIEF_DOWNLOAD_DIR
    "${CMAKE_BINARY_DIR}/_downloads"
    CACHE PATH "Download cache for pinned third-party archives and headers (ADR 0003)")

# --- Pins (keep in sync with THIRD_PARTY.md) -----------------------------------------------

# SDL3 3.4.18 (zlib): tag release-3.4.18, commit 829a65d769d935c4852f8159e964312c0957260a.
set(MAPLETHIEF_SDL3_URL
    "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz")
set(MAPLETHIEF_SDL3_SHA256 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
# SDL3 for the Nintendo Switch (zlib; ADR 0138): nx-mod/sdl3-nx, SDL 3.4.18 with devkitPro's
# Switch backend, commit 07ea26a3ca7b60bac414823f0f45e8738c61c217 of branch switch-sdl3-deko3d.
# A GitHub-generated archive: if its hash ever changes, configure fails, and the commit is the pin
# to re-derive it from. A new pin needs scripts/build-shaders run again: SDL's renderer shaders
# are built from this archive (#242, ADR 0143), and CTest shaders.stamps checks this hash.
set(MAPLETHIEF_SDL3_SWITCH_URL
    "https://github.com/nx-mod/sdl3-nx/archive/07ea26a3ca7b60bac414823f0f45e8738c61c217.tar.gz")
set(MAPLETHIEF_SDL3_SWITCH_SHA256 d6511751694fb868c123d0da142ad0417420bb1fe650bca25fb2ad6c8be81801)

# doctest 2.5.3 (MIT): tag v2.5.3, commit 2d0a9359a60c51affe2a9bebb1be1dca47868151.
set(MAPLETHIEF_DOCTEST_URL
    "https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz")
set(MAPLETHIEF_DOCTEST_SHA256 eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f)

# dr_mp3 (Unlicense or MIT-0): tag mp3-<version> of mackron/dr_libs.
set(MAPLETHIEF_DR_MP3_VERSION 0.7.3)
set(MAPLETHIEF_DR_MP3_COMMIT 5690d4671d7ad07ae6021756d7222eb159745f06)
set(MAPLETHIEF_DR_MP3_SHA256 d80e6800ddde29d50988ce894affe601858802d1bb8f047f98b84b33ae66c11a)
# dr_flac (Unlicense or MIT-0): the same dr_libs commit (dr_flac 0.13.3; ADR 0106).
set(MAPLETHIEF_DR_FLAC_VERSION 0.13.3)
set(MAPLETHIEF_DR_FLAC_SHA256 a35468f278f17cf9538ce189debf76a5854a77e139980f7b5185d67566a0c837)

# stb_image 2.30 + stb_image_write 1.16 (MIT or Unlicense): nothings/stb has no tags.
set(MAPLETHIEF_STB_COMMIT 013ac3beddff3dbffafd5177e7972067cd2b5083)
set(MAPLETHIEF_STB_IMAGE_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
set(MAPLETHIEF_STB_IMAGE_WRITE_SHA256
    cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)
# stb_truetype 1.26 (MIT or Unlicense), the same commit.
set(MAPLETHIEF_STB_TRUETYPE_SHA256 a34d8d536ce7c11b9163ab2d524721c1f4df1452cce6595c4f11d3048384f925)

# NanumGothic Regular (SIL OFL 1.1), Latin + Hangul: the text font of UIFont (#51, ADR 0036).
# google/fonts has no tags; pinned to the commit that last touched ofl/nanumgothic/.
set(MAPLETHIEF_FONT_COMMIT 133ccbee9a8b408eb71f31a36ccb9116f5c695ad)
set(MAPLETHIEF_FONT_SHA256 76f45ef4a6bcff344c837c95a7dcc26e017e38b5846d5ae0cdcb5b86be2e2d31)
set(MAPLETHIEF_FONT_OFL_SHA256 eeacf16032901d0ed0456876ec77b8f0fda6b3fecec7d972f8543eb602e6c30f)
# Fira Sans Regular (SIL OFL 1.1), Latin, Latin Extended, Greek and Cyrillic: the fallback for
# the characters NanumGothic lacks (text packs, #189, ADR 0108). The same google/fonts commit.
set(MAPLETHIEF_FALLBACK_FONT_SHA256
    c29556a2719bf613ef3d5e070e40d903a8965d9c081beca1375dc1e6e0f93c23)
set(MAPLETHIEF_FALLBACK_FONT_OFL_SHA256
    8f24842e9174beda18a556c2ae7d54f5dc444340c19a3a9ef77e23bca366adbd)

# --- Helpers -------------------------------------------------------------------------------

# Download <url> to MAPLETHIEF_DOWNLOAD_DIR/<relpath> unless a file with the expected
# SHA-256 is already there. Sets <out_var> to the cached path.
function(_maplethief_download out_var relpath url sha256)
    set(cached "${MAPLETHIEF_DOWNLOAD_DIR}/${relpath}")
    set(have "")
    if(EXISTS "${cached}")
        file(SHA256 "${cached}" have)
    endif()
    if(NOT have STREQUAL sha256)
        message(STATUS "Downloading ${url}")
        file(
            DOWNLOAD "${url}" "${cached}.part"
            EXPECTED_HASH SHA256=${sha256}
            TLS_VERIFY ON
            STATUS result)
        list(GET result 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${cached}.part")
            message(FATAL_ERROR "Download of ${url} failed: ${result}")
        endif()
        file(RENAME "${cached}.part" "${cached}")
    endif()
    set(${out_var} "${cached}" PARENT_SCOPE)
endfunction()

# Declare a FetchContent dependency whose archive comes from the download cache. The
# archive's hash is checked again on extraction.
function(_maplethief_declare_archive name url sha256)
    string(TOUPPER "${name}" upper)
    if(FETCHCONTENT_SOURCE_DIR_${upper})
        set(archive "${url}") # never fetched: FetchContent uses the override directory
    else()
        get_filename_component(file "${url}" NAME)
        _maplethief_download(archive "${file}" "${url}" "${sha256}")
    endif()
    FetchContent_Declare(
        ${name}
        URL "${archive}"
        URL_HASH SHA256=${sha256}
        SYSTEM
        EXCLUDE_FROM_ALL)
endfunction()

# Copy a cached single-file library into <build dir>/_deps/<dir>/include and set
# <out_var> to that include directory.
function(_maplethief_header out_var dir name url sha256)
    _maplethief_download(cached "${dir}/${name}" "${url}" "${sha256}")
    set(include_dir "${CMAKE_BINARY_DIR}/_deps/${dir}/include")
    file(MAKE_DIRECTORY "${include_dir}")
    file(COPY_FILE "${cached}" "${include_dir}/${name}" ONLY_IF_DIFFERENT)
    set(${out_var} "${include_dir}" PARENT_SCOPE)
endfunction()

# --- Dependencies --------------------------------------------------------------------------

# SDL3: window, input, gamepads, audio device, paths. Static on every platform (iOS needs
# it, and one link model everywhere is simpler). The options are normal variables in the
# function scope, so they only affect SDL. The Nintendo Switch builds nx-mod/sdl3-nx instead of
# the upstream release (ADR 0138), without Vulkan: Mesa's NVK driver would make the NRO GPL.
function(maplethief_use_sdl3)
    if(TARGET SDL3::SDL3)
        return()
    endif()
    if(NINTENDO_SWITCH)
        # The archive is named after the commit; keep the cached file apart from other SDLs.
        if(FETCHCONTENT_SOURCE_DIR_SDL3)
            set(archive "${MAPLETHIEF_SDL3_SWITCH_URL}")
        else()
            _maplethief_download(archive "sdl3-nx/sdl3-nx-07ea26a3.tar.gz"
                                 "${MAPLETHIEF_SDL3_SWITCH_URL}" "${MAPLETHIEF_SDL3_SWITCH_SHA256}")
        endif()
        # The window follows a dock or undock: a fix to the backend at this commit (#238).
        # SDL's GPU renderer gets DKSH shaders and the deko3d swapchain follows the window (#242,
        # ADR 0143; cmake/patches/sdl3-nx-render-shaders.h comes from scripts/build-shaders).
        FetchContent_Declare(
            SDL3
            URL "${archive}"
            URL_HASH SHA256=${MAPLETHIEF_SDL3_SWITCH_SHA256}
            PATCH_COMMAND "${CMAKE_COMMAND}" -P
                          "${PROJECT_SOURCE_DIR}/cmake/patches/sdl3-nx-window-size.cmake"
            COMMAND "${CMAKE_COMMAND}" -P
                    "${PROJECT_SOURCE_DIR}/cmake/patches/sdl3-nx-gpu-renderer.cmake"
            SYSTEM
            EXCLUDE_FROM_ALL)
        set(SDL_VULKAN OFF)
    else()
        _maplethief_declare_archive(SDL3 "${MAPLETHIEF_SDL3_URL}" "${MAPLETHIEF_SDL3_SHA256}")
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    FetchContent_MakeAvailable(SDL3)
endfunction()

# doctest::doctest (header only), for C/C++ unit tests run through CTest. Also defines
# doctest_discover_tests() (functions are global, and doctest.cmake keeps its helper
# script path in the cache).
function(maplethief_use_doctest)
    if(TARGET doctest::doctest)
        return()
    endif()
    _maplethief_declare_archive(doctest "${MAPLETHIEF_DOCTEST_URL}"
                                "${MAPLETHIEF_DOCTEST_SHA256}")
    set(DOCTEST_WITH_TESTS OFF)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
    set(DOCTEST_NO_INSTALL ON)
    FetchContent_MakeAvailable(doctest)
    include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
endfunction()

# maplethief::dr_mp3: MP3 decoding for AVAudioPlayer BGM (#63). Exactly one runtime
# source file defines DR_MP3_IMPLEMENTATION before including dr_mp3.h.
function(maplethief_use_dr_mp3)
    if(TARGET maplethief::dr_mp3)
        return()
    endif()
    set(base "https://raw.githubusercontent.com/mackron/dr_libs/${MAPLETHIEF_DR_MP3_COMMIT}")
    _maplethief_header(include_dir "dr_mp3-${MAPLETHIEF_DR_MP3_VERSION}" dr_mp3.h
                       "${base}/dr_mp3.h" "${MAPLETHIEF_DR_MP3_SHA256}")
    add_library(maplethief_dr_mp3 INTERFACE)
    target_include_directories(maplethief_dr_mp3 SYSTEM INTERFACE "${include_dir}")
    add_library(maplethief::dr_mp3 ALIAS maplethief_dr_mp3)
endfunction()

# maplethief::dr_flac: FLAC decoding for audio packs (#188, ADR 0106). Exactly one runtime
# source file defines DR_FLAC_IMPLEMENTATION before including dr_flac.h.
function(maplethief_use_dr_flac)
    if(TARGET maplethief::dr_flac)
        return()
    endif()
    set(base "https://raw.githubusercontent.com/mackron/dr_libs/${MAPLETHIEF_DR_MP3_COMMIT}")
    _maplethief_header(include_dir "dr_flac-${MAPLETHIEF_DR_FLAC_VERSION}" dr_flac.h
                       "${base}/dr_flac.h" "${MAPLETHIEF_DR_FLAC_SHA256}")
    add_library(maplethief_dr_flac INTERFACE)
    target_include_directories(maplethief_dr_flac SYSTEM INTERFACE "${include_dir}")
    add_library(maplethief::dr_flac ALIAS maplethief_dr_flac)
endfunction()

# maplethief::stb: PNG decoding, including Apple CgBI PNGs (stb_image), PNG encoding for
# UIImagePNGRepresentation and test diff images (stb_image_write), and glyph outlines for
# UIFont (stb_truetype). Each user defines the *_IMPLEMENTATION macros in one source file,
# with the *_STATIC macros, so that the runtime's and the golden helper's copies never clash.
function(maplethief_use_stb)
    if(TARGET maplethief::stb)
        return()
    endif()
    set(base "https://raw.githubusercontent.com/nothings/stb/${MAPLETHIEF_STB_COMMIT}")
    # stb has no versions: the cache directory is named after the commit, e.g. stb-013ac3be.
    string(SUBSTRING "${MAPLETHIEF_STB_COMMIT}" 0 8 short_commit)
    _maplethief_header(include_dir "stb-${short_commit}" stb_image.h "${base}/stb_image.h"
                       "${MAPLETHIEF_STB_IMAGE_SHA256}")
    _maplethief_header(include_dir "stb-${short_commit}" stb_image_write.h
                       "${base}/stb_image_write.h" "${MAPLETHIEF_STB_IMAGE_WRITE_SHA256}")
    _maplethief_header(include_dir "stb-${short_commit}" stb_truetype.h "${base}/stb_truetype.h"
                       "${MAPLETHIEF_STB_TRUETYPE_SHA256}")
    # The licence text at the end of stb_image.h (the same in every stb header), for the kit's
    # licenses/ (cmake/Packaging.cmake).
    file(READ "${include_dir}/stb_image.h" header)
    string(FIND "${header}" "This software is available under 2 licenses" at)
    string(SUBSTRING "${header}" ${at} -1 licence)
    string(REGEX REPLACE "\\*/[ \t\r\n]*$" "" licence "${licence}")
    file(WRITE "${CMAKE_BINARY_DIR}/_deps/stb-${short_commit}/LICENSE.txt" "${licence}")
    set(MAPLETHIEF_STB_LICENSE "${CMAKE_BINARY_DIR}/_deps/stb-${short_commit}/LICENSE.txt"
        CACHE INTERNAL "stb's licence text")
    add_library(maplethief_stb INTERFACE)
    target_include_directories(maplethief_stb SYSTEM INTERFACE "${include_dir}")
    add_library(maplethief::stb ALIAS maplethief_stb)
endfunction()

# The bundled font (#51, ADR 0036): NanumGothic-Regular.ttf, embedded into the runtime as a
# generated C++ source that defines
#
#     extern const unsigned char mt_cg_font_data[];  // the TTF file's bytes
#     extern const std::size_t mt_cg_font_size;
#
# Sets <out_var> to that source (below the build tree, never committed) and the cache entry
# MAPLETHIEF_FONT_LICENSE to the font's OFL.txt for the kit's licenses/. The source is written
# at configure time, and again only when the font's hash changes.
function(maplethief_use_font out_var)
    set(base
        "https://raw.githubusercontent.com/google/fonts/${MAPLETHIEF_FONT_COMMIT}/ofl/nanumgothic")
    string(SUBSTRING "${MAPLETHIEF_FONT_COMMIT}" 0 8 short_commit)
    set(dir "nanumgothic-${short_commit}")
    _maplethief_download(ttf "${dir}/NanumGothic-Regular.ttf" "${base}/NanumGothic-Regular.ttf"
                         "${MAPLETHIEF_FONT_SHA256}")
    _maplethief_download(ofl "${dir}/OFL.txt" "${base}/OFL.txt" "${MAPLETHIEF_FONT_OFL_SHA256}")
    set(MAPLETHIEF_FONT_LICENSE "${ofl}" CACHE INTERNAL "The bundled font's licence (OFL 1.1)")
    _maplethief_embed_font(out "${ttf}" "${MAPLETHIEF_FONT_SHA256}"
                           "${CMAKE_BINARY_DIR}/_deps/${dir}/cg_font_data.cpp" mt_cg_font
                           "${base}/NanumGothic-Regular.ttf"
                           "NanumGothic: Copyright (c) 2010, NHN Corporation")
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# The fallback font (#189, ADR 0108): FiraSans-Regular.ttf, embedded as a generated source that
# defines mt_cg_fallback_font_data[] and mt_cg_fallback_font_size. UIFont's text draws a
# character NanumGothic has no glyph for with it (none of the game's own characters: a text
# pack's accented Latin, Greek and Cyrillic). Sets the cache entry
# MAPLETHIEF_FALLBACK_FONT_LICENSE to its OFL.txt.
function(maplethief_use_fallback_font out_var)
    set(base "https://raw.githubusercontent.com/google/fonts/${MAPLETHIEF_FONT_COMMIT}/ofl/firasans")
    string(SUBSTRING "${MAPLETHIEF_FONT_COMMIT}" 0 8 short_commit)
    set(dir "firasans-${short_commit}")
    _maplethief_download(ttf "${dir}/FiraSans-Regular.ttf" "${base}/FiraSans-Regular.ttf"
                         "${MAPLETHIEF_FALLBACK_FONT_SHA256}")
    _maplethief_download(ofl "${dir}/OFL.txt" "${base}/OFL.txt"
                         "${MAPLETHIEF_FALLBACK_FONT_OFL_SHA256}")
    set(MAPLETHIEF_FALLBACK_FONT_LICENSE "${ofl}"
        CACHE INTERNAL "The fallback font's licence (OFL 1.1)")
    _maplethief_embed_font(out "${ttf}" "${MAPLETHIEF_FALLBACK_FONT_SHA256}"
                           "${CMAKE_BINARY_DIR}/_deps/${dir}/cg_fallback_font_data.cpp"
                           mt_cg_fallback_font "${base}/FiraSans-Regular.ttf"
                           "Fira Sans: Copyright (c) 2012-2015, The Mozilla Foundation and Telefonica S.A.")
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# Writes <out> (a C++ source defining <symbol>_data[] and <symbol>_size with the bytes of <ttf>)
# at configure time, and again only when the font's hash <sha256> changes. Sets <out_var>.
function(_maplethief_embed_font out_var ttf sha256 out symbol url notice)
    set(stamp "${out}.sha256")
    set(have "")
    if(EXISTS "${out}" AND EXISTS "${stamp}")
        file(READ "${stamp}" have)
    endif()
    if(NOT have STREQUAL sha256)
        message(STATUS "Embedding ${ttf}")
        file(READ "${ttf}" hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
        # Eight bytes per line, so that no compiler meets a 2 MB line.
        set(byte "0x[0-9a-f][0-9a-f],")
        string(REGEX REPLACE "(${byte}${byte}${byte}${byte}${byte}${byte}${byte}${byte})" "\\1\n"
                             hex "${hex}")
        file(
            WRITE "${out}"
            "// Generated by cmake/Dependencies.cmake (_maplethief_embed_font) from\n"
            "// ${url}\n"
            "// ${notice}; SIL Open Font License 1.1.\n"
            "#include <cstddef>\n\n"
            "extern const unsigned char ${symbol}_data[];\n"
            "extern const std::size_t ${symbol}_size;\n\n"
            "alignas(4) const unsigned char ${symbol}_data[] = {\n${hex}};\n"
            "const std::size_t ${symbol}_size = sizeof ${symbol}_data;\n")
        file(WRITE "${stamp}" "${sha256}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()
