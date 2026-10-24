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
#     maplethief_use_stb()       # -> maplethief::stb (stb_image.h + stb_image_write.h)
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

# doctest 2.5.3 (MIT): tag v2.5.3, commit 2d0a9359a60c51affe2a9bebb1be1dca47868151.
set(MAPLETHIEF_DOCTEST_URL
    "https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz")
set(MAPLETHIEF_DOCTEST_SHA256 eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f)

# dr_mp3 0.7.3 (Unlicense or MIT-0): tag mp3-0.7.3 of mackron/dr_libs.
set(MAPLETHIEF_DR_MP3_COMMIT 5690d4671d7ad07ae6021756d7222eb159745f06)
set(MAPLETHIEF_DR_MP3_SHA256 d80e6800ddde29d50988ce894affe601858802d1bb8f047f98b84b33ae66c11a)

# stb_image 2.30 + stb_image_write 1.16 (MIT or Unlicense): nothings/stb has no tags.
set(MAPLETHIEF_STB_COMMIT 013ac3beddff3dbffafd5177e7972067cd2b5083)
set(MAPLETHIEF_STB_IMAGE_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
set(MAPLETHIEF_STB_IMAGE_WRITE_SHA256
    cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)

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
# function scope, so they only affect SDL.
function(maplethief_use_sdl3)
    if(TARGET SDL3::SDL3)
        return()
    endif()
    _maplethief_declare_archive(SDL3 "${MAPLETHIEF_SDL3_URL}" "${MAPLETHIEF_SDL3_SHA256}")
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
    _maplethief_header(include_dir dr_mp3-0.7.3 dr_mp3.h "${base}/dr_mp3.h"
                       "${MAPLETHIEF_DR_MP3_SHA256}")
    add_library(maplethief_dr_mp3 INTERFACE)
    target_include_directories(maplethief_dr_mp3 SYSTEM INTERFACE "${include_dir}")
    add_library(maplethief::dr_mp3 ALIAS maplethief_dr_mp3)
endfunction()

# maplethief::stb: PNG decoding, including Apple CgBI PNGs (stb_image), and PNG encoding
# for UIImagePNGRepresentation and test diff images (stb_image_write). Exactly one source
# file per binary defines STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION.
function(maplethief_use_stb)
    if(TARGET maplethief::stb)
        return()
    endif()
    set(base "https://raw.githubusercontent.com/nothings/stb/${MAPLETHIEF_STB_COMMIT}")
    _maplethief_header(include_dir stb-013ac3be stb_image.h "${base}/stb_image.h"
                       "${MAPLETHIEF_STB_IMAGE_SHA256}")
    _maplethief_header(include_dir stb-013ac3be stb_image_write.h "${base}/stb_image_write.h"
                       "${MAPLETHIEF_STB_IMAGE_WRITE_SHA256}")
    add_library(maplethief_stb INTERFACE)
    target_include_directories(maplethief_stb SYSTEM INTERFACE "${include_dir}")
    add_library(maplethief::stb ALIAS maplethief_stb)
endfunction()
