# The guest program (#385, ADR 0213): an armv7 iOS app of our own (tests/guest/) that runs through
# every layer of the port where the game cannot: in CI.
#
# maplethief_guest_program()
#   The cache path MAPLETHIEF_GUEST_DIR (default build/guest; empty turns it off) names where
#   scripts/guest-image wrote the image (MapleThiefGuest) and its bundle (Guest.app). If the image
#   is there, the configure step recompiles it, `python -m recomp all --no-hooks --work
#   <build dir>/guest` (well under a second; again whenever the image changes), and adds
#     maplethief_guest_gen  its generated C, as maplethief_gen is the game's (GeneratedCode.cmake)
#     MapleThiefGuest       the frontend (platform/src/main.cpp) linked with it instead of the
#                           game's code: the same runtime and platform shell, no game hooks
#                           (recomp all --no-hooks lifts none, so the frontend's handlers never run)
#   The test platform.guest_program (tests/guest/guest_program_test.cmake) runs it headless and
#   checks its frame against tests/golden/data/guest_program.png, its log and its sound. Without
#   the image (any machine but a Mac that ran scripts/guest-image, or CI's jobs that did not
#   download it) the test is still listed and reports SKIPPED.
#   Needs Python3_EXECUTABLE, so the top level calls it after find_package(Python3).

function(maplethief_guest_program)
    set(MAPLETHIEF_GUEST_DIR
        "${PROJECT_SOURCE_DIR}/build/guest"
        CACHE PATH "The guest program's image from scripts/guest-image (ADR 0213); empty: off")
    set(dir "${MAPLETHIEF_GUEST_DIR}")
    set(golden "${PROJECT_SOURCE_DIR}/tests/golden/data/guest_program.png")
    set(script "${PROJECT_SOURCE_DIR}/tests/guest/guest_program_test.cmake")
    set(work "${CMAKE_BINARY_DIR}/guest")
    if(dir)
        cmake_path(ABSOLUTE_PATH dir BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    endif()
    if(NOT dir OR NOT EXISTS "${dir}/MapleThiefGuest" OR CMAKE_SYSTEM_NAME MATCHES
                                                         "^(iOS|Android|NintendoSwitch)$")
        message(STATUS "Guest program: none in '${dir}' (scripts/guest-image writes it); "
                       "platform.guest_program skips")
        add_test(NAME platform.guest_program COMMAND "${CMAKE_COMMAND}" -DPROGRAM= -P "${script}")
        set_tests_properties(platform.guest_program PROPERTIES SKIP_RETURN_CODE 77)
        if(dir AND IS_DIRECTORY "${dir}")
            set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                                                           "${dir}")
        endif()
        return()
    endif()
    set(image "${dir}/MapleThiefGuest")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                                                   "${image}")
    # Not -I: `-m recomp` needs the source directory on sys.path.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -E -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
                recomp all --slice "${image}" --work "${work}" --no-hooks --units 8
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    file(WRITE "${work}/recomp.log" "${out}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Guest program: python -m recomp all failed on ${image} (status "
                            "${status}):\n${out}")
    endif()
    maplethief_add_generated_code(maplethief_guest_gen "${work}/gen")
    maplethief_debug_opt(maplethief_guest_gen)
    target_sources(maplethief_guest_gen
                   PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src/recomp/dispatch.c")
    target_link_libraries(maplethief_guest_gen PUBLIC runtime)
    if(NOT APPLE AND NOT MSVC)
        target_link_options(maplethief_guest_gen INTERFACE "LINKER:--undefined=mt_dispatch")
    endif()
    maplethief_mem_checked(checked)
    target_compile_definitions(maplethief_guest_gen PUBLIC "MT_MEM_CHECKED=${checked}")

    set(platform "${PROJECT_SOURCE_DIR}/platform")
    add_executable(MapleThiefGuest "${platform}/src/main.cpp" "${platform}/src/gfx_prototype.cpp")
    maplethief_strict_fp("${platform}/src/gfx_prototype.cpp")
    target_link_libraries(MapleThiefGuest PRIVATE maplethief_frontend runtime SDL3::SDL3
                                                  maplethief_guest_gen)
    target_compile_definitions(
        MapleThiefGuest PRIVATE MT_HAVE_GENERATED_CODE=1 MT_APP_ID="${MAPLETHIEF_APP_ID}"
                                MT_DEV_BUNDLE_DIR="${dir}/Guest.app")
    maplethief_target_defaults(MapleThiefGuest)
    if(WIN32)
        set_target_properties(MapleThiefGuest PROPERTIES WIN32_EXECUTABLE ON)
    endif()

    add_test(
        NAME platform.guest_program
        COMMAND
            "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThiefGuest>"
            "-DBUNDLE=${dir}/Guest.app" "-DSLICE=${image}" "-DGOLDEN=${golden}"
            "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/guest_program" "-DTOOLS=${PROJECT_SOURCE_DIR}/tools"
            "-DPYTHON=${Python3_EXECUTABLE}" -P "${script}")
    set_tests_properties(platform.guest_program PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
                                                           LABELS "guest")
    message(STATUS "Guest program: ${image} recompiled into ${work}/gen (target MapleThiefGuest, "
                   "test platform.guest_program)")
endfunction()
