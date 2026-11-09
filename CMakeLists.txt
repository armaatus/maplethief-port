# MapleThief port: top-level build (ADR 0002, ADR 0003, ADR 0004, ADR 0005).
#
# Targets:
#   runtime     static library, the HLE runtime (runtime/)
#   MapleThief  the game executable, an SDL3 frontend (platform/); a .app bundle on macOS
#   tools       byte-compiles the Python tooling (tools/) with the found Python
#   recomp      byte-compiles the Python recompiler (recomp/) with the found Python
#   maplethief_gen  static library of the recompiler's generated C (work/gen/), only when
#               python -m recomp lift has written it (cmake/GeneratedCode.cmake, ADR 0021)
#   maplethief_symbols  the guest symbol table <build dir>/maplethief.sym for the runtime's
#               diagnostics, only when the recompiler's outputs exist (ADR 0030)
#   *_test      doctest executables (tests/), registered with CTest; maplethief_golden is
#               the golden-image helper library for them
#   run, smoke  start MapleThief, or run its headless smoke test by hand (platform/)
#
# Use the presets (CMakePresets.json): `cmake --workflow --preset mac-debug` configures,
# builds and tests in one command; the <os>-release workflows also package the kit with CPack
# (cmake/Packaging.cmake, ADR 0023). Everything is written below build/.

cmake_minimum_required(VERSION 3.28)

# The version (ADR 0024): the release tag without its "v", X.Y.Z or X.Y.Z-rc.N. scripts/release
# passes it through the environment variable MAPLETHIEF_VERSION, read on the first configure;
# -DMAPLETHIEF_VERSION=... works too. Its numeric part X.Y.Z is the project version (and the
# bundle's CFBundleVersion); the whole string is MT_VERSION, `MapleThief --version` and the
# kit's file name.
if(DEFINED ENV{MAPLETHIEF_VERSION} AND NOT "$ENV{MAPLETHIEF_VERSION}" STREQUAL "")
    set(_maplethief_version_default "$ENV{MAPLETHIEF_VERSION}")
else()
    set(_maplethief_version_default 0.0.1)
endif()
set(MAPLETHIEF_VERSION
    "${_maplethief_version_default}"
    CACHE STRING "Version: X.Y.Z or X.Y.Z-rc.N (the release tag without its v, ADR 0024)")
if(NOT MAPLETHIEF_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)(-rc\\.[0-9]+)?$")
    message(FATAL_ERROR "MAPLETHIEF_VERSION '${MAPLETHIEF_VERSION}' is not X.Y.Z or X.Y.Z-rc.N")
endif()
set(_maplethief_version_numeric "${CMAKE_MATCH_1}")

project(
    MapleThief
    VERSION ${_maplethief_version_numeric}
    DESCRIPTION "Native port of MapleStory Thief Edition by static recompilation"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: "
                        "cmake --preset <os>-debug (builds into build/<preset>).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit targets are supported. On Windows, configure from an x64 "
                        "Visual Studio developer environment (see docs/build/README.md).")
endif()

# Never read or write the user package registry (~/.cmake/packages, or HKCU on Windows):
# the build must not write outside its build tree, and find_package() must not depend on
# what else is installed on the machine.
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)

# Warnings are errors in CI (ADR 0002). CI services, GitHub Actions included, set CI in the
# environment; locally the default is off so that a newer compiler doesn't block a build.
if(DEFINED ENV{CI})
    set(_maplethief_werror_default ON)
else()
    set(_maplethief_werror_default OFF)
endif()
option(MAPLETHIEF_WARNINGS_AS_ERRORS "Treat warnings in our own code as errors"
       ${_maplethief_werror_default})

