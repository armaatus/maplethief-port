# Builds a generated program through cmake/GeneratedCode.cmake, the code path of the
# top-level target maplethief_gen (ADR 0021). tests/test_recomp_output.py configures it for
# a synthetic program (so CI checks the build plumbing and the C dialect with MSVC cl on
# Windows, without the IPA) and tests/test_recomp_output_real.py for the real one:
#
#   -DMAPLETHIEF_GEN_DIR=<dir>  the recompiler's output (gen.cmake and the sources)
#   -DGEN_MAIN=<main.c>         optional: a driver linked with tests/recomp/test_runtime.c
#                               into the executable gen_program
#
# Warnings are errors, as in CI.

cmake_minimum_required(VERSION 3.28)
project(maplethief_gen_test LANGUAGES C)

if(NOT MAPLETHIEF_GEN_DIR)
    message(FATAL_ERROR "pass -DMAPLETHIEF_GEN_DIR=<directory with gen.cmake>")
endif()

set(MAPLETHIEF_WARNINGS_AS_ERRORS ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/GeneratedCode.cmake")
maplethief_add_generated_code(maplethief_gen "${MAPLETHIEF_GEN_DIR}")

if(GEN_MAIN)
    add_executable(gen_program "${GEN_MAIN}" "${CMAKE_CURRENT_LIST_DIR}/../test_runtime.c")
    target_include_directories(gen_program PRIVATE "${CMAKE_CURRENT_LIST_DIR}/.."
                                                   "${MAPLETHIEF_GEN_DIR}")
    target_link_libraries(gen_program PRIVATE maplethief_gen)
    set_target_properties(gen_program PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON
                                                 C_EXTENSIONS OFF COMPILE_WARNING_AS_ERROR ON)
    if(MSVC)
        target_compile_options(gen_program PRIVATE /W4 /utf-8)
        target_compile_definitions(gen_program PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(gen_program PRIVATE -Wall -Wextra)
        if(NOT APPLE)
            target_link_libraries(gen_program PRIVATE m)
        endif()
    endif()
endif()
