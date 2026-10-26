# MapleThief port: top-level build (ADR 0002, ADR 0003, ADR 0004, ADR 0005).
#
# Targets:
#   runtime     static library, the HLE runtime (runtime/)
#   MapleThief  the game executable, an SDL3 frontend (platform/); a .app bundle on macOS
#   tools       byte-compiles the Python tooling (tools/) with the found Python
#   recomp      byte-compiles the Python recompiler (recomp/) with the found Python
#   maplethief_gen  static library of the recompiler's generated C (work/gen/), only when
#               python -m recomp lift has written it (cmake/GeneratedCode.cmake, ADR 0021)
#   *_test      doctest executables (tests/), registered with CTest; maplethief_golden is
#               the golden-image helper library for them
#
# Use the presets (CMakePresets.json): `cmake --workflow --preset mac-debug` configures,
# builds and tests in one command. Everything is written below build/.

cmake_minimum_required(VERSION 3.28)

project(
    MapleThief
    VERSION 0.0.1
    DESCRIPTION "Native port of MapleStory Thief Edition by static recompilation"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: "
                        "cmake --preset <os>-debug (builds into build/<preset>).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit targets are supported. On Windows, configure from an x64 "
                        "Visual Studio developer environment (see README).")
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
endfunction()

include(cmake/Dependencies.cmake)
maplethief_use_sdl3()

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