# Language standard and warnings for our own targets only (ADR 0002). Third-party targets
# keep their own settings and are exempt from warnings-as-errors.
function(maplethief_target_defaults target)
    set_target_properties(
        ${target}
        PROPERTIES CXX_STANDARD 20
                   CXX_STANDARD_REQUIRED ON
                   CXX_EXTENSIONS OFF
                   C_STANDARD 17
                   C_STANDARD_REQUIRED ON
                   C_EXTENSIONS OFF
                   COMPILE_WARNING_AS_ERROR ${MAPLETHIEF_WARNINGS_AS_ERRORS})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    # clang's nearest matches for the /W4 warnings MSVC failed on before anyone ran Windows
    # (ADR 0126): C4459 (a declaration hides a global) and C4244 (int to float, float to int).
    # The Mac then catches them before the release pull request does.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(${target} PRIVATE -Wshadow -Wimplicit-int-float-conversion
                                                 -Wfloat-conversion)
    endif()
    # MSVC: the run-time checks taken out of the Debug flags above (ADR 0134), except for a
    # target with optimised code, where maplethief_debug_opt() places them per source.
    get_target_property(debug_opt ${target} MAPLETHIEF_DEBUG_OPT)
    if(MSVC AND NOT debug_opt)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:/RTC1>")
    endif()
endfunction()

# maplethief_debug_opt(<target> [<source>...]): compiles a whole target, or these of its sources
# (in the calling directory), at -O2 (/O2) in the Debug configuration, so that a Debug build runs
# the game at its real speed (#136, ADR 0134). The -g of the Debug flags, asserts (no NDEBUG) and
# MT_MEM_CHECKED stay. Call it before maplethief_target_defaults(<target>), and after the target's
# last target_sources() when naming sources.
function(maplethief_debug_opt target)
    if(MSVC)
        set(option /O2)
    else()
        set(option -O2)
    endif()
    set(optimised "$<$<CONFIG:Debug>:${option}>")
    set_target_properties(${target} PROPERTIES MAPLETHIEF_DEBUG_OPT ON)
    if(NOT ARGN)
        target_compile_options(${target} PRIVATE "${optimised}")
        return()
    endif()
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS "${optimised}")
    if(MSVC)
        # /RTC1 cannot be on the target: it would clash with /O2 on the sources above.
        get_target_property(others ${target} SOURCES)
        list(REMOVE_ITEM others ${ARGN})
        set_property(SOURCE ${others} APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Debug>:/RTC1>")
    endif()
endfunction()

# maplethief_strict_fp(<source>...): compiles these sources (of targets in the calling
# directory) without floating-point contraction, so that no compiler fuses a multiply and an add
# into an FMA and every float operation rounds the same on every CPU (ADR 0039: mt_gfx and the
# scenes of its golden images). The generated code does the same (cmake/GeneratedCode.cmake).
function(maplethief_strict_fp)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(option /clang:-ffp-contract=off)
    elseif(MSVC)
        set(option /fp:precise) # cl does not contract under /fp:precise since VS 2022
    else()
        set(option -ffp-contract=off)
    endif()
    # APPEND: a source can also be optimised in Debug (maplethief_debug_opt()).
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS "${option}")
endfunction()

include(cmake/Dependencies.cmake)
maplethief_use_sdl3()

# MSVC's default Debug flags are /Od /Ob0 /RTC1. /RTC1 and /O2 refuse each other (D8016), and a
# per-target /O2 would only override /Od with a warning, so for our own code the three leave the
# directory-wide flags (ADR 0134). /Od and /Ob0 are cl's defaults; /RTC1 comes back through
# maplethief_target_defaults() and maplethief_debug_opt(). Placed after SDL: third-party keeps
# its own flags.
if(MSVC)
    foreach(lang C CXX)
        foreach(flag /Od /Ob0 /RTC1)
            string(REPLACE "${flag}" "" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
        endforeach()
    endforeach()
endif()

enable_testing()

add_subdirectory(runtime)
include(cmake/GeneratedCode.cmake)
maplethief_generated_code()
add_subdirectory(platform)
add_subdirectory(tests)

# --- Python parts (tools/, recomp/) ----------------------------------------------------------
#
# They have nothing to compile. Each target byte-compiles its package so that a syntax error
# fails the build, with the bytecode cache redirected into the build tree (no __pycache__
# next to the sources). Their tests run through pytest (scripts/test, ADR 0005).

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

function(maplethief_python_target name dir)
    add_custom_target(
        ${name} ALL
        COMMAND "${Python3_EXECUTABLE}" -I -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
                compileall -q "${dir}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Byte-compiling ${dir}/ with Python ${Python3_VERSION}"
        VERBATIM)
endfunction()

maplethief_python_target(tools tools)
maplethief_python_target(recomp recomp)
maplethief_symbols() # <build dir>/maplethief.sym when the recompiler's outputs exist (ADR 0030)

include(cmake/Packaging.cmake)
