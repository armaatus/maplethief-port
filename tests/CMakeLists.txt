# C/C++ tests (ADR 0005). Each area gets one doctest executable; every doctest test case is
# registered with CTest as "<area>.<test case name>", next to platform.smoke. The Python
# tests in this directory run through pytest, not CTest (scripts/test runs both).

maplethief_use_doctest()

# Where tests write files for a human to look at, e.g. golden-image diffs. The presets point
# it at build/test-output/<preset>; the MAPLETHIEF_TEST_OUTPUT_DIR environment variable
# overrides it when a test runs.
set(MAPLETHIEF_TEST_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/test-output"
    CACHE PATH "Directory for test output files such as golden-image diffs (ADR 0005)")

# One main() for every doctest executable.
add_library(maplethief_doctest_main OBJECT cpp/doctest_main.cpp)
target_link_libraries(maplethief_doctest_main PUBLIC doctest::doctest)
# Let doctest include <ostream> instead of forward-declaring std::basic_ostream itself. Without
# it, MSVC's STL (VS 2026, MSVC 14.51) fails to compile any CHECK that prints a
# std::string_view ("use of undefined type std::basic_ostream"). PUBLIC, so that every test
# translation unit sees the same doctest configuration as the main().
target_compile_definitions(maplethief_doctest_main PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
maplethief_target_defaults(maplethief_doctest_main)

# maplethief_add_cpp_test(<target> AREA <area> SOURCES <src>... [LIBRARIES <lib>...])
#
# Builds <target> from the sources plus the shared doctest main, and registers each of its
# test cases with CTest as "<area>.<test case name>".
function(maplethief_add_cpp_test target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "AREA" "SOURCES;LIBRARIES")
    if(NOT arg_AREA OR NOT arg_SOURCES)
        message(FATAL_ERROR "maplethief_add_cpp_test(${target}): AREA and SOURCES are required")
    endif()
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE maplethief_doctest_main ${arg_LIBRARIES})
    maplethief_target_defaults(${target})
    doctest_discover_tests(
        ${target}
        TEST_PREFIX "${arg_AREA}."
        PROPERTIES LABELS cpp TIMEOUT 60)
endfunction()

maplethief_add_cpp_test(
    runtime_test
    AREA runtime
    SOURCES cpp/runtime_test.cpp
            cpp/recomp_headers_test.cpp
            cpp/imports_test.cpp
            cpp/memory_test.cpp
            cpp/guest_trap.cpp
    LIBRARIES runtime)

# The real image in guest memory (ADR 0025): maps work/slices/MapleThief.armv7 when
# scripts/bootstrap has written it, and is reported as skipped otherwise (always in CI).
add_executable(memory_real_test cpp/memory_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(memory_real_test PRIVATE runtime)
target_compile_definitions(
    memory_real_test PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7")
maplethief_target_defaults(memory_real_test)
add_test(NAME runtime.memory_real_image COMMAND memory_real_test)
set_tests_properties(runtime.memory_real_image PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE
                                                          77 TIMEOUT 60)

# The Objective-C runtime (ADR 0026) defines its own mt_trap / mt_dispatch test doubles, so it
# is a separate executable.
maplethief_add_cpp_test(
    objc_test
    AREA objc
    SOURCES cpp/objc_test.cpp
    LIBRARIES runtime)

add_subdirectory(golden)
