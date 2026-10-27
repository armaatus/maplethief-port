# The recompiler's generated C (work/gen/) in the build (ADR 0021, issue #38).
#
# maplethief_add_generated_code(<target> <gen_dir>)
#   Adds the static library <target> from the C sources listed in <gen_dir>/gen.cmake, which
#   `python -m recomp lift` writes next to them. Compiled as ADR 0002 and ADR 0014 require:
#   C17 without extensions, floating-point contraction off, the support headers of
#   runtime/include/maplethief/recomp/, -Wall -Wextra -Wpedantic or /W4. Warnings are errors
#   when MAPLETHIEF_WARNINGS_AS_ERRORS is on (CI). The recompiler rewrites a file only when
#   its content changes, so an incremental build recompiles only the changed units.
#   Self-contained, so that tests/recomp/gen_project/ builds a synthetic program with it.
#
# maplethief_generated_code()
#   The top-level build's hook. The cache path MAPLETHIEF_GEN_DIR (default work/gen; empty
#   turns it off) names the directory. If it holds a gen.cmake, the target maplethief_gen is
#   added; otherwise the configure step only says why it is skipped, so a checkout without
#   the IPA (CI) configures and builds as before. A change to gen.cmake (new or removed
#   units) re-runs the configure step by itself. The cache string MAPLETHIEF_MEM_MODEL picks
#   the guest memory model of maplethief_gen (ADR 0025, maplethief_mem_checked below).
#   maplethief_gen also compiles runtime/src/recomp/dispatch.c, the code that reads the
#   generated dispatch tables, so a program that links it and the runtime is complete.
#
# maplethief_mem_checked(<out_var>)
#   The value of MT_MEM_CHECKED for the generated code (ADR 0025): 1 (bounds-checked accesses
#   that trap with the guest PC) or 0 (no checks; the runtime must hold the full 4 GiB
#   reservation). MAPLETHIEF_MEM_MODEL "checked" or "reserved" chooses one; "auto" (the
#   default) is checked in the Debug configuration and on iOS, reserved otherwise. A generator
#   expression where it depends on the configuration.

function(maplethief_add_generated_code target gen_dir)
    cmake_path(ABSOLUTE_PATH gen_dir NORMALIZE)
    set(MAPLETHIEF_GEN_SOURCES "")
    include("${gen_dir}/gen.cmake")
    if(NOT MAPLETHIEF_GEN_SOURCES)
        message(FATAL_ERROR "${gen_dir}/gen.cmake lists no sources; re-run python -m recomp lift")
    endif()
    list(TRANSFORM MAPLETHIEF_GEN_SOURCES PREPEND "${gen_dir}/")
    add_library(${target} STATIC ${MAPLETHIEF_GEN_SOURCES})
    cmake_path(SET include NORMALIZE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/include")
    target_include_directories(${target} PRIVATE "${gen_dir}" PUBLIC "${include}")
    set_target_properties(${target} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON
                                               C_EXTENSIONS OFF)
    if(DEFINED MAPLETHIEF_WARNINGS_AS_ERRORS)
        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR
                                                   ${MAPLETHIEF_WARNINGS_AS_ERRORS})
    endif()
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # clang-cl: contraction off said explicitly (together with /fp:precise it warns that
        # one overrides the other). /bigobj: a unit can hold more than 2^16 COFF sections.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /bigobj /clang:-ffp-contract=off)
    elseif(MSVC)
        # cl: /fp:precise does not contract since VS 2022.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /fp:precise /bigobj)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
    endif()
endfunction()

function(maplethief_mem_checked out_var)
    set(model "${MAPLETHIEF_MEM_MODEL}")
    if(NOT model)
        set(model auto)
    endif()
    if(NOT model MATCHES "^(auto|checked|reserved)$")
        message(FATAL_ERROR "MAPLETHIEF_MEM_MODEL is '${model}'; use auto, checked or reserved")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        if(model STREQUAL "reserved")
            message(FATAL_ERROR "MAPLETHIEF_MEM_MODEL reserved: iOS cannot reserve 4 GiB (ADR 0025)")
        endif()
        set(model checked)
    endif()
    if(model STREQUAL "checked")
        set(${out_var} 1 PARENT_SCOPE)
    elseif(model STREQUAL "reserved")
        set(${out_var} 0 PARENT_SCOPE)
    else()
        set(${out_var} "$<IF:$<CONFIG:Debug>,1,0>" PARENT_SCOPE)
    endif()
endfunction()

function(maplethief_generated_code)
    set(MAPLETHIEF_GEN_DIR
        "${PROJECT_SOURCE_DIR}/work/gen"
        CACHE PATH "Generated C from python -m recomp lift (ADR 0021); empty to leave it out")
    set(MAPLETHIEF_MEM_MODEL
        "auto"
        CACHE STRING "Guest memory model of the generated code (ADR 0025): auto, checked, reserved")
    set_property(CACHE MAPLETHIEF_MEM_MODEL PROPERTY STRINGS auto checked reserved)
    set(dir "${MAPLETHIEF_GEN_DIR}")
    if(NOT dir)
        message(STATUS "Generated code: off (MAPLETHIEF_GEN_DIR is empty)")
    elseif(EXISTS "${dir}/gen.cmake")
        maplethief_add_generated_code(maplethief_gen "${dir}")
        # The dispatch code that reads the generated tables (ADR 0014 #35), so that a program
        # linking maplethief_gen and the runtime has mt_dispatch (the boot, ADR 0027).
        target_sources(maplethief_gen PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src/recomp/dispatch.c")
        # The generated code calls the runtime (mt_trap, mt_mem_fault, the SjLj hooks) and the
        # runtime calls mt_dispatch: with this, CMake repeats runtime after maplethief_gen on
        # the link line, which single-pass linkers (GNU ld) need.
        if(TARGET runtime)
            target_link_libraries(maplethief_gen PUBLIC runtime)
        endif()
        # PUBLIC: what links maplethief_gen sees the same MT_MEM_CHECKED, so that it asks
        # mt_mem_create for MT_MEM_RESERVE_FOR_BUILD (maplethief/memory.h).
        maplethief_mem_checked(checked)
        target_compile_definitions(maplethief_gen PUBLIC "MT_MEM_CHECKED=${checked}")
        get_target_property(sources maplethief_gen SOURCES)
        list(LENGTH sources count)
        message(STATUS "Generated code: ${count} C files in ${dir} (target maplethief_gen, "
                       "memory model ${MAPLETHIEF_MEM_MODEL})")
    else()
        message(STATUS "Generated code: none in ${dir} (python -m recomp lift writes it); "
                       "target maplethief_gen skipped")
        # Re-run the configure step once the recompiler has written its output there.
        cmake_path(GET dir PARENT_PATH parent)
        foreach(watch IN ITEMS "${dir}" "${parent}")
            if(IS_DIRECTORY "${watch}")
                set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND
                             PROPERTY CMAKE_CONFIGURE_DEPENDS "${watch}")
                break()
            endif()
        endforeach()
    endif()
endfunction()
